/*
 * xbased native core — device MMIO through fault decoding (mmio.h): the
 * faulting host instruction replayed (cpu/x64emu.h) against the device.
 */
#include "native/core/mmio.h"
#include <stdlib.h>
#include <time.h>

#include <stdio.h>
#include <ucontext.h>
#include <unistd.h>

#define MAX_DEVS  16
#define MAX_SITES 1024
#define LOG_FIRST 400

static NcMmioDev g_devs[MAX_DEVS];
static unsigned g_ndevs;

static struct {
    uint32_t addr;
    bool write;
    uint32_t count;
    uint64_t last;
} g_sites[MAX_SITES];
static unsigned g_logged;
static uint64_t g_total;
static uint64_t g_other;       /* accesses whose site found no slot in a few probes */

void nc_mmio_register(const NcMmioDev *dev)
{
    if (g_ndevs < MAX_DEVS) {
        g_devs[g_ndevs++] = *dev;
    }
}

static const NcMmioDev *dev_at(uint32_t addr)
{
    for (unsigned i = 0; i < g_ndevs; i++) {
        if (addr - g_devs[i].base < g_devs[i].size) {
            return &g_devs[i];
        }
    }
    return NULL;
}

/* The per-site tally (nc_mmio_dump_stats), a few probes at most: with the
 * table full (the APU's voice registers alone fill it), each access probed
 * all 1024 slots, a fifth of Crimson Sea's guest thread in push-buffer mode
 * (its trap handler's ~110k register accesses a second). */
#define SITE_PROBES 8

static void count(uint32_t addr, bool write, uint64_t value)
{
    unsigned h = ((addr ^ write) * 2654435761u) % MAX_SITES;
    for (unsigned n = 0; n < SITE_PROBES; n++, h = (h + 1) % MAX_SITES) {
        if (!g_sites[h].count || (g_sites[h].addr == addr && g_sites[h].write == write)) {
            g_sites[h].addr = addr;
            g_sites[h].write = write;
            g_sites[h].count++;
            g_sites[h].last = value;
            return;
        }
    }
    g_other++;
}

/* An access to dev's window at off. A device is handed only what is inside
 * its window: it indexes its registers with the offset. An access that
 * reaches past an end of the window (a dword at its last two bytes, or one
 * that begins below it) went to the device whole, and a register file was
 * read and written past its end. Of such an access the bytes inside go to
 * the device one at a time; the rest are no device's: they read as ones and
 * writes to them go nowhere. So do sizes a device isn't asked for (3, 5-7). */
static uint64_t dev_read(const NcMmioDev *dev, uint32_t off, unsigned bytes)
{
    if (off < dev->size && bytes <= dev->size - off && bytes <= 8 && !(bytes & (bytes - 1))) {
        return dev->read(dev->opaque, off, bytes);
    }
    uint64_t v = 0;
    for (unsigned i = 0; i < bytes && i < 8; i++) {
        const uint32_t o = off + i;
        v |= (o < dev->size ? dev->read(dev->opaque, o, 1) & 0xFF : 0xFFull) << (8 * i);
    }
    return v;
}

static void dev_write(const NcMmioDev *dev, uint32_t off, uint64_t v, unsigned bytes)
{
    if (off < dev->size && bytes <= dev->size - off && bytes <= 8 && !(bytes & (bytes - 1))) {
        dev->write(dev->opaque, off, v, bytes);
        return;
    }
    for (unsigned i = 0; i < bytes && i < 8; i++) {
        const uint32_t o = off + i;
        if (o < dev->size) {
            dev->write(dev->opaque, o, (v >> (8 * i)) & 0xFF, 1);
        }
    }
}

#include "native/cpu/x64emu.h"
#include "native/cpu/cpu.h"

/* A string instruction's elements: a device's registers where its window
 * is, ordinary memory elsewhere (a copy between RAM and a device). */
static uint64_t str_rd(void *opaque, uint64_t addr, unsigned bytes)
{
    const NcMmioDev *dev = addr >> 32 ? NULL : dev_at((uint32_t)addr);
    uint64_t v = 0;
    if (dev) {
        v = dev_read(dev, (uint32_t)addr - dev->base, bytes);
        g_total++;
        count((uint32_t)addr, false, v);
    } else {
        memcpy(&v, (const void *)(uintptr_t)addr, bytes);
    }
    return v;
}

static void str_wr(void *opaque, uint64_t addr, uint64_t v, unsigned bytes)
{
    const NcMmioDev *dev = addr >> 32 ? NULL : dev_at((uint32_t)addr);
    if (dev) {
        dev_write(dev, (uint32_t)addr - dev->base, v, bytes);
        g_total++;
        count((uint32_t)addr, true, v);
    } else {
        memcpy((void *)(uintptr_t)addr, &v, bytes);
    }
}

bool nc_mmio_fault(const NcVm *vm, const siginfo_t *info, void *ucontext)
{
    const uint64_t guest = nc_vm_guest_of(vm, info->si_addr);
    if (guest == UINT64_MAX) {
        return false;
    }
    const NcMmioDev *dev = dev_at((uint32_t)guest);
    if (!dev) {
        return false;
    }
    const NcX64Mem sm = {str_rd, str_wr, NULL};
    if (nc_x64_string(ucontext, &sm)) {
        return true;
    }
    NcX64Access a;
    if (!nc_x64_access(ucontext, (uint64_t)(uintptr_t)info->si_addr, &a)) {
        static unsigned logged;
        if (logged++ < 16) {
            const uint8_t *pc = (const uint8_t *)nc_cpu_context_pc(ucontext);
            nc_log("xbased-native-mmio", "%s %08x: the host instruction at %p isn't one to replay "
                   "(%02x %02x %02x %02x %02x %02x %02x %02x)", dev->name, (uint32_t)guest, (const void *)pc, pc[0],
                   pc[1], pc[2], pc[3], pc[4], pc[5], pc[6], pc[7]);
        }
        return false;
    }
    const uint64_t first64 = nc_vm_guest_of(vm, (void *)(uintptr_t)a.ea);
    if (first64 == UINT64_MAX) {
        return false;
    }
    const uint32_t first = (uint32_t)first64;
    uint8_t data[16] = {0};
    uint64_t v = 0;
    if (a.read) {
        if (a.bytes == 16) {
            const uint64_t lo = dev_read(dev, first - dev->base, 8), hi = dev_read(dev, first + 8 - dev->base, 8);
            memcpy(data, &lo, 8);
            memcpy(data + 8, &hi, 8);
        } else {
            v = dev_read(dev, first - dev->base, a.bytes);
            memcpy(data, &v, 8);
        }
    }
    if (!nc_x64_run(ucontext, data)) {
        return false;
    }
    if (a.write) {
        if (a.bytes == 16) {
            uint64_t lo, hi;
            memcpy(&lo, data, 8);
            memcpy(&hi, data + 8, 8);
            dev_write(dev, first - dev->base, lo, 8);
            dev_write(dev, first + 8 - dev->base, hi, 8);
        } else {
            v = 0;
            memcpy(&v, data, a.bytes > 8 ? 8 : a.bytes);
            dev_write(dev, first - dev->base, v, a.bytes);
        }
    }

    g_total++;
    count(first, a.write, v);
    if (g_logged < LOG_FIRST) {
        g_logged++;
        char line[160];
        int n = snprintf(line, sizeof(line), "xbased-native-mmio: %s %s+%06x %s %0*llx (%u)\n", dev->name,
                         a.write ? "w" : "r", first - dev->base, a.write ? "<-" : "->",
                         (int)(2 * (a.bytes > 8 ? 8 : a.bytes)), (unsigned long long)v, a.bytes);
        nc_dump_line(2, line, (size_t)n);
    }
    return true;
}

bool nc_ram_access_span(const NcVm *vm, const siginfo_t *info, void *ucontext, uint32_t *lo,
                        uint32_t *hi, bool *store)
{
    NcX64Access a;
    if (!nc_x64_access(ucontext, (uint64_t)(uintptr_t)info->si_addr, &a)) {
        return false;
    }
    const uint64_t g = nc_vm_guest_of(vm, (void *)(uintptr_t)a.ea);
    const uint32_t p0 = g == UINT64_MAX ? UINT32_MAX : nc_vm_phys(vm, (uint32_t)g);
    if (p0 == UINT32_MAX || (p0 & (NC_PAGE - 1)) + a.bytes > NC_PAGE || (uint64_t)p0 + a.bytes > vm->phys_end) {
        return false;
    }
    *lo = p0;
    *hi = p0 + a.bytes;
    *store = a.write;
    return true;
}

bool nc_ram_emulate(NcVm *vm, const siginfo_t *info, void *ucontext,
                    void (*stored)(uint32_t pa, uint32_t n))
{
    NcX64Access a;
    if (!nc_x64_access(ucontext, (uint64_t)(uintptr_t)info->si_addr, &a)) {
        return false;
    }
    const uint64_t g = nc_vm_guest_of(vm, (void *)(uintptr_t)a.ea);
    const uint32_t pa = g == UINT64_MAX ? UINT32_MAX : nc_vm_phys(vm, (uint32_t)g);
    if (pa == UINT32_MAX || (pa & (NC_PAGE - 1)) + a.bytes > NC_PAGE || (uint64_t)pa + a.bytes > vm->phys_end) {
        return false;
    }
    uint8_t data[16];
    memcpy(data, vm->phys + pa, a.bytes);
    if (!nc_x64_run(ucontext, data)) {
        return false;
    }
    if (a.write) {
        stored(pa, a.bytes);
        memcpy(vm->phys + pa, data, a.bytes);
    }
    return true;
}

/* nc_mmio_access's store may run again (nc_mmio_retry); one was asked for */
static __thread bool t_retry_ok, t_retry;

bool nc_mmio_retry(void)
{
    if (!t_retry_ok) {
        return false;
    }
    t_retry = true;
    return true;
}

bool nc_mmio_take_retry(void)
{
    const bool r = t_retry;
    t_retry = false;
    return r;
}

bool nc_mmio_access(uint32_t addr, unsigned bytes, bool store, uint64_t *v)
{
    const NcMmioDev *dev = dev_at(addr);
    if (!dev) {
        return false;
    }
    const uint64_t mask = bytes < 8 ? (1ull << (8 * bytes)) - 1 : ~0ull;
    if (store) {
        t_retry_ok = true;
        dev_write(dev, addr - dev->base, *v & mask, bytes);
        t_retry_ok = false;
    } else {
        *v = dev_read(dev, addr - dev->base, bytes) & mask;
    }
    g_total++;
    count(addr, store, *v);
    return true;
}

uint64_t nc_mmio_total(void)
{
    return g_total;
}

void nc_mmio_dump_stats(int fd)
{
    char line[160];
    int n = snprintf(line, sizeof(line), "xbased-native-mmio: %llu accesses (%llu at sites not tallied); "
                     "busiest sites:\n", (unsigned long long)g_total, (unsigned long long)g_other);
    nc_dump_line(fd, line, (size_t)n);
    bool shown[MAX_SITES] = {0};
    const char *top = getenv("NC_MMIO_TOP");
    const int limit = top ? atoi(top) : 16;
    for (int k = 0; k < limit; k++) {
        int best = -1;
        for (int i = 0; i < MAX_SITES; i++) {
            if (g_sites[i].count && !shown[i] && (best < 0 || g_sites[i].count > g_sites[best].count)) {
                best = i;
            }
        }
        if (best < 0) {
            break;
        }
        shown[best] = true;
        const NcMmioDev *dev = dev_at(g_sites[best].addr);
        n = snprintf(line, sizeof(line), "xbased-native-mmio:   %s %08x %s x%u last %llx\n", dev ? dev->name : "?",
                     g_sites[best].addr, g_sites[best].write ? "w" : "r", g_sites[best].count,
                     (unsigned long long)g_sites[best].last);
        nc_dump_line(fd, line, (size_t)n);
    }
}
