// flat_app.c: LAS Recomp on an Android tablet or phone, on its own screen - no headset. A NativeActivity: the game runs
// as on the Quest (android/xr_app.c), the Vulkan backend in its offscreen mode, and every finished frame is scaled into
// the window's swapchain (runtime/nvn/vulkan/vk_xr.h: the instance and the device are made here, with the surface and
// swapchain extensions; the frame is copied into the swapchain image in the frame's own command buffer, and presented
// once that command buffer is on the queue). A gamepad (Bluetooth or USB) is the Switch pad, by the buttons' places
// (Nintendo's layout: the bottom button is B, the right one A); the sound goes out through AAudio (android/aaudio_sdl.c).
// Its files, in the app's external files folder (/sdcard/Android/data/com.lasrecomp.tablet/files): extracted/ (the game,
// adb push), save/, settings.ini (written with the first-person view on if missing), env.txt (KEY=VALUE lines, tests),
// lasrecomp.log. The game's image: the screen's shape, LARECOMP_FLAT_WIDTH pixels wide (half a screen 2560 or wider, else
// 1920; LARECOMP_OFFSCREEN sets it).
// Asked by Guillaume, 27/10/2026: « compile moi une version la plus optimisée possible pour la tablette » (Xiaomi Pad 7,
// Snapdragon 7+ Gen 3: a stronger processor than the Quest 3's, A-FAIRE/lasrecomp-port-tablette-android-plat.md).
#include <android/log.h>
#include <android/native_window.h>
#include <android/window.h>
#include <android_native_app_glue.h>
#include <dlfcn.h>
#include <libgen.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define VK_USE_PLATFORM_ANDROID_KHR
#include <vulkan/vulkan.h>
#include "nvn/vulkan/vk_dispatch.h"  // (every Vulkan call here through the backend's pointers: the driver it loaded)
#include "nvn/vulkan/vk_xr.h"
#include "adrenotools/driver.h"

#define ALOG(...) __android_log_print(ANDROID_LOG_INFO, "lasrecomp", __VA_ARGS__)
#define FLOG(...) (fprintf(stderr, "[flat] " __VA_ARGS__), fputc('\n', stderr), __android_log_print(ANDROID_LOG_INFO, "lasrecomp", __VA_ARGS__))  // (logcat too: before the log file, and after)

extern int lasrecomp_main(int argc, char **argv);
extern void input_host_pad(uint64_t buttons, int32_t lx, int32_t ly, int32_t rx, int32_t ry);

// ------------------------------------------------------------------ the window (the activity's thread)
static ANativeWindow *_Atomic g_window;
static _Atomic bool g_window_lost;  // (the app sent to the background: the swapchain goes, made again on return)

// ------------------------------------------------------------------ Vulkan (the game's thread: the backend's hooks)
static VkInstance g_vi;
static VkPhysicalDevice g_pd;
static VkDevice g_dev;
static VkQueue g_q;
static VkSurfaceKHR g_surf;
static VkSwapchainKHR g_sc;
static VkImage g_img[8];
static VkSemaphore g_done[8];  // (each image's: signalled after the frame, waited by its present)
static uint32_t g_nimg;
static VkExtent2D g_ext;
static VkFence g_acq;
static int g_acquired = -1;
static bool g_remake;

static const char *const k_inst_exts[] = {VK_KHR_SURFACE_EXTENSION_NAME, VK_KHR_ANDROID_SURFACE_EXTENSION_NAME};
static VkResult hk_create_instance(const VkInstanceCreateInfo *ci, VkInstance *out) {
    const char **e = calloc(ci->enabledExtensionCount + 2, sizeof *e);
    uint32_t n = 0;
    for (uint32_t i = 0; i < ci->enabledExtensionCount; i++) e[n++] = ci->ppEnabledExtensionNames[i];
    for (int k = 0; k < 2; k++) {
        bool have = false;
        for (uint32_t i = 0; i < n; i++) have |= !strcmp(e[i], k_inst_exts[k]);
        if (!have) e[n++] = k_inst_exts[k];
    }
    VkInstanceCreateInfo c2 = *ci;
    c2.enabledExtensionCount = n, c2.ppEnabledExtensionNames = e;
    VkResult r = vkCreateInstance(&c2, NULL, out);
    FLOG("Vulkan instance: %d (%u extensions)", r, n);
    free(e);
    if (r == VK_SUCCESS) g_vi = *out;
    return r;
}
static VkPhysicalDevice hk_physical_device(VkInstance vi) {
    uint32_t n = 1;
    VkPhysicalDevice pd = VK_NULL_HANDLE;
    vkEnumeratePhysicalDevices(vi, &n, &pd);
    return pd;
}
// LARECOMP_FLAT_MEMREPORT=1 (research): every allocation the driver makes for us, its own included, by object type
// (VK_EXT_device_memory_report) - 27/10 the system counted 2 GB of GPU memory for the game at its start and some 50 MB
// more for each pipeline built, while the backend's allocator held 160 MB. Logged with LARECOMP_FLAT_MEMLOG.
static _Atomic int64_t g_rep_bytes[64], g_rep_count[64];
static void VKAPI_PTR mem_report(const VkDeviceMemoryReportCallbackDataEXT *d, void *u) {
    (void)u;
    int t = d->objectType < 64 ? (int)d->objectType : 63;
    if (d->type == VK_DEVICE_MEMORY_REPORT_EVENT_TYPE_ALLOCATE_EXT || d->type == VK_DEVICE_MEMORY_REPORT_EVENT_TYPE_IMPORT_EXT)
        atomic_fetch_add(&g_rep_bytes[t], (int64_t)d->size), atomic_fetch_add(&g_rep_count[t], 1);
    else if (d->type == VK_DEVICE_MEMORY_REPORT_EVENT_TYPE_FREE_EXT || d->type == VK_DEVICE_MEMORY_REPORT_EVENT_TYPE_UNIMPORT_EXT)
        atomic_fetch_sub(&g_rep_bytes[t], (int64_t)d->size), atomic_fetch_sub(&g_rep_count[t], 1);
    if (d->size >= (32u << 20) && d->type == VK_DEVICE_MEMORY_REPORT_EVENT_TYPE_ALLOCATE_EXT)
        FLOG("driver allocation: %llu MB for object type %d", (unsigned long long)(d->size >> 20), (int)d->objectType);
}
static void mem_report_log(void) {
    for (int t = 0; t < 64; t++) {
        int64_t b = atomic_load(&g_rep_bytes[t]), n = atomic_load(&g_rep_count[t]);
        if (b > (1 << 20)) FLOG("driver memory: object type %d, %lld MB in %lld allocations", t, (long long)(b >> 20), (long long)n);
    }
}
static VkResult hk_create_device(VkPhysicalDevice pd, const VkDeviceCreateInfo *ci, VkDevice *out) {
    const char **e = calloc(ci->enabledExtensionCount + 2, sizeof *e);
    uint32_t n = 0;
    bool have = false;
    for (uint32_t i = 0; i < ci->enabledExtensionCount; i++) {
        e[n++] = ci->ppEnabledExtensionNames[i];
        have |= !strcmp(e[n - 1], VK_KHR_SWAPCHAIN_EXTENSION_NAME);
    }
    if (!have) e[n++] = VK_KHR_SWAPCHAIN_EXTENSION_NAME;
    VkDeviceCreateInfo c2 = *ci;
    VkDeviceDeviceMemoryReportCreateInfoEXT rep = {.sType = VK_STRUCTURE_TYPE_DEVICE_DEVICE_MEMORY_REPORT_CREATE_INFO_EXT,
                                                   .pNext = ci->pNext, .pfnUserCallback = mem_report};
    VkPhysicalDeviceDeviceMemoryReportFeaturesEXT rf = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DEVICE_MEMORY_REPORT_FEATURES_EXT,
                                                        .pNext = &rep, .deviceMemoryReport = VK_TRUE};
    const char *mr = getenv("LARECOMP_FLAT_MEMREPORT");
    if (mr && *mr == '1') {
        uint32_t ns = 0;
        vkEnumerateDeviceExtensionProperties(pd, NULL, &ns, NULL);
        VkExtensionProperties *sup = calloc(ns ? ns : 1, sizeof *sup);
        vkEnumerateDeviceExtensionProperties(pd, NULL, &ns, sup);
        bool ok = false;
        for (uint32_t i = 0; i < ns; i++) ok |= !strcmp(sup[i].extensionName, VK_EXT_DEVICE_MEMORY_REPORT_EXTENSION_NAME);
        free(sup);
        if (ok) e[n++] = VK_EXT_DEVICE_MEMORY_REPORT_EXTENSION_NAME, c2.pNext = &rf;
        FLOG("device memory report: %s", ok ? "on" : "not supported");
    }
    c2.enabledExtensionCount = n, c2.ppEnabledExtensionNames = e;
    VkResult r = vkCreateDevice(pd, &c2, NULL, out);
    FLOG("Vulkan device: %d (%u extensions)", r, n);
    free(e);
    return r;
}
static void hk_device_ready(VkInstance vi, VkPhysicalDevice pd, VkDevice dev, uint32_t qfam) {
    g_vi = vi, g_pd = pd, g_dev = dev;
    vkGetDeviceQueue(dev, qfam, 0, &g_q);
    VkFenceCreateInfo fi = {.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    vkCreateFence(dev, &fi, NULL, &g_acq);
    FLOG("device ready: queue family %u", qfam);
}

static void swapchain_drop(void) {
    if (!g_dev) return;
    vk_queue_lock();
    vkQueueWaitIdle(g_q);
    vk_queue_unlock();
    for (uint32_t i = 0; i < g_nimg; i++) if (g_done[i]) vkDestroySemaphore(g_dev, g_done[i], NULL), g_done[i] = VK_NULL_HANDLE;
    if (g_sc) vkDestroySwapchainKHR(g_dev, g_sc, NULL), g_sc = VK_NULL_HANDLE;
    if (g_surf) vkDestroySurfaceKHR(g_vi, g_surf, NULL), g_surf = VK_NULL_HANDLE;
    g_nimg = 0, g_acquired = -1;
}

// The swapchain on the window, made at the first frame it is there (and again after the app came back, or when the
// window changed size): FIFO (the display's own pace), as many images as the system wants plus one, written by copies.
static bool swapchain_ready(void) {
    if (atomic_exchange(&g_window_lost, false) || g_remake) swapchain_drop(), g_remake = false;
    if (g_sc) return true;
    ANativeWindow *w = atomic_load(&g_window);
    if (!w || !g_dev) return false;
    VkAndroidSurfaceCreateInfoKHR si = {.sType = VK_STRUCTURE_TYPE_ANDROID_SURFACE_CREATE_INFO_KHR, .window = w};
    if (vkCreateAndroidSurfaceKHR(g_vi, &si, NULL, &g_surf) != VK_SUCCESS) { FLOG("no surface"); return false; }
    VkSurfaceCapabilitiesKHR cap;
    vkGetPhysicalDeviceSurfaceCapabilitiesKHR(g_pd, g_surf, &cap);
    uint32_t nf = 0;
    vkGetPhysicalDeviceSurfaceFormatsKHR(g_pd, g_surf, &nf, NULL);
    VkSurfaceFormatKHR fmts[32];
    nf = nf > 32 ? 32 : nf;
    vkGetPhysicalDeviceSurfaceFormatsKHR(g_pd, g_surf, &nf, fmts);
    VkSurfaceFormatKHR f = fmts[0];
    for (uint32_t i = 0; i < nf; i++)  // (8 bits a channel, not sRGB: the game's image is already encoded)
        if (fmts[i].format == VK_FORMAT_R8G8B8A8_UNORM || fmts[i].format == VK_FORMAT_B8G8R8A8_UNORM) { f = fmts[i]; break; }
    VkExtent2D ext = cap.currentExtent;
    if (ext.width == 0xFFFFFFFFu) ext.width = (uint32_t)ANativeWindow_getWidth(w), ext.height = (uint32_t)ANativeWindow_getHeight(w);
    // The swapchain at the game's image size, the display's compositor scaling it to the screen: our scaled blit wrote
    // a full screen a frame (3200x2136 on the tablet). LARECOMP_FLAT_SCREEN_SIZE=1: the screen's size, our blit
    // scaling, as before.
    unsigned ow = 0, oh = 0;
    const char *os = getenv("LARECOMP_OFFSCREEN"), *ss = getenv("LARECOMP_FLAT_SCREEN_SIZE");
    if (!(ss && atoi(ss)) && os && sscanf(os, "%ux%u", &ow, &oh) == 2 && ow && oh &&
        ow >= cap.minImageExtent.width && oh >= cap.minImageExtent.height &&
        (!cap.maxImageExtent.width || (ow <= cap.maxImageExtent.width && oh <= cap.maxImageExtent.height)))
        ext.width = ow, ext.height = oh;
    uint32_t n = cap.minImageCount + 1;
    if (cap.maxImageCount && n > cap.maxImageCount) n = cap.maxImageCount;
    VkSwapchainCreateInfoKHR ci = {
        .sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR, .surface = g_surf, .minImageCount = n, .imageFormat = f.format,
        .imageColorSpace = f.colorSpace, .imageExtent = ext, .imageArrayLayers = 1,
        .imageUsage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT,
        .imageSharingMode = VK_SHARING_MODE_EXCLUSIVE,
        // (the system turns the image if the screen is turned: the identity when it may, else what the surface has now)
        .preTransform = (cap.supportedTransforms & VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR) ? VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR
                                                                                           : cap.currentTransform,
        .compositeAlpha = (cap.supportedCompositeAlpha & VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR) ? VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR
                                                                                             : VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR,
        .presentMode = VK_PRESENT_MODE_FIFO_KHR, .clipped = VK_TRUE};
    VkResult r = vkCreateSwapchainKHR(g_dev, &ci, NULL, &g_sc);
    if (r != VK_SUCCESS) { FLOG("no swapchain: %d", r); vkDestroySurfaceKHR(g_vi, g_surf, NULL), g_surf = VK_NULL_HANDLE; return false; }
    g_nimg = 8;
    vkGetSwapchainImagesKHR(g_dev, g_sc, &g_nimg, g_img);
    VkSemaphoreCreateInfo smi = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    for (uint32_t i = 0; i < g_nimg; i++) vkCreateSemaphore(g_dev, &smi, NULL, &g_done[i]);
    g_ext = ext;
    FLOG("swapchain %ux%u, format %d, %u images, transform %#x", ext.width, ext.height, f.format, g_nimg, ci.preTransform);
    return true;
}

static void barrier(VkCommandBuffer cb, VkImage im, VkImageLayout from, VkImageLayout to, VkAccessFlags sa, VkAccessFlags da,
                    VkPipelineStageFlags ss, VkPipelineStageFlags ds) {
    VkImageMemoryBarrier b = {.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, .srcAccessMask = sa, .dstAccessMask = da,
                              .oldLayout = from, .newLayout = to, .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                              .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .image = im,
                              .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
    vkCmdPipelineBarrier(cb, ss, ds, 0, 0, NULL, 0, NULL, 1, &b);
}

// A finished frame (the game's thread, its commands not submitted yet): the next swapchain image taken, the game's
// image scaled into it (its shape kept, black around it), left ready to present.
static void hk_frame(VkCommandBuffer cb, VkImage src, VkExtent2D ext) {
    if (!swapchain_ready()) return;
    uint32_t idx;
    VkResult r = vkAcquireNextImageKHR(g_dev, g_sc, 100000000ull, VK_NULL_HANDLE, g_acq, &idx);
    if (r == VK_ERROR_OUT_OF_DATE_KHR || r == VK_ERROR_SURFACE_LOST_KHR) { g_remake = true; return; }
    if (r != VK_SUCCESS && r != VK_SUBOPTIMAL_KHR) return;
    vkWaitForFences(g_dev, 1, &g_acq, VK_TRUE, 1000000000ull);
    vkResetFences(g_dev, 1, &g_acq);
    VkImage dst = g_img[idx];
    barrier(cb, src, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
            VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT,
            VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
    barrier(cb, dst, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0, VK_ACCESS_TRANSFER_WRITE_BIT,
            VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
    static const VkClearColorValue black = {{0, 0, 0, 1}};
    VkImageSubresourceRange all = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCmdClearColorImage(cb, dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &black, 1, &all);
    // (its shape kept: as wide as the screen if it is wider, else as tall)
    double sa = (double)ext.width / ext.height, da = (double)g_ext.width / g_ext.height;
    int32_t w = (int32_t)g_ext.width, h = (int32_t)g_ext.height;
    if (sa > da) h = (int32_t)(g_ext.width / sa + 0.5); else w = (int32_t)(g_ext.height * sa + 0.5);
    int32_t x0 = ((int32_t)g_ext.width - w) / 2, y0 = ((int32_t)g_ext.height - h) / 2;
    VkImageBlit bl = {.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, .srcOffsets = {{0, 0, 0}, {(int32_t)ext.width, (int32_t)ext.height, 1}},
                      .dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, .dstOffsets = {{x0, y0, 0}, {x0 + w, y0 + h, 1}}};
    vkCmdBlitImage(cb, src, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &bl, VK_FILTER_LINEAR);
    barrier(cb, dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, VK_ACCESS_TRANSFER_WRITE_BIT, 0,
            VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT);
    barrier(cb, src, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_ACCESS_TRANSFER_READ_BIT,
            VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT);
    g_acquired = (int)idx;
}

// The frame's command buffer is on the queue: an empty submission after it signals the image's semaphore (a signal
// waits for every command submitted before it on the queue), which the present waits for.
static void hk_frame_submitted(void) {
    if (g_acquired < 0) return;
    uint32_t idx = (uint32_t)g_acquired;
    g_acquired = -1;
    VkSubmitInfo si = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .signalSemaphoreCount = 1, .pSignalSemaphores = &g_done[idx]};
    VkPresentInfoKHR pi = {.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR, .waitSemaphoreCount = 1, .pWaitSemaphores = &g_done[idx],
                           .swapchainCount = 1, .pSwapchains = &g_sc, .pImageIndices = &idx};
    vk_queue_lock();
    vkQueueSubmit(g_q, 1, &si, VK_NULL_HANDLE);
    VkResult r = vkQueuePresentKHR(g_q, &pi);
    vk_queue_unlock();
    if (r == VK_ERROR_OUT_OF_DATE_KHR || r == VK_SUBOPTIMAL_KHR || r == VK_ERROR_SURFACE_LOST_KHR) g_remake = true;
    static uint64_t t0, n;  // (images shown, every 2 s: this path has no "[vk] t=" line of its own)
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    uint64_t now = (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
    n++;
    if (!t0) t0 = now;
    if (now - t0 >= 2000000000ull) {
        extern const char *renderer_stats(void);
        FLOG("%.1f fps (%s)", n * 1e9 / (double)(now - t0), renderer_stats());
        t0 = now, n = 0;
    }
}

// The shell's window handed to the backend (vk_xr.h surface / size), the default: the backend's own swapchain on it,
// its first-person warp presenter showing each of the game's 30 images a second turned to the moment of each of the
// display's 60 refreshes - the frames above only when LARECOMP_FLAT_OFFSCREEN=1 (offscreen, copied in hk_frame, 30).
static int g_img_w, g_img_h;
static VkSurfaceKHR hk_surface(VkInstance inst) {
    ANativeWindow *w = atomic_load(&g_window);
    if (!w) return VK_NULL_HANDLE;
    // (the display at 60: 144 on the tablet, and the presenter draws once a refresh - 60 is two refreshes a game image)
    // (libnativewindow's, found at run time: linked by name, liblasrecomp.so did not load at all - 27/10)
    static int32_t (*set_rate)(ANativeWindow *, float, int8_t);
    static bool looked;
    if (!looked) {
        looked = true;
        void *nw = dlopen("libnativewindow.so", RTLD_NOW);
        set_rate = nw ? (int32_t (*)(ANativeWindow *, float, int8_t))dlsym(nw, "ANativeWindow_setFrameRate") : NULL;
    }
    if (set_rate) set_rate(w, 60.0f, 0);  // (ANATIVEWINDOW_FRAME_RATE_COMPATIBILITY_DEFAULT)
    VkAndroidSurfaceCreateInfoKHR si = {.sType = VK_STRUCTURE_TYPE_ANDROID_SURFACE_CREATE_INFO_KHR, .window = w};
    VkSurfaceKHR sf;
    if (vkCreateAndroidSurfaceKHR(inst, &si, NULL, &sf) != VK_SUCCESS) { FLOG("no surface for the window"); return VK_NULL_HANDLE; }
    FLOG("the window handed to the game: %dx%d at 60 Hz", g_img_w, g_img_h);
    return sf;
}
static void hk_size(int *w, int *h) {
    bool on = atomic_load(&g_window) != NULL;
    *w = on ? g_img_w : 0, *h = on ? g_img_h : 0;
}

static VkXrHooks g_hooks = {hk_create_instance, hk_physical_device, hk_create_device, hk_device_ready, hk_frame, hk_frame_submitted,
                            NULL, NULL, .flat = true};

// ------------------------------------------------------------------ the gamepad (the activity's thread)
enum { BTN_A, BTN_B, BTN_X, BTN_Y, BTN_LSTICK, BTN_RSTICK, BTN_L, BTN_R, BTN_ZL, BTN_ZR, BTN_PLUS, BTN_MINUS,
       BTN_LEFT, BTN_UP, BTN_RIGHT, BTN_DOWN };
static uint64_t g_buttons;
static float g_axis[8];  // lx ly rx ry lt rt hatx haty
static int key_button(int32_t kc) {  // (by place: Android's A is the bottom button, the Switch's B)
    switch (kc) {
    case AKEYCODE_BUTTON_A: return BTN_B;
    case AKEYCODE_BUTTON_B: return BTN_A;
    case AKEYCODE_BUTTON_X: return BTN_Y;
    case AKEYCODE_BUTTON_Y: return BTN_X;
    case AKEYCODE_BUTTON_L1: return BTN_L;
    case AKEYCODE_BUTTON_R1: return BTN_R;
    case AKEYCODE_BUTTON_L2: return BTN_ZL;
    case AKEYCODE_BUTTON_R2: return BTN_ZR;
    case AKEYCODE_BUTTON_THUMBL: return BTN_LSTICK;
    case AKEYCODE_BUTTON_THUMBR: return BTN_RSTICK;
    case AKEYCODE_BUTTON_START: return BTN_PLUS;
    case AKEYCODE_BUTTON_SELECT: return BTN_MINUS;
    case AKEYCODE_DPAD_LEFT: return BTN_LEFT;
    case AKEYCODE_DPAD_UP: return BTN_UP;
    case AKEYCODE_DPAD_RIGHT: return BTN_RIGHT;
    case AKEYCODE_DPAD_DOWN: return BTN_DOWN;
    default: return -1;
    }
}
static int32_t stick(float v) {
    if (v > -0.08f && v < 0.08f) v = 0;  // (a dead zone: pads at rest seldom read 0)
    return (int32_t)(v < -1 ? -32767 : v > 1 ? 32767 : v * 32767);
}
static void pad_publish(void) {
    uint64_t b = g_buttons;
    if (g_axis[4] > 0.5f) b |= 1ull << BTN_ZL;  // (the triggers as axes, on pads that send them so)
    if (g_axis[5] > 0.5f) b |= 1ull << BTN_ZR;
    if (g_axis[6] < -0.5f) b |= 1ull << BTN_LEFT;  // (the cross as a hat)
    if (g_axis[6] > 0.5f) b |= 1ull << BTN_RIGHT;
    if (g_axis[7] < -0.5f) b |= 1ull << BTN_UP;
    if (g_axis[7] > 0.5f) b |= 1ull << BTN_DOWN;
    input_host_pad(b, stick(g_axis[0]), stick(-g_axis[1]), stick(g_axis[2]), stick(-g_axis[3]));  // (up positive)
}
static int32_t on_input(struct android_app *app, AInputEvent *e) {
    (void)app;
    int32_t type = AInputEvent_getType(e), src = AInputEvent_getSource(e);
    if (type == AINPUT_EVENT_TYPE_KEY) {
        int bit = key_button(AKeyEvent_getKeyCode(e));
        if (bit < 0) return 0;
        int32_t act = AKeyEvent_getAction(e);
        if (act == AKEY_EVENT_ACTION_DOWN) g_buttons |= 1ull << bit;
        else if (act == AKEY_EVENT_ACTION_UP) g_buttons &= ~(1ull << bit);
        pad_publish();
        return 1;
    }
    if (type == AINPUT_EVENT_TYPE_MOTION && (src & AINPUT_SOURCE_JOYSTICK) == AINPUT_SOURCE_JOYSTICK) {
        static const int32_t ax[8] = {AMOTION_EVENT_AXIS_X, AMOTION_EVENT_AXIS_Y, AMOTION_EVENT_AXIS_Z, AMOTION_EVENT_AXIS_RZ,
                                      AMOTION_EVENT_AXIS_LTRIGGER, AMOTION_EVENT_AXIS_RTRIGGER, AMOTION_EVENT_AXIS_HAT_X,
                                      AMOTION_EVENT_AXIS_HAT_Y};
        for (int i = 0; i < 8; i++) g_axis[i] = AMotionEvent_getAxisValue(e, ax[i], 0);
        // (some pads put the triggers on brake and gas)
        float brake = AMotionEvent_getAxisValue(e, AMOTION_EVENT_AXIS_BRAKE, 0), gas = AMotionEvent_getAxisValue(e, AMOTION_EVENT_AXIS_GAS, 0);
        if (brake > g_axis[4]) g_axis[4] = brake;
        if (gas > g_axis[5]) g_axis[5] = gas;
        pad_publish();
        return 1;
    }
    return 0;
}

// ------------------------------------------------------------------ the activity
static void on_cmd(struct android_app *app, int32_t cmd) {
    switch (cmd) {
    case APP_CMD_INIT_WINDOW: atomic_store(&g_window, app->window); FLOG("window %dx%d", ANativeWindow_getWidth(app->window), ANativeWindow_getHeight(app->window)); break;
    case APP_CMD_TERM_WINDOW: atomic_store(&g_window, NULL); atomic_store(&g_window_lost, true); break;
    case APP_CMD_DESTROY: FLOG("destroyed"); fflush(stderr); _exit(0);
    default: break;
    }
}

static void read_env_file(void) {  // (env.txt: KEY=VALUE per line, for tests)
    FILE *f = fopen("env.txt", "r");
    if (!f) return;
    char line[2048];
    while (fgets(line, sizeof line, f)) {
        line[strcspn(line, "\r\n")] = 0;
        char *eq = strchr(line, '=');
        if (line[0] == '#' || !eq) continue;
        *eq = 0;
        setenv(line, eq + 1, 1);
        FLOG("env %s=%s", line, eq + 1);
    }
    fclose(f);
}

// LARECOMP_FLAT_MEMLOG=<seconds>: the device memory Vulkan has given us (vk_log_memory) and the system's available
// memory, every so often (27/10: the tablet killed the game for want of memory while 3.9 GB were nobody's in dumpsys)
static void *mem_thread(void *arg) {
    int every = (int)(intptr_t)arg;
    extern void vk_log_memory(const char *why);
    for (;;) {
        sleep(every);
        FILE *f = fopen("/proc/meminfo", "r");
        char line[128];
        while (f && fgets(line, sizeof line, f)) if (!strncmp(line, "MemAvailable", 12)) { line[strcspn(line, "\n")] = 0; FLOG("%s", line); }
        if (f) fclose(f);
        if (g_dev) vk_log_memory("android");
        mem_report_log();
    }
    return NULL;
}

// The driver. Mesa's Turnip when <files>/turnip/vulkan.ad07xx.so is there (LARECOMP_FLAT_TURNIP=0: never), through
// libadrenotools as on the Quest (android/xr_app.c load_vulkan), kept only if it creates an instance and finds the GPU;
// else the system's. 27/10: with Qualcomm's driver every pipeline held some 2 MB of GPU memory (3 GB for the game's
// first few hundred, the tablet killed it before its first image) and the biggest took two minutes to build.
static bool copy_file(const char *from, const char *to) {
    FILE *a = fopen(from, "rb"), *b = a ? fopen(to, "wb") : NULL;
    char buf[65536];
    size_t n;
    bool ok = a && b;
    while (ok && (n = fread(buf, 1, sizeof buf, a)) > 0) ok = fwrite(buf, 1, n, b) == n;
    if (a) fclose(a);
    if (b) fclose(b);
    return ok;
}
static bool driver_works(void) {
    uint32_t ne = 0;  // (the tablet presents through the driver's own surface: without it, nothing to show)
    VkExtensionProperties ex[64];
    bool surf = false;
    if (lr_vkEnumerateInstanceExtensionProperties && lr_vkEnumerateInstanceExtensionProperties(NULL, &ne, NULL) == VK_SUCCESS) {
        if (ne > 64) ne = 64;
        lr_vkEnumerateInstanceExtensionProperties(NULL, &ne, ex);
        for (uint32_t i = 0; i < ne; i++) surf |= !strcmp(ex[i].extensionName, VK_KHR_ANDROID_SURFACE_EXTENSION_NAME);
    }
    if (!surf) {
        FLOG("driver: no %s", VK_KHR_ANDROID_SURFACE_EXTENSION_NAME);
        return false;
    }
    VkApplicationInfo ai = {.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO, .apiVersion = VK_API_VERSION_1_1};
    VkInstanceCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pApplicationInfo = &ai};
    VkInstance vi;
    if (!lr_vkCreateInstance || lr_vkCreateInstance(&ci, NULL, &vi) != VK_SUCCESS) return false;
    lr_vk_load_instance(vi);
    uint32_t n = 0;
    lr_vkEnumeratePhysicalDevices(vi, &n, NULL);
    VkPhysicalDeviceProperties pp = {0};
    if (n) {
        VkPhysicalDevice pd;
        n = 1;
        lr_vkEnumeratePhysicalDevices(vi, &n, &pd);
        lr_vkGetPhysicalDeviceProperties(pd, &pp);
    }
    lr_vkDestroyInstance(vi, NULL);
    if (n) FLOG("driver: %s, Vulkan %u.%u.%u, driver %#x", pp.deviceName, VK_API_VERSION_MAJOR(pp.apiVersion),
                VK_API_VERSION_MINOR(pp.apiVersion), VK_API_VERSION_PATCH(pp.apiVersion), pp.driverVersion);
    return n > 0;
}
static void load_vulkan(struct android_app *app) {
    const char *t = getenv("LARECOMP_FLAT_TURNIP");
    char src[700], dir[700], tmp[700], dst[800], hook[1024] = "";
    snprintf(src, sizeof src, "%s/turnip/vulkan.ad07xx.so", app->activity->externalDataPath);
    struct stat st;
    if (!(t && *t == '0') && stat(src, &st) == 0) {
        snprintf(dir, sizeof dir, "%s/turnip/", app->activity->internalDataPath);
        snprintf(tmp, sizeof tmp, "%s/tmp/", app->activity->internalDataPath);
        snprintf(dst, sizeof dst, "%svulkan.ad07xx.so", dir);
        mkdir(dir, 0700), mkdir(tmp, 0700);
        Dl_info di;
        if (dladdr((void *)load_vulkan, &di) && di.dli_fname) {
            snprintf(hook, sizeof hook, "%s", di.dli_fname);
            char *sl = strrchr(hook, '/');
            if (sl) *sl = 0;
        }
        bool copied = copy_file(src, dst);  // (dlopen refuses a library on shared storage)
        void *h = copied ? adrenotools_open_libvulkan(RTLD_NOW, ADRENOTOOLS_DRIVER_CUSTOM, tmp, hook, dir, "vulkan.ad07xx.so", NULL, NULL) : NULL;
        PFN_vkGetInstanceProcAddr gipa = h ? (PFN_vkGetInstanceProcAddr)dlsym(h, "vkGetInstanceProcAddr") : NULL;
        if (gipa) {
            lr_vk_load_global(gipa);
            if (driver_works()) {
                setenv("LARECOMP_VK_NO_HOST_IMPORT", "1", 0);  // (as on the Quest with Turnip: device.c)
                FLOG("Vulkan: Mesa Turnip (%s)", src);
                return;
            }
            FLOG("Turnip loaded but no GPU found with it: the system's driver");
        } else FLOG("Turnip not loaded (%s): the system's driver", copied ? "adrenotools failed" : "no copy");
    }
    void *sys = dlopen("libvulkan.so", RTLD_NOW);
    lr_vk_load_global((PFN_vkGetInstanceProcAddr)dlsym(sys, "vkGetInstanceProcAddr"));
    driver_works();
    FLOG("Vulkan: the system's driver");
}

static void *game_thread(void *arg) {
    (void)arg;
    char *argv[] = {"lasrecomp", "extracted", "--play", NULL};
    int r = lasrecomp_main(3, argv);
    FLOG("the game returned %d", r);
    fflush(stderr);
    _exit(r);
    return NULL;
}

void android_main(struct android_app *app) {
    ALOG("android_main");
    app->onAppCmd = on_cmd;
    app->onInputEvent = on_input;
    const char *dir = app->activity->externalDataPath;
    if (!dir || chdir(dir) != 0) { ALOG("no data folder (%s)", dir ? dir : "null"); return; }
    if (freopen("lasrecomp.log", "w", stderr)) { setvbuf(stderr, NULL, _IOLBF, 0); dup2(2, 1); }
    FLOG("data folder %s", dir);
    ANativeActivity_setWindowFlags(app->activity, AWINDOW_FLAG_FULLSCREEN | AWINDOW_FLAG_KEEP_SCREEN_ON, 0);
    struct stat st;
    if (stat("extracted/exefs/main", &st) != 0) FLOG("no game files: adb push them into %s/extracted", dir);
    // the game code ships in the app (lib/arm64-v8a/liblasgame.so), next to this library
    Dl_info di;
    char lib[1024] = "";
    if (dladdr((void *)android_main, &di) && di.dli_fname) {
        char tmp[1024];
        snprintf(tmp, sizeof tmp, "%s", di.dli_fname);
        snprintf(lib, sizeof lib, "%s/liblasgame.so", dirname(tmp));
        setenv("LARECOMP_GAME_LIB", lib, 0);
    }
    if (stat("settings.ini", &st) != 0) {  // (the first-person view on, the screen's shape)
        FILE *f = fopen("settings.ini", "w");
        // (the image at its own size, no dynamic resolution: the compositor's doubling is then the only scaling - a
        //  dynamic one at uneven ratios drew a curtain of fine dark lines over the whole screen, 27/10; SMAA on: 30 held
        //  at the village's heaviest places once the fragment page marks were off, 28/10)
        if (f) fputs("first_person = 1\naspect = unlocked\nresolution = match\nantialias = smaa\nshow_fps = 1\n", f), fclose(f);
    }
    setenv("LARECOMP_FONT", "Arial.ttf", 0);
    setenv("LARECOMP_SAVE_DIR", "save", 0);
    setenv("LARECOMP_SETTINGS", "settings.ini", 0);
    setenv("LARECOMP_NO_LAUNCHER", "1", 0);
    setenv("LARECOMP_OFFSCREEN_MENU", "1", 0);  // (LAS Recomp's menu and the fps drawn into the image this shell shows: device.c)
    // the frame ahead and the asynchronous submission (runtime/nvn/objects.c): the game's thread, the translation and
    // the GPU each on their own frame, as on the Quest and the Mac
    setenv("LARECOMP_FRAME_AHEAD", "1", 0);
    // the game held in its 30 fps mode (runtime/nvn/pace.c): the tablet's GPU takes 22-28 ms a frame outdoors, and the
    // game's own choice went back and forth between 30 and 60 - 30 to 50 images a second, felt as lag (27/10)
    setenv("LARECOMP_GAME_30", "1", 0);
    setenv("LARECOMP_FP_WARP_HZ", "60", 0);  // (the first-person presenter at 60 whatever the display's rate: device.c)
    // one submission a frame, not one every 8 passes (runtime/nvn/vulkan/exec.c): each submission flushes the vertex
    // and page caches - at the village's heaviest place 15% less translation, 30 held (28.8-30 with spikes to 23 before,
    // 28/10)
    setenv("LARECOMP_CHUNK_PASSES", "0", 0);
    // fragment shaders don't mark the game memory pages they miss (runtime/nvn/vulkan/exec.c gp_glsl): under Turnip
    // the marks - storage writes, early fragment tests - cost every pass, 41 ms of GPU a frame at the village's heaviest
    // place against 30 without (depth pre-pass 16.6 -> 12.0 ms, colour 10.6 -> 7.8); the Qualcomm driver pays nothing
    // for them (the Quest, 47.8 against 47.7). The pages come from the bound storage buffers and the other stages' marks,
    // as for the fragment shaders that discard. Found by the three-way consult (Gemini, GPT), 28/10
    setenv("LARECOMP_VK_GPAGE_NO_FS_MARKS", "1", 0);
    setenv("LARECOMP_ASYNC_SUBMIT", "1", 0);
    read_env_file();
    // the window first: the game's image takes the screen's shape
    while (!atomic_load(&g_window)) {
        int events;
        struct android_poll_source *src;
        if (ALooper_pollOnce(-1, NULL, &events, (void **)&src) >= 0 && src) src->process(app, src);
        if (app->destroyRequested) return;
    }
    if (!getenv("LARECOMP_OFFSCREEN")) {
        ANativeWindow *w = atomic_load(&g_window);
        int ww = ANativeWindow_getWidth(w), wh = ANativeWindow_getHeight(w);
        if (wh > ww) { int t = ww; ww = wh; wh = t; }  // (landscape)
        // half the screen's width where that is still a fair size (the tablet: 1600x1068 for 3200x2136), else 1920:
        // the compositor then doubles each pixel. At 1920 on that screen, two scalings at uneven ratios one after the
        // other (the dynamic resolution's, then 1920 to 3200) left a fine curtain of dark lines over the whole picture,
        // every 2.5 and 8 pixels of the screen (Guillaume, 27/10; measured on screenshots)
        const char *fw = getenv("LARECOMP_FLAT_WIDTH");
        int gw = fw ? atoi(fw) : ww >= 2560 ? ww / 2 : 1920;
        if (gw > ww) gw = ww;
        int gh = (int)((double)gw * wh / ww + 0.5) & ~1;
        char size[32];
        snprintf(size, sizeof size, "%dx%d", gw, gh);
        const char *off = getenv("LARECOMP_FLAT_OFFSCREEN");
        if (off && atoi(off)) setenv("LARECOMP_OFFSCREEN", size, 1);
        else g_img_w = gw, g_img_h = gh, g_hooks.surface = hk_surface, g_hooks.size = hk_size;
        FLOG("the game's image %s for a %dx%d screen%s", size, ww, wh, g_hooks.surface ? ", in the window" : ", offscreen");
    }
    load_vulkan(app);
    g_vk_xr = &g_hooks;
    pthread_attr_t at;
    pthread_attr_init(&at);
    pthread_attr_setstacksize(&at, 64u << 20);  // (the game's main thread wants a large stack, as on the Quest)
    pthread_t t;
    pthread_create(&t, &at, game_thread, NULL);
    const char *ml = getenv("LARECOMP_FLAT_MEMLOG");
    if (ml && atoi(ml) > 0) { pthread_t mt; pthread_create(&mt, NULL, mem_thread, (void *)(intptr_t)atoi(ml)); }
    for (;;) {
        int events;
        struct android_poll_source *src;
        if (ALooper_pollOnce(-1, NULL, &events, (void **)&src) >= 0 && src) src->process(app, src);
        if (app->destroyRequested) { fflush(stderr); _exit(0); }
    }
}
