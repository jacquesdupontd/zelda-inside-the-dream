// vk_xr.h: an OpenXR shell plugged under the Vulkan backend's offscreen mode (the Quest 3 app, android/xr_app.c).
// It creates the Vulkan instance and device through the OpenXR runtime (XR_KHR_vulkan_enable2: the runtime adds the
// extensions it needs), opens its session once the device exists, and receives every finished frame: the game's
// image, to copy into its swapchain in the frame's own command buffer.
#pragma once
#include <stdbool.h>
#include <vulkan/vulkan.h>

typedef struct VkXrHooks {
    VkResult (*create_instance)(const VkInstanceCreateInfo *ci, VkInstance *out);
    VkPhysicalDevice (*physical_device)(VkInstance inst);  // the GPU the headset uses
    VkResult (*create_device)(VkPhysicalDevice phys, const VkDeviceCreateInfo *ci, VkDevice *out);
    void (*device_ready)(VkInstance inst, VkPhysicalDevice phys, VkDevice dev, uint32_t queue_family);
    // a finished frame: `image` (layout COLOR_ATTACHMENT_OPTIMAL, the frame's commands not submitted yet), recorded
    // into `cb`; then frame_submitted once the command buffer is on the queue
    void (*frame)(VkCommandBuffer cb, VkImage image, VkExtent2D extent);
    void (*frame_submitted)(void);
    // optional (NULL: the copy in `frame`): the swapchain's own images, which the backend then renders each frame's
    // final pass into, without a copy - `images` lists them once the device is ready (viewed as RGBA8 UNORM, whatever
    // the swapchain's format); `acquire`, before that final pass, takes the next one: its index, or -1 when there is
    // none (that frame goes to an image of the backend's own, and through `frame`'s copy)
    uint32_t (*images)(VkImage *out, uint32_t max);
    int (*acquire)(void);
    // a flat screen's shell (android/flat_app.c, the tablet): these hooks only, nothing of a headset's - the black at
    // a door, the widescreen, the game's projection in the view from above and the held layer as on a desktop
    bool flat;
    // optional, a flat shell only: its window handed to the backend, which then makes and presents its own swapchain as
    // with a desktop window - the first-person warp presenter at the display's rate included - instead of rendering
    // offscreen into `frame`. surface: one for the shell's window now (VK_NULL_HANDLE while it has none); size: the
    // image's size (0 x 0 while there is no window)
    VkSurfaceKHR (*surface)(VkInstance inst);
    void (*size)(int *w, int *h);
} VkXrHooks;

extern VkXrHooks *g_vk_xr;
static inline bool vk_xr_headset(void) { return g_vk_xr && !g_vk_xr->flat; }  // (a headset's shell, not the tablet's)
// The queue is shared with the OpenXR runtime, which may use it from the shell's own thread: every submission holds it.
void vk_queue_lock(void);
void vk_queue_unlock(void);
