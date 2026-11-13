// android/turnip_test/xr_turnip.c: does Turnip (Mesa's Adreno Vulkan driver, the Quest 3 build of
// github.com/Jbbrack03/Quest-3-Turnip-Driver) work under Meta's OpenXR runtime? A test app with no game in it
// (A-FAIRE/lasrecomp-feuille-de-route-quest-60fps-et-rapatriement.md; the consultant's decisive test, 21/10/2026):
//   - the driver loaded by libadrenotools from the app's private files, or the system's (the baseline);
//   - a plain Vulkan instance first, to log which driver answers (turnip or Qualcomm's);
//   - then its vkGetInstanceProcAddr handed to OpenXR (XR_KHR_vulkan_enable2): instance, device, session, one swapchain
//     per eye - the swapchain images are the runtime's, imported into our device: the unknown;
//   - each frame both eyes cleared to their colour (left red, right blue, pulsing), for 20 s, then the app quits.
// Every step and its result is logged: logcat tag xrturnip, and <external files>/turnip_test.log.
// Driver: <external files>/turnip/vulkan.ad07xx.so (adb push), copied into the private files (dlopen refuses storage).
// <external files>/use_system (an empty file): the system driver even when the Turnip file is there.
// XRT_BRIDGE=1 (env.txt): the bridge - OpenXR keeps the system driver (its compositor reads the swapchain images in the
// layout Qualcomm's driver writes: with Turnip they showed dashes, compressed differently, or black without UBWC), and
// Turnip draws each eye into a shared AHardwareBuffer (CPU-readable usage: a linear layout both drivers agree on) that
// the system device copies into the swapchain image. XRT_AHB_CPU=0: without the CPU usage (the driver's own layout).
#define VK_NO_PROTOTYPES
#define VK_USE_PLATFORM_ANDROID_KHR
#include <android/hardware_buffer.h>
#include <android/log.h>
#include <android_native_app_glue.h>
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <jni.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include <vulkan/vulkan.h>
#define XR_USE_PLATFORM_ANDROID
#define XR_USE_GRAPHICS_API_VULKAN
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>

#include "adrenotools/driver.h"

static FILE *g_log;
#define L(...)                                                                    \
    do {                                                                          \
        __android_log_print(ANDROID_LOG_INFO, "xrturnip", __VA_ARGS__);           \
        if (g_log) { fprintf(g_log, __VA_ARGS__); fputc('\n', g_log); fflush(g_log); } \
    } while (0)

static double now_s(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec + t.tv_nsec * 1e-9; }

static bool copy_file(const char *from, const char *to) {
    int a = open(from, O_RDONLY), b = -1;
    if (a < 0) return false;
    b = open(to, O_WRONLY | O_CREAT | O_TRUNC, 0700);
    if (b < 0) { close(a); return false; }
    char buf[65536];
    ssize_t n;
    bool ok = true;
    while ((n = read(a, buf, sizeof buf)) > 0) if (write(b, buf, (size_t)n) != n) { ok = false; break; }
    close(a), close(b);
    return ok && n == 0;
}

static PFN_vkGetInstanceProcAddr gipa;    // the driver OpenXR uses
static PFN_vkGetInstanceProcAddr gipa_t;  // the bridge: Turnip's

static void log_device(VkInstance vi, VkPhysicalDevice pd, const char *when);
// ---- the bridge: a device of Turnip's own, and an AHardwareBuffer imported into both devices
typedef struct { VkInstance inst; VkPhysicalDevice pd; VkDevice dev; VkQueue q; uint32_t qfam; VkCommandPool pool;
                 VkCommandBuffer cb; VkFence fence; PFN_vkGetDeviceProcAddr gdpa; } Dev;
#define DF(d, name) ((PFN_##name)(d)->gdpa((d)->dev, #name))
static const char *g_bridge_exts[] = {"VK_ANDROID_external_memory_android_hardware_buffer", "VK_EXT_queue_family_foreign",
                                      "VK_KHR_external_memory", "VK_KHR_dedicated_allocation", "VK_KHR_get_memory_requirements2",
                                      "VK_KHR_sampler_ycbcr_conversion", "VK_KHR_maintenance1", "VK_KHR_bind_memory2"};
static void dev_common(Dev *d) {
    d->gdpa = (PFN_vkGetDeviceProcAddr)gipa_t(d->inst, "vkGetDeviceProcAddr");
}
static bool turnip_device(Dev *d) {
    PFN_vkCreateInstance ci_ = (PFN_vkCreateInstance)gipa_t(NULL, "vkCreateInstance");
    VkApplicationInfo ai = {.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO, .pApplicationName = "turnip bridge", .apiVersion = VK_API_VERSION_1_1};
    VkInstanceCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pApplicationInfo = &ai};
    VkResult r = ci_(&ci, NULL, &d->inst);
    L("bridge: Turnip instance %d", r);
    if (r) return false;
    uint32_t n = 1;
    ((PFN_vkEnumeratePhysicalDevices)gipa_t(d->inst, "vkEnumeratePhysicalDevices"))(d->inst, &n, &d->pd);
    PFN_vkGetInstanceProcAddr keep = gipa;
    gipa = gipa_t, log_device(d->inst, d->pd, "bridge"), gipa = keep;
    uint32_t nq = 8;
    VkQueueFamilyProperties qp[8];
    ((PFN_vkGetPhysicalDeviceQueueFamilyProperties)gipa_t(d->inst, "vkGetPhysicalDeviceQueueFamilyProperties"))(d->pd, &nq, qp);
    for (uint32_t i = 0; i < nq; i++) if (qp[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) { d->qfam = i; break; }
    float prio = 1;
    VkDeviceQueueCreateInfo qci = {.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, .queueFamilyIndex = d->qfam, .queueCount = 1, .pQueuePriorities = &prio};
    VkDeviceCreateInfo dci = {.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, .queueCreateInfoCount = 1, .pQueueCreateInfos = &qci,
                              .enabledExtensionCount = 8, .ppEnabledExtensionNames = g_bridge_exts};
    r = ((PFN_vkCreateDevice)gipa_t(d->inst, "vkCreateDevice"))(d->pd, &dci, NULL, &d->dev);
    L("bridge: Turnip device %d", r);
    if (r) return false;
    dev_common(d);
    return true;
}
static void dev_cmd(Dev *d) {
    DF(d, vkGetDeviceQueue)(d->dev, d->qfam, 0, &d->q);
    VkCommandPoolCreateInfo pci = {.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT, .queueFamilyIndex = d->qfam};
    DF(d, vkCreateCommandPool)(d->dev, &pci, NULL, &d->pool);
    VkCommandBufferAllocateInfo cai = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, .commandPool = d->pool,
                                       .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1};
    DF(d, vkAllocateCommandBuffers)(d->dev, &cai, &d->cb);
    VkFenceCreateInfo fci = {.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    DF(d, vkCreateFence)(d->dev, &fci, NULL, &d->fence);
}
static bool ahb_image(Dev *d, AHardwareBuffer *ahb, uint32_t w, uint32_t h, VkImage *img, const char *who) {
    VkAndroidHardwareBufferFormatPropertiesANDROID fp = {.sType = VK_STRUCTURE_TYPE_ANDROID_HARDWARE_BUFFER_FORMAT_PROPERTIES_ANDROID};
    VkAndroidHardwareBufferPropertiesANDROID pp = {.sType = VK_STRUCTURE_TYPE_ANDROID_HARDWARE_BUFFER_PROPERTIES_ANDROID, .pNext = &fp};
    VkResult r = DF(d, vkGetAndroidHardwareBufferPropertiesANDROID)(d->dev, ahb, &pp);
    L("bridge %s: buffer properties %d, format %d, size %llu, types %#x", who, r, fp.format, (unsigned long long)pp.allocationSize, pp.memoryTypeBits);
    if (r) return false;
    VkExternalMemoryImageCreateInfo emi = {.sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO,
                                           .handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_ANDROID_HARDWARE_BUFFER_BIT_ANDROID};
    VkImageCreateInfo ici = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .pNext = &emi, .imageType = VK_IMAGE_TYPE_2D,
                             .format = fp.format ? fp.format : VK_FORMAT_R8G8B8A8_UNORM, .extent = {w, h, 1}, .mipLevels = 1, .arrayLayers = 1,
                             .samples = VK_SAMPLE_COUNT_1_BIT, .tiling = VK_IMAGE_TILING_OPTIMAL,
                             .usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                             .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED};
    r = DF(d, vkCreateImage)(d->dev, &ici, NULL, img);
    if (r) { L("bridge %s: image %d", who, r); return false; }
    VkImportAndroidHardwareBufferInfoANDROID imp = {.sType = VK_STRUCTURE_TYPE_IMPORT_ANDROID_HARDWARE_BUFFER_INFO_ANDROID, .buffer = ahb};
    VkMemoryDedicatedAllocateInfo ded = {.sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO, .pNext = &imp, .image = *img};
    VkMemoryAllocateInfo mai = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .pNext = &ded, .allocationSize = pp.allocationSize,
                                .memoryTypeIndex = (uint32_t)__builtin_ctz(pp.memoryTypeBits ? pp.memoryTypeBits : 1)};
    VkDeviceMemory mem;
    r = DF(d, vkAllocateMemory)(d->dev, &mai, NULL, &mem);
    if (!r) r = DF(d, vkBindImageMemory)(d->dev, *img, mem, 0);
    L("bridge %s: imported %d", who, r);
    return r == VK_SUCCESS;
}
static void barrier1(Dev *d, VkImage img, VkImageLayout from, VkImageLayout to, uint32_t src_q, uint32_t dst_q, VkAccessFlags sa, VkAccessFlags da) {
    VkImageMemoryBarrier b = {.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, .srcAccessMask = sa, .dstAccessMask = da, .oldLayout = from,
                              .newLayout = to, .srcQueueFamilyIndex = src_q, .dstQueueFamilyIndex = dst_q, .image = img,
                              .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
    DF(d, vkCmdPipelineBarrier)(d->cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, NULL, 0, NULL, 1, &b);
}
static VkResult dev_run(Dev *d) {  // (submit d->cb and wait: the test's CPU sync between the two drivers)
    VkSubmitInfo si = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &d->cb};
    VkResult r = DF(d, vkQueueSubmit)(d->q, 1, &si, d->fence);
    if (!r) r = DF(d, vkWaitForFences)(d->dev, 1, &d->fence, VK_TRUE, 1000000000ull);
    DF(d, vkResetFences)(d->dev, 1, &d->fence);
    return r;
}
static void dev_begin(Dev *d) {
    VkCommandBufferBeginInfo bi = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
    DF(d, vkResetCommandBuffer)(d->cb, 0);
    DF(d, vkBeginCommandBuffer)(d->cb, &bi);
}

static const char *driver_id_name(uint32_t id) {
    switch (id) {
    case VK_DRIVER_ID_QUALCOMM_PROPRIETARY: return "Qualcomm proprietary";
    case VK_DRIVER_ID_MESA_TURNIP: return "Mesa Turnip";
    default: return "other";
    }
}

static void log_device(VkInstance vi, VkPhysicalDevice pd, const char *when) {
    PFN_vkGetPhysicalDeviceProperties2 props2 = (PFN_vkGetPhysicalDeviceProperties2)gipa(vi, "vkGetPhysicalDeviceProperties2");
    VkPhysicalDeviceDriverProperties dp = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES};
    VkPhysicalDeviceProperties2 p = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, .pNext = &dp};
    if (props2) props2(pd, &p);
    L("%s: device \"%s\", api %u.%u.%u, driver id %u (%s) \"%s\" \"%s\"", when, p.properties.deviceName,
      VK_API_VERSION_MAJOR(p.properties.apiVersion), VK_API_VERSION_MINOR(p.properties.apiVersion),
      VK_API_VERSION_PATCH(p.properties.apiVersion), dp.driverID, driver_id_name(dp.driverID), dp.driverName, dp.driverInfo);
}

// A plain instance (no OpenXR): which driver does this loader give?
static void plain_probe(void) {
    PFN_vkCreateInstance create = (PFN_vkCreateInstance)gipa(NULL, "vkCreateInstance");
    VkApplicationInfo ai = {.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO, .pApplicationName = "turnip test", .apiVersion = VK_API_VERSION_1_1};
    VkInstanceCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pApplicationInfo = &ai};
    VkInstance vi = VK_NULL_HANDLE;
    VkResult r = create ? create(&ci, NULL, &vi) : VK_ERROR_INITIALIZATION_FAILED;
    L("plain Vulkan instance: %d", r);
    if (r != VK_SUCCESS) return;
    PFN_vkEnumeratePhysicalDevices en = (PFN_vkEnumeratePhysicalDevices)gipa(vi, "vkEnumeratePhysicalDevices");
    uint32_t n = 4;
    VkPhysicalDevice pds[4];
    r = en(vi, &n, pds);
    L("plain Vulkan: %u physical device(s), %d", n, r);
    for (uint32_t i = 0; i < n; i++) log_device(vi, pds[i], "plain");
    ((PFN_vkDestroyInstance)gipa(vi, "vkDestroyInstance"))(vi, NULL);
}

static void poll_android(struct android_app *app, int timeout) {
    int events;
    struct android_poll_source *src;
    while (ALooper_pollOnce(timeout, NULL, &events, (void **)&src) >= 0) {
        if (src) src->process(app, src);
        timeout = 0;
    }
}

void android_main(struct android_app *app) {
    char path[512], drvdir[512], tmpdir[512], hookdir[512];
    const char *ext = app->activity->externalDataPath, *in = app->activity->internalDataPath;
    snprintf(path, sizeof path, "%s/turnip_test.log", ext);
    g_log = fopen(path, "w");
    L("turnip test: external %s, internal %s", ext, in);
    {   // <external files>/env.txt: KEY=VALUE lines set before the driver loads (TU_DEBUG=noubwc, MESA_..., tests)
        snprintf(path, sizeof path, "%s/env.txt", ext);
        FILE *f = fopen(path, "r");
        char line[512];
        while (f && fgets(line, sizeof line, f)) {
            line[strcspn(line, "\r\n")] = 0;
            char *eq = strchr(line, '=');
            if (!eq || line[0] == '#') continue;
            *eq = 0;
            setenv(line, eq + 1, 1);
            L("env %s=%s", line, eq + 1);
        }
        if (f) fclose(f);
    }

    // ---- the Vulkan loader: Turnip through libadrenotools, or the system's
    Dl_info di;
    dladdr((void *)android_main, &di);
    snprintf(hookdir, sizeof hookdir, "%s", di.dli_fname);
    char *slash = strrchr(hookdir, '/');
    if (slash) *slash = 0;
    snprintf(drvdir, sizeof drvdir, "%s/turnip/", in);
    snprintf(tmpdir, sizeof tmpdir, "%s/tmp/", in);
    mkdir(drvdir, 0700), mkdir(tmpdir, 0700);
    char src[512], dst[512];
    snprintf(src, sizeof src, "%s/turnip/vulkan.ad07xx.so", ext);
    snprintf(dst, sizeof dst, "%svulkan.ad07xx.so", drvdir);
    snprintf(path, sizeof path, "%s/use_system", ext);
    int src_ok = access(src, R_OK), src_errno = errno, sys_mark = access(path, F_OK);
    L("driver file %s: %s; use_system mark: %s", src, src_ok == 0 ? "readable" : strerror(src_errno), sys_mark == 0 ? "present" : "absent");
    bool want_turnip = src_ok == 0 && sys_mark != 0;
    void *vk = NULL;
    const char *be = getenv("XRT_BRIDGE");
    bool bridge = want_turnip && be && *be == '1';
    if (bridge) {
        bool copied = copy_file(src, dst);
        void *vt = adrenotools_open_libvulkan(RTLD_NOW, ADRENOTOOLS_DRIVER_CUSTOM, tmpdir, hookdir, drvdir, "vulkan.ad07xx.so", NULL, NULL);
        gipa_t = vt ? (PFN_vkGetInstanceProcAddr)dlsym(vt, "vkGetInstanceProcAddr") : NULL;
        L("bridge: Turnip copied %s, loaded %p, gipa %p", copied ? "yes" : "no", vt, (void *)gipa_t);
        if (!gipa_t) bridge = false;
        want_turnip = false;  // (OpenXR: the system driver)
    }
    if (want_turnip) {
        bool copied = copy_file(src, dst);
        L("Turnip: %s copied to %s: %s; hooks in %s", src, dst, copied ? "yes" : strerror(errno), hookdir);
        vk = adrenotools_open_libvulkan(RTLD_NOW, ADRENOTOOLS_DRIVER_CUSTOM, tmpdir, hookdir, drvdir, "vulkan.ad07xx.so", NULL, NULL);
        L("adrenotools_open_libvulkan: %p (%s)", vk, vk ? "ok" : dlerror());
    }
    if (!vk) {
        vk = dlopen("libvulkan.so", RTLD_NOW | RTLD_LOCAL);
        L("system libvulkan: %p%s", vk, want_turnip ? " (Turnip was wanted: FAILED to load it)" : "");
    }
    gipa = vk ? (PFN_vkGetInstanceProcAddr)dlsym(vk, "vkGetInstanceProcAddr") : NULL;
    if (!gipa) { L("RESULT: no vkGetInstanceProcAddr"); goto out; }
    plain_probe();

    // ---- OpenXR
    PFN_xrInitializeLoaderKHR init_loader = NULL;
    xrGetInstanceProcAddr(XR_NULL_HANDLE, "xrInitializeLoaderKHR", (PFN_xrVoidFunction *)&init_loader);
    if (init_loader) {
        XrLoaderInitInfoAndroidKHR li = {.type = XR_TYPE_LOADER_INIT_INFO_ANDROID_KHR, .applicationVM = app->activity->vm,
                                         .applicationContext = app->activity->clazz};
        init_loader((const XrLoaderInitInfoBaseHeaderKHR *)&li);
    }
    const char *exts[] = {XR_KHR_ANDROID_CREATE_INSTANCE_EXTENSION_NAME, XR_KHR_VULKAN_ENABLE2_EXTENSION_NAME};
    XrInstanceCreateInfoAndroidKHR aci = {.type = XR_TYPE_INSTANCE_CREATE_INFO_ANDROID_KHR, .applicationVM = app->activity->vm,
                                          .applicationActivity = app->activity->clazz};
    XrInstanceCreateInfo xci = {.type = XR_TYPE_INSTANCE_CREATE_INFO, .next = &aci, .enabledExtensionCount = 2, .enabledExtensionNames = exts};
    strcpy(xci.applicationInfo.applicationName, "TurnipTest");
    strcpy(xci.applicationInfo.engineName, "TurnipTest");
    xci.applicationInfo.apiVersion = XR_MAKE_VERSION(1, 0, 0);
    XrInstance xi = XR_NULL_HANDLE;
    XrResult xr = xrCreateInstance(&xci, &xi);
    L("xrCreateInstance: %d", xr);
    if (XR_FAILED(xr)) goto out;
    XrSystemGetInfo sgi = {.type = XR_TYPE_SYSTEM_GET_INFO, .formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY};
    XrSystemId sys = 0;
    xr = xrGetSystem(xi, &sgi, &sys);
    L("xrGetSystem: %d", xr);
    if (XR_FAILED(xr)) goto out;
    PFN_xrGetVulkanGraphicsRequirements2KHR req2;
    PFN_xrCreateVulkanInstanceKHR xr_vk_instance;
    PFN_xrGetVulkanGraphicsDevice2KHR xr_vk_pd;
    PFN_xrCreateVulkanDeviceKHR xr_vk_device;
    xrGetInstanceProcAddr(xi, "xrGetVulkanGraphicsRequirements2KHR", (PFN_xrVoidFunction *)&req2);
    xrGetInstanceProcAddr(xi, "xrCreateVulkanInstanceKHR", (PFN_xrVoidFunction *)&xr_vk_instance);
    xrGetInstanceProcAddr(xi, "xrGetVulkanGraphicsDevice2KHR", (PFN_xrVoidFunction *)&xr_vk_pd);
    xrGetInstanceProcAddr(xi, "xrCreateVulkanDeviceKHR", (PFN_xrVoidFunction *)&xr_vk_device);
    XrGraphicsRequirementsVulkan2KHR req = {.type = XR_TYPE_GRAPHICS_REQUIREMENTS_VULKAN2_KHR};
    xr = req2(xi, sys, &req);
    L("graphics requirements: %d, Vulkan %llx to %llx", xr, (unsigned long long)req.minApiVersionSupported,
      (unsigned long long)req.maxApiVersionSupported);

    VkApplicationInfo vai = {.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO, .pApplicationName = "turnip test", .apiVersion = VK_API_VERSION_1_1};
    VkInstanceCreateInfo vci = {.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pApplicationInfo = &vai};
    XrVulkanInstanceCreateInfoKHR xvi = {.type = XR_TYPE_VULKAN_INSTANCE_CREATE_INFO_KHR, .systemId = sys,
                                         .pfnGetInstanceProcAddr = gipa, .vulkanCreateInfo = &vci};
    VkInstance vi = VK_NULL_HANDLE;
    VkResult vr = VK_ERROR_INITIALIZATION_FAILED;
    xr = xr_vk_instance(xi, &xvi, &vi, &vr);
    L("Vulkan instance through OpenXR: xr %d vk %d", xr, vr);
    if (XR_FAILED(xr) || vr != VK_SUCCESS) goto out;
    XrVulkanGraphicsDeviceGetInfoKHR gdi = {.type = XR_TYPE_VULKAN_GRAPHICS_DEVICE_GET_INFO_KHR, .systemId = sys, .vulkanInstance = vi};
    VkPhysicalDevice pd = VK_NULL_HANDLE;
    xr = xr_vk_pd(xi, &gdi, &pd);
    L("OpenXR's physical device: xr %d, %p", xr, (void *)pd);
    if (XR_FAILED(xr) || !pd) goto out;
    log_device(vi, pd, "OpenXR");

    PFN_vkGetPhysicalDeviceQueueFamilyProperties qfp = (PFN_vkGetPhysicalDeviceQueueFamilyProperties)gipa(vi, "vkGetPhysicalDeviceQueueFamilyProperties");
    uint32_t nq = 8, qfam = 0;
    VkQueueFamilyProperties qp[8];
    qfp(pd, &nq, qp);
    for (uint32_t i = 0; i < nq; i++) if (qp[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) { qfam = i; break; }
    float prio = 1;
    VkDeviceQueueCreateInfo qci = {.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, .queueFamilyIndex = qfam, .queueCount = 1,
                                   .pQueuePriorities = &prio};
    VkDeviceCreateInfo dci = {.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, .queueCreateInfoCount = 1, .pQueueCreateInfos = &qci};
    if (bridge) dci.enabledExtensionCount = 8, dci.ppEnabledExtensionNames = g_bridge_exts;
    XrVulkanDeviceCreateInfoKHR xdi = {.type = XR_TYPE_VULKAN_DEVICE_CREATE_INFO_KHR, .systemId = sys, .pfnGetInstanceProcAddr = gipa,
                                       .vulkanPhysicalDevice = pd, .vulkanCreateInfo = &dci};
    VkDevice dev = VK_NULL_HANDLE;
    vr = VK_ERROR_INITIALIZATION_FAILED;
    xr = xr_vk_device(xi, &xdi, &dev, &vr);
    L("Vulkan device through OpenXR: xr %d vk %d (queue family %u)", xr, vr, qfam);
    if (XR_FAILED(xr) || vr != VK_SUCCESS) goto out;

    PFN_vkGetDeviceProcAddr gdpa = (PFN_vkGetDeviceProcAddr)gipa(vi, "vkGetDeviceProcAddr");
#define VKD(name) PFN_##name name##_ = (PFN_##name)gdpa(dev, #name)
    VKD(vkGetDeviceQueue); VKD(vkCreateCommandPool); VKD(vkAllocateCommandBuffers); VKD(vkBeginCommandBuffer);
    VKD(vkEndCommandBuffer); VKD(vkCmdPipelineBarrier); VKD(vkCmdClearColorImage); VKD(vkQueueSubmit); VKD(vkCreateFence);
    VKD(vkWaitForFences); VKD(vkResetFences); VKD(vkResetCommandBuffer); VKD(vkDeviceWaitIdle);
    VkQueue queue;
    vkGetDeviceQueue_(dev, qfam, 0, &queue);
    VkCommandPoolCreateInfo pci = {.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
                                   .queueFamilyIndex = qfam};
    VkCommandPool pool;
    vkCreateCommandPool_(dev, &pci, NULL, &pool);
    VkCommandBuffer cb;
    VkCommandBufferAllocateInfo cai = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, .commandPool = pool,
                                       .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1};
    vkAllocateCommandBuffers_(dev, &cai, &cb);
    VkFence fence;
    VkFenceCreateInfo fci = {.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    vkCreateFence_(dev, &fci, NULL, &fence);

    // ---- the session, a swapchain per eye
    XrGraphicsBindingVulkan2KHR gb = {.type = XR_TYPE_GRAPHICS_BINDING_VULKAN2_KHR, .instance = vi, .physicalDevice = pd, .device = dev,
                                      .queueFamilyIndex = qfam, .queueIndex = 0};
    XrSessionCreateInfo sci = {.type = XR_TYPE_SESSION_CREATE_INFO, .next = &gb, .systemId = sys};
    XrSession session = XR_NULL_HANDLE;
    xr = xrCreateSession(xi, &sci, &session);
    L("xrCreateSession: %d", xr);
    if (XR_FAILED(xr)) goto out;
    XrReferenceSpaceCreateInfo rsi = {.type = XR_TYPE_REFERENCE_SPACE_CREATE_INFO, .referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL,
                                      .poseInReferenceSpace = {{0, 0, 0, 1}, {0, 0, 0}}};
    XrSpace space;
    xrCreateReferenceSpace(session, &rsi, &space);
    XrViewConfigurationView vcv[2] = {{.type = XR_TYPE_VIEW_CONFIGURATION_VIEW}, {.type = XR_TYPE_VIEW_CONFIGURATION_VIEW}};
    uint32_t nv = 2;
    xrEnumerateViewConfigurationViews(xi, sys, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, 2, &nv, vcv);
    int64_t fmt = 0, fmts[128];
    uint32_t nf = 0;
    xrEnumerateSwapchainFormats(session, 0, &nf, NULL);
    if (nf > 128) nf = 128;
    xrEnumerateSwapchainFormats(session, nf, &nf, fmts);
    for (uint32_t i = 0; i < nf && !fmt; i++) if (fmts[i] == VK_FORMAT_R8G8B8A8_SRGB) fmt = fmts[i];
    if (!fmt && nf) fmt = fmts[0];
    XrSwapchain sc[2];
    XrSwapchainImageVulkan2KHR img[2][8];
    uint32_t nimg[2] = {0, 0};
    for (int e = 0; e < 2; e++) {
        XrSwapchainCreateInfo ci = {.type = XR_TYPE_SWAPCHAIN_CREATE_INFO,
                                    .usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT,
                                    .format = fmt, .sampleCount = 1, .width = vcv[e].recommendedImageRectWidth,
                                    .height = vcv[e].recommendedImageRectHeight, .faceCount = 1, .arraySize = 1, .mipCount = 1};
        xr = xrCreateSwapchain(session, &ci, &sc[e]);
        L("swapchain %d (%ux%u, format %lld): %d", e, ci.width, ci.height, (long long)fmt, xr);
        if (XR_FAILED(xr)) goto out;
        for (int i = 0; i < 8; i++) img[e][i] = (XrSwapchainImageVulkan2KHR){.type = XR_TYPE_SWAPCHAIN_IMAGE_VULKAN2_KHR};
        xr = xrEnumerateSwapchainImages(sc[e], 8, &nimg[e], (XrSwapchainImageBaseHeader *)img[e]);
        L("swapchain %d: %u images, %d", e, nimg[e], xr);
    }

    // ---- the bridge's devices and shared images
    Dev ds = {.inst = vi, .pd = pd, .dev = dev, .qfam = qfam}, dt = {0};
    VkImage ahb_s[2] = {0}, ahb_t[2] = {0};
    if (bridge) {
        ds.gdpa = (PFN_vkGetDeviceProcAddr)gipa(vi, "vkGetDeviceProcAddr");
        dev_cmd(&ds);
        if (!turnip_device(&dt)) { L("RESULT: bridge, no Turnip device"); goto out; }
        dev_cmd(&dt);
        const char *ce = getenv("XRT_AHB_CPU");
        bool cpu = !ce || *ce != '0';
        for (int e = 0; e < 2; e++) {
            AHardwareBuffer_Desc ad = {.width = vcv[e].recommendedImageRectWidth, .height = vcv[e].recommendedImageRectHeight, .layers = 1,
                                       .format = AHARDWAREBUFFER_FORMAT_R8G8B8A8_UNORM,
                                       .usage = AHARDWAREBUFFER_USAGE_GPU_SAMPLED_IMAGE | AHARDWAREBUFFER_USAGE_GPU_COLOR_OUTPUT |
                                                (cpu ? AHARDWAREBUFFER_USAGE_CPU_READ_RARELY : 0)};
            AHardwareBuffer *ahb = NULL;
            int ar = AHardwareBuffer_allocate(&ad, &ahb);
            AHardwareBuffer_Desc got = {0};
            if (ahb) AHardwareBuffer_describe(ahb, &got);
            L("bridge: AHardwareBuffer %d (%ux%u, stride %u, usage %#llx)", ar, got.width, got.height, got.stride, (unsigned long long)got.usage);
            if (ar || !ahb_image(&dt, ahb, ad.width, ad.height, &ahb_t[e], "Turnip") || !ahb_image(&ds, ahb, ad.width, ad.height, &ahb_s[e], "system")) {
                L("RESULT: bridge, the shared image failed");
                goto out;
            }
        }
    }

    // ---- frames: both eyes cleared each frame, 20 s
    bool running = false, done = false;
    double t0 = 0;
    int frames = 0, bad = 0;
    XrResult first_bad = XR_SUCCESS;
    while (!done && !app->destroyRequested) {
        poll_android(app, running ? 0 : 50);
        XrEventDataBuffer ev = {.type = XR_TYPE_EVENT_DATA_BUFFER};
        while (xrPollEvent(xi, &ev) == XR_SUCCESS) {
            if (ev.type == XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED) {
                XrSessionState s = ((XrEventDataSessionStateChanged *)&ev)->state;
                L("session state %d", s);
                if (s == XR_SESSION_STATE_READY) {
                    XrSessionBeginInfo bi = {.type = XR_TYPE_SESSION_BEGIN_INFO, .primaryViewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO};
                    xr = xrBeginSession(session, &bi);
                    L("xrBeginSession: %d", xr);
                    running = XR_SUCCEEDED(xr), t0 = now_s();
                } else if (s == XR_SESSION_STATE_STOPPING) {
                    xrEndSession(session), running = false;
                } else if (s == XR_SESSION_STATE_EXITING || s == XR_SESSION_STATE_LOSS_PENDING) {
                    done = true;
                }
            }
            ev = (XrEventDataBuffer){.type = XR_TYPE_EVENT_DATA_BUFFER};
        }
        if (!running) continue;
        XrFrameState fs = {.type = XR_TYPE_FRAME_STATE};
        XrFrameWaitInfo fwi = {.type = XR_TYPE_FRAME_WAIT_INFO};
        if (XR_FAILED(xr = xrWaitFrame(session, &fwi, &fs))) { if (!bad++) first_bad = xr; continue; }
        XrFrameBeginInfo fbi = {.type = XR_TYPE_FRAME_BEGIN_INFO};
        xrBeginFrame(session, &fbi);
        XrView views[2] = {{.type = XR_TYPE_VIEW}, {.type = XR_TYPE_VIEW}};
        XrViewState vs = {.type = XR_TYPE_VIEW_STATE};
        XrViewLocateInfo vli = {.type = XR_TYPE_VIEW_LOCATE_INFO, .viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO,
                                .displayTime = fs.predictedDisplayTime, .space = space};
        uint32_t nviews = 2;
        xrLocateViews(session, &vli, &vs, 2, &nviews, views);
        XrCompositionLayerProjectionView pv[2];
        float pulse = 0.5f + 0.5f * sinf((float)(now_s() - t0) * 3.0f);
        for (int e = 0; e < 2; e++) {
            uint32_t idx = 0;
            XrSwapchainImageAcquireInfo ai = {.type = XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
            XrSwapchainImageWaitInfo wi = {.type = XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO, .timeout = XR_INFINITE_DURATION};
            if (XR_FAILED(xr = xrAcquireSwapchainImage(sc[e], &ai, &idx)) || XR_FAILED(xr = xrWaitSwapchainImage(sc[e], &wi))) {
                if (!bad++) first_bad = xr;
                continue;
            }
            VkImage image = img[e][idx].image;
            if (bridge) {
                dev_begin(&dt);
                barrier1(&dt, ahb_t[e], VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_QUEUE_FAMILY_FOREIGN_EXT, dt.qfam, 0, VK_ACCESS_TRANSFER_WRITE_BIT);
                VkClearColorValue cc = {.float32 = {e == 0 ? pulse : 0.05f, 0.05f, e == 1 ? pulse : 0.05f, 1}};
                VkImageSubresourceRange rr0 = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
                DF(&dt, vkCmdClearColorImage)(dt.cb, ahb_t[e], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &cc, 1, &rr0);
                barrier1(&dt, ahb_t[e], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL, dt.qfam, VK_QUEUE_FAMILY_FOREIGN_EXT, VK_ACCESS_TRANSFER_WRITE_BIT, 0);
                DF(&dt, vkEndCommandBuffer)(dt.cb);
                VkResult r1 = dev_run(&dt);
                dev_begin(&ds);
                barrier1(&ds, ahb_s[e], VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_QUEUE_FAMILY_FOREIGN_EXT, ds.qfam, 0, VK_ACCESS_TRANSFER_READ_BIT);
                barrier1(&ds, image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED, 0, VK_ACCESS_TRANSFER_WRITE_BIT);
                VkImageCopy ic = {.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, .dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                                  .extent = {vcv[e].recommendedImageRectWidth, vcv[e].recommendedImageRectHeight, 1}};
                DF(&ds, vkCmdCopyImage)(ds.cb, ahb_s[e], VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &ic);
                barrier1(&ds, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT);
                barrier1(&ds, ahb_s[e], VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL, ds.qfam, VK_QUEUE_FAMILY_FOREIGN_EXT, VK_ACCESS_TRANSFER_READ_BIT, 0);
                DF(&ds, vkEndCommandBuffer)(ds.cb);
                VkResult r2 = dev_run(&ds);
                if ((r1 || r2) && !bad++) first_bad = (XrResult)(r1 ? r1 : r2);
                XrSwapchainImageReleaseInfo ri0 = {.type = XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
                xrReleaseSwapchainImage(sc[e], &ri0);
                pv[e] = (XrCompositionLayerProjectionView){.type = XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW, .pose = views[e].pose, .fov = views[e].fov,
                                                           .subImage = {.swapchain = sc[e], .imageRect = {{0, 0}, {(int32_t)vcv[e].recommendedImageRectWidth,
                                                                                                             (int32_t)vcv[e].recommendedImageRectHeight}}}};
                continue;
            }
            VkCommandBufferBeginInfo bi = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
            vkResetCommandBuffer_(cb, 0);
            vkBeginCommandBuffer_(cb, &bi);
            VkImageSubresourceRange rr = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
            VkImageMemoryBarrier b1 = {.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
                                       .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED, .newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                       .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                                       .image = image, .subresourceRange = rr};
            vkCmdPipelineBarrier_(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1, &b1);
            VkClearColorValue c = {.float32 = {e == 0 ? pulse : 0.05f, 0.05f, e == 1 ? pulse : 0.05f, 1}};
            vkCmdClearColorImage_(cb, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &c, 1, &rr);
            VkImageMemoryBarrier b2 = b1;
            b2.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT, b2.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
            b2.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, b2.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
            vkCmdPipelineBarrier_(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, 0, 0, NULL, 0, NULL, 1, &b2);
            vkEndCommandBuffer_(cb);
            VkSubmitInfo si = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &cb};
            vr = vkQueueSubmit_(queue, 1, &si, fence);
            if (vr == VK_SUCCESS) vr = vkWaitForFences_(dev, 1, &fence, VK_TRUE, 1000000000ull);
            vkResetFences_(dev, 1, &fence);
            if (vr != VK_SUCCESS) { if (!bad++) first_bad = (XrResult)vr; L("eye %d: Vulkan %d", e, vr); }
            XrSwapchainImageReleaseInfo ri = {.type = XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
            xrReleaseSwapchainImage(sc[e], &ri);
            pv[e] = (XrCompositionLayerProjectionView){.type = XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW, .pose = views[e].pose,
                                                       .fov = views[e].fov,
                                                       .subImage = {.swapchain = sc[e], .imageRect = {{0, 0}, {(int32_t)vcv[e].recommendedImageRectWidth,
                                                                                                         (int32_t)vcv[e].recommendedImageRectHeight}}}};
        }
        XrCompositionLayerProjection layer = {.type = XR_TYPE_COMPOSITION_LAYER_PROJECTION, .space = space, .viewCount = 2, .views = pv};
        const XrCompositionLayerBaseHeader *layers[1] = {(const XrCompositionLayerBaseHeader *)&layer};
        XrFrameEndInfo fei = {.type = XR_TYPE_FRAME_END_INFO, .displayTime = fs.predictedDisplayTime,
                              .environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE, .layerCount = fs.shouldRender ? 1 : 0,
                              .layers = layers};
        xr = xrEndFrame(session, &fei);
        if (XR_FAILED(xr) && !bad++) first_bad = xr;
        if (++frames % 120 == 1) L("frame %d: end %d, %d bad so far", frames, xr, bad);
        if (now_s() - t0 > 20) { xrRequestExitSession(session); }
    }
    vkDeviceWaitIdle_(dev);
    L("RESULT: driver %s, %d frames, %d failures (first %d)", bridge ? "bridge (Turnip draws, system copies)" : want_turnip ? "Turnip wanted" : "system",
      frames, bad, first_bad);
out:
    L("done");
    if (g_log) fclose(g_log);
    // (the process ends with the test: a second start in the same process had the first run's Vulkan loaded and its
    //  OpenXR instance still alive - xrCreateInstance answered XR_ERROR_LIMIT_REACHED)
    ANativeActivity_finish(app->activity);
    for (int i = 0; i < 50 && !app->destroyRequested; i++) poll_android(app, 20);
    _exit(0);
}
