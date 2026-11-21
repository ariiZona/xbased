/*
 * xbased HLE GPU — Vulkan driver quirks: what a driver gets wrong that the
 * back end works around. Each quirk is a bit with a name and the drivers it
 * is on for by default, picked from the driver's identity at device
 * creation (quirks.c). A new one is a row in that table and a test of
 * hle_quirk() where the back end works around it.
 *
 * A/B on any driver: XBASED_HLE_QUIRKS at start, or the live property
 * debug.xbased.quirks: names separated by commas or spaces, "+name" (or the
 * bare name) on and "-name" off on top of the driver's defaults, "+all" /
 * "-all" every quirk. Empty: the driver's defaults.
 */
#ifndef HW_XBOX_HLE_VK_QUIRKS_H
#define HW_XBOX_HLE_VK_QUIRKS_H

#include "hw/xbox/hle/vk/vkdev.h"

#include <stdbool.h>
#include <stdint.h>

enum {
    /* Every dynamic state recorded again after a pipeline bind: for telling
     * whether a driver loses state a pipeline bind should keep (A/B; on for
     * no driver). */
    HLE_QUIRK_STATE_AFTER_PIPELINE = 1u << 0,
    /* A present with a semaphore to wait on: the presenter waits for its
     * pass itself and presents with nothing to wait for (vk/swapchain.c).
     * On for every driver; -present_cpu_wait presents with the wait. */
    HLE_QUIRK_PRESENT_CPU_WAIT = 1u << 1,
    /* A vertex attribute whose address its component size doesn't divide
     * (Vulkan's rule: a float at a 2-byte boundary, as an Xbox vertex of 18
     * bytes puts every other one) is read wrong (AMD's vertex fetch): such a
     * binding's vertices are copied to the arena with each attribute moved to
     * an offset its components divide (pb/pbdraw.c). */
    HLE_QUIRK_VB_ALIGN = 1u << 2,
    /* The swapchain image's acquire semaphore is never signalled when no
     * image was free at the acquire (MoltenVK 1.4.2: MVKSwapchain ignores
     * the timeout and returns a busy image; its later signal goes to
     * encodeDeferredSignal with a nil command buffer, a no-op): a pass
     * waiting on it would hold the one Metal queue for good. Found in its
     * source while chasing a freeze at Halo 2's Cairo Station intro (now
     * and then, the GPU short of a submitted value); not shown to be that
     * freeze's cause. The presenter acquires with a fence instead
     * (signalled on the CPU), waits for it itself, and its pass waits for
     * nothing (vk/swapchain.c). */
    HLE_QUIRK_ACQUIRE_FENCE = 1u << 3,
};

extern uint32_t g_hle_quirks;

static inline bool hle_quirk(uint32_t q)
{
    return (g_hle_quirks & q) != 0;
}

/* The driver's quirks (the device is created), and XBASED_HLE_QUIRKS. */
void hle_quirks_init(VkPhysicalDevice phys);

/* Once a frame: debug.xbased.quirks changed. */
void hle_quirks_poll(void);

#endif
