/*
 * xbased HLE GPU — Vulkan driver quirks (see quirks.h).
 */
#include "hw/xbox/hle/base.h"
#include "hw/xbox/hle/vk/quirks.h"

#include "hw/xbox/hle/hle.h"

#include "native/core/props.h"
#include "hw/xbox/hle/prop.h"

uint32_t g_hle_quirks;

typedef struct HleDriver {
    VkPhysicalDeviceProperties props;
    VkPhysicalDeviceDriverProperties drv;
} HleDriver;

typedef struct HleQuirk {
    uint32_t bit;
    const char *name;
    bool (*on)(const HleDriver *d);     /* the drivers it is on for; NULL: none */
} HleQuirk;

/* every driver */
static bool any(const HleDriver *d)
{
    (void)d;
    return true;
}

static bool moltenvk(const HleDriver *d)
{
    return d->drv.driverID == VK_DRIVER_ID_MOLTENVK;
}

/* AMD hardware, any driver (vendor 0x1002) */
static bool amd(const HleDriver *d)
{
    return d->props.vendorID == 0x1002;
}

/* Seen on (driver, version, what showed it). */
static const HleQuirk k_quirks[] = {
    { HLE_QUIRK_STATE_AFTER_PIPELINE, "state_after_pipeline", NULL },
    { HLE_QUIRK_PRESENT_CPU_WAIT, "present_cpu_wait", any },
    /* AMD's proprietary driver (Radeon 890M, 26.10.1, an ROG Xbox Ally X,
     * 2026-11-04): Grand Theft Auto San Andreas' and Vice City's characters
     * stretched into spikes; their vertices are 9 bytes, three 16-bit
     * integers at offset 3, which AMD's vertex fetch reads only at an offset
     * their size divides. */
    { HLE_QUIRK_VB_ALIGN, "vb_align", amd },
    /* MoltenVK 1.4.2 (Radeon Pro 5300M, macOS 15, 2026-11-21): MVKImage.mm's
     * deferred acquire signal is encoded into a nil command buffer (see
     * quirks.h); found while Halo 2 froze now and then at Cairo Station's
     * intro (the GPU short of a submitted value, 4 submits pending, no
     * error), that freeze not reproduced since nor tied to it. */
    { HLE_QUIRK_ACQUIRE_FENCE, "acquire_fence", moltenvk },
};

static uint32_t g_defaults;

static void names(uint32_t mask, char *out, size_t len)
{
    size_t n = 0;
    out[0] = 0;
    for (size_t i = 0; i < ARRAY_SIZE(k_quirks); i++) {
        if (mask & k_quirks[i].bit) {
            n += (size_t)snprintf(out + n, n < len ? len - n : 0, "%s%s", n ? "," : "",
                                  k_quirks[i].name);
        }
    }
    if (!n) {
        snprintf(out, len, "-");
    }
}

/* The defaults with an override's "+name" / "-name" applied. */
static uint32_t apply(uint32_t mask, const char *spec)
{
    char buf[256];
    snprintf(buf, sizeof(buf), "%s", spec ? spec : "");
    for (char *save = NULL, *t = strtok_r(buf, ", ", &save); t; t = strtok_r(NULL, ", ", &save)) {
        bool off = *t == '-';
        const char *name = *t == '-' || *t == '+' ? t + 1 : t;
        uint32_t bits = 0;
        if (!strcmp(name, "all")) {
            for (size_t i = 0; i < ARRAY_SIZE(k_quirks); i++) {
                bits |= k_quirks[i].bit;
            }
        }
        for (size_t i = 0; i < ARRAY_SIZE(k_quirks); i++) {
            if (!strcmp(name, k_quirks[i].name)) {
                bits = k_quirks[i].bit;
            }
        }
        if (!bits) {
            hle_log("xbased-hle-back", "quirks: \"%s\" is not a quirk (ignored)", name);
        }
        mask = off ? mask & ~bits : mask | bits;
    }
    return mask;
}

static void set(uint32_t mask, const char *why)
{
    char on[256], def[256];
    g_hle_quirks = mask;
    names(mask, on, sizeof(on));
    names(g_defaults, def, sizeof(def));
    hle_log("xbased-hle-back", "quirks: %s (driver defaults %s%s%s)", on, def, why ? "; " : "",
            why ? why : "");
}

void hle_quirks_init(VkPhysicalDevice phys)
{
    HleDriver d = { .drv = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES } };
    VkPhysicalDeviceProperties2 p2 = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2,
                                       .pNext = &d.drv };
    vkGetPhysicalDeviceProperties2(phys, &p2);
    d.props = p2.properties;
    g_defaults = 0;
    for (size_t i = 0; i < ARRAY_SIZE(k_quirks); i++) {
        if (k_quirks[i].on && k_quirks[i].on(&d)) {
            g_defaults |= k_quirks[i].bit;
        }
    }
    hle_log("xbased-hle-back", "vk: driver \"%s\" (id %u) %s", d.drv.driverName, d.drv.driverID,
            d.drv.driverInfo);
    const char *e = getenv("XBASED_HLE_QUIRKS");
    set(apply(g_defaults, e), e ? "XBASED_HLE_QUIRKS" : NULL);
}

void hle_quirks_poll(void)
{
    static char last[NC_PROP_VALUE_MAX];
    static uint32_t polls;
    if (polls++ % 30) {
        return;
    }
    char v[NC_PROP_VALUE_MAX] = "";
    hle_prop_get("debug.xbased.quirks", v);
    if (strcmp(v, last)) {
        strcpy(last, v);
        set(apply(g_defaults, v), v[0] ? "debug.xbased.quirks" : NULL);
    }
}

