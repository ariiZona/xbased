/*
 * xbased native core — objects and handles.
 *
 * Objects live in guest memory as on the Xbox: an OBJECT_HEADER
 * (PointerCount, HandleCount, Type, Flags) followed by the body; handles
 * (multiples of 4) map to the body. Type is the VA of the kernel's type
 * object (PsThreadObjectType, ExEventObjectType, ...).
 */
#include "native/kernel/kernel.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>

#define OB_HEADER 0x10u

/* Body size of each type's objects, for freeing them: file and symbolic-link
 * objects, all one size, whose host state goes with them (nc_io_close). */
static uint32_t g_ob_size[NC_OB_TYPES];

/* Events, semaphores, mutants and threads go as the console's do: with their
 * last handle and reference (ObfDereferenceObject), unless a thread waits on
 * one; a thread once it has ended and is off its stack, and not while a
 * mutant still names it its owner. Kept for good, the ones a title made and
 * closed filled the pool: RalliSport Challenge 2 makes and closes events,
 * mutants and threads loading each race (the console's count stays the
 * same race after race), the pool grew a chunk at a time where the
 * contiguous blocks go, and its main menu's 25.3 MiB found no room with 48
 * MiB free. Each one's body size is kept here as it is made (threads' differ
 * by their extension); one made before a savestate's load isn't known and
 * stays, as all of them did before. */
typedef struct VaMap {
    uint32_t *key, *val;
    uint32_t cap, n;
} VaMap;
static VaMap g_sized;
/* mutants that have had an owner (ke.c's acquire): a thread one names stays */
static VaMap g_owned;
/* closed ones a wait kept: freed once nothing waits on them (ob_retry) */
static uint32_t *g_waited;
static unsigned g_nwaited, g_waited_cap;

static uint32_t map_hash(uint32_t va, uint32_t cap)
{
    return ((va >> 4) * 2654435761u) & (cap - 1);
}

static uint32_t map_get(const VaMap *m, uint32_t va)
{
    if (!m->cap) {
        return 0;
    }
    for (uint32_t i = map_hash(va, m->cap);; i = (i + 1) & (m->cap - 1)) {
        if (!m->key[i]) {
            return 0;
        }
        if (m->key[i] == va) {
            return m->val[i];
        }
    }
}

static void map_put(VaMap *m, uint32_t va, uint32_t val)
{
    if ((m->n + 1) * 2 > m->cap) {
        VaMap g = {0};
        g.cap = m->cap ? m->cap * 2 : 1024;
        g.key = calloc(g.cap, sizeof(uint32_t));
        g.val = calloc(g.cap, sizeof(uint32_t));
        if (!g.key || !g.val) {
            free(g.key);
            free(g.val);
            return;                     /* (not known: it stays) */
        }
        for (uint32_t i = 0; i < m->cap; i++) {
            if (m->key[i]) {
                map_put(&g, m->key[i], m->val[i]);
            }
        }
        free(m->key);
        free(m->val);
        *m = g;
    }
    uint32_t i = map_hash(va, m->cap);
    while (m->key[i] && m->key[i] != va) {
        i = (i + 1) & (m->cap - 1);
    }
    m->n += !m->key[i];
    m->key[i] = va;
    m->val[i] = val;
}

static void map_del(VaMap *m, uint32_t va)
{
    if (!m->cap) {
        return;
    }
    uint32_t i = map_hash(va, m->cap);
    while (m->key[i] != va) {
        if (!m->key[i]) {
            return;
        }
        i = (i + 1) & (m->cap - 1);
    }
    m->key[i] = 0;
    m->n--;
    /* the entries after it in its run move back where a lookup finds them */
    for (uint32_t j = (i + 1) & (m->cap - 1); m->key[j]; j = (j + 1) & (m->cap - 1)) {
        const uint32_t h = map_hash(m->key[j], m->cap);
        if (((j - h) & (m->cap - 1)) >= ((j - i) & (m->cap - 1))) {
            m->key[i] = m->key[j];
            m->val[i] = m->val[j];
            m->key[j] = 0;
            i = j;
        }
    }
}

static bool counted(enum NcObType type)
{
    return type == NC_OB_EVENT || type == NC_OB_SEMAPHORE || type == NC_OB_MUTANT || type == NC_OB_THREAD;
}

static void waited_drop(uint32_t object)
{
    for (unsigned i = 0; i < g_nwaited; i++) {
        if (g_waited[i] == object) {
            g_waited[i] = g_waited[--g_nwaited];
            return;
        }
    }
}

static void waited_add(uint32_t object)
{
    for (unsigned i = 0; i < g_nwaited; i++) {
        if (g_waited[i] == object) {
            return;
        }
    }
    if (g_nwaited == g_waited_cap) {
        const unsigned cap = g_waited_cap ? g_waited_cap * 2 : 64;
        uint32_t *w = cap <= 65536 ? realloc(g_waited, cap * sizeof(uint32_t)) : NULL;
        if (!w) {
            return;                     /* (it stays) */
        }
        g_waited = w;
        g_waited_cap = cap;
    }
    g_waited[g_nwaited++] = object;
}

static enum NcObType type_of(NcKernel *k, uint32_t object);

void nc_ob_mutant_owned(uint32_t mutant)
{
    if (!map_get(&g_owned, mutant)) {
        map_put(&g_owned, mutant, 1);
    }
}

/* A mutant (the title's own too) names the thread its owner: it never
 * released it, and a thread made where this one was would own it. */
static bool mutant_owner_is(NcKernel *k, uint32_t ethread)
{
    for (uint32_t i = 0; i < g_owned.cap; i++) {
        const uint32_t m = g_owned.key[i];
        if (m && nc_vm_phys(k->vm, m) != UINT32_MAX && nc_ke_mutant_owner(k, m) == ethread) {
            return true;
        }
    }
    return false;
}

/* An event, semaphore, mutant or thread with no handle and no reference
 * left goes; true if it went. */
static bool release_counted(NcKernel *k, uint32_t object, enum NcObType type)
{
    const uint32_t hdr = object - OB_HEADER;
    const uint32_t size = map_get(&g_sized, object);
    if (!size || nc_vm_rd32(k->vm, hdr + 0x4) || nc_vm_rd32(k->vm, hdr) > 1 ||
        nc_vm_rd32(k->vm, hdr + 0x8) != k->type_va[type]) {
        return false;
    }
    if (type == NC_OB_THREAD && (nc_ps_thread_of(k, object) || mutant_owner_is(k, object))) {
        return false;       /* running or not yet off its stack (nc_ob_thread_gone), or a mutant's owner */
    }
    if (nc_ke_object_waited(k, object)) {
        waited_add(object);
        return false;
    }
    waited_drop(object);
    map_del(&g_sized, object);
    if (type == NC_OB_MUTANT) {
        map_del(&g_owned, object);
    }
    nc_vm_wr32(k->vm, hdr + 0x8, 0);    /* a stale lookup of it fails the type check */
    k->pool_ob -= OB_HEADER + size;
    nc_pool_free(k, hdr, OB_HEADER + size);
    return true;
}

/* The closed ones waits kept, again: those nothing waits on now go. */
static void ob_retry(NcKernel *k)
{
    for (unsigned i = g_nwaited; i-- > 0;) {
        if (i >= g_nwaited) {
            continue;
        }
        const uint32_t object = g_waited[i];
        const enum NcObType type = type_of(k, object);
        if (type >= NC_OB_TYPES || !counted(type) || !map_get(&g_sized, object)) {
            waited_drop(object);        /* (not one any more) */
        } else if (!nc_ke_object_waited(k, object) && !release_counted(k, object, type)) {
            waited_drop(object);        /* a handle or a reference again: it goes with that one */
        }
    }
}

void nc_ob_thread_gone(NcKernel *k, uint32_t ethread)
{
    release_counted(k, ethread, NC_OB_THREAD);
}

void nc_ob_reference(NcKernel *k, uint32_t object)
{
    const uint32_t hdr = object - OB_HEADER;
    nc_vm_wr32(k->vm, hdr, nc_vm_rd32(k->vm, hdr) + 1);
}

/* A savestate loaded: its objects are none of those this run made. */
void nc_ob_forget(void)
{
    free(g_sized.key);
    free(g_sized.val);
    g_sized = (VaMap){0};
    free(g_owned.key);
    free(g_owned.val);
    g_owned = (VaMap){0};
    g_nwaited = 0;
}

static bool freeable(enum NcObType type)
{
    return type == NC_OB_FILE || type == NC_OB_SYMLINK || type == NC_OB_TIMER;
}

static enum NcObType type_of(NcKernel *k, uint32_t object)
{
    const uint32_t tva = nc_vm_rd32(k->vm, object - OB_HEADER + 0x8);
    for (unsigned t = 0; t < NC_OB_TYPES; t++) {
        if (k->type_va[t] == tva) {
            return (enum NcObType)t;
        }
    }
    return NC_OB_TYPES;
}

/* Frees an object no handle or reference holds any more (PointerCount 1 is
 * the one it was created with). */
static void release(NcKernel *k, uint32_t object, enum NcObType type)
{
    if (type < NC_OB_TYPES && counted(type)) {
        release_counted(k, object, type);
        return;
    }
    const uint32_t hdr = object - OB_HEADER;
    if (type >= NC_OB_TYPES || !freeable(type) || !g_ob_size[type] || g_ob_size[type] == UINT32_MAX ||
        nc_vm_rd32(k->vm, hdr + 0x4) ||
        nc_vm_rd32(k->vm, hdr) > 1) {
        return;
    }
    if (type == NC_OB_TIMER) {
        /* A timer goes as the console's does with its last handle (its
         * delete routine cancels it): out of the timer queue first, and
         * kept while a thread still waits on it. Kept for good, the timers
         * of a title that makes one per wait filled RAM: The Nightmare
         * Before Christmas: Oogie's Revenge's 171,000 by its title movie,
         * 9.3 MiB of pool, and the movie's next block found no room (#326). */
        if (nc_ke_object_waited(k, object)) {
            return;
        }
        nc_timer_forget(k, object);
    }
    nc_vm_wr32(k->vm, hdr + 0x8, 0);    /* a stale lookup of it fails the type check */
    k->pool_ob -= OB_HEADER + g_ob_size[type];
    nc_pool_free(k, hdr, OB_HEADER + g_ob_size[type]);
}

void nc_ob_dereference(NcKernel *k, uint32_t object)
{
    const uint32_t hdr = object - OB_HEADER;
    const uint32_t count = nc_vm_rd32(k->vm, hdr);
    if (count) {
        nc_vm_wr32(k->vm, hdr, count - 1);
    }
    release(k, object, type_of(k, object));
}

uint32_t nc_ob_create(NcKernel *k, enum NcObType type, uint32_t body_size)
{
    if (g_nwaited) {
        ob_retry(k);
    }
    if (freeable(type) && g_ob_size[type] != UINT32_MAX) {
        if (g_ob_size[type] && g_ob_size[type] != body_size) {
            nc_log("xbased-native", "ob: type %u objects of %u and %u bytes: not freed", type, g_ob_size[type],
                   body_size);
            g_ob_size[type] = UINT32_MAX;   /* for good: the sizes are mixed */
        } else {
            g_ob_size[type] = body_size;
        }
    }
    uint32_t h = nc_pool_alloc(k, OB_HEADER + body_size);
    if (!h) {
        return 0;
    }
    k->pool_ob += OB_HEADER + body_size;
    if (counted(type) && body_size) {
        map_put(&g_sized, h + OB_HEADER, body_size);
    }
    nc_vm_wr32(k->vm, h + 0x0, 1);              /* PointerCount */
    nc_vm_wr32(k->vm, h + 0x8, k->type_va[type]); /* Type */
    return h + OB_HEADER;
}

/* Handles are taken round the table from the one after the last given, so
 * a handle just closed isn't the next one opened. Test Drive closes the
 * file handle its archive slots keep (closed already, after their headers
 * were read) a second time as it frees them; taking the lowest free handle,
 * that number was its loader thread's by then, the thread's handle went,
 * the next thread got the number, and the game resumed that one forever
 * waiting for the loader (a hang at its race's loading screen). */
uint32_t nc_ob_handle(NcKernel *k, uint32_t object, enum NcObType type)
{
    static unsigned s_next = 1;
    for (unsigned n = 1; n < NC_MAX_HANDLES; n++) {
        const unsigned i = s_next >= NC_MAX_HANDLES ? 1 : s_next;
        s_next = i + 1;
        if (!k->handle_table[i].object) {
            k->handle_table[i] = (NcHandle){object, type};
            uint32_t hdr = object - OB_HEADER;
            nc_vm_wr32(k->vm, hdr + 0x4, nc_vm_rd32(k->vm, hdr + 0x4) + 1); /* HandleCount */
            return i * 4;
        }
    }
    nc_log("xbased-native", "ob: out of handles");
    return 0;
}

uint32_t nc_ob_lookup(NcKernel *k, uint32_t handle, enum NcObType *type)
{
    uint32_t i = handle / 4;
    if (handle & 3 || !i || i >= NC_MAX_HANDLES || !k->handle_table[i].object) {
        return 0;
    }
    if (type) {
        *type = k->handle_table[i].type;
    }
    return k->handle_table[i].object;
}

/* Closes a handle; a file's host state goes with its last handle. */
bool nc_ob_close(NcKernel *k, uint32_t handle)
{
    uint32_t i = handle / 4;
    if (handle & 3 || !i || i >= NC_MAX_HANDLES || !k->handle_table[i].object) {
        return false;
    }
    const uint32_t object = k->handle_table[i].object;
    const enum NcObType type = k->handle_table[i].type;
    const uint32_t hdr = object - OB_HEADER;
    const uint32_t count = nc_vm_rd32(k->vm, hdr + 0x4) - 1;
    nc_vm_wr32(k->vm, hdr + 0x4, count);
    k->handle_table[i].object = 0;
    if (!count) {
        nc_ob_unname(k, object);    /* a named object goes from the namespace with its last handle */
    }
    if (!count && (type == NC_OB_FILE || type == NC_OB_SYMLINK)) {
        nc_io_close(k, object);
    }
    if (!count) {
        release(k, object, type);
    }
    return true;
}

/* NtClose(Handle): a file or symbolic link goes with its last handle and
 * reference (release). */
static uint64_t NtClose(NcKernel *k, NcFrame *f, const uint32_t *a)
{
    return nc_ob_close(k, a[0]) ? NC_STATUS_SUCCESS : NC_STATUS_INVALID_HANDLE;
}

/* NtDuplicateObject(SourceHandle, TargetHandle, Options): DUPLICATE_CLOSE_SOURCE is 1.
 * NtCurrentThread (-2) duplicates to a real handle of the running thread. */
static uint64_t NtDuplicateObject(NcKernel *k, NcFrame *f, const uint32_t *a)
{
    enum NcObType type = NC_OB_THREAD;
    uint32_t obj = a[0] == 0xFFFFFFFEu && k->current ? k->current->ethread : nc_ob_lookup(k, a[0], &type);
    if (!obj) {
        return NC_STATUS_INVALID_HANDLE;
    }
    uint32_t h = nc_ob_handle(k, obj, type);
    if (!h) {
        return NC_STATUS_INSUFFICIENT_RESOURCES;
    }
    nc_vm_wr32(k->vm, a[1], h);
    if ((a[2] & 1) && a[0] != 0xFFFFFFFEu) {
        nc_ob_close(k, a[0]);
    }
    return NC_STATUS_SUCCESS;
}

/* --------------------------------------------------------- named objects */

/* Events, semaphores and mutants created with a name (the named objects
 * XAPI's CreateEvent makes and OpenEvent opens by ObOpenObjectByName), in a
 * list in the kernel's memory so a savestate keeps it with the objects:
 * the first node at NC_OB_NAMES, a node {next, object, type, the name's
 * length, the name}. A name goes with the last handle (nc_ob_unname). */
#define NC_OB_NAMES (NC_KERNEL_VA + 0xA400u)
#define OBJ_OPENIF 0x80u
#define STATUS_OBJECT_NAME_EXISTS 0x40000000u
#define STATUS_OBJECT_NAME_NOT_FOUND 0xC0000034u
#define STATUS_OBJECT_NAME_COLLISION 0xC0000035u

size_t nc_ob_attr_name(NcKernel *k, uint32_t attrs, char *out, size_t n)
{
    out[0] = 0;
    const uint32_t str = attrs ? nc_vm_rd32(k->vm, attrs + 4) : 0;
    if (!str || n < 2) {
        return 0;
    }
    size_t len = nc_vm_rd32(k->vm, str) & 0xFFFF;
    const uint32_t buf = nc_vm_rd32(k->vm, str + 4);
    if (!buf) {
        return 0;
    }
    len = len < n - 1 ? len : n - 1;
    for (size_t i = 0; i < len; i++) {
        out[i] = (char)*(const uint8_t *)nc_vm_host(k->vm, buf + (uint32_t)i);
    }
    out[len] = 0;
    return len;
}

static uint32_t named(NcKernel *k, const char *name, enum NcObType *type)
{
    const size_t len = strlen(name);
    uint32_t node = nc_vm_rd32(k->vm, NC_OB_NAMES);
    for (unsigned guard = 0; node && guard < 4096; guard++, node = nc_vm_rd32(k->vm, node)) {
        if (nc_vm_rd32(k->vm, node + 12) != len) {
            continue;
        }
        size_t i = 0;
        while (i < len && tolower(*(const uint8_t *)nc_vm_host(k->vm, node + 16 + (uint32_t)i)) ==
                              tolower((unsigned char)name[i])) {
            i++;
        }
        if (i == len) {
            /* the list is guest memory: a type that is none (the title's
             * stray write, a damaged state) is no object, not an index past
             * the kernel's type table */
            const uint32_t t = nc_vm_rd32(k->vm, node + 8);
            if (t >= NC_OB_TYPES) {
                return 0;
            }
            *type = (enum NcObType)t;
            return nc_vm_rd32(k->vm, node + 4);
        }
    }
    return 0;
}

void nc_ob_name(NcKernel *k, uint32_t object, enum NcObType type, const char *name)
{
    const size_t len = strlen(name);
    const uint32_t node = nc_pool_alloc(k, 16 + (uint32_t)len + 1);
    if (!node) {
        return;
    }
    nc_vm_wr32(k->vm, node, nc_vm_rd32(k->vm, NC_OB_NAMES));
    nc_vm_wr32(k->vm, node + 4, object);
    nc_vm_wr32(k->vm, node + 8, (uint32_t)type);
    nc_vm_wr32(k->vm, node + 12, (uint32_t)len);
    for (size_t i = 0; i <= len; i++) {
        *(uint8_t *)nc_vm_host(k->vm, node + 16 + (uint32_t)i) = (uint8_t)name[i];
    }
    nc_vm_wr32(k->vm, NC_OB_NAMES, node);
}

/* The object's name, if it has one, out of the list: as on the Xbox, a
 * named object that isn't permanent leaves the namespace when its last
 * handle closes, and opening the name fails from then on. Chicago
 * Enforcer's loading screen opens its LoadScreenSave and LoadScreenLoad
 * events until they're gone: kept here, it spun on them for good. */
void nc_ob_unname(NcKernel *k, uint32_t object)
{
    uint32_t prev = NC_OB_NAMES, node = nc_vm_rd32(k->vm, NC_OB_NAMES);
    for (unsigned guard = 0; node && guard < 4096; guard++) {
        const uint32_t next = nc_vm_rd32(k->vm, node);
        if (nc_vm_rd32(k->vm, node + 4) == object) {
            nc_vm_wr32(k->vm, prev, next);
            nc_pool_free(k, node, 16 + nc_vm_rd32(k->vm, node + 12) + 1);
        } else {
            prev = node;
        }
        node = next;
    }
}

bool nc_ob_create_named(NcKernel *k, uint32_t attrs, const char *name, enum NcObType type, uint32_t handle_out,
                        uint32_t *status)
{
    enum NcObType have;
    const uint32_t obj = named(k, name, &have);
    if (!obj) {
        return false;
    }
    if (have != type) {
        *status = NC_STATUS_OBJECT_TYPE_MISMATCH;
    } else if (!(nc_vm_rd32(k->vm, attrs + 8) & OBJ_OPENIF)) {
        *status = STATUS_OBJECT_NAME_COLLISION;
    } else {
        const uint32_t h = nc_ob_handle(k, obj, type);
        nc_vm_wr32(k->vm, handle_out, h);
        *status = h ? STATUS_OBJECT_NAME_EXISTS : NC_STATUS_INSUFFICIENT_RESOURCES;
    }
    return true;
}

/* ObOpenObjectByName(ObjectAttributes, ObjectType, ParseContext, Handle):
 * a named event, semaphore or mutant (OpenEvent, OpenSemaphore, OpenMutex;
 * Chicago Enforcer's loading screen opens its events by name, and the
 * emulator stopped there as a new game started). */
static uint64_t ObOpenObjectByName(NcKernel *k, NcFrame *f, const uint32_t *a)
{
    char name[256];
    nc_ob_attr_name(k, a[0], name, sizeof(name));
    enum NcObType type;
    const uint32_t obj = name[0] ? named(k, name, &type) : 0;
    if (!obj) {
        static unsigned told;
        if (told++ < 50) {
            nc_log("xbased-native", "ob: open %s: no such object", name[0] ? name : "(no name)");
        }
        return STATUS_OBJECT_NAME_NOT_FOUND;
    }
    if (a[1] && k->type_va[type] != a[1]) {
        return NC_STATUS_OBJECT_TYPE_MISMATCH;
    }
    const uint32_t h = nc_ob_handle(k, obj, type);
    if (!h) {
        return NC_STATUS_INSUFFICIENT_RESOURCES;
    }
    nc_vm_wr32(k->vm, a[3], h);
    return NC_STATUS_SUCCESS;
}

void nc_ob_register(NcKernel *k)
{
    nc_kernel_impl(k, "NtClose", NtClose);
    nc_kernel_impl(k, "NtDuplicateObject", NtDuplicateObject);
    nc_kernel_impl(k, "ObOpenObjectByName", ObOpenObjectByName);
}
