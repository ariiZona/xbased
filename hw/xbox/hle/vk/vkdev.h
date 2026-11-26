/*
 * xbased HLE GPU — its own Vulkan instance and device, and what the device
 * has (HleCaps).
 */
#ifndef HW_XBOX_HLE_VK_VKDEV_H
#define HW_XBOX_HLE_VK_VKDEV_H

#include <volk.h>

/* Command buffers in flight (back.c's ring, one recording each): a slot's
 * recording starts once its previous submission has completed, so the
 * per-slot resources (batch.c's indices, gamma.c, zread.c) hold one each. */
#define HLE_FRAMES 3

/* VK_KHR_dynamic_rendering_local_read (Vulkan 1.4) for headers older than it:
 * what HleCaps.fbfetch uses */
#ifndef VK_KHR_dynamic_rendering_local_read
#define VK_KHR_DYNAMIC_RENDERING_LOCAL_READ_EXTENSION_NAME "VK_KHR_dynamic_rendering_local_read"
#define VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DYNAMIC_RENDERING_LOCAL_READ_FEATURES_KHR \
    ((VkStructureType)1000232000)
#define VK_IMAGE_LAYOUT_RENDERING_LOCAL_READ_KHR ((VkImageLayout)1000232000)
typedef struct VkPhysicalDeviceDynamicRenderingLocalReadFeaturesKHR {
    VkStructureType sType;
    void *pNext;
    VkBool32 dynamicRenderingLocalRead;
} VkPhysicalDeviceDynamicRenderingLocalReadFeaturesKHR;
#endif

/* VK_EXT_attachment_feedback_loop_layout for headers older than it: what
 * HleCaps.fbloop uses */
#ifndef VK_EXT_attachment_feedback_loop_layout
#define VK_EXT_ATTACHMENT_FEEDBACK_LOOP_LAYOUT_EXTENSION_NAME "VK_EXT_attachment_feedback_loop_layout"
#define VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ATTACHMENT_FEEDBACK_LOOP_LAYOUT_FEATURES_EXT \
    ((VkStructureType)1000339000)
#define VK_IMAGE_LAYOUT_ATTACHMENT_FEEDBACK_LOOP_OPTIMAL_EXT ((VkImageLayout)1000339000)
#define VK_IMAGE_USAGE_ATTACHMENT_FEEDBACK_LOOP_BIT_EXT 0x00080000
#define VK_PIPELINE_CREATE_COLOR_ATTACHMENT_FEEDBACK_LOOP_BIT_EXT 0x02000000
typedef struct VkPhysicalDeviceAttachmentFeedbackLoopLayoutFeaturesEXT {
    VkStructureType sType;
    void *pNext;
    VkBool32 attachmentFeedbackLoopLayout;
} VkPhysicalDeviceAttachmentFeedbackLoopLayoutFeaturesEXT;
#endif

/* The bindless set (vk/draw.c): images per kind (2D, cube, 3D) and samplers. */
#define HLE_MAX_IMAGES   4096
#define HLE_MAX_SAMPLERS 256
/* The draws' push block (shader/ff.h HleFfPush) */
#define HLE_PUSH_BYTES   256

typedef struct HleCaps {
    bool floor_ok;
    char missing[256];          /* floor features absent (comma list) */
    /* What some drivers lack, each with a substitute. debug.xbased.caps /
     * XBASED_HLE_CAPS turns them off by name ("-bc,-vids,..."): any device runs
     * the substitutes. */
    bool bc;                    /* BC1-3 sampled: DXT uploaded as is, else decoded to
                                   A8R8G8B8 on the way into the arena (pb/pbres.c) */
    bool vids;                  /* VK_EXT_vertex_input_dynamic_state, else the vertex
                                   layout is in the pipeline (vk/draw.c PipeKey.vi) */
    bool legacy_vattr;          /* VK_EXT_legacy_vertex_attributes: an attribute may lie at an
                                   address its component size does not divide */
    bool push;                  /* the push block (HleFfPush) fits the push constants,
                                   else it is a storage buffer (vk/draw.c, set 3) */
    bool push_desc;             /* VK_KHR_push_descriptor for fbfetch's set 2, else sets
                                   from a pool per command buffer */
    bool cbc;                   /* VK_EXT_custom_border_color, else cbc_any is off too */
    bool cbc_any;               /* custom border colours without a format, for every
                                   sampler, else the nearest standard colour */
    bool x4444;                 /* A4R4G4B4 sampled (VK_EXT_4444_formats, core in 1.3),
                                   else 4444 textures expand to A8R8G8B8 on the way into
                                   the arena (pb/pbres.c) */
    bool x565, x1555;           /* R5G6B5 / A1R5G5B5 sampled (not on Intel Macs' Metal: MoltenVK),
                                   else those textures expand to A8R8G8B8 as 4444's do */
    bool rt565, rt1555;         /* ... and as colour targets, else such a surface isn't
                                   rendered (vk/surf.c) */
    bool bindless;              /* descriptor indexing (the bindless set, its limits), else
                                   each draw's textures in a set of their own at fixed
                                   places (vk/draw.c fixed_bind) */
    bool depth_clamp;           /* depthClamp, else a z-clamped draw's vertex shader clamps
                                   z to the depth range (shader/ff.h HLE_ZCLAMP_GLSL) */
    bool d24s8;                 /* D24_UNORM_S8_UINT a sampled, filtered depth attachment,
                                   else D32_SFLOAT_S8_UINT (vk/surf.c) */
    bool vfmt3;                 /* the 3-component 8- and 16-bit vertex formats as the driver
                                   reports them; off (debug), taken as missing, as on AMD's:
                                   their 4-component forms then (vk/draw.c vfmt) */
    /* Below Vulkan 1.3 (older drivers, Mesa's hasvk): 1.3's parts from their
     * extensions where the driver has them, else emulated (vk/compat.c); each
     * true on a 1.3 device */
    bool vk13;                  /* else timeline semaphores and descriptor indexing from
                                   their extensions, SPIR-V 1.3 (shader/glsl_compile.c) */
    bool dyn_render;            /* dynamic rendering, else render passes (compat.c) */
    bool sync2;                 /* synchronization2, else 1.0's barriers and submits */
    bool eds1;                  /* extended dynamic state, else the draws' cull, topology,
                                   depth and stencil state and vertex strides are in their
                                   pipelines (vk/draw.c PipeKey.ds) */
    bool eds2;                  /* extended dynamic state 2, else depth bias and primitive
                                   restart enables are too, and strips aren't batched */
    /* enabled where present */
    bool gpl, eds3, descriptor_buffer, maint5, sscaled, local_read;
    bool fbfetch;               /* local read (+ push descriptors or pooled sets):
                                   combiner stages that sample the bound colour target
                                   read it as an input attachment (vk/draw.c
                                   hle_draw_fb_stages) */
    bool fbloop;                /* attachment feedback loops (dynamic rendering's): a draw
                                   whose stages sample the colour target it renders reads
                                   the target itself, its pass reopened after the earlier
                                   draws' writes, not a copy made before it (vk/draw.c
                                   hle_draw_loop_begin) */
    bool exec_props, aniso;
    bool host_query_reset, occl_precise;        /* visibility tests */
    bool wide_lines;            /* enabled when present: lines as wide as the NV2A's
                                   SET_LINE_WIDTH */
    bool fill_lines;            /* fillModeNonSolid: SET_FRONT / BACK_POLYGON_MODE's
                                   lines and points */
    bool cache_control;         /* pipelineCreationCacheControl: a pipeline asked of the
                                   cache only (vk/draw.c, asynchronous builds) */
    bool swapchain;             /* the window's surface and VK_KHR_swapchain: the window's
                                   presenter (vk/swapchain.c) */
    bool share;                 /* Windows: images and timeline semaphores exported as NT
                                   handles (the game's frame presents them: vk/share.h) */
    uint8_t luid[8];            /* the device's LUID (luid_valid), the frame's device found by it */
    bool luid_valid;
    float max_lod_bias;         /* maxSamplerLodBias */
    float max_aniso;            /* maxSamplerAnisotropy (0: no anisotropic filtering) */
} HleCaps;

typedef struct HleVk {
    VkInstance inst;
    VkPhysicalDevice phys;
    VkDevice dev;
    VkQueue queue;
    uint32_t qfam;
    struct VolkDeviceTable t;   /* device-level entry points of `dev` (volk.h declares
                                 * only the struct tag, no typedef) */
    VkPhysicalDeviceProperties props;
    VkPhysicalDeviceMemoryProperties mem;
    HleCaps caps;
} HleVk;

extern HleVk g_hvk;

/* 0 = ready; -1 = no Vulkan; -2 = floor not met (g_hvk.caps.missing).
 * Vulkan 1.1 is the floor's version: 1.2's and 1.3's parts come from their
 * extensions (timeline semaphores required) or vk/compat.c. */
int hle_vk_init(void);
/* The back end is up (hle_back_start done): other threads may use g_hvk. */
bool hle_vk_ready(void);

/* The device can't go on (VK_ERROR_DEVICE_LOST: a driver fault; or a
 * submit that failed, whose timeline value would never signal): every wait
 * on the GPU returns (hle_tl_lose), the back end and the presenter stop,
 * and the host hears why (hle_host_gpu_lost). Once; later calls do nothing. */
void hle_vk_lose(VkResult r, const char *what);
/* hle_vk_lose was called: nothing more goes to the device. */
bool hle_vk_lost(void);
/* r == VK_SUCCESS; else false, the first few logged, and a lost device
 * reported (hle_vk_lose). */
bool hle_vk_check(VkResult r, const char *what);
/* r's name ("VK_ERROR_DEVICE_LOST"), or its number. */
const char *hle_vk_result_text(VkResult r);
void hle_vk_set_ready(void);
/* g_hvk.queue's external synchronization: the render thread, the window's
 * presenter and a capture submit to it, and vkDeviceWaitIdle needs it too. */
void hle_vk_queue_lock(void);
void hle_vk_queue_unlock(void);
/* The time its lock was waited for since the last call (statistics). */
int64_t hle_vk_queue_wait_ns(void);
int hle_vk_memtype(uint32_t bits, VkMemoryPropertyFlags want,
                   VkMemoryPropertyFlags avoid);

#endif
