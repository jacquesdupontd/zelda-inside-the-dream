// Vulkan device, window, swapchain and presentation (counterpart of the window/present half of metal.m).
#include <SDL3/SDL.h>
#include <SDL3/SDL_vulkan.h>
#include <math.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include "vk_xr.h"

#include "../../paths.h"
#include "../../rt.h"  // (HEAP_MAX: lazy_heap_predicted)
#include "../../settings.h"
#include "../../ui/menu.h"
#include "../rqueue.h"
#include "vk.h"
#include "../../platform.h"
#include "record.h"
#include "../output_scale.h"
#include "../renderer.h"
#include "../pace.h"
#include "../shader_prep.h"
#include "../interp.h"
#include <pthread.h>
#include <dirent.h>

VkG g_vk;
static SDL_Window *window;

// The system memory heap a whole-heap import (exec.c host_import) lands in and what it can hold: the largest heap with
// a host-visible memory type that isn't device-local (all of them device-local: the largest host-visible one), the
// smaller of its size and its budget (budget 0: unknown). -1: no host-visible heap. Plain types, so
// tools/test_gmem_lowmem.py builds it without Vulkan headers.
typedef struct { uint64_t size, budget; bool device_local, host_visible; } ImportHeap;
static int import_heap_room(const ImportHeap *h, uint32_t n, uint64_t *room) {
    int best = -1;
    for (int pass = 0; pass < 2 && best < 0; pass++)
        for (uint32_t i = 0; i < n; i++)
            if (h[i].host_visible && (pass || !h[i].device_local) && (best < 0 || h[i].size > h[best].size)) best = (int)i;
    if (best >= 0) *room = h[best].budget && h[best].budget < h[best].size ? h[best].budget : h[best].size;
    return best;
}
// What the heap must hold beyond the import: the transient ring's chunks once they spill to system memory (64 MB
// each), readback, the driver's own. Issue #205 (8 GB laptop, Ryzen 5 5500U, AMD's Windows driver): system heap
// 2738 MB, budget 2601 - the 3220 MB import was accepted, then the first submission failed with out of device memory.
// An 8 GB desktop with a GPU of its own (system heap ~3950 MB, budget ~3840) stays on one import.
#define IMPORT_MARGIN (256ull << 20)
// Under this much room one import should fit but might not (a driver's own limits, other programs): the import is
// tried once at the start (exec.c whole_import_probe_oom), with buffer device addresses enabled so lazy chunks can take
// over. Machines with more (an RTX with 16 GB or more of RAM) skip the probe and keep the device as before.
#define IMPORT_PROBE_BELOW (2 * HEAP_MAX)

// Whether one import of the whole heap fits its system memory heap (import_heap_room), from the physical device;
// *probe: it fits, but not by much (IMPORT_PROBE_BELOW), or LARECOMP_VK_SIM_HEAP_OOM=1 (the probe then fails).
static bool whole_import_fits(bool budget_ext, bool *probe) {
    VkPhysicalDeviceMemoryBudgetPropertiesEXT bp = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_BUDGET_PROPERTIES_EXT};
    VkPhysicalDeviceMemoryProperties2 mp2 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_PROPERTIES_2,
                                             .pNext = budget_ext ? &bp : NULL};
    vkGetPhysicalDeviceMemoryProperties2(g_vk.phys, &mp2);
    const VkPhysicalDeviceMemoryProperties *mp = &mp2.memoryProperties;
    ImportHeap h[VK_MAX_MEMORY_HEAPS] = {{0}};
    for (uint32_t i = 0; i < mp->memoryHeapCount; i++) {
        h[i].size = mp->memoryHeaps[i].size;
        h[i].budget = budget_ext ? bp.heapBudget[i] : 0;
        h[i].device_local = (mp->memoryHeaps[i].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) != 0;
    }
    for (uint32_t i = 0; i < mp->memoryTypeCount; i++)
        if (mp->memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) h[mp->memoryTypes[i].heapIndex].host_visible = true;
    uint64_t room = 0;
    int heap = import_heap_room(h, mp->memoryHeapCount, &room);
    const char *sim = getenv("LARECOMP_VK_SIM_HEAP_OOM");
    if (heap < 0 || room >= HEAP_MAX + IMPORT_MARGIN) {
        *probe = (heap >= 0 && room < IMPORT_PROBE_BELOW) || (sim && atoi(sim));
        return true;
    }
    LOG("[vk] guest heap: memory heap %d holds %llu MB (size %llu, budget %llu), one import needs %llu MB: "
        "imported in chunks as the game binds them\n", heap, (unsigned long long)(room >> 20),
        (unsigned long long)(h[heap].size >> 20), (unsigned long long)(h[heap].budget >> 20),
        (unsigned long long)((HEAP_MAX + IMPORT_MARGIN) >> 20));
    return false;
}

// Whether the guest heap will be imported chunk by chunk (exec.c gmem_lazy): on a GPU whose storage buffers can't
// span the heap (maxStorageBufferRange < HEAP_MAX: Intel, 1 GB); where the system memory heap or its budget can't hold
// one import of the whole heap (whole_import_fits: 8 GB laptops, issue #205); and on RADV when the dma-buf heap
// (exec.c guest_heap_dmabuf) has no cached host-visible memory type that holds it - the same test, which needs only
// the physical device - or LARECOMP_VK_GMEM_LAZY=1 (=0: never; a GPU with a short range then gets windows of one
// import, exec.c gmem_chunk_shift). Decided before the device exists because the lazy import needs buffer device
// addresses, and on RADV enabling them puts every allocation on the driver's global BO list (amdgpu checks all of
// them each submission): a device that imports the heap another way shouldn't pay for that.
// *probe: one import is to be tried first (whole_import_fits), lazy chunks if it runs out of memory.
static bool lazy_heap_predicted(bool budget_ext, bool *probe) {
    *probe = false;
    const char *e = getenv("LARECOMP_VK_GMEM_LAZY");
    if (e) return atoi(e) != 0;
    if (getenv("LARECOMP_VK_NO_GMEM")) return false;
    uint64_t lim = g_vk.props.limits.maxStorageBufferRange;
    if (lim && lim < HEAP_MAX) return true;  // (UHD 770 main pass: lazy 8.4 ms, 512 MB windows 8.5)
#ifdef _WIN32
    // The Windows AMD integrated driver accepts one import of the whole heap (the probe writes both ends) and then
    // faults a shader read of it: device lost, as soon as a save loads. Chunks stay inside the pages it maps.
    if (g_vk.props.vendorID == 0x1002 && g_vk.props.deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU)
        return true;
#endif
#ifdef __linux__
    // RADV / PanVK with the dma-buf heap (exec.c guest_heap_dmabuf): no import at all, so whole_import_fits doesn't apply; any
    // cached host-visible memory type will do, as there (a heap's reported size is only the driver's split: a ROG Ally
    // has 3305 MB for 3328, and the allocation works). Lazy chunks over it would never be imported: every shader access
    // a miss, and the render thread remaking the chunk set each frame (15 ms a frame on the Ally).
    const char *d = getenv("LARECOMP_VK_HEAP_DMABUF");
    if ((d ? atoi(d) != 0 : g_vk.heap_dmabuf_default) && (g_vk.dmabuf || g_vk.map_placed)) {
        VkPhysicalDeviceMemoryProperties mp;
        vkGetPhysicalDeviceMemoryProperties(g_vk.phys, &mp);
        const VkMemoryPropertyFlags want = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT | VK_MEMORY_PROPERTY_HOST_CACHED_BIT;
        for (uint32_t i = 0; i < mp.memoryTypeCount; i++)
            if ((mp.memoryTypes[i].propertyFlags & want) == want) return false;
    }
#endif
    if (!whole_import_fits(budget_ext, probe)) return true;
    if (!strstr(g_vk.props.deviceName, "RADV")) return false;
    *probe = false;  // (RADV without the dma-buf heap: lazy chunks, no probe)
    return true;
}
static atomic_bool ui_ready;
static VkDebugUtilsMessengerEXT messenger;
static void exit_after_shot(void);  // (UI thread section)

// ================================================================== instance / device
static VKAPI_ATTR VkBool32 VKAPI_CALL debug_cb(VkDebugUtilsMessageSeverityFlagBitsEXT sev, VkDebugUtilsMessageTypeFlagsEXT type,
                                               const VkDebugUtilsMessengerCallbackDataEXT *d, void *user) {
    (void)type, (void)user;
    static atomic_uint n;
    bool stop = sev >= VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT && getenv("LARECOMP_VK_VALIDATION_STOP");
    if (sev >= VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT &&
        (stop || atomic_fetch_add_explicit(&n, 1, memory_order_relaxed) < 500)) LOG("[vk] %s\n", d->pMessage);
    if (stop) {
        LOG("[vk] stopping at the first validation error\n");
        fflush(stderr);
        _Exit(EXIT_FAILURE);  // do not run GPU cleanup after the validation failure
    }
    return VK_FALSE;
}

// LARECOMP_VK_VALIDATION=1: Khronos validation layer (MSYS2 package vulkan-validation-layers)
// LARECOMP_VK_VALIDATION_STOP=1: require that layer and its callback, and exit nonzero on the first error.
static bool validating;  // the validation layer is on (validation_exit)
// LARECOMP_OFFSCREEN=<w>x<h>: no window - the game renders into an image of its own, with no SDL video (a native
// process on the Quest 3 run over adb: the first step toward its app, A-FAIRE/lasrecomp-quest3-appsw.md; the window
// shots still work: LARECOMP_WINDOW_SHOT). Also the image an OpenXR app will copy into its swapchain.
static int g_off_w = -1, g_off_h;
static bool offscreen(void) {
    if (g_off_w < 0) {
        g_off_w = 0;
        const char *e = getenv("LARECOMP_OFFSCREEN");
        if (e && sscanf(e, "%dx%d", &g_off_w, &g_off_h) != 2) g_off_w = 0;
    }
    return g_off_w > 0;
}

VkXrHooks *g_vk_xr;  // (set by an OpenXR shell before the game starts: vk_xr.h)
// A flat shell's window (vk_xr.h surface / size: the Android tablet): the backend's own swapchain on it, no SDL window
static bool shell_window(void) { return g_vk_xr && g_vk_xr->surface && g_vk_xr->size; }
static void window_px(int *w, int *h) {
    if (shell_window()) g_vk_xr->size(w, h);
    else SDL_GetWindowSizeInPixels(window, w, h);
}
static SDL_Mutex *g_queue_mutex;  // (made with the device, before any submission)
static uint32_t g_qcount, g_xr_qindex;  // (the game's queue family's queues; the one OpenXR is given)
uint32_t vk_xr_queue_index(void) { return g_xr_qindex; }
void vk_queue_lock(void) { SDL_LockMutex(g_queue_mutex); }
void vk_queue_unlock(void) { SDL_UnlockMutex(g_queue_mutex); }

static void create_instance(void) {
#ifdef __ANDROID__
    vk_android_load();  // (the player's driver package, or the system's: android_driver.c)
#endif
    Uint32 n_sdl = 0;
    const char *const *sdl_ext = offscreen() || shell_window() ? NULL : SDL_Vulkan_GetInstanceExtensions(&n_sdl);
    const char *ext[16];
    uint32_t n_ext = 0;
    for (Uint32 i = 0; i < n_sdl && n_ext < 14; i++) ext[n_ext++] = sdl_ext[i];
    bool strict = getenv("LARECOMP_VK_VALIDATION_STOP") != NULL;
    bool validate = strict || getenv("LARECOMP_VK_VALIDATION") != NULL;
    if (validate) ext[n_ext++] = VK_EXT_DEBUG_UTILS_EXTENSION_NAME;
    const char *layers[] = {"VK_LAYER_KHRONOS_validation"};
    VkApplicationInfo app = {.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO, .pApplicationName = "LARecomp",
                             .pEngineName = "LARecomp", .apiVersion = VK_API_VERSION_1_3};
    VkInstanceCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pApplicationInfo = &app,
                               .enabledExtensionCount = n_ext, .ppEnabledExtensionNames = ext,
                               .enabledLayerCount = validate ? 1 : 0, .ppEnabledLayerNames = layers};
    VkResult r = g_vk_xr ? g_vk_xr->create_instance(&ci, &g_vk.inst) : vkCreateInstance(&ci, NULL, &g_vk.inst);
#ifdef LR_VK_DISPATCH  // (Android: the commands through pointers, from the driver the app chose - vk_dispatch.h)
    if (r == VK_SUCCESS) lr_vk_load_instance(g_vk.inst);
#endif
    if (r != VK_SUCCESS && validate) {
        if (strict) {
            LOG("[vk] cannot create an instance with required validation (%d)\n", r);
            fflush(stderr);
            _Exit(EXIT_FAILURE);
        }
        LOG("[vk] validation layer unavailable (%d); continuing without\n", r);
        ci.enabledLayerCount = 0;
        ci.enabledExtensionCount--;
        validate = false;
        r = vkCreateInstance(&ci, NULL, &g_vk.inst);
    }
    validating = validate;
    if (r != VK_SUCCESS) FATAL("vkCreateInstance: %d (is a Vulkan 1.3 driver installed?)\n", r);
#ifdef __ANDROID__
    volkLoadInstance(g_vk.inst);
#endif
    if (validate) {
        PFN_vkCreateDebugUtilsMessengerEXT f =
            (PFN_vkCreateDebugUtilsMessengerEXT)vkGetInstanceProcAddr(g_vk.inst, "vkCreateDebugUtilsMessengerEXT");
        VkDebugUtilsMessengerCreateInfoEXT mi = {
            .sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT,
            .messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT,
            .messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
                           VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT,
            .pfnUserCallback = debug_cb};
        VkResult result = f ? f(g_vk.inst, &mi, NULL, &messenger) : VK_ERROR_EXTENSION_NOT_PRESENT;
        if (result != VK_SUCCESS && strict) {
            LOG("[vk] required validation callback unavailable (%d)\n", result);
            fflush(stderr);
            _Exit(EXIT_FAILURE);
        }
        if (result == VK_SUCCESS) LOG("[vk] validation enabled%s\n", strict ? " (stop on error)" : "");
    }
}

static bool has_ext(VkExtensionProperties *e, uint32_t n, const char *name) {
    for (uint32_t i = 0; i < n; i++) if (!strcmp(e[i].extensionName, name)) return true;
    return false;
}

// Discrete GPU first (LARECOMP_GPU=<index> picks one), needs Vulkan 1.3 and a queue that can present.
static void pick_device(void) {
    uint32_t n = 0;
    vkEnumeratePhysicalDevices(g_vk.inst, &n, NULL);
    if (!n) FATAL("no Vulkan device\n");
    VkPhysicalDevice *pd = calloc(n, sizeof *pd);
    vkEnumeratePhysicalDevices(g_vk.inst, &n, pd);
    int want = getenv("LARECOMP_GPU") ? atoi(getenv("LARECOMP_GPU")) : -1, best = -1, best_score = -1;
    VkPhysicalDevice xr_gpu = g_vk_xr ? g_vk_xr->physical_device(g_vk.inst) : VK_NULL_HANDLE;
    for (uint32_t i = 0; i < n; i++) {
        VkPhysicalDeviceProperties p;
        vkGetPhysicalDeviceProperties(pd[i], &p);
        LOG("[vk] gpu %u: %s (api %u.%u.%u)\n", i, p.deviceName, VK_API_VERSION_MAJOR(p.apiVersion),
            VK_API_VERSION_MINOR(p.apiVersion), VK_API_VERSION_PATCH(p.apiVersion));
        if (p.apiVersion < VK_API_VERSION_1_3) continue;
        int score = p.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU ? 3 : p.deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU ? 2 : 1;
        if ((int)i == want) score = 100;
        if (pd[i] == xr_gpu) score = 200;
        if (score > best_score) { best_score = score; best = (int)i; }
    }
    if (best < 0) FATAL("no Vulkan 1.3 device\n");
    g_vk.phys = pd[best];
    free(pd);
    vkGetPhysicalDeviceProperties(g_vk.phys, &g_vk.props);
    uint32_t nq = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(g_vk.phys, &nq, NULL);
    VkQueueFamilyProperties *qp = calloc(nq, sizeof *qp);
    vkGetPhysicalDeviceQueueFamilyProperties(g_vk.phys, &nq, qp);
    g_vk.qfam = UINT32_MAX;
    for (uint32_t i = 0; i < nq; i++) {
        VkBool32 present = offscreen();
        if (!present) vkGetPhysicalDeviceSurfaceSupportKHR(g_vk.phys, i, g_vk.surface, &present);
        if ((qp[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) && (qp[i].queueFlags & VK_QUEUE_COMPUTE_BIT) && present) {
            g_vk.qfam = i, g_vk.ts_bits = qp[i].timestampValidBits;
            break;
        }
    }
    if (g_vk.qfam != UINT32_MAX) g_qcount = qp[g_vk.qfam].queueCount;
    free(qp);
    if (g_vk.qfam == UINT32_MAX) FATAL("no graphics+present queue\n");
    LOG("[vk] using %s\n", g_vk.props.deviceName);
    settings_resolve_defaults(g_vk.props.deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU);
    // On by default on integrated GPUs only: lean usage and the SMAA stencil measured faster on an Intel UHD 770 and
    // slightly slower on an RTX 3080 Ti; skipping the per-frame target clears saved 3.7 ms on the UHD 770 (2x) with
    // identical images. LARECOMP_VK_LEAN_USAGE / LARECOMP_VK_SMAA_STENCIL / LARECOMP_NO_FRAME_CLEAR =0 / =1 override.
    bool igpu = g_vk.props.deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU;
    const char *e = getenv("LARECOMP_VK_LEAN_USAGE");
    g_vk.lean_usage = e && *e ? atoi(e) != 0 : igpu;
    e = getenv("LARECOMP_VK_SMAA_STENCIL");
    g_vk.smaa_stencil = e && *e ? atoi(e) != 0 : igpu;
    e = getenv("LARECOMP_NO_FRAME_CLEAR");
    g_vk.no_frame_clear = e && *e ? atoi(e) != 0 : igpu;
    LOG("[vk] lean usage %s, SMAA stencil %s, per-frame target clears %s (%s GPU)\n", g_vk.lean_usage ? "on" : "off",
        g_vk.smaa_stencil ? "on" : "off", g_vk.no_frame_clear ? "off" : "on", igpu ? "integrated" : "not an integrated");
}

// The GPU's PCI-style vendor ID (0x5143 Qualcomm, 0x13B5 Arm, ...); 0 before a device is picked. Android's start
// screen offers driver packages only on Adreno (runtime/ui/launcher.mm driver_rows).
uint32_t vk_gpu_vendor(void) { return g_vk.phys ? g_vk.props.vendorID : 0; }

static bool g_fault_ext;
// After VK_ERROR_DEVICE_LOST: what the driver says faulted (VK_EXT_device_fault), into the log.
void vk_device_fault_report(void) {
    static int once;
    if (once++ || !g_fault_ext) return;
    PFN_vkGetDeviceFaultInfoEXT get = (PFN_vkGetDeviceFaultInfoEXT)vkGetDeviceProcAddr(g_vk.dev, "vkGetDeviceFaultInfoEXT");
    if (!get) return;
    VkDeviceFaultCountsEXT c = {.sType = VK_STRUCTURE_TYPE_DEVICE_FAULT_COUNTS_EXT};
    if (get(g_vk.dev, &c, NULL) != VK_SUCCESS) { LOG("[vk] device fault: no info\n"); return; }
    VkDeviceFaultAddressInfoEXT addr[16];
    VkDeviceFaultVendorInfoEXT vend[16];
    if (c.addressInfoCount > 16) c.addressInfoCount = 16;
    if (c.vendorInfoCount > 16) c.vendorInfoCount = 16;
    c.vendorBinarySize = 0;
    VkDeviceFaultInfoEXT info = {.sType = VK_STRUCTURE_TYPE_DEVICE_FAULT_INFO_EXT, .pAddressInfos = addr, .pVendorInfos = vend};
    if (get(g_vk.dev, &c, &info) != VK_SUCCESS && 0) return;
    LOG("[vk] device fault: %s\n", info.description);
    static const char *const kind[] = {"none", "read invalid", "write invalid", "execute invalid", "ip unknown", "ip invalid", "ip valid"};
    for (uint32_t i = 0; i < c.addressInfoCount; i++)
        LOG("[vk]   address %s %#llx (precision %#llx)\n", addr[i].addressType < 7 ? kind[addr[i].addressType] : "?",
            (unsigned long long)addr[i].reportedAddress, (unsigned long long)addr[i].addressPrecision);
    for (uint32_t i = 0; i < c.vendorInfoCount; i++)
        LOG("[vk]   vendor: %s (code %#llx data %#llx)\n", vend[i].description, (unsigned long long)vend[i].vendorFaultCode,
            (unsigned long long)vend[i].vendorFaultData);
}

// Each memory heap: its size, the driver's budget for this process (smaller when other programs hold that memory),
// this process's use and the part LAS Recomp allocated through VMA. Logged at start and when an allocation fails
// (issue #45: out of device memory on a 6 GB GTX 1060 - our own use, or other programs'?).
void vk_log_memory(const char *why) {
    if (!g_vk.vma) return;
    const VkPhysicalDeviceMemoryProperties *mp;
    vmaGetMemoryProperties(g_vk.vma, &mp);
    VmaBudget b[VK_MAX_MEMORY_HEAPS];
    vmaGetHeapBudgets(g_vk.vma, b);
    for (uint32_t i = 0; i < mp->memoryHeapCount; i++)
        LOG("[vk] memory (%s) heap %u %s: %llu MB, budget %llu MB, in use %llu MB, ours %llu MB in %u allocations\n", why, i,
            mp->memoryHeaps[i].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT ? "device-local" : "system",
            (unsigned long long)(mp->memoryHeaps[i].size >> 20), (unsigned long long)(b[i].budget >> 20),
            (unsigned long long)(b[i].usage >> 20), (unsigned long long)(b[i].statistics.allocationBytes >> 20),
            b[i].statistics.allocationCount);
}

// ---- pipeline cache file
#define PCACHE_FILE "cache/vk_pipelines.bin"
#define PCACHE_TAG "cache/vk_pipelines.bin.tag"
static size_t pcache_saved;  // bytes of the last save (or load)
void vk_pipeline_cache_save(void);

static void pipeline_cache_tag(char *out, size_t n) {
    char gpu[400], uuid[2 * VK_UUID_SIZE + 1];
    for (int i = 0; i < VK_UUID_SIZE; i++) snprintf(uuid + 2 * i, 3, "%02x", g_vk.props.pipelineCacheUUID[i]);
    snprintf(gpu, sizeof gpu, "%s, vendor %x device %x, driver %u, cache %s", g_vk.props.deviceName, g_vk.props.vendorID,
             g_vk.props.deviceID, g_vk.props.driverVersion, uuid);
    shader_cache_tag(out, n, gpu);
}

static void *pipeline_cache_saver(void *a) {  // every 3 minutes, when it grew
    (void)a;
    for (;;) {
        SDL_Delay(180000);
        if (rq_stopping()) break;  // (quitting: backend_quit saves it)
        vk_pipeline_cache_save();
    }
    return NULL;
}

// ---- pipelines from our own stored binaries (VK_KHR_pipeline_binary, issue #509)
// AMD's Windows driver keeps nothing of its own compile in our VkPipelineCache (vk_pipelines.bin stayed at 395 KB,
// every launch compiled all pipelines again: 44-54 s). Where it's on (pb.on: AMD's drivers by default,
// LARECOMP_VK_PIPELINE_BINARY=0 / =1 overrides), each pipeline's binaries are kept in cache/vk_binaries/<its key>.bin
// and the next launch creates it from them. The file name is the driver's key for the pipeline's create info
// (vkGetPipelineKeyKHR: shaders and state); the directory's tag adds this build, the translator, the GPU, the driver
// and the driver's global key - another one empties it. Other drivers: VkPipelineCache only, as before.
#define PBIN_DIR "cache/vk_binaries"
#define PBIN_TAG "cache/vk_binaries.tag"
#define PBIN_MAGIC 0x4250414cu  // "LAPB"
#define PBIN_MAX 8              // binaries per pipeline (one per stage at most here)
#ifdef VK_KHR_pipeline_binary
static struct {
    bool on;
    PFN_vkGetPipelineKeyKHR get_key;
    PFN_vkCreatePipelineBinariesKHR create;
    PFN_vkDestroyPipelineBinaryKHR destroy;
    PFN_vkGetPipelineBinaryDataKHR get_data;
    PFN_vkReleaseCapturedPipelineDataKHR release;
} pb;
static _Atomic int pb_loaded, pb_saved, pb_failed;

static void pb_path(char *out, size_t n, const VkPipelineBinaryKeyKHR *k) {
    char hex[2 * VK_MAX_PIPELINE_BINARY_KEY_SIZE_KHR + 1] = "";
    for (uint32_t i = 0; i < k->keySize && i < VK_MAX_PIPELINE_BINARY_KEY_SIZE_KHR; i++) snprintf(hex + 2 * i, 3, "%02x", k->key[i]);
    snprintf(out, n, PBIN_DIR "/%s.bin", hex);
}

static void pb_clear_dir(void) {
    DIR *d = opendir(PBIN_DIR);
    if (!d) return;
    struct dirent *e;
    int n = 0;
    char p[600];
    while ((e = readdir(d))) {
        size_t l = strlen(e->d_name);
        if (l < 5 || strcmp(e->d_name + l - 4, ".bin")) continue;
        snprintf(p, sizeof p, PBIN_DIR "/%s", e->d_name);
        n += remove(p) == 0;
    }
    closedir(d);
    if (n) LOG("[vk] pipeline binaries: removed %d from another build / GPU / driver\n", n);
}

// After the device: the functions, and the directory's tag (with the driver's global key)
static void pb_setup(void) {
#define PB_FN(f, name) (pb.f = (void *)vkGetDeviceProcAddr(g_vk.dev, name))
    if (!PB_FN(get_key, "vkGetPipelineKeyKHR") || !PB_FN(create, "vkCreatePipelineBinariesKHR") ||
        !PB_FN(destroy, "vkDestroyPipelineBinaryKHR") || !PB_FN(get_data, "vkGetPipelineBinaryDataKHR") ||
        !PB_FN(release, "vkReleaseCapturedPipelineDataKHR")) {
        LOG("[vk] pipeline binaries: functions missing; pipeline cache only\n");
        pb.on = false;
        return;
    }
#undef PB_FN
    VkPipelineBinaryKeyKHR gk = {.sType = VK_STRUCTURE_TYPE_PIPELINE_BINARY_KEY_KHR};
    if (pb.get_key(g_vk.dev, NULL, &gk) != VK_SUCCESS) {
        LOG("[vk] pipeline binaries: no global key; pipeline cache only\n");
        pb.on = false;
        return;
    }
    char tag[700], base[512], hex[2 * VK_MAX_PIPELINE_BINARY_KEY_SIZE_KHR + 1] = "";
    for (uint32_t i = 0; i < gk.keySize && i < VK_MAX_PIPELINE_BINARY_KEY_SIZE_KHR; i++) snprintf(hex + 2 * i, 3, "%02x", gk.key[i]);
    pipeline_cache_tag(base, sizeof base);
    snprintf(tag, sizeof tag, "%s, binary key %s", base, hex);
    mkdir("cache", 0755);
    mkdir(PBIN_DIR, 0755);
    if (!shader_cache_tag_ok(PBIN_TAG, tag)) {
        pb_clear_dir();
        shader_cache_tag_write(PBIN_TAG, tag);
    }
}

typedef union { VkGraphicsPipelineCreateInfo g; VkComputePipelineCreateInfo c; } AnyPipeCI;
static VkResult pb_create_pipeline(AnyPipeCI *ci, bool compute, VkPipelineCache cache, VkPipeline *out) {
    return compute ? vkCreateComputePipelines(g_vk.dev, cache, 1, &ci->c, NULL, out)
                   : vkCreateGraphicsPipelines(g_vk.dev, cache, 1, &ci->g, NULL, out);
}

// The stored binaries for key k as handles (false: none, or unusable)
static bool pb_load(const VkPipelineBinaryKeyKHR *k, VkPipelineBinaryKHR *bins, uint32_t *nbins) {
    char path[600];
    pb_path(path, sizeof path, k);
    FILE *f = fopen(path, "rb");
    if (!f) return false;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *buf = sz > 16 ? malloc((size_t)sz) : NULL;
    bool ok = buf && fread(buf, 1, (size_t)sz, f) == (size_t)sz;
    fclose(f);
    // u32 magic, u32 key size, key, u32 count; per binary: u32 key size, key, u64 data size, data
    size_t at = 0, end = ok ? (size_t)sz : 0;
#define PB_TAKE(dst, n) (at + (n) <= end ? (memcpy((dst), buf + at, (n)), at += (n), true) : false)
    uint32_t magic = 0, ks = 0, n = 0;
    uint8_t key[VK_MAX_PIPELINE_BINARY_KEY_SIZE_KHR];
    ok = ok && PB_TAKE(&magic, 4) && magic == PBIN_MAGIC && PB_TAKE(&ks, 4) && ks == k->keySize &&
         ks <= sizeof key && PB_TAKE(key, ks) && !memcmp(key, k->key, ks) && PB_TAKE(&n, 4) && n && n <= PBIN_MAX;
    VkPipelineBinaryKeyKHR keys[PBIN_MAX];
    VkPipelineBinaryDataKHR data[PBIN_MAX];
    for (uint32_t i = 0; ok && i < n; i++) {
        keys[i] = (VkPipelineBinaryKeyKHR){.sType = VK_STRUCTURE_TYPE_PIPELINE_BINARY_KEY_KHR};
        uint64_t ds = 0;
        ok = PB_TAKE(&keys[i].keySize, 4) && keys[i].keySize <= VK_MAX_PIPELINE_BINARY_KEY_SIZE_KHR &&
             PB_TAKE(keys[i].key, keys[i].keySize) && PB_TAKE(&ds, 8) && ds && ds <= end - at;
        if (ok) data[i] = (VkPipelineBinaryDataKHR){(size_t)ds, buf + at}, at += (size_t)ds;
    }
#undef PB_TAKE
    if (ok) {
        VkPipelineBinaryKeysAndDataKHR kd = {.binaryCount = n, .pPipelineBinaryKeys = keys, .pPipelineBinaryData = data};
        VkPipelineBinaryCreateInfoKHR ci = {.sType = VK_STRUCTURE_TYPE_PIPELINE_BINARY_CREATE_INFO_KHR, .pKeysAndDataInfo = &kd};
        VkPipelineBinaryHandlesInfoKHR h = {.sType = VK_STRUCTURE_TYPE_PIPELINE_BINARY_HANDLES_INFO_KHR,
                                            .pipelineBinaryCount = n, .pPipelineBinaries = bins};
        ok = pb.create(g_vk.dev, &ci, NULL, &h) == VK_SUCCESS;
        *nbins = n;
    }
    free(buf);
    if (!ok) remove(path);  // (damaged, or refused by the driver: captured again)
    return ok;
}

// A pipeline created with CAPTURE_DATA: its binaries into the file for key k, then the captured data released
static void pb_capture(VkPipeline p, const VkPipelineBinaryKeyKHR *k) {
    VkPipelineBinaryCreateInfoKHR ci = {.sType = VK_STRUCTURE_TYPE_PIPELINE_BINARY_CREATE_INFO_KHR, .pipeline = p};
    VkPipelineBinaryHandlesInfoKHR h = {.sType = VK_STRUCTURE_TYPE_PIPELINE_BINARY_HANDLES_INFO_KHR};
    VkPipelineBinaryKHR bins[PBIN_MAX];
    uint32_t n = 0;
    bool ok = pb.create(g_vk.dev, &ci, NULL, &h) == VK_SUCCESS && h.pipelineBinaryCount && h.pipelineBinaryCount <= PBIN_MAX;
    if (ok) {
        h.pPipelineBinaries = bins;
        ok = pb.create(g_vk.dev, &ci, NULL, &h) == VK_SUCCESS;
        n = ok ? h.pipelineBinaryCount : 0;
    }
    size_t total = 12 + k->keySize, cap = 0;
    uint8_t *buf = NULL;
    for (uint32_t i = 0; ok && i < n; i++) {
        VkPipelineBinaryDataInfoKHR di = {.sType = VK_STRUCTURE_TYPE_PIPELINE_BINARY_DATA_INFO_KHR, .pipelineBinary = bins[i]};
        VkPipelineBinaryKeyKHR bk = {.sType = VK_STRUCTURE_TYPE_PIPELINE_BINARY_KEY_KHR};
        size_t ds = 0;
        ok = pb.get_data(g_vk.dev, &di, &bk, &ds, NULL) == VK_SUCCESS && ds;
        if (!ok) break;
        size_t need = total + 12 + bk.keySize + ds;
        if (need > cap) {
            cap = need * 2;
            uint8_t *nb = realloc(buf, cap);
            if (!nb) { ok = false; break; }
            buf = nb;
        }
        uint8_t *w = buf + total;
        uint64_t ds64 = ds;
        memcpy(w, &bk.keySize, 4);
        memcpy(w + 4, bk.key, bk.keySize);
        memcpy(w + 4 + bk.keySize, &ds64, 8);
        ok = pb.get_data(g_vk.dev, &di, &bk, &ds, w + 12 + bk.keySize) == VK_SUCCESS && ds == ds64;
        total = need;
    }
    if (ok && n && buf) {
        uint32_t magic = PBIN_MAGIC;
        memcpy(buf, &magic, 4);
        memcpy(buf + 4, &k->keySize, 4);
        memcpy(buf + 8, k->key, k->keySize);
        memcpy(buf + 8 + k->keySize, &n, 4);
        char path[600];
        pb_path(path, sizeof path, k);
        if (shader_cache_write(path, buf, total)) atomic_fetch_add(&pb_saved, 1);
    } else if (atomic_fetch_add(&pb_failed, 1) < 5) LOG("[vk] pipeline binaries: capture failed (%u binaries)\n", n);
    free(buf);
    for (uint32_t i = 0; i < n; i++) pb.destroy(g_vk.dev, bins[i], NULL);
    VkReleaseCapturedPipelineDataInfoKHR ri = {.sType = VK_STRUCTURE_TYPE_RELEASE_CAPTURED_PIPELINE_DATA_INFO_KHR, .pipeline = p};
    pb.release(g_vk.dev, &ri, NULL);
}

static VkResult pb_make(const void *info, bool compute, VkPipeline *out) {
    AnyPipeCI ci;
    if (compute) ci.c = *(const VkComputePipelineCreateInfo *)info;
    else ci.g = *(const VkGraphicsPipelineCreateInfo *)info;
    const void **pn = compute ? &ci.c.pNext : &ci.g.pNext;
    VkPipelineCreateFlags flags = compute ? ci.c.flags : ci.g.flags;
    const void *next = *pn;
    VkPipelineCreateInfoKHR ki = {.sType = VK_STRUCTURE_TYPE_PIPELINE_CREATE_INFO_KHR, .pNext = (void *)info};
    VkPipelineBinaryKeyKHR k = {.sType = VK_STRUCTURE_TYPE_PIPELINE_BINARY_KEY_KHR};
    if (pb.get_key(g_vk.dev, &ki, &k) != VK_SUCCESS || !k.keySize) return pb_create_pipeline(&ci, compute, g_vk.pcache, out);
    VkPipelineBinaryKHR bins[PBIN_MAX];
    uint32_t n = 0;
    if (pb_load(&k, bins, &n)) {  // (a pipeline from binaries is created without a pipeline cache)
        VkPipelineBinaryInfoKHR bi = {.sType = VK_STRUCTURE_TYPE_PIPELINE_BINARY_INFO_KHR, .pNext = next,
                                      .binaryCount = n, .pPipelineBinaries = bins};
        *pn = &bi;
        VkResult r = pb_create_pipeline(&ci, compute, VK_NULL_HANDLE, out);
        for (uint32_t i = 0; i < n; i++) pb.destroy(g_vk.dev, bins[i], NULL);
        if (r == VK_SUCCESS) { atomic_fetch_add(&pb_loaded, 1); return r; }
        if (atomic_fetch_add(&pb_failed, 1) < 5) LOG("[vk] pipeline binaries: creating from stored ones failed (%d); compiling\n", r);
        char path[600];
        pb_path(path, sizeof path, &k);
        remove(path);
    }
    VkPipelineCreateFlags2CreateInfoKHR fl = {.sType = VK_STRUCTURE_TYPE_PIPELINE_CREATE_FLAGS_2_CREATE_INFO_KHR, .pNext = next,
                                              .flags = (VkPipelineCreateFlags2KHR)flags | VK_PIPELINE_CREATE_2_CAPTURE_DATA_BIT_KHR};
    *pn = &fl;
    VkResult r = pb_create_pipeline(&ci, compute, VK_NULL_HANDLE, out);
    if (r == VK_SUCCESS) pb_capture(*out, &k);
    else {  // (as before)
        *pn = next;
        r = pb_create_pipeline(&ci, compute, g_vk.pcache, out);
    }
    return r;
}

static void pb_log(void) {  // (vk_pipeline_cache_save: under its lock)
    static int last[3] = {-1, -1, -1};
    int l = atomic_load(&pb_loaded), s = atomic_load(&pb_saved), f = atomic_load(&pb_failed);
    if (!pb.on || (l == last[0] && s == last[1] && f == last[2])) return;
    last[0] = l, last[1] = s, last[2] = f;
    LOG("[vk] pipeline binaries: %d pipelines from stored binaries, %d compiled and stored, %d failed\n", l, s, f);
}
#endif

bool g_shader_stats;
__thread char g_stats_label[128];  // (the caller's: which shaders the pipeline is made of)
static void log_pipeline_stats(VkPipeline p) {
    PFN_vkGetPipelineExecutablePropertiesKHR props = (PFN_vkGetPipelineExecutablePropertiesKHR)vkGetDeviceProcAddr(g_vk.dev, "vkGetPipelineExecutablePropertiesKHR");
    PFN_vkGetPipelineExecutableStatisticsKHR stats = (PFN_vkGetPipelineExecutableStatisticsKHR)vkGetDeviceProcAddr(g_vk.dev, "vkGetPipelineExecutableStatisticsKHR");
    if (!props || !stats) return;
    VkPipelineInfoKHR pi = {.sType = VK_STRUCTURE_TYPE_PIPELINE_INFO_KHR, .pipeline = p};
    VkPipelineExecutablePropertiesKHR ep[8];
    uint32_t ne = 8;
    for (uint32_t i = 0; i < ne; i++) ep[i] = (VkPipelineExecutablePropertiesKHR){.sType = VK_STRUCTURE_TYPE_PIPELINE_EXECUTABLE_PROPERTIES_KHR};
    if (props(g_vk.dev, &pi, &ne, ep) < 0) return;
    for (uint32_t e = 0; e < ne; e++) {
        VkPipelineExecutableInfoKHR ei = {.sType = VK_STRUCTURE_TYPE_PIPELINE_EXECUTABLE_INFO_KHR, .pipeline = p, .executableIndex = e};
        VkPipelineExecutableStatisticKHR st[32];
        uint32_t ns = 32;
        for (uint32_t i = 0; i < ns; i++) st[i] = (VkPipelineExecutableStatisticKHR){.sType = VK_STRUCTURE_TYPE_PIPELINE_EXECUTABLE_STATISTIC_KHR};
        if (stats(g_vk.dev, &ei, &ns, st) < 0) continue;
        char line[1024];
        int l = snprintf(line, sizeof line, "[stats] %s | %s:", g_stats_label, ep[e].name);
        for (uint32_t i = 0; i < ns && l < (int)sizeof line - 48; i++) {
            switch (st[i].format) {
            case VK_PIPELINE_EXECUTABLE_STATISTIC_FORMAT_BOOL32_KHR: l += snprintf(line + l, sizeof line - l, " %s=%u", st[i].name, st[i].value.b32); break;
            case VK_PIPELINE_EXECUTABLE_STATISTIC_FORMAT_INT64_KHR: l += snprintf(line + l, sizeof line - l, " %s=%lld", st[i].name, (long long)st[i].value.i64); break;
            case VK_PIPELINE_EXECUTABLE_STATISTIC_FORMAT_UINT64_KHR: l += snprintf(line + l, sizeof line - l, " %s=%llu", st[i].name, (unsigned long long)st[i].value.u64); break;
            default: l += snprintf(line + l, sizeof line - l, " %s=%.3f", st[i].name, st[i].value.f64);
            }
        }
        LOG("%s\n", line);
    }
}

VkResult vk_create_graphics_pipeline(const VkGraphicsPipelineCreateInfo *ci, VkPipeline *out) {
    if (g_shader_stats) {
        VkGraphicsPipelineCreateInfo c = *ci;
        c.flags |= VK_PIPELINE_CREATE_CAPTURE_STATISTICS_BIT_KHR;
        VkResult r = vkCreateGraphicsPipelines(g_vk.dev, VK_NULL_HANDLE, 1, &c, NULL, out);
        if (r == VK_SUCCESS) log_pipeline_stats(*out);
        return r;
    }
#ifdef VK_KHR_pipeline_binary
    if (pb.on) return pb_make(ci, false, out);
#endif
    return vkCreateGraphicsPipelines(g_vk.dev, g_vk.pcache, 1, ci, NULL, out);
}

VkResult vk_create_compute_pipeline(const VkComputePipelineCreateInfo *ci, VkPipeline *out) {
    if (g_shader_stats) {
        VkComputePipelineCreateInfo c = *ci;
        c.flags |= VK_PIPELINE_CREATE_CAPTURE_STATISTICS_BIT_KHR;
        VkResult r = vkCreateComputePipelines(g_vk.dev, VK_NULL_HANDLE, 1, &c, NULL, out);
        if (r == VK_SUCCESS) log_pipeline_stats(*out);
        return r;
    }
#ifdef VK_KHR_pipeline_binary
    if (pb.on) return pb_make(ci, true, out);
#endif
    return vkCreateComputePipelines(g_vk.dev, g_vk.pcache, 1, ci, NULL, out);
}

static void create_device(void) {
    uint32_t ne = 0;
    vkEnumerateDeviceExtensionProperties(g_vk.phys, NULL, &ne, NULL);
    VkExtensionProperties *ep = calloc(ne, sizeof *ep);
    vkEnumerateDeviceExtensionProperties(g_vk.phys, NULL, &ne, ep);
    if (getenv("LARECOMP_VK_LIST_EXT")) {  // every extension the device offers, in the log, and what foveation it has
        for (uint32_t i = 0; i < ne; i++) LOG("[vk] device extension %s (v%u)\n", ep[i].extensionName, ep[i].specVersion);
        VkPhysicalDeviceFragmentShadingRateFeaturesKHR vf = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FRAGMENT_SHADING_RATE_FEATURES_KHR};
        VkPhysicalDeviceFragmentDensityMapFeaturesEXT df = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FRAGMENT_DENSITY_MAP_FEATURES_EXT, .pNext = &vf};
        VkPhysicalDeviceFeatures2 f2 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, .pNext = &df};
        vkGetPhysicalDeviceFeatures2(g_vk.phys, &f2);
        VkPhysicalDeviceFragmentShadingRatePropertiesKHR vp = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FRAGMENT_SHADING_RATE_PROPERTIES_KHR};
        VkPhysicalDeviceFragmentDensityMapPropertiesEXT dp = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FRAGMENT_DENSITY_MAP_PROPERTIES_EXT, .pNext = &vp};
        VkPhysicalDeviceProperties2 p2 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, .pNext = &dp};
        vkGetPhysicalDeviceProperties2(g_vk.phys, &p2);
        LOG("[vk] shading rate: pipeline %d primitive %d attachment %d, attachment texel %ux%u..%ux%u, non-trivial combiners %d\n",
            vf.pipelineFragmentShadingRate, vf.primitiveFragmentShadingRate, vf.attachmentFragmentShadingRate,
            vp.minFragmentShadingRateAttachmentTexelSize.width, vp.minFragmentShadingRateAttachmentTexelSize.height,
            vp.maxFragmentShadingRateAttachmentTexelSize.width, vp.maxFragmentShadingRateAttachmentTexelSize.height,
            vp.fragmentShadingRateNonTrivialCombinerOps);
        LOG("[vk] density map: %d dynamic %d non-subsampled images %d, texel %ux%u..%ux%u, invocations %d\n",
            df.fragmentDensityMap, df.fragmentDensityMapDynamic, df.fragmentDensityMapNonSubsampledImages,
            dp.minFragmentDensityTexelSize.width, dp.minFragmentDensityTexelSize.height,
            dp.maxFragmentDensityTexelSize.width, dp.maxFragmentDensityTexelSize.height, dp.fragmentDensityInvocations);
        uint32_t nr = 0;
        PFN_vkGetPhysicalDeviceFragmentShadingRatesKHR gr = (PFN_vkGetPhysicalDeviceFragmentShadingRatesKHR)vkGetInstanceProcAddr(g_vk.inst, "vkGetPhysicalDeviceFragmentShadingRatesKHR");
        VkPhysicalDeviceFragmentShadingRateKHR rates[16];
        for (int i = 0; i < 16; i++) rates[i] = (VkPhysicalDeviceFragmentShadingRateKHR){.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FRAGMENT_SHADING_RATE_KHR};
        if (gr) { nr = 16; gr(g_vk.phys, &nr, rates); }
        for (uint32_t i = 0; i < nr; i++) LOG("[vk] shading rate %ux%u (samples %#x)\n", rates[i].fragmentSize.width, rates[i].fragmentSize.height, rates[i].sampleCounts);
    }
    const char *ext[20];
    uint32_t n_ext = 0;
    ext[n_ext++] = VK_KHR_SWAPCHAIN_EXTENSION_NAME;
    g_vk.custom_border = has_ext(ep, ne, VK_EXT_CUSTOM_BORDER_COLOR_EXTENSION_NAME);
    if (g_vk.custom_border) ext[n_ext++] = VK_EXT_CUSTOM_BORDER_COLOR_EXTENSION_NAME;
    g_vk.divisor = has_ext(ep, ne, VK_EXT_VERTEX_ATTRIBUTE_DIVISOR_EXTENSION_NAME);
    if (g_vk.divisor) ext[n_ext++] = VK_EXT_VERTEX_ATTRIBUTE_DIVISOR_EXTENSION_NAME;
    g_vk.host_import = has_ext(ep, ne, VK_EXT_EXTERNAL_MEMORY_HOST_EXTENSION_NAME);  // guest heap as a GPU buffer (exec.c)
    // LARECOMP_VK_NO_HOST_IMPORT=1: not used even where the driver has it (Turnip for the Quest 3 has it on KGSL's
    // coherent memory - shown for 64 MB imports; the guest heap is GBs: the page cache path, proven there, first)
    if (g_vk.host_import && getenv("LARECOMP_VK_NO_HOST_IMPORT") && atoi(getenv("LARECOMP_VK_NO_HOST_IMPORT"))) g_vk.host_import = false;
    // LARECOMP_VK_HOST_IMPORT=0: not used - the GPU then reads copies of the game's memory, made with each submission
    // (the Quest's page cache, exec.c gp_*), as the frame ahead needs: read live, the game's next frame showed through
    // (tiles of light missing, Windows, 22/10)
    {
        const char *hi = getenv("LARECOMP_VK_HOST_IMPORT");
        if (hi && *hi == '0' && g_vk.host_import) g_vk.host_import = false, LOG("[vk] host memory import off (LARECOMP_VK_HOST_IMPORT=0): page cache\n");
    }
    if (g_vk.host_import) ext[n_ext++] = VK_EXT_EXTERNAL_MEMORY_HOST_EXTENSION_NAME;
    bool fault = has_ext(ep, ne, VK_EXT_DEVICE_FAULT_EXTENSION_NAME);  // device-lost diagnostics (vk_device_fault_report)
    if (fault) ext[n_ext++] = VK_EXT_DEVICE_FAULT_EXTENSION_NAME;
    g_fault_ext = fault;
    // the headset's foveation (exec.c vrs_*): the shading rate by attachment, under an OpenXR shell, asked for only
    // (LARECOMP_VRS=1): in the headset, worn, the 2x2 part covered 76 to 84% of the image and it looked like 500 x 500
    // pixels (Guillaume, 21/10) - where the image falls in the lenses isn't the flat capture's
    VkPhysicalDeviceFragmentShadingRateFeaturesKHR fsr = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FRAGMENT_SHADING_RATE_FEATURES_KHR};
    const char *ve = getenv("LARECOMP_VRS");
    if (g_vk_xr && ve && *ve && *ve != '0' && has_ext(ep, ne, VK_KHR_FRAGMENT_SHADING_RATE_EXTENSION_NAME)) {
        VkPhysicalDeviceFeatures2 q = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, .pNext = &fsr};
        vkGetPhysicalDeviceFeatures2(g_vk.phys, &q);
        g_vk.vrs = fsr.pipelineFragmentShadingRate && fsr.attachmentFragmentShadingRate;
        if (g_vk.vrs) ext[n_ext++] = VK_KHR_FRAGMENT_SHADING_RATE_EXTENSION_NAME;
    }
    fsr = (VkPhysicalDeviceFragmentShadingRateFeaturesKHR){.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FRAGMENT_SHADING_RATE_FEATURES_KHR,
                                                          .pipelineFragmentShadingRate = VK_TRUE, .attachmentFragmentShadingRate = VK_TRUE};
    bool budget = has_ext(ep, ne, VK_EXT_MEMORY_BUDGET_EXTENSION_NAME);  // (real per-heap budgets: vk_log_memory)
    if (budget) ext[n_ext++] = VK_EXT_MEMORY_BUDGET_EXTENSION_NAME;
    // pipeline-building features: logged below (issue #92); dynamic vertex input used, the library one only measured
    bool vids_ext = has_ext(ep, ne, VK_EXT_VERTEX_INPUT_DYNAMIC_STATE_EXTENSION_NAME);
#ifdef VK_EXT_graphics_pipeline_library
    bool gpl_ext = has_ext(ep, ne, VK_EXT_GRAPHICS_PIPELINE_LIBRARY_EXTENSION_NAME) &&
                   has_ext(ep, ne, VK_KHR_PIPELINE_LIBRARY_EXTENSION_NAME);
#endif
#ifdef VK_EXT_extended_dynamic_state3
    bool eds3_ext = has_ext(ep, ne, VK_EXT_EXTENDED_DYNAMIC_STATE_3_EXTENSION_NAME);
#endif
#ifdef VK_KHR_pipeline_binary
    bool pbin_ext = has_ext(ep, ne, VK_KHR_PIPELINE_BINARY_EXTENSION_NAME) && has_ext(ep, ne, VK_KHR_MAINTENANCE_5_EXTENSION_NAME);
#endif
#ifdef __linux__
    // the guest heap as driver memory mapped at HEAP_BASE (exec.c backend_guest_heap_prepare)
    g_vk.dmabuf = has_ext(ep, ne, VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME) &&
                  has_ext(ep, ne, VK_EXT_EXTERNAL_MEMORY_DMA_BUF_EXTENSION_NAME);
    if (g_vk.dmabuf) {
        ext[n_ext++] = VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME;
        ext[n_ext++] = VK_EXT_EXTERNAL_MEMORY_DMA_BUF_EXTENSION_NAME;
    }
    // ... mapped there by the driver itself (its own DRM file, as every vkMapMemory), not through the dma-buf (issue #92)
    // (older Vulkan headers, e.g. a distro's, lack it: then the dma-buf mapping only)
#ifdef VK_EXT_map_memory_placed
    bool placed_ext = has_ext(ep, ne, VK_EXT_MAP_MEMORY_PLACED_EXTENSION_NAME) && has_ext(ep, ne, VK_KHR_MAP_MEMORY_2_EXTENSION_NAME);
#endif
#endif
    // LARECOMP_VK_SHADER_STATS=1: each pipeline's statistics from the driver (registers, spills, cycles: tuning shaders)
    bool pexec_ext = getenv("LARECOMP_VK_SHADER_STATS") && atoi(getenv("LARECOMP_VK_SHADER_STATS")) &&
                     has_ext(ep, ne, VK_KHR_PIPELINE_EXECUTABLE_PROPERTIES_EXTENSION_NAME);
    free(ep);

    // features the backend relies on (all present on current desktop drivers)
    VkPhysicalDeviceVulkan12Features have12 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
    VkPhysicalDeviceFeatures2 have = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, .pNext = &have12};
#if defined(__linux__) && defined(VK_EXT_map_memory_placed)
    VkPhysicalDeviceMapMemoryPlacedFeaturesEXT have_placed = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAP_MEMORY_PLACED_FEATURES_EXT};
    if (placed_ext) have12.pNext = &have_placed;
#endif
    VkPhysicalDeviceVertexInputDynamicStateFeaturesEXT have_vids = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VERTEX_INPUT_DYNAMIC_STATE_FEATURES_EXT};
    if (vids_ext) have_vids.pNext = have.pNext, have.pNext = &have_vids;
#ifdef VK_EXT_graphics_pipeline_library
    VkPhysicalDeviceGraphicsPipelineLibraryFeaturesEXT have_gpl = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_GRAPHICS_PIPELINE_LIBRARY_FEATURES_EXT};
    if (gpl_ext) have_gpl.pNext = have.pNext, have.pNext = &have_gpl;
#endif
#ifdef VK_EXT_extended_dynamic_state3
    VkPhysicalDeviceExtendedDynamicState3FeaturesEXT have_eds3 = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTENDED_DYNAMIC_STATE_3_FEATURES_EXT};
    if (eds3_ext) have_eds3.pNext = have.pNext, have.pNext = &have_eds3;
#endif
#ifdef VK_KHR_pipeline_binary
    VkPhysicalDevicePipelineBinaryFeaturesKHR have_pbin = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PIPELINE_BINARY_FEATURES_KHR};
    VkPhysicalDeviceMaintenance5FeaturesKHR have_m5 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAINTENANCE_5_FEATURES_KHR};
    if (pbin_ext) have_pbin.pNext = &have_m5, have_m5.pNext = have.pNext, have.pNext = &have_pbin;
#endif
    vkGetPhysicalDeviceFeatures2(g_vk.phys, &have);
    {   // what the driver offers for building pipelines (one line, for #92 reports)
        VkPhysicalDeviceDriverProperties drv = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES};
        VkPhysicalDeviceProperties2 pr = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, .pNext = &drv};
        char gpl[96] = "no extension", eds3[160] = "no extension";
#ifdef VK_EXT_graphics_pipeline_library
        VkPhysicalDeviceGraphicsPipelineLibraryPropertiesEXT gplp = {
            .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_GRAPHICS_PIPELINE_LIBRARY_PROPERTIES_EXT};
        if (gpl_ext) gplp.pNext = pr.pNext, pr.pNext = &gplp;
#else
        snprintf(gpl, sizeof gpl, "not in this build's headers");
#endif
#ifndef VK_EXT_extended_dynamic_state3
        snprintf(eds3, sizeof eds3, "not in this build's headers");
#endif
#ifdef VK_KHR_pipeline_binary
        VkPhysicalDevicePipelineBinaryPropertiesKHR pbp = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PIPELINE_BINARY_PROPERTIES_KHR};
        if (pbin_ext) pbp.pNext = pr.pNext, pr.pNext = &pbp;
#endif
        vkGetPhysicalDeviceProperties2(g_vk.phys, &pr);
        // the heap as driver memory (exec.c guest_heap_dmabuf) by default: RADV, and PanVK (Mali-G610, Mesa 27.0-devel,
        // issue #483: an 81-minute run without faults, the Field 15 -> 21 fps, ~85% fewer uploads). PanVK's device name
        // is the GPU's ("Mali-G610 MC4"), hence the driver ID.
        g_vk.heap_dmabuf_default = strstr(g_vk.props.deviceName, "RADV") != NULL || drv.driverID == VK_DRIVER_ID_MESA_PANVK;
        // command buffers released before each begin (record.c begin_cb_run) by default on Arm's own driver: the implicit
        // reset of vkBeginCommandBuffer keeps the memory vkCmdBeginRendering allocates (64 KB blocks of host memory), so
        // the process grew by ~490 MB an hour, until it ran out of memory (libmali g29p1 on RK3588; PanVK and Turnip don't).
        const char *cbr = getenv("LARECOMP_VK_CB_RELEASE");
        g_vk.cb_release = cbr ? atoi(cbr) != 0 : drv.driverID == VK_DRIVER_ID_ARM_PROPRIETARY;
        if (g_vk.cb_release) LOG("[vk] command buffers release their memory at each begin%s\n", cbr ? " (LARECOMP_VK_CB_RELEASE)" : ": Arm's driver");
        // command buffers reset as soon as their submission completes (exec.c cb_early_reset) by default on PanVK. PanVK
        // gives a command buffer one shader scratch (TLS) allocation at vkEndCommandBuffer: the largest per-thread stack
        // of the shaders it used, rounded up to a power of two, x 2048 threads x the core ID range (19 on RK3588: 1 KB a
        // thread is 38 MB, 8 KB is 304 MB). From Mesa 25.3 (0e88dd575fc0) a reset hands it back to the command pool's
        // free list for the next command buffer to reuse; it is freed only by vkResetCommandPool(RELEASE_RESOURCES) or
        // the pool's destruction. Before 25.3 a reset freed it. Either way, every command buffer between its
        // vkEndCommandBuffer and its reset holds its own: kept until the slot's next use, 24 submissions later, there are
        // as many of them as slots whose last submission used a stack-heavy shader. Reset early, only the submissions
        // still in flight hold one.
        {
            const char *cer = getenv("LARECOMP_VK_CB_EARLY_RESET");
            g_vk.cb_early_reset = cer && *cer ? atoi(cer) != 0 : drv.driverID == VK_DRIVER_ID_MESA_PANVK;
            if (cer && *cer)
                LOG("[vk] command buffers reset once their submission completes: %s (LARECOMP_VK_CB_EARLY_RESET)\n",
                    g_vk.cb_early_reset ? "on" : "off");
            else if (g_vk.cb_early_reset)
                LOG("[vk] command buffers reset once their submission completes: PanVK keeps a command buffer's shader "
                    "scratch memory until its reset\n");
        }
#ifdef VK_EXT_graphics_pipeline_library
        if (gpl_ext)
            snprintf(gpl, sizeof gpl, "%s (fast linking %s, independent interpolation decoration %s)",
                     have_gpl.graphicsPipelineLibrary ? "yes" : "no", gplp.graphicsPipelineLibraryFastLinking ? "yes" : "no",
                     gplp.graphicsPipelineLibraryIndependentInterpolationDecoration ? "yes" : "no");
#endif
#ifdef VK_EXT_extended_dynamic_state3
        if (eds3_ext)
            snprintf(eds3, sizeof eds3, "polygon mode %d, samples %d, alpha to coverage %d, blend enable %d, "
                     "blend equation %d, write mask %d", have_eds3.extendedDynamicState3PolygonMode,
                     have_eds3.extendedDynamicState3RasterizationSamples, have_eds3.extendedDynamicState3AlphaToCoverageEnable,
                     have_eds3.extendedDynamicState3ColorBlendEnable, have_eds3.extendedDynamicState3ColorBlendEquation,
                     have_eds3.extendedDynamicState3ColorWriteMask);
#endif
        LOG("[vk] driver %s %s; graphics pipeline library: %s; vertex input dynamic state: %s; extended dynamic state 3: %s\n",
            drv.driverName, drv.driverInfo, gpl, vids_ext ? (have_vids.vertexInputDynamicState ? "yes" : "no") : "no extension",
            eds3);
        // The Android emulator (gfxstream to MoltenVK) lists ASTC but textures sampled that way come out empty: the HUD's
        // hearts and the file select's icons were missing (API 35 image on an M3 Max). LARECOMP_VK_CPU_ASTC=1 / =0 overrides.
        const char *ca = getenv("LARECOMP_VK_CPU_ASTC");
        g_vk.cpu_astc = ca ? atoi(ca) != 0 : strstr(drv.driverName, "MoltenVK") != NULL;
        LOG("[vk] ASTC %s (%s)\n", g_vk.cpu_astc ? "decoded on the CPU" : "as the driver lists it", ca ? "LARECOMP_VK_CPU_ASTC" : "default for this GPU");
#ifdef VK_KHR_pipeline_binary
        // our own pipeline binaries (pb_make, #509): by default on AMD's drivers only, whose own cache kept nothing
        // between launches; LARECOMP_VK_PIPELINE_BINARY=0 / =1 turns them off / on
        const char *e = getenv("LARECOMP_VK_PIPELINE_BINARY");
        bool amd = drv.driverID == VK_DRIVER_ID_AMD_PROPRIETARY || drv.driverID == VK_DRIVER_ID_AMD_OPEN_SOURCE;
        bool can = pbin_ext && have_pbin.pipelineBinaries && have_m5.maintenance5;
        pb.on = can && (e && *e ? atoi(e) != 0 : amd);
        if (pbin_ext)
            LOG("[vk] pipeline binaries: feature %s, internal cache %d (control %d, prefers it %d, precompiled %d), "
                "compressed %d -> %s\n", have_pbin.pipelineBinaries ? "yes" : "no", pbp.pipelineBinaryInternalCache,
                pbp.pipelineBinaryInternalCacheControl, pbp.pipelineBinaryPrefersInternalCache,
                pbp.pipelineBinaryPrecompiledInternalCache, pbp.pipelineBinaryCompressedData,
                pb.on ? "stored by us (cache/vk_binaries)" : !can ? "pipeline cache (not supported)" :
                e && *e ? "pipeline cache (LARECOMP_VK_PIPELINE_BINARY=0)" : "pipeline cache (not an AMD driver)");
        else if (e && atoi(e)) LOG("[vk] pipeline binaries: no VK_KHR_pipeline_binary; pipeline cache\n");
        if (pb.on) {
            ext[n_ext++] = VK_KHR_MAINTENANCE_5_EXTENSION_NAME;
            ext[n_ext++] = VK_KHR_PIPELINE_BINARY_EXTENSION_NAME;
        }
#endif
    }
    {   // dynamic vertex input (exec.c): fewer pipelines; LARECOMP_VK_DYN_VERTEX=0 keeps the layout in each pipeline
        const char *e = getenv("LARECOMP_VK_DYN_VERTEX");
        bool off = e && *e && !atoi(e);
        // Intel's frozen Windows driver for its older integrated GPUs (UHD 630, Ice Lake...: Vulkan 1.3.215 and older)
        // draws nothing with it - a black picture (#8, #357); LARECOMP_VK_DYN_VERTEX=1 still turns it on there
        bool intel_legacy = false;
#ifdef _WIN32
        intel_legacy = g_vk.props.vendorID == 0x8086 && g_vk.props.apiVersion <= VK_MAKE_API_VERSION(0, 1, 3, 215);
#endif
        if (intel_legacy && !(e && *e)) off = true;
        g_vk.vids = vids_ext && have_vids.vertexInputDynamicState && !off;
        if (g_vk.vids) ext[n_ext++] = VK_EXT_VERTEX_INPUT_DYNAMIC_STATE_EXTENSION_NAME;
        else LOG("[vk] vertex input: in each pipeline (%s)\n",
                 intel_legacy && !(e && *e) ? "Intel's older Windows driver: drew nothing with it" :
                 off ? "LARECOMP_VK_DYN_VERTEX=0" :
                 !vids_ext ? "no VK_EXT_vertex_input_dynamic_state" : "vertexInputDynamicState not supported");
    }
#ifdef VK_EXT_graphics_pipeline_library
    {   // LARECOMP_VK_GPL_PROBE=1: library builds timed after shader preparation (exec.c gpl_probe); nothing else uses it
        const char *e = getenv("LARECOMP_VK_GPL_PROBE");
        if (e && atoi(e)) {
            g_vk.gpl_probe = gpl_ext && have_gpl.graphicsPipelineLibrary;
            if (g_vk.gpl_probe) {
                ext[n_ext++] = VK_KHR_PIPELINE_LIBRARY_EXTENSION_NAME;
                ext[n_ext++] = VK_EXT_GRAPHICS_PIPELINE_LIBRARY_EXTENSION_NAME;
            } else LOG("[gpl] probe: the driver has no graphics pipeline library; skipped\n");
        }
    }
#else
    if (getenv("LARECOMP_VK_GPL_PROBE") && atoi(getenv("LARECOMP_VK_GPL_PROBE"))) LOG("[gpl] probe: this build's Vulkan headers have no graphics pipeline library; skipped\n");
#endif
#if defined(__linux__) && defined(VK_EXT_map_memory_placed)
    have12.pNext = NULL;
    g_vk.map_placed = placed_ext && have_placed.memoryMapPlaced;
    g_vk.unmap_reserve = g_vk.map_placed && have_placed.memoryUnmapReserve;
    if (g_vk.map_placed) {
        ext[n_ext++] = VK_KHR_MAP_MEMORY_2_EXTENSION_NAME;
        ext[n_ext++] = VK_EXT_MAP_MEMORY_PLACED_EXTENSION_NAME;
    }
#endif
    bool probe;
    g_vk.lazy_heap = lazy_heap_predicted(budget, &probe);
    g_vk.heap_probe = !g_vk.lazy_heap && probe && g_vk.host_import && have12.bufferDeviceAddress;
    g_vk.bda = (g_vk.lazy_heap || g_vk.heap_probe) && have12.bufferDeviceAddress;
    VkPhysicalDeviceVertexAttributeDivisorFeaturesEXT div = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VERTEX_ATTRIBUTE_DIVISOR_FEATURES_EXT,
        .vertexAttributeInstanceRateDivisor = VK_TRUE, .vertexAttributeInstanceRateZeroDivisor = VK_FALSE};
    VkPhysicalDeviceCustomBorderColorFeaturesEXT cbc = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_CUSTOM_BORDER_COLOR_FEATURES_EXT, .customBorderColors = VK_TRUE,
        .customBorderColorWithoutFormat = VK_TRUE, .pNext = g_vk.divisor ? &div : NULL};
    VkPhysicalDeviceVulkan13Features f13 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES,
                                            .dynamicRendering = VK_TRUE, .synchronization2 = VK_TRUE,
                                            .shaderDemoteToHelperInvocation = VK_TRUE, .shaderTerminateInvocation = VK_TRUE,
                                            .maintenance4 = VK_TRUE,
                                            .pNext = g_vk.custom_border ? (void *)&cbc : g_vk.divisor ? (void *)&div : NULL};
    VkPhysicalDeviceVulkan12Features f12 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES,
                                            // (as supported: Turnip on an Adreno 643 has no 8-bit storage, and asking for a
                                            // missing feature fails the whole device; the translated shaders don't use it)
                                            .timelineSemaphore = VK_TRUE, .shaderInt8 = have12.shaderInt8,
                                            .samplerMirrorClampToEdge = have12.samplerMirrorClampToEdge,
                                            .uniformAndStorageBuffer8BitAccess = have12.uniformAndStorageBuffer8BitAccess,
                                            .storageBuffer8BitAccess = have12.storageBuffer8BitAccess,
                                            .scalarBlockLayout = have12.scalarBlockLayout, .bufferDeviceAddress = g_vk.bda, .pNext = &f13};
    VkPhysicalDeviceVulkan11Features f11 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES,
                                            .storageBuffer16BitAccess = VK_TRUE, .pNext = &f12};
    VkPhysicalDeviceFaultFeaturesEXT ff = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FAULT_FEATURES_EXT, .deviceFault = VK_TRUE,
                                           .pNext = &f11};
    VkPhysicalDeviceFeatures2 f = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, .pNext = fault ? (void *)&ff : (void *)&f11};
    if (g_vk.vrs) fsr.pNext = f.pNext, f.pNext = &fsr;
#if defined(__linux__) && defined(VK_EXT_map_memory_placed)
    VkPhysicalDeviceMapMemoryPlacedFeaturesEXT placed = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAP_MEMORY_PLACED_FEATURES_EXT,
                                                         .memoryMapPlaced = VK_TRUE, .memoryUnmapReserve = g_vk.unmap_reserve,
                                                         .pNext = f.pNext};
    if (g_vk.map_placed) f.pNext = &placed;
#endif
    VkPhysicalDeviceVertexInputDynamicStateFeaturesEXT en_vids = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VERTEX_INPUT_DYNAMIC_STATE_FEATURES_EXT, .vertexInputDynamicState = VK_TRUE};
    if (g_vk.vids) en_vids.pNext = f.pNext, f.pNext = &en_vids;
#ifdef VK_KHR_pipeline_binary
    VkPhysicalDeviceMaintenance5FeaturesKHR en_m5 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAINTENANCE_5_FEATURES_KHR,
                                                     .maintenance5 = VK_TRUE};
    VkPhysicalDevicePipelineBinaryFeaturesKHR en_pbin = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PIPELINE_BINARY_FEATURES_KHR,
                                                         .pipelineBinaries = VK_TRUE, .pNext = &en_m5};
    if (pb.on) en_m5.pNext = f.pNext, f.pNext = &en_pbin;
#endif
#ifdef VK_EXT_graphics_pipeline_library
    VkPhysicalDeviceGraphicsPipelineLibraryFeaturesEXT en_gpl = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_GRAPHICS_PIPELINE_LIBRARY_FEATURES_EXT, .graphicsPipelineLibrary = VK_TRUE};
    if (g_vk.gpl_probe) en_gpl.pNext = f.pNext, f.pNext = &en_gpl;
#endif
    VkPhysicalDeviceFeatures *b = &f.features, *h = &have.features;
    b->independentBlend = h->independentBlend;
    b->dualSrcBlend = h->dualSrcBlend;
    b->fillModeNonSolid = h->fillModeNonSolid;
    b->samplerAnisotropy = h->samplerAnisotropy;
    b->textureCompressionBC = h->textureCompressionBC;
#if PLAT_TOUCH
    b->textureCompressionASTC_LDR = h->textureCompressionASTC_LDR;  // (phones: the game's UI icons are ASTC, sampled natively)
#endif
    b->shaderInt64 = h->shaderInt64;
    b->depthBiasClamp = h->depthBiasClamp;
    b->depthClamp = h->depthClamp;
    b->largePoints = h->largePoints;
    b->wideLines = h->wideLines;
    b->imageCubeArray = h->imageCubeArray;
    b->sampleRateShading = h->sampleRateShading;
    b->fragmentStoresAndAtomics = h->fragmentStoresAndAtomics;
    b->vertexPipelineStoresAndAtomics = h->vertexPipelineStoresAndAtomics;
    g_vk.vs_atomics = h->vertexPipelineStoresAndAtomics;
    g_vk.fs_atomics = h->fragmentStoresAndAtomics;
    b->robustBufferAccess = h->robustBufferAccess;  // guest shaders index cbufs freely: keep OOB reads harmless
    if (getenv("LARECOMP_VK_NO_ROBUST")) b->robustBufferAccess = VK_FALSE;  // (test: what the bounds checks cost)
    g_vk.ssbo_chunks = h->fragmentStoresAndAtomics && h->vertexPipelineStoresAndAtomics && h->robustBufferAccess;
    b->shaderClipDistance = h->shaderClipDistance;
    if (!h->shaderInt64) LOG("[vk] warning: no shaderInt64 (64-bit address math in shaders)\n");
    VkPhysicalDevicePipelineExecutablePropertiesFeaturesKHR en_pexec = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PIPELINE_EXECUTABLE_PROPERTIES_FEATURES_KHR, .pipelineExecutableInfo = VK_TRUE};
    if (pexec_ext) {
        en_pexec.pNext = f.pNext, f.pNext = &en_pexec;
        ext[n_ext++] = VK_KHR_PIPELINE_EXECUTABLE_PROPERTIES_EXTENSION_NAME;
        g_shader_stats = true;
        LOG("[vk] shader statistics on (LARECOMP_VK_SHADER_STATS): one [stats] line per pipeline stage, pipelines not cached\n");
    }
    // A headset: OpenXR given a queue of its own (LARECOMP_VK_XR_QUEUE=0: the game's). The runtime puts its own work on
    // the queue it is given at each xrEndFrame / xrReleaseSwapchainImage; on the game's, it waited behind the game's
    // frame (25 ms of GPU) - the compositor took our frame as not ready, and xrWaitFrame dropped a refresh in three: the
    // frame loop at 89 a second, not 120 (21/10). Our images are finished before they are released (xr_app.c
    // release_thread waits for their submission): nothing of ours needs the runtime's work ordered after it.
    float prio[2] = {1.0f, 1.0f};
    const char *xq = getenv("LARECOMP_VK_XR_QUEUE");
    g_xr_qindex = g_vk_xr && g_qcount >= 2 && !(xq && *xq == '0') ? 1 : 0;
    VkDeviceQueueCreateInfo qi = {.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, .queueFamilyIndex = g_vk.qfam,
                                  .queueCount = 1 + g_xr_qindex, .pQueuePriorities = prio};
    if (g_vk_xr) LOG("[vk] queue family %u: %u queues; OpenXR on queue %u\n", g_vk.qfam, g_qcount, g_xr_qindex);
    VkDeviceCreateInfo di = {.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, .pNext = &f, .queueCreateInfoCount = 1,
                             .pQueueCreateInfos = &qi, .enabledExtensionCount = n_ext, .ppEnabledExtensionNames = ext};
    VK_CHECK(g_vk_xr ? g_vk_xr->create_device(g_vk.phys, &di, &g_vk.dev) : vkCreateDevice(g_vk.phys, &di, NULL, &g_vk.dev));
#ifdef __ANDROID__
    volkLoadDevice(g_vk.dev);
#endif
    vkGetDeviceQueue(g_vk.dev, g_vk.qfam, 0, &g_vk.queue);
    if (g_vk.vids) {
        g_vk.set_vertex_input = (PFN_vkCmdSetVertexInputEXT)vkGetDeviceProcAddr(g_vk.dev, "vkCmdSetVertexInputEXT");
        if (!g_vk.set_vertex_input) g_vk.vids = false;
        LOG("[vk] vertex input: %s\n", g_vk.vids ? "dynamic (VK_EXT_vertex_input_dynamic_state)" :
            "in each pipeline (vkCmdSetVertexInputEXT missing)");
    }
    g_queue_mutex = SDL_CreateMutex();
    if (g_vk_xr) g_vk_xr->device_ready(g_vk.inst, g_vk.phys, g_vk.dev, g_vk.qfam);

    VmaAllocatorCreateInfo ai = {.physicalDevice = g_vk.phys, .device = g_vk.dev, .instance = g_vk.inst,
                                 .vulkanApiVersion = VK_API_VERSION_1_3,
                                 .flags = budget ? VMA_ALLOCATOR_CREATE_EXT_MEMORY_BUDGET_BIT : 0};
#ifdef __ANDROID__
    VmaVulkanFunctions vf = {.vkGetInstanceProcAddr = vkGetInstanceProcAddr, .vkGetDeviceProcAddr = vkGetDeviceProcAddr};
    ai.pVulkanFunctions = &vf;  // (VMA_DYNAMIC_VULKAN_FUNCTIONS: from what volk loaded)
#endif
    VK_CHECK(vmaCreateAllocator(&ai, &g_vk.vma));
    vk_log_memory("start");

    VkSemaphoreTypeCreateInfo st = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO,
                                    .semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE, .initialValue = 0};
    VkSemaphoreCreateInfo si = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO, .pNext = &st};
    VK_CHECK(vkCreateSemaphore(g_vk.dev, &si, NULL, &g_vk.timeline));
    VkCommandPoolCreateInfo pi = {.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
                                  .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT, .queueFamilyIndex = g_vk.qfam};
    VK_CHECK(vkCreateCommandPool(g_vk.dev, &pi, NULL, &g_vk.pool));

    // pipeline cache: cache/vk_pipelines.bin (written by vk_pipeline_cache_save), used only with its tag
    // (vk_pipelines.bin.tag: this build, the translator, the GPU and its driver - another one starts it empty)
    char tag[512];
    pipeline_cache_tag(tag, sizeof tag);
    size_t sz = 0;
    void *data = NULL;
    // LARECOMP_VK_PIPELINE_CACHE=0 (test): start with an empty one, saved over the old as usual. The Android emulator's
    // gfxstream hangs in vkCreatePipelineCache given a large one (~300 MB after a session).
    const char *pce = getenv("LARECOMP_VK_PIPELINE_CACHE");
    bool pc_load = !(pce && !atoi(pce));
    FILE *fp = pc_load && shader_cache_tag_ok(PCACHE_TAG, tag) ? fopen(PCACHE_FILE, "rb") : NULL;
    if (!pc_load) LOG("[vk] pipeline cache: starting empty (LARECOMP_VK_PIPELINE_CACHE=0)\n");
    else if (!fp && !access(PCACHE_FILE, F_OK)) LOG("[vk] pipeline cache from another build / GPU / driver: starting a new one\n");
    if (fp) {
        fseek(fp, 0, SEEK_END);
        sz = (size_t)ftell(fp);
        fseek(fp, 0, SEEK_SET);
        data = malloc(sz);
        if (fread(data, 1, sz, fp) != sz) sz = 0;
        fclose(fp);
    }
    VkPipelineCacheCreateInfo pc = {.sType = VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO, .initialDataSize = sz,
                                    .pInitialData = data};
    if (vkCreatePipelineCache(g_vk.dev, &pc, NULL, &g_vk.pcache) != VK_SUCCESS) {
        pc.initialDataSize = 0;
        VK_CHECK(vkCreatePipelineCache(g_vk.dev, &pc, NULL, &g_vk.pcache));
    }
    free(data);
    if (pc.initialDataSize) LOG("[vk] pipeline cache: loaded %zu bytes\n", pc.initialDataSize);
    pcache_saved = pc.initialDataSize;
#ifdef VK_KHR_pipeline_binary
    if (pb.on) pb_setup();
#endif
    pthread_t t;  // (kept every few minutes while it grows, not only at a clean exit)
    pthread_create(&t, NULL, pipeline_cache_saver, NULL);
    pthread_detach(t);
}

// The pipeline cache written while the game runs, a few seconds after new pipelines were made, on a thread of its own:
// written only at exit before, which never comes on the Quest (the app is stopped, not quit) - every start compiled
// everything again, and the island stayed black, then untextured, for half a minute (Guillaume, 21/10).
static _Atomic bool g_pc_saving;
static void *pc_save_thread(void *a) {
    (void)a;
    void vk_pipeline_cache_save(void);
    vk_pipeline_cache_save();
    atomic_store(&g_pc_saving, false);
    return NULL;
}
static void pc_save_now_and_then(void) {
    extern uint64_t vk_pipelines_made(void);
    static uint64_t saved, seen, since;
    uint64_t made = vk_pipelines_made(), now = SDL_GetTicksNS();
    if (made != seen) seen = made, since = now;  // (wait for a lull: 3 s without new ones)
    if (made == saved || now - since < 3000000000ull || atomic_exchange(&g_pc_saving, true)) return;
    saved = made;
    pthread_t t;
    if (pthread_create(&t, NULL, pc_save_thread, NULL) == 0) pthread_detach(t);
    else atomic_store(&g_pc_saving, false);
}

// Saved atomically (temp file + rename: a crash mid-write leaves the previous one), with its tag; one save at a time
void vk_pipeline_cache_save(void) {
    static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
    pthread_mutex_lock(&lock);
    size_t sz = 0;
    if (vkGetPipelineCacheData(g_vk.dev, g_vk.pcache, &sz, NULL) == VK_SUCCESS && sz && sz != pcache_saved) {
        void *data = malloc(sz);
        if (vkGetPipelineCacheData(g_vk.dev, g_vk.pcache, &sz, data) == VK_SUCCESS && shader_cache_write(PCACHE_FILE, data, sz)) {
            char tag[512];
            pipeline_cache_tag(tag, sizeof tag);
            if (!shader_cache_tag_ok(PCACHE_TAG, tag)) shader_cache_tag_write(PCACHE_TAG, tag);
            pcache_saved = sz;
            LOG("[vk] pipeline cache: saved %zu bytes\n", sz);
        }
        free(data);
    }
#ifdef VK_KHR_pipeline_binary
    pb_log();
#endif
    pthread_mutex_unlock(&lock);
}

// ================================================================== swapchain
static bool sc_interp;  // (the swapchain was made for Frame Generation: FIFO; the menu's toggle makes a new one if the mode differs)
static VkPresentModeKHR sc_mode;  // (the swapchain's present mode)
static bool wp_enabled(void);
static VkPresentModeKHR present_mode(void) {
    // MAILBOX by default: the game paces itself (vi.c's emulated 60 Hz vsync), and with FIFO every submission
    // queued behind the one waiting for a window image, so the game's fence waits followed the presentation engine:
    // a throttled / captured / busy compositor slowed the whole game (4-21 fps with the GPU mostly idle; mailbox:
    // 60). No tearing. LARECOMP_VK_PRESENT=fifo|immediate|mailbox to choose.
    // Frame Generation (frame interpolation, backend_present_now) presents its in-betweens before the frame's image,
    // one display refresh apart: FIFO, where they queue (mailbox would replace them before they were shown).
    const char *e = getenv("LARECOMP_VK_PRESENT");
    sc_interp = interp_wanted() == INTERP_ON;
    if (!e && sc_interp) e = "fifo";
    if (!e && wp_enabled()) e = "fifo";  // (the presenter at the display's rate: one image per refresh, its pace)
    VkPresentModeKHR want = !e || !strcmp(e, "mailbox") ? VK_PRESENT_MODE_MAILBOX_KHR
                          : !strcmp(e, "immediate") ? VK_PRESENT_MODE_IMMEDIATE_KHR : VK_PRESENT_MODE_FIFO_KHR;
    uint32_t n = 0;
    vkGetPhysicalDeviceSurfacePresentModesKHR(g_vk.phys, g_vk.surface, &n, NULL);
    VkPresentModeKHR m[16];
    if (n > 16) n = 16;
    vkGetPhysicalDeviceSurfacePresentModesKHR(g_vk.phys, g_vk.surface, &n, m);
    for (uint32_t i = 0; i < n; i++) if (m[i] == want) return want;
    return VK_PRESENT_MODE_FIFO_KHR;
}

// Frame Generation's toggle needs a new swapchain only when the mode really changes. Where the driver offers no mailbox
// (AMD on Windows) both settings are FIFO, and remaking the same swapchain left 60 fps stuttering on a 144 Hz display
// until a restart (#602; 60 Hz was fine). present_mode updates sc_interp: asked once per toggle.
static bool sc_mode_changed(void) {
    return sc_interp != (interp_wanted() == INTERP_ON) && present_mode() != sc_mode;
}

#ifdef __ANDROID__
static bool recreate_surface(void);
#endif
static void create_offscreen(void) {  // two images in place of the swapchain's (LARECOMP_OFFSCREEN)
    g_vk.sc_format = VK_FORMAT_R8G8B8A8_UNORM;
    g_vk.sc_extent = (VkExtent2D){(uint32_t)g_off_w, (uint32_t)g_off_h};
    g_vk.sc_phys = g_vk.sc_extent, g_vk.sc_quarter = 0;  // (beta 6 draws the window image over sc_phys: 0 x 0, the headset saw black)
    g_vk.sc_count = 2;
    for (uint32_t i = 0; i < 2; i++) {
        VkImageCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D, .format = g_vk.sc_format,
                                .extent = {g_vk.sc_extent.width, g_vk.sc_extent.height, 1}, .mipLevels = 1, .arrayLayers = 1,
                                .samples = VK_SAMPLE_COUNT_1_BIT, .tiling = VK_IMAGE_TILING_OPTIMAL,
                                .usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                                         VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT};
        VmaAllocationCreateInfo ai = {.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE};
        VmaAllocation mem;
        VK_CHECK(vmaCreateImage(g_vk.vma, &ci, &ai, &g_vk.sc_images[i], &mem, NULL));
        VkImageViewCreateInfo vi = {.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = g_vk.sc_images[i],
                                    .viewType = VK_IMAGE_VIEW_TYPE_2D, .format = g_vk.sc_format,
                                    .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
        VK_CHECK(vkCreateImageView(g_vk.dev, &vi, NULL, &g_vk.sc_views[i]));
    }
    // an OpenXR shell's swapchain images, drawn into directly (vk_xr.h images / acquire): the copy into them was 0.54 ms
    // of the Quest's GPU per frame at 1408x1216 (measured 21/10). LARECOMP_XR_DIRECT=0: the copy.
    const char *e = getenv("LARECOMP_XR_DIRECT");
    uint32_t n = g_vk_xr && g_vk_xr->images && g_vk_xr->acquire && !(e && *e == '0') ? g_vk_xr->images(g_vk.sc_images + 2, 10) : 0;
    for (uint32_t i = 2; i < 2 + n; i++) {
        VkImageViewCreateInfo vi = {.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = g_vk.sc_images[i],
                                    .viewType = VK_IMAGE_VIEW_TYPE_2D, .format = g_vk.sc_format,
                                    .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
        VK_CHECK(vkCreateImageView(g_vk.dev, &vi, NULL, &g_vk.sc_views[i]));
    }
    g_vk.sc_count = 2 + n;
    LOG("[vk] offscreen %ux%u (no window)%s\n", g_vk.sc_extent.width, g_vk.sc_extent.height,
        n ? ", the final pass drawn into the headset's swapchain" : "");
}

// (a shell's window gone: the swapchain and its surface let go, all GPU work done; made again on the next window)
static void shell_window_drop(void) {
    vkDeviceWaitIdle(g_vk.dev);
    for (uint32_t i = 0; i < g_vk.sc_count; i++) vkDestroyImageView(g_vk.dev, g_vk.sc_views[i], NULL);
    g_vk.sc_count = 0;
    if (g_vk.swapchain) vkDestroySwapchainKHR(g_vk.dev, g_vk.swapchain, NULL);
    if (g_vk.surface) vkDestroySurfaceKHR(g_vk.inst, g_vk.surface, NULL);
    g_vk.swapchain = VK_NULL_HANDLE, g_vk.surface = VK_NULL_HANDLE;
    LOG("[vk] the shell's window is gone: swapchain dropped\n");
}

static void create_swapchain(void) {
    VkSurfaceCapabilitiesKHR caps;
#ifdef __ANDROID__
    {   // (the window can go while the swapchain is made - the app sent to the background at start: made again, or
        // no swapchain until the window is back)
        VkResult r = g_vk.surface ? vkGetPhysicalDeviceSurfaceCapabilitiesKHR(g_vk.phys, g_vk.surface, &caps)
                                  : VK_ERROR_SURFACE_LOST_KHR;
        if (r == VK_ERROR_SURFACE_LOST_KHR) {
            if (!recreate_surface()) return;
            r = vkGetPhysicalDeviceSurfaceCapabilitiesKHR(g_vk.phys, g_vk.surface, &caps);
        }
        if (r == VK_ERROR_SURFACE_LOST_KHR) return;
        VK_CHECK(r);
    }
#else
    VK_CHECK(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(g_vk.phys, g_vk.surface, &caps));
#endif
    int w, h;
    SDL_GetWindowSizeInPixels(window, &w, &h);
#ifdef __ANDROID__
    // The surface reports the panel's own (portrait) orientation; the activity is landscape. Pre-rotation: the images
    // in the panel's orientation with its transform, everything drawn into them turned (sc_quarter: vk_sc_rect /
    // vk_sc_viewport, and the present, ImGui and menu piece vertex shaders). Otherwise the images are the window's
    // size and SurfaceFlinger turns them with the GPU at every present: on the Retroid Pocket 5 (Adreno 650) the
    // game's layer was CLIENT composited, ROT_90, while the system bars went to the display hardware. Ported from
    // Nycolazs's LASRecomp-Android-Plus (MIT), which measured ~13% of an S20 FE's GPU time for that composition.
    // LARECOMP_VK_PRETRANSFORM=0: the system turns them (the earlier way, from mamcjr's LASRecompAndroid, MIT).
    VkExtent2D ext = {(uint32_t)w, (uint32_t)h};
    VkSurfaceTransformFlagBitsKHR transform = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR;
    static int pre = -1;
    if (pre < 0) { const char *e = getenv("LARECOMP_VK_PRETRANSFORM"); pre = !e || atoi(e) != 0; }
    g_vk.sc_quarter = 0;
    if (pre && (caps.currentTransform == VK_SURFACE_TRANSFORM_ROTATE_90_BIT_KHR ||
                caps.currentTransform == VK_SURFACE_TRANSFORM_ROTATE_270_BIT_KHR) &&
        (caps.supportedTransforms & caps.currentTransform)) {
        transform = caps.currentTransform;
        g_vk.sc_quarter = transform == VK_SURFACE_TRANSFORM_ROTATE_90_BIT_KHR ? 1 : 3;
    }
    LOG("[vk] swapchain: surface %ux%u transform %#x, window %dx%d%s\n", caps.currentExtent.width, caps.currentExtent.height,
        caps.currentTransform, w, h, g_vk.sc_quarter ? ", pre-rotated" : pre ? "" : " (LARECOMP_VK_PRETRANSFORM=0)");
#else
    VkExtent2D ext = caps.currentExtent.width != UINT32_MAX ? caps.currentExtent : (VkExtent2D){(uint32_t)w, (uint32_t)h};
    VkSurfaceTransformFlagBitsKHR transform = caps.currentTransform;
#endif
    if (!ext.width || !ext.height) return;  // minimised
    VkExtent2D phys = g_vk.sc_quarter ? (VkExtent2D){ext.height, ext.width} : ext;
    uint32_t nf = 0;
    vkGetPhysicalDeviceSurfaceFormatsKHR(g_vk.phys, g_vk.surface, &nf, NULL);
    VkSurfaceFormatKHR fmts[64];
    if (nf > 64) nf = 64;
    vkGetPhysicalDeviceSurfaceFormatsKHR(g_vk.phys, g_vk.surface, &nf, fmts);
    // scanout shows the bytes the game wrote (see the raw views in the present pass): a UNORM swapchain like Metal's
    VkSurfaceFormatKHR sf = fmts[0];
    for (uint32_t i = 0; i < nf; i++)
        if (fmts[i].format == VK_FORMAT_B8G8R8A8_UNORM && fmts[i].colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) { sf = fmts[i]; break; }
    uint32_t count = caps.minImageCount + 1;
    if (caps.maxImageCount && count > caps.maxImageCount) count = caps.maxImageCount;
    if (count > VK_SC_MAX) count = VK_SC_MAX;
#ifdef __ANDROID__
    if (count < caps.minImageCount)
        FATAL("Surface needs %u swapchain images; renderer supports at most %d.\n", caps.minImageCount, VK_SC_MAX);
#endif
    VkSwapchainKHR old = g_vk.swapchain;
    VkSwapchainCreateInfoKHR ci = {
        .sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR, .surface = g_vk.surface, .minImageCount = count,
        .imageFormat = sf.format, .imageColorSpace = sf.colorSpace, .imageExtent = phys, .imageArrayLayers = 1,
        .imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
        .imageSharingMode = VK_SHARING_MODE_EXCLUSIVE, .preTransform = transform,
        .compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR, .presentMode = present_mode(), .clipped = VK_TRUE,
        .oldSwapchain = old};
    sc_mode = ci.presentMode;
#ifdef __ANDROID__
    {   // (the window went away after its surface was made - sent to the background right then: no swapchain now, the
        // next frame makes the surface and swapchain again instead of stopping. From mamcjr's LASRecompAndroid, MIT,
        // after BOTWRecompAndroid.)
        VkResult r = vkCreateSwapchainKHR(g_vk.dev, &ci, NULL, &g_vk.swapchain);
        if (r == VK_ERROR_SURFACE_LOST_KHR || r == VK_ERROR_NATIVE_WINDOW_IN_USE_KHR) {
            LOG("[vk] window surface lost while making the swapchain (%d): made again at the next frame\n", r);
            for (uint32_t i = 0; i < g_vk.sc_count; i++) vkDestroyImageView(g_vk.dev, g_vk.sc_views[i], NULL);
            g_vk.sc_count = 0;
            if (old) vkDestroySwapchainKHR(g_vk.dev, old, NULL);
            g_vk.swapchain = VK_NULL_HANDLE;
            return;
        }
        VK_CHECK(r);
    }
#else
    VK_CHECK(vkCreateSwapchainKHR(g_vk.dev, &ci, NULL, &g_vk.swapchain));
#endif
    static const char *const modes[] = {"immediate", "mailbox", "fifo", "fifo-relaxed"};
    LOG("[vk] swapchain %ux%u, %u images, %s\n", phys.width, phys.height, count,
        ci.presentMode < 4 ? modes[ci.presentMode] : "other present mode");
    for (uint32_t i = 0; i < g_vk.sc_count; i++) vkDestroyImageView(g_vk.dev, g_vk.sc_views[i], NULL);
    if (old) vkDestroySwapchainKHR(g_vk.dev, old, NULL);
    g_vk.sc_format = sf.format;
    g_vk.sc_extent = ext;
    g_vk.sc_phys = phys;
#ifdef __ANDROID__
    VK_CHECK(vkGetSwapchainImagesKHR(g_vk.dev, g_vk.swapchain, &g_vk.sc_count, NULL));
    if (g_vk.sc_count > VK_SC_MAX)
        FATAL("Driver created %u swapchain images; renderer supports at most %d.\n", g_vk.sc_count, VK_SC_MAX);
    VK_CHECK(vkGetSwapchainImagesKHR(g_vk.dev, g_vk.swapchain, &g_vk.sc_count, g_vk.sc_images));
#else
    g_vk.sc_count = 8;
    vkGetSwapchainImagesKHR(g_vk.dev, g_vk.swapchain, &g_vk.sc_count, g_vk.sc_images);
#endif
    for (uint32_t i = 0; i < g_vk.sc_count; i++) {
        VkImageViewCreateInfo vi = {.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = g_vk.sc_images[i],
                                    .viewType = VK_IMAGE_VIEW_TYPE_2D, .format = sf.format,
                                    .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
        VK_CHECK(vkCreateImageView(g_vk.dev, &vi, NULL, &g_vk.sc_views[i]));
    }
}

#ifdef __ANDROID__
// Android destroys the window's surface whenever another activity covers the app (the folder picker, the home screen)
// and SDL makes a new one on return: the old VkSurfaceKHR is lost for good (VK_ERROR_SURFACE_LOST_KHR on every acquire,
// a black screen). So each swapchain rebuild there makes the surface again from SDL's current window first; the old
// swapchain belongs to the old window and can't be passed on. False: no window right now (in the background).
static bool recreate_surface(void) {
    for (uint32_t i = 0; i < g_vk.sc_count; i++) vkDestroyImageView(g_vk.dev, g_vk.sc_views[i], NULL);
    g_vk.sc_count = 0;
    if (g_vk.swapchain) vkDestroySwapchainKHR(g_vk.dev, g_vk.swapchain, NULL);
    g_vk.swapchain = VK_NULL_HANDLE;
    if (g_vk.surface) vkDestroySurfaceKHR(g_vk.inst, g_vk.surface, NULL);
    g_vk.surface = VK_NULL_HANDLE;
    if (!SDL_Vulkan_CreateSurface(window, g_vk.inst, NULL, &g_vk.surface)) {
        LOG("[vk] no window surface yet: %s\n", SDL_GetError());
        g_vk.surface = VK_NULL_HANDLE;
        return false;
    }
    LOG("[vk] window surface made again (the app was in the background)\n");
    return true;
}
#endif

static void recreate_swapchain(void) {
    rec_drain();  // (every queued present and submission made: the queue is idle for the wait)
    vkDeviceWaitIdle(g_vk.dev);
#ifdef __ANDROID__
    if (!recreate_surface()) return;
#endif
    create_swapchain();
}

// Time this thread spends waiting here - for a frame slot's GPU work, a window image, the presentation engine -
// counts as idle for the frame pacer (pace.c), like the game's own waits: in FIFO (exclusive fullscreen
// on Windows offers no mailbox) acquire / present block until the display's refresh, and counted as busy that made
// every frame look ~16.6 ms: the game's 30 fps mode outdoors on a fast GPU (issue #44). GPU-bound frames still show
// in the game's GPU counters.
extern __thread uint64_t g_blocked_ns;  // kernel.c
#define WAITING(x) do { uint64_t w0_ = plat_now_ns(); rq_watch(#x, 0); x; rq_watch(NULL, 0); g_blocked_ns += plat_now_ns() - w0_; } while (0)
// the same, also counted by place (dynamic resolution's log / LARECOMP_VK_PROF: 0 frame slot, 1 acquire, 2 present)
uint64_t g_vk_frame_waits[4];
#define WAITING_AT(i, x) do { uint64_t w0_ = plat_now_ns(); rq_watch(#x, 0); x; rq_watch(NULL, 0); uint64_t w1_ = plat_now_ns() - w0_; \
                              g_blocked_ns += w1_; g_vk_frame_waits[i] += w1_; } while (0)

// ================================================================== frames (start screen and game)
// Acquire semaphores rotate per frame in flight; a slot is reused once the submission that waited on it is done.
#define FRAMES 3
static VkSemaphore acquire_sem[FRAMES], done_sem[VK_SC_MAX];
static uint64_t slot_seq[FRAMES];
static int slot;

static VkSemaphore binary_semaphore(void) {
    VkSemaphoreCreateInfo si = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    VkSemaphore s;
    VK_CHECK(vkCreateSemaphore(g_vk.dev, &si, NULL, &s));
    return s;
}

static void image_barrier(VkCommandBuffer cb, VkImage img, VkImageLayout from, VkImageLayout to) {
    VkImageMemoryBarrier2 b = {.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
                               .srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, .srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT,
                               .dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                               .dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT,
                               .oldLayout = from, .newLayout = to, .image = img,
                               .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
                               .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED};
    VkDependencyInfo di = {.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &b};
    rec_CmdPipelineBarrier2(cb, &di);
}
// (the presenter thread and its recordings: their own command buffers, recorded at once - never through the recorder
//  thread, which records the render thread's alone: through it they landed on the render thread's ring, 12/11/2026)
static void image_barrier_now(VkCommandBuffer cb, VkImage img, VkImageLayout from, VkImageLayout to) {
    VkImageMemoryBarrier2 b = {.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
                               .srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, .srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT,
                               .dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                               .dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT,
                               .oldLayout = from, .newLayout = to, .image = img,
                               .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
                               .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED};
    VkDependencyInfo di = {.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &b};
    vkCmdPipelineBarrier2(cb, &di);
}

// ================================================================== first person at the display's rate (LARECOMP_FP_WARP)
// As runtime/nvn/metal.m's presenter on the Mac (21/10), for Linux and Windows (Guillaume's 165 Hz screen): each frame is
// composed into a slot image of the window's size instead of a window image, and a thread of its own presents one image
// per display refresh (FIFO), the newest slot turned from its frame's view to the view of that moment (firstperson.c
// fp_warp_frame_view, fp_warp_view_at, the Mac's): in first person the camera turns at the screen's rate while the game
// makes 60 images a second. The HUD turns with the image for now (the Mac's layer apart: to come). On by default with
// a window; LARECOMP_FP_WARP=0: the game's images presented as before. LARECOMP_FP_WARP_TEST_DEG: that much more turn.
#define WSLOTS 3
typedef struct {
    VkImage img;
    VmaAllocation mem;
    VkImageView view;
    VkDescriptorSet set;
    uint32_t w, h;
    uint64_t read_seq;                // the presenter's last submission that reads it (wp_tl)
    int turn;                         // its view may be turned (first person in play)
    float yaw, pitch, tx, ty;         // the view it was drawn with, the field's tangents
    uint64_t ns;                      // when that view was published (the game thread)
    float vr[4], crop[4];             // the game image's place in it and the part of the game's view it shows
    bool shown;
    VkImage img3d;                    // its 3D image without the HUD (exec.c vk_present_draw), when has3d
    VmaAllocation mem3d;
    VkImageView view3d;
    bool has3d;
    VkImage imgd;                     // its scene depth (exec.c vk_fp_scene_depth), when hasd: the image moved by it
    VmaAllocation memd;
    VkImageView viewd;
    VkFormat dfmt;
    uint32_t dimw, dimh;
    float dk[5];                      // the depth's part of its texture (x, y), the projection's [10] and [11], NVN's depth mode
    float eye[3];                     // the eye the view was drawn from (has_eye)
    float ovs;                        // how much wider than the screen its view was drawn (firstperson.c; 1: as the screen)
    bool hasd, has_eye;
} WSlot;
static WSlot ws[WSLOTS];
static int wp_cur = -1, wp_prev = -1;  // (render thread) the slot this frame is composed into, the last one
static _Atomic int wp_latest = -1;     // the newest finished slot
// (the window's size in pixels, from the game's frame: the presenter's swapchain follows it - Wayland says nothing when a
// window changes size, no OUT_OF_DATE: made at 1280x720 then made fullscreen, it stayed 1280x720 on a 2560x1440 screen,
// enlarged by the desktop: « je vois des pixels », Guillaume, 22/10)
static _Atomic uint32_t wp_win_w, wp_win_h;
static pthread_mutex_t wp_lock = PTHREAD_MUTEX_INITIALIZER;
static VkSemaphore wp_tl;              // the presenter's submissions, counted (timeline)
static uint64_t wp_tl_n;               // (under wp_lock)
static VkDescriptorSetLayout wp_dsl;
static VkDescriptorPool wp_dpool;
static VkPipelineLayout wp_layout;
static VkPipeline wp_pipe;
static VkFormat wp_pipe_fmt;
static _Atomic int64_t wp_delay;       // (ns) from a view's publication to the refresh its image is first shown at
static bool wp_started;
static bool offscreen(void);
static void create_swapchain(void);

// One presenter at a time (Beta 6 merge, 12/11/2026, as metal.m: the reorientation instead of replayed in-betweens):
// Frame Generation on at the start (Video tab) - its in-betweens present and this presenter stays off; else this one,
// and Frame Generation is left out (turned on later, it waits for the next start)
static bool wp_enabled(void) {
    static int on = -1;
    if (on < 0) { const char *e = getenv("LARECOMP_FP_WARP"); on = !offscreen() && !(e && *e == '0') && interp_wanted() != INTERP_ON; }
    return on;
}

// (render thread) the slots at the swapchain's size; destroyed and made again when it changes (all GPU work done)
static void wp_slots(uint32_t w, uint32_t h) {
    if (ws[0].img && ws[0].w == w && ws[0].h == h) return;
    pthread_mutex_lock(&wp_lock);
    atomic_store(&wp_latest, -1);
    rec_drain();  // (the recorder's packets that name the old slots, recorded and submitted before they go)
    vk_queue_lock();
    vkDeviceWaitIdle(g_vk.dev);
    vk_queue_unlock();
    if (!wp_dsl) {
        VkDescriptorSetLayoutBinding b[3] = {{0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, NULL},
                                             {1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, NULL},
                                             {2, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, NULL}};
        VkDescriptorSetLayoutCreateInfo li = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, .bindingCount = 3, .pBindings = b};
        VK_CHECK(vkCreateDescriptorSetLayout(g_vk.dev, &li, NULL, &wp_dsl));
        VkDescriptorPoolSize ps = {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 3 * WSLOTS};
        VkDescriptorPoolCreateInfo pi = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, .maxSets = WSLOTS, .poolSizeCount = 1,
                                         .pPoolSizes = &ps};
        VK_CHECK(vkCreateDescriptorPool(g_vk.dev, &pi, NULL, &wp_dpool));
        VkDescriptorSetLayout ls[WSLOTS];
        VkDescriptorSet sets[WSLOTS];
        for (int i = 0; i < WSLOTS; i++) ls[i] = wp_dsl;
        VkDescriptorSetAllocateInfo ai = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, .descriptorPool = wp_dpool,
                                          .descriptorSetCount = WSLOTS, .pSetLayouts = ls};
        VK_CHECK(vkAllocateDescriptorSets(g_vk.dev, &ai, sets));
        for (int i = 0; i < WSLOTS; i++) ws[i].set = sets[i];
        VkSemaphoreTypeCreateInfo ti = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO, .semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE};
        VkSemaphoreCreateInfo si = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO, .pNext = &ti};
        VK_CHECK(vkCreateSemaphore(g_vk.dev, &si, NULL, &wp_tl));
    }
    for (int i = 0; i < WSLOTS; i++) {
        WSlot *s = &ws[i];
        if (s->img) vkDestroyImageView(g_vk.dev, s->view, NULL), vmaDestroyImage(g_vk.vma, s->img, s->mem);
        if (s->img3d) vkDestroyImageView(g_vk.dev, s->view3d, NULL), vmaDestroyImage(g_vk.vma, s->img3d, s->mem3d);
        if (s->imgd) vkDestroyImageView(g_vk.dev, s->viewd, NULL), vmaDestroyImage(g_vk.vma, s->imgd, s->memd);
        s->imgd = VK_NULL_HANDLE, s->viewd = VK_NULL_HANDLE, s->dfmt = VK_FORMAT_UNDEFINED, s->hasd = s->has_eye = false;
        VkImageCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D, .format = g_vk.sc_format,
                                .extent = {w, h, 1}, .mipLevels = 1, .arrayLayers = 1, .samples = VK_SAMPLE_COUNT_1_BIT,
                                .tiling = VK_IMAGE_TILING_OPTIMAL,
                                .usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
                                         VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT};
        VmaAllocationCreateInfo ai = {.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE};
        VK_CHECK(vmaCreateImage(g_vk.vma, &ci, &ai, &s->img, &s->mem, NULL));
        VkImageViewCreateInfo vi = {.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = s->img, .viewType = VK_IMAGE_VIEW_TYPE_2D,
                                    .format = g_vk.sc_format, .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
        VK_CHECK(vkCreateImageView(g_vk.dev, &vi, NULL, &s->view));
        VK_CHECK(vmaCreateImage(g_vk.vma, &ci, &ai, &s->img3d, &s->mem3d, NULL));
        vi.image = s->img3d;
        VK_CHECK(vkCreateImageView(g_vk.dev, &vi, NULL, &s->view3d));
        image_barrier(vk_cmd(), s->img3d, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL);
        s->w = w, s->h = h, s->read_seq = 0, s->turn = 0, s->shown = true, s->has3d = false;
        VkDescriptorImageInfo ii[3] = {{vk_default_sampler(true), s->view, VK_IMAGE_LAYOUT_GENERAL},
                                       {vk_default_sampler(true), s->view3d, VK_IMAGE_LAYOUT_GENERAL},
                                       {vk_default_sampler(false), s->view3d, VK_IMAGE_LAYOUT_GENERAL}};  // (no depth yet)
        VkWriteDescriptorSet wr[3];
        for (int k = 0; k < 3; k++)
            wr[k] = (VkWriteDescriptorSet){.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = s->set, .dstBinding = (uint32_t)k,
                                           .descriptorCount = 1, .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                                           .pImageInfo = &ii[k]};
        vkUpdateDescriptorSets(g_vk.dev, 3, wr, 0, NULL);
    }
    wp_cur = wp_prev = -1;
    pthread_mutex_unlock(&wp_lock);
    LOG("[fpwarp] %d slots of %ux%u: one image presented per display refresh, turned in first person\n", WSLOTS, w, h);
}

// (exec.c vk_present_draw) the slot being composed: its second image, for the frame's 3D image without the HUD
static bool g_fp_has3d;
bool vk_fp_warp_on(void) { return wp_enabled(); }
bool vk_fp_slot3d(VkImage *img, VkImageView *view) {
    if (wp_cur < 0 || !ws[wp_cur].img3d) return false;
    *img = ws[wp_cur].img3d, *view = ws[wp_cur].view3d;
    return true;
}
void vk_fp_slot_has3d(void) { g_fp_has3d = true; }
// The overlay's lens (menu.mm ui_lens). In first person the presenter shows the middle 1 / k of a view drawn k times wider
// than the screen (firstperson.c), k times bigger - and the menu and the rest of ImGui, drawn into the same window image,
// came out k times bigger and cut at the edges (Guillaume 13/11: the bar at the top, the open menu). Drawn k times
// smaller round the game's centre, they reach the screen at their size. Exactly when fpwarp.frag crops the window image:
// the frame goes to the presenter, its lens has an overscan, and the 3D has no image of its own (with one - sep - what
// differs from it, the HUD and the UI, is shown where it lies, uncropped). 1: shown as drawn.
float vk_overlay_lens(float *cx, float *cy) {
    extern bool fp_warp_frame_lens(float *tx, float *ty, float *ovs);
    extern void vk_present_place(float *vr, float *crop);
    float tx = 0, ty = 0, k = 1, vr[4], crop[4];
    if (wp_cur < 0 || g_fp_has3d || !fp_warp_frame_lens(&tx, &ty, &k) || k <= 1 || tx <= 0 || ty <= 0) return 1;
    vk_present_place(vr, crop);
    if (vr[2] <= 0 || crop[2] <= 0) return 1;
    *cx = vr[0] + vr[2] / 2, *cy = vr[1] + vr[3] / 2;  // (the shown part is the middle of the game's place: crop centred)
    return k;
}

// (vk_frame_end, outside rendering) the frame's scene depth into the slot's own depth image, made at its format and
// size the first time (and when they change: all GPU work done, as wp_slots)
static bool wp_slot_depth(VkCommandBuffer cb, WSlot *s) {
    extern bool vk_fp_scene_depth(VkImage *img, VkFormat *fmt, uint32_t *w, uint32_t *h, float *k5);
    VkImage src;
    VkFormat f;
    uint32_t w, h;
    if (!vk_fp_scene_depth(&src, &f, &w, &h, s->dk)) return false;
    if (!s->imgd || s->dfmt != f || s->dimw != w || s->dimh != h) {
        pthread_mutex_lock(&wp_lock);
        rec_drain();
        vk_queue_lock();
        vkDeviceWaitIdle(g_vk.dev);
        vk_queue_unlock();
        if (s->imgd) vkDestroyImageView(g_vk.dev, s->viewd, NULL), vmaDestroyImage(g_vk.vma, s->imgd, s->memd);
        s->imgd = VK_NULL_HANDLE;
        VkImageCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D, .format = f,
                                .extent = {w, h, 1}, .mipLevels = 1, .arrayLayers = 1, .samples = VK_SAMPLE_COUNT_1_BIT,
                                .tiling = VK_IMAGE_TILING_OPTIMAL, .usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT};
        VmaAllocationCreateInfo ai = {.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE};
        bool ok = vmaCreateImage(g_vk.vma, &ci, &ai, &s->imgd, &s->memd, NULL) == VK_SUCCESS;
        if (ok) {
            VkImageViewCreateInfo vi = {.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = s->imgd, .viewType = VK_IMAGE_VIEW_TYPE_2D,
                                        .format = f, .subresourceRange = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1}};
            ok = vkCreateImageView(g_vk.dev, &vi, NULL, &s->viewd) == VK_SUCCESS;
            if (!ok) vmaDestroyImage(g_vk.vma, s->imgd, s->memd);
        }
        if (!ok) {
            s->imgd = VK_NULL_HANDLE;
            pthread_mutex_unlock(&wp_lock);
            return false;
        }
        bool st = f == VK_FORMAT_D16_UNORM_S8_UINT || f == VK_FORMAT_D24_UNORM_S8_UINT || f == VK_FORMAT_D32_SFLOAT_S8_UINT;
        vk_image_init_layout(cb, s->imgd, VK_IMAGE_ASPECT_DEPTH_BIT | (st ? VK_IMAGE_ASPECT_STENCIL_BIT : 0));
        VkDescriptorImageInfo ii = {vk_default_sampler(false), s->viewd, VK_IMAGE_LAYOUT_GENERAL};
        VkWriteDescriptorSet wr = {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = s->set, .dstBinding = 2, .descriptorCount = 1,
                                   .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .pImageInfo = &ii};
        vkUpdateDescriptorSets(g_vk.dev, 1, &wr, 0, NULL);
        s->dfmt = f, s->dimw = w, s->dimh = h;
        pthread_mutex_unlock(&wp_lock);
        static int said;
        if (!said++) LOG("[fpwarp] the image moved as well as turned, by its depth (%ux%u, format %d)\n", w, h, (int)f);
    }
    vk_full_barrier(cb);
    VkImageCopy c = {.srcSubresource = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 0, 1}, .dstSubresource = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 0, 1},
                     .extent = {w, h, 1}};
    rec_CmdCopyImage(cb, src, VK_IMAGE_LAYOUT_GENERAL, s->imgd, VK_IMAGE_LAYOUT_GENERAL, 1, &c);
    vk_full_barrier(cb);
    return true;
}

// (render thread) the pass's pipeline, for the swapchain's format
static void wp_pipeline(void) {
    if (wp_pipe && wp_pipe_fmt == g_vk.sc_format) return;
    if (!wp_layout) {
        VkPushConstantRange r = {VK_SHADER_STAGE_FRAGMENT_BIT, 0, 160};
        VkPipelineLayoutCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, .setLayoutCount = 1, .pSetLayouts = &wp_dsl,
                                         .pushConstantRangeCount = 1, .pPushConstantRanges = &r};
        VK_CHECK(vkCreatePipelineLayout(g_vk.dev, &ci, NULL, &wp_layout));
    }
    VkPipelineShaderStageCreateInfo stages[2] = {
        {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_VERTEX_BIT,
         .module = vk_builtin_shader("post.vert"), .pName = "main"},
        {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_FRAGMENT_BIT,
         .module = vk_builtin_shader("fpwarp.frag"), .pName = "main"}};
    VkPipelineVertexInputStateCreateInfo vi = {.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    VkPipelineInputAssemblyStateCreateInfo ia = {.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
                                                 .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST};
    VkPipelineViewportStateCreateInfo vs = {.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO, .viewportCount = 1, .scissorCount = 1};
    VkPipelineRasterizationStateCreateInfo rs = {.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
                                                 .polygonMode = VK_POLYGON_MODE_FILL, .lineWidth = 1};
    VkPipelineMultisampleStateCreateInfo ms = {.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
                                               .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT};
    VkPipelineColorBlendAttachmentState cba = {.colorWriteMask = 0xF};
    VkPipelineColorBlendStateCreateInfo cb = {.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO, .attachmentCount = 1,
                                              .pAttachments = &cba};
    VkDynamicState dyn[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dy = {.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO, .dynamicStateCount = 2, .pDynamicStates = dyn};
    VkFormat fmt = g_vk.sc_format;
    VkPipelineRenderingCreateInfo ri = {.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO, .colorAttachmentCount = 1,
                                        .pColorAttachmentFormats = &fmt};
    VkPipelineDepthStencilStateCreateInfo ds = {.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
    VkGraphicsPipelineCreateInfo pi = {.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, .pNext = &ri, .stageCount = 2,
                                       .pStages = stages, .pVertexInputState = &vi, .pInputAssemblyState = &ia, .pViewportState = &vs,
                                       .pRasterizationState = &rs, .pMultisampleState = &ms, .pDepthStencilState = &ds,
                                       .pColorBlendState = &cb, .pDynamicState = &dy, .layout = wp_layout};
    VK_CHECK(vkCreateGraphicsPipelines(g_vk.dev, g_vk.pcache, 1, &pi, NULL, &wp_pipe));
    wp_pipe_fmt = fmt;
}

static void wp_basis(float yaw, float pitch, float *r, float *u, float *f) {  // (metal.m warp_basis)
    float cp = cosf(pitch);
    f[0] = sinf(yaw) * cp, f[1] = sinf(pitch), f[2] = -cosf(yaw) * cp;
    r[0] = cosf(yaw), r[1] = 0, r[2] = sinf(yaw);
    u[0] = -r[2] * f[1], u[1] = r[2] * f[0] - r[0] * f[2], u[2] = r[0] * f[1];  // (r x f)
}
// fpwarp.frag's parameters: the frame's view turned to the moment's - the view of now less the pipeline's delay (one
// timeline, as the Mac's: a late game frame's next image goes on turning)
static void wp_params(const WSlot *s, uint64_t now, uint32_t dw, uint32_t dh, float *p) {
    extern void fp_warp_view_at(uint64_t t, float *yaw, float *pitch);
    float yaw = s->yaw, pitch = s->pitch;
    int64_t dl = atomic_load(&wp_delay);
    uint64_t t = dl > 0 && now > (uint64_t)dl ? now - (uint64_t)dl : s->ns;
    if (s->turn) {
        fp_warp_view_at(t, &yaw, &pitch);
        static float test = -1e9f;
        if (test < -1e8f) test = getenv("LARECOMP_FP_WARP_TEST_DEG") ? (float)atof(getenv("LARECOMP_FP_WARP_TEST_DEG")) : 0;
        yaw += test * (float)M_PI / 180.0f;
    }
    float a[3][3], b[3][3];
    wp_basis(s->yaw, s->pitch, a[0], a[1], a[2]);
    wp_basis(yaw, pitch, b[0], b[1], b[2]);
    for (int i = 0; i < 3; i++) {
        for (int j = 0; j < 3; j++) p[i * 4 + j] = a[i][0] * b[j][0] + a[i][1] * b[j][1] + a[i][2] * b[j][2];
        p[i * 4 + 3] = 0;
    }
    float sx = s->w ? (float)dw / (float)s->w : 1, sy = s->h ? (float)dh / (float)s->h : 1;  // (a swapchain resized meanwhile)
    p[12] = s->vr[0] * sx, p[13] = s->vr[1] * sy, p[14] = s->vr[2] * sx, p[15] = s->vr[3] * sy;
    memcpy(p + 16, s->crop, 16);
    p[20] = s->tx, p[21] = s->ty, p[22] = (float)dw, p[23] = (float)dh;
    float ovs = s->ovs > 1 ? s->ovs : 1;
    p[24] = (s->turn || ovs > 1) && s->tx > 0 && s->ty > 0 && s->vr[2] > 0 && s->crop[2] > 0 ? 1.0f : 0.0f;
    p[25] = s->has3d && p[24] > 0.5f ? 1.0f : 0.0f;  // (the HUD kept still: the 3D image turned under it)
    p[26] = s->tx / ovs, p[27] = s->ty / ovs;       // (the screen's tangents: the part of the frame's wider view shown)
    // the move, by the depth: the moment's eye less the frame's, in the frame's camera axes (p[28..30], on: p[31]); the
    // depth's terms (p[32..35]: the projection's [10] and [11], NVN's depth mode, rows the other way up); its part of its
    // texture, the depth below which a pixel is what he holds (exec.c: kept on screen), a test view of it (p[36..39]).
    // A jump of a unit or more (a door, a warp): turned only.
    static float near = -1, flip = -1, show = -1;
    if (near < 0) {
        near = 0.001f;  // (exec.c FP_HELD_Z, twice: what he holds is drawn nearer than any of the scene)
        flip = getenv("LARECOMP_FP_WARP_DFLIP") && atoi(getenv("LARECOMP_FP_WARP_DFLIP")) ? 1.0f : 0.0f;
        show = getenv("LARECOMP_FP_WARP_SHOWD") ? (float)atoi(getenv("LARECOMP_FP_WARP_SHOWD")) : 0.0f;  // (2: the depth alone)
    }
    memset(p + 28, 0, 12 * sizeof *p);
    float eye[3];
    extern bool fp_warp_eye_at(uint64_t t, float *eye);
    if (p[24] > 0.5f && s->hasd && s->has_eye && fp_warp_eye_at(t, eye)) {
        float dv[3] = {eye[0] - s->eye[0], eye[1] - s->eye[1], eye[2] - s->eye[2]};
        if (dv[0] * dv[0] + dv[1] * dv[1] + dv[2] * dv[2] < 1.0f) {
            for (int i = 0; i < 3; i++) p[28 + i] = a[i][0] * dv[0] + a[i][1] * dv[1] + a[i][2] * dv[2];
            p[31] = 1;
        }
    }
    if (s->hasd) {
        p[32] = s->dk[2], p[33] = s->dk[3], p[34] = s->dk[4], p[35] = flip;
        p[36] = s->dk[0], p[37] = s->dk[1], p[38] = near, p[39] = show;
    }
}

// LARECOMP_FPWARP_DUMP=<seconds>:<count> (a test): from <seconds> after the presenter's start, <count> presents
// recorded at 640x360 - what the window was given, and the slot it was made from - with the slot, whether it was new, its
// view's time and the turn applied; written when done to fpwarp-dump-out.raw, -slot.raw (RGBA, 640x360 each, one after
// the other) and -meta.csv in the working directory. Made to see the flashes Guillaume saw on Windows (22/10), which the
// game's own image does not have (none with LARECOMP_FP_WARP=0).
#define WD_W 640
#define WD_H 360
// LARECOMP_FPWARP_DUMP=ring:<count>: the last <count> presents always kept (a ring, in memory), written - oldest first -
// when a file fpwarp-dump-now appears in the working directory (created from outside the moment Guillaume says "now");
// the file is then removed and the ring goes on.
typedef struct { double at; int count, i; VkImage img[2]; VmaAllocation mem[2]; VkBuffer buf[2]; VmaAllocation bmem[2];
                 void *map[2]; char *meta; size_t mlen; bool done, ring; long long total; } WDump;
static WDump wd;
static bool wd_begin(uint64_t t0, uint64_t now) {  // (true when this present is to be recorded)
    if (wd.at == 0) {
        const char *e = getenv("LARECOMP_FPWARP_DUMP");
        if (e && !strncmp(e, "ring:", 5)) wd.ring = true, wd.count = atoi(e + 5), wd.at = wd.count > 0 ? 0.001 : -1;
        else if (!e || sscanf(e, "%lf:%d", &wd.at, &wd.count) != 2 || wd.count <= 0) wd.at = -1;
        if (wd.at > 0) {  // (both modes: the images and buffers)
            for (int k = 0; k < 2; k++) {
                VkImageCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D, .format = g_vk.sc_format,
                                        .extent = {WD_W, WD_H, 1}, .mipLevels = 1, .arrayLayers = 1, .samples = VK_SAMPLE_COUNT_1_BIT,
                                        .tiling = VK_IMAGE_TILING_OPTIMAL,
                                        .usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT};
                VmaAllocationCreateInfo ai = {.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE};
                VK_CHECK(vmaCreateImage(g_vk.vma, &ci, &ai, &wd.img[k], &wd.mem[k], NULL));
                VkBufferCreateInfo bi = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = (VkDeviceSize)WD_W * WD_H * 4 * wd.count,
                                         .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT};
                VmaAllocationCreateInfo bai = {.usage = VMA_MEMORY_USAGE_AUTO_PREFER_HOST,
                                               .flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT};
                VmaAllocationInfo inf;
                VK_CHECK(vmaCreateBuffer(g_vk.vma, &bi, &bai, &wd.buf[k], &wd.bmem[k], &inf));
                wd.map[k] = inf.pMappedData;
            }
            wd.meta = calloc((size_t)wd.count, 160);
            LOG("[fpwarp] recording %d presents from %.1f s (LARECOMP_FPWARP_DUMP)\n", wd.count, wd.at);
        }
    }
    if (wd.ring) return wd.at > 0;
    return wd.at > 0 && !wd.done && wd.i < wd.count && (now - t0) / 1e9 >= wd.at;
}
static void wd_finish(void);
#ifdef _WIN32
__declspec(dllimport) short __stdcall GetAsyncKeyState(int key);  // (user32; windows.h left out of this file)
#endif
static void wd_ring_check(void) {  // (the presenter, after a recorded present: the trigger, at most every 250 ms)
    static uint64_t last;
    uint64_t t = plat_now_ns();
#ifdef _WIN32
    // F9 on the keyboard: now - pressed the moment he sees it (a message reaches us 10 s or more later, the ring
    // holds 3: Guillaume's « LA » came too late twice, 22/10)
    static bool down;  // (once per press)
    bool is_down = (GetAsyncKeyState(0x78) & 0x8000) != 0, key = is_down && !down;
    down = is_down;
#else
    bool key = false;
#endif
    if (!key && t - last < 250000000ull) return;
    last = t;
    FILE *f = key ? NULL : fopen("fpwarp-dump-now", "rb");
    if (!f && !key) return;
    if (f) fclose(f), remove("fpwarp-dump-now");
    if (key) LOG("[fpwarp] F9: the last presents written\n");
    wd_finish();
    wd.done = false;
}
static void wd_record(VkCommandBuffer cb, VkImage sc, VkImage slot, uint32_t sw, uint32_t sh) {
    VkImageBlit b = {.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, .srcOffsets = {{0, 0, 0}, {(int32_t)sw, (int32_t)sh, 1}},
                     .dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, .dstOffsets = {{0, 0, 0}, {WD_W, WD_H, 1}}};
    VkBufferImageCopy c = {.bufferOffset = (VkDeviceSize)WD_W * WD_H * 4 * (wd.ring ? (int)(wd.total % wd.count) : wd.i),
                           .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                           .imageExtent = {WD_W, WD_H, 1}};
    image_barrier_now(cb, sc, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    for (int k = 0; k < 2; k++) {
        image_barrier_now(cb, wd.img[k], VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        vkCmdBlitImage(cb, k ? slot : sc, k ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, wd.img[k],
                       VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &b, VK_FILTER_LINEAR);
        image_barrier_now(cb, wd.img[k], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
        vkCmdCopyImageToBuffer(cb, wd.img[k], VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, wd.buf[k], 1, &c);
    }
    image_barrier_now(cb, sc, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
}
static void wd_finish(void) {  // (after the last recorded present: everything written out)
    vk_queue_lock();
    vkDeviceWaitIdle(g_vk.dev);
    vk_queue_unlock();
    const char *names[2] = {"fpwarp-dump-out.raw", "fpwarp-dump-slot.raw"};
    int n = wd.ring ? (int)(wd.total < wd.count ? wd.total : wd.count) : wd.i;
    int first = wd.ring && wd.total > wd.count ? (int)(wd.total % wd.count) : 0;  // (the oldest kept)
    size_t fsz = (size_t)WD_W * WD_H * 4;
    for (int k = 0; k < 2; k++) {
        vmaInvalidateAllocation(g_vk.vma, wd.bmem[k], 0, VK_WHOLE_SIZE);
        FILE *f = fopen(names[k], "wb");
        if (f) {
            for (int j = 0; j < n; j++) fwrite((char *)wd.map[k] + fsz * (size_t)((first + j) % (wd.ring ? wd.count : n ? n : 1)), fsz, 1, f);
            fclose(f);
        }
    }
    FILE *f = fopen("fpwarp-dump-meta.csv", "w");
    if (f) {
        fprintf(f, "i,t_ms,slot,new,turn,applied,rot02,rot00,view_ms,img,has3d\n");
        for (int j = 0; j < n; j++) fputs(wd.meta + (size_t)((first + j) % (wd.ring ? wd.count : n ? n : 1)) * 160, f);
        fclose(f);
    }
    wd.i = n;
    wd.done = true;
    LOG("[fpwarp] %d presents recorded (fpwarp-dump-*.raw, -meta.csv)\n", wd.i);
}

// The presenter's thread: at every refresh (FIFO acquire / present), the newest slot through fpwarp.frag into the window
static void *wp_thread(void *arg) {
    (void)arg;
    extern void plat_thread_name(const char *name);
    plat_thread_name("fp-warp-present");
    VkCommandPool pool;
    VkCommandPoolCreateInfo pi = {.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
                                  .queueFamilyIndex = g_vk.qfam};
    VK_CHECK(vkCreateCommandPool(g_vk.dev, &pi, NULL, &pool));
    VkCommandBuffer cbs[3];
    VkCommandBufferAllocateInfo cai = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, .commandPool = pool,
                                       .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 3};
    VK_CHECK(vkAllocateCommandBuffers(g_vk.dev, &cai, cbs));
    VkFence fence[3];
    VkSemaphore acq[3], done[8];
    // The window's image waited for here, on a fence, not on the queue: a submission waiting on the acquire semaphore
    // held the queue - the game's own work behind it - until the display freed an image, up to a refresh each time (the
    // game on Windows then missed 60 fps one frame in five and went back and forth between its 30 and 60 fps modes:
    // Guillaume's "ça accélère, ça décélère", 22/10). LARECOMP_FP_WARP_ACQ_SEM=1: the semaphore as before.
    VkFence acqf[3];
    const char *ase = getenv("LARECOMP_FP_WARP_ACQ_SEM");
    bool cpu_acq = !(ase && *ase == '1');
    VkFenceCreateInfo afi = {.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    for (int i = 0; i < 3; i++) VK_CHECK(vkCreateFence(g_vk.dev, &afi, NULL, &acqf[i]));
    VkFenceCreateInfo fi = {.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO, .flags = VK_FENCE_CREATE_SIGNALED_BIT};
    for (int i = 0; i < 3; i++) { VK_CHECK(vkCreateFence(g_vk.dev, &fi, NULL, &fence[i])); acq[i] = binary_semaphore(); }
    for (int i = 0; i < 8; i++) done[i] = binary_semaphore();
    uint64_t t_log = plat_now_ns(), n_pres = 0, n_turned = 0, n_new = 0, n_moved = 0;
    static int log = -1;
    if (log < 0) log = getenv("LARECOMP_LOG_FPWARP") != NULL;
    // (Windows: each image after the display's vertical blank - a driver's Enhanced Sync makes FIFO not wait:
    //  runtime/platform/windows.c plat_wait_vblank; LARECOMP_FP_WARP_VBLANK=0: FIFO alone)
    extern bool plat_wait_vblank(void *hwnd);
    const char *vbe = getenv("LARECOMP_FP_WARP_VBLANK");
    bool vblank = !vbe || *vbe != '0';
    void *hwnd = SDL_GetPointerProperty(SDL_GetWindowProperties(window), "SDL.window.win32.hwnd", NULL);
    // LARECOMP_FP_WARP_HZ=<n>: at most n images a second, whatever the display's refresh (the tablet's shell: 60 - its
    // display went up to 120 under the presenter, which drew twice as often and slowed the game to 11 fps, 27/10)
    const char *hze = getenv("LARECOMP_FP_WARP_HZ");
    double max_hz = hze ? atof(hze) : 0;
    uint64_t last_pres = 0;
    for (unsigned n = 0;; n++) {
        unsigned k = n % 3;
        vkWaitForFences(g_vk.dev, 1, &fence[k], VK_TRUE, UINT64_MAX);
        if (max_hz > 0 && last_pres) {  // (a little under the period: the present still makes the refresh it aims at)
            uint64_t gap = (uint64_t)(1e9 / max_hz) - 1500000, t = plat_now_ns();
            if (t - last_pres < gap) usleep((useconds_t)((gap - (t - last_pres)) / 1000));
        }
        last_pres = plat_now_ns();
        if (shell_window()) {  // (a shell's window: gone - its swapchain let go here, the thread that presents -, or back)
            int sw = 0, sh = 0;
            g_vk_xr->size(&sw, &sh);
            if ((!sw || !sh) && (g_vk.swapchain || g_vk.surface)) { vk_queue_lock(); shell_window_drop(); vk_queue_unlock(); }
            if (sw && sh && !g_vk.swapchain) { vk_queue_lock(); create_swapchain(); vk_queue_unlock(); }
        }
        if (atomic_load(&wp_latest) < 0 || !g_vk.swapchain) { usleep(2000); continue; }
        if (vblank && hwnd) vblank = plat_wait_vblank(hwnd);
        uint32_t ww = atomic_load(&wp_win_w), wh = atomic_load(&wp_win_h);
        static uint32_t tried_w, tried_h;  // (once per new size: a surface that gives another size is not asked again)
        if (ww && wh && (ww != g_vk.sc_extent.width || wh != g_vk.sc_extent.height) && (ww != tried_w || wh != tried_h)) {
            tried_w = ww, tried_h = wh;
            vk_queue_lock();
            vkDeviceWaitIdle(g_vk.dev);
            create_swapchain();
            vk_queue_unlock();
            LOG("[fpwarp] the window is %ux%u: swapchain made again, %ux%u\n", ww, wh, g_vk.sc_extent.width, g_vk.sc_extent.height);
            continue;
        }
        uint32_t img = 0;
        VkResult r = vkAcquireNextImageKHR(g_vk.dev, g_vk.swapchain, 1000000000ull, cpu_acq ? VK_NULL_HANDLE : acq[k],
                                           cpu_acq ? acqf[k] : VK_NULL_HANDLE, &img);
        if (cpu_acq && (r == VK_SUCCESS || r == VK_SUBOPTIMAL_KHR)) {
            vkWaitForFences(g_vk.dev, 1, &acqf[k], VK_TRUE, 1000000000ull);
            vkResetFences(g_vk.dev, 1, &acqf[k]);
        }
        if (r == VK_ERROR_OUT_OF_DATE_KHR) { vk_queue_lock(); vkDeviceWaitIdle(g_vk.dev); create_swapchain(); vk_queue_unlock(); continue; }
        if (r == VK_ERROR_SURFACE_LOST_KHR && shell_window()) { vk_queue_lock(); shell_window_drop(); vk_queue_unlock(); continue; }
        if (r != VK_SUCCESS && r != VK_SUBOPTIMAL_KHR) { usleep(2000); continue; }
        pthread_mutex_lock(&wp_lock);
        int lat = atomic_load(&wp_latest);
        if (lat < 0) lat = 0;  // (slots made again meanwhile: an image of the new ones, whatever it holds)
        WSlot s = ws[lat];
        uint64_t mine = ++wp_tl_n;
        ws[lat].read_seq = mine;
        bool first = !ws[lat].shown;
        ws[lat].shown = true;
        pthread_mutex_unlock(&wp_lock);
        uint64_t now = plat_now_ns();
        if (first) {
            n_new++;
            if (s.turn && now > s.ns && now - s.ns < 200000000ull) {
                int64_t d = (int64_t)(now - s.ns), e = atomic_load(&wp_delay);
                atomic_store(&wp_delay, e ? e + (d - e) / 32 : d);
            }
        }
        float pc[40];
        wp_params(&s, now, g_vk.sc_extent.width, g_vk.sc_extent.height, pc);
        n_turned += pc[24] > 0.5f, n_moved += pc[31] > 0.5f;
        VkCommandBuffer cb = cbs[k];
        vkResetCommandBuffer(cb, 0);
        VkCommandBufferBeginInfo bi = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
        vkBeginCommandBuffer(cb, &bi);
        image_barrier_now(cb, s.img, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL);  // (the composition's writes, before)
        image_barrier_now(cb, g_vk.sc_images[img], VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
        VkRenderingAttachmentInfo ca = {.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO, .imageView = g_vk.sc_views[img],
                                        .imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                                        .loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE, .storeOp = VK_ATTACHMENT_STORE_OP_STORE};
        VkRenderingInfo ri = {.sType = VK_STRUCTURE_TYPE_RENDERING_INFO, .renderArea = {{0, 0}, g_vk.sc_extent}, .layerCount = 1,
                              .colorAttachmentCount = 1, .pColorAttachments = &ca};
        vkCmdBeginRendering(cb, &ri);
        vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, wp_pipe);
        vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, wp_layout, 0, 1, &s.set, 0, NULL);
        VkViewport vp = {0, 0, (float)g_vk.sc_extent.width, (float)g_vk.sc_extent.height, 0, 1};
        VkRect2D sc = {{0, 0}, g_vk.sc_extent};
        vkCmdSetViewport(cb, 0, 1, &vp);
        vkCmdSetScissor(cb, 0, 1, &sc);
        vkCmdPushConstants(cb, wp_layout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, 160, pc);
        vkCmdDraw(cb, 3, 1, 0, 0);
        vkCmdEndRendering(cb);
        static uint64_t wd_t0;
        if (!wd_t0) wd_t0 = now;
        bool rec = wd_begin(wd_t0, now);
        if (rec) {
            wd_record(cb, g_vk.sc_images[img], s.img, g_vk.sc_extent.width, g_vk.sc_extent.height);
            int at = wd.ring ? (int)(wd.total % wd.count) : wd.i;
            snprintf(wd.meta + (size_t)at * 160, 160, "%lld,%.3f,%d,%d,%d,%d,%.6f,%.6f,%.3f,%u,%d\n", wd.total, (now - wd_t0) / 1e6, lat,
                     first ? 1 : 0, s.turn ? 1 : 0, pc[24] > 0.5f ? 1 : 0, pc[2], pc[0], s.ns ? (double)(s.ns % 1000000000000ull) / 1e6 : 0.0, img,
                     s.has3d ? 1 : 0);
            wd.i++, wd.total++;
        }
        vk_rec_frame(cb, g_vk.sc_images[img], g_vk.sc_extent.width, g_vk.sc_extent.height, wp_tl, mine, first);  // (record_vk.c)
        image_barrier_now(cb, g_vk.sc_images[img], VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);
        vkEndCommandBuffer(cb);
        VkSemaphoreSubmitInfo wsi = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO, .semaphore = acq[k],
                                     .stageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT};
        VkSemaphoreSubmitInfo ssi[2] = {{.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO, .semaphore = done[img % 8],
                                         .stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT},
                                        {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO, .semaphore = wp_tl, .value = mine,
                                         .stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT}};
        VkCommandBufferSubmitInfo cbi = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO, .commandBuffer = cb};
        VkSubmitInfo2 si = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2, .waitSemaphoreInfoCount = cpu_acq ? 0 : 1, .pWaitSemaphoreInfos = &wsi,
                            .commandBufferInfoCount = 1, .pCommandBufferInfos = &cbi, .signalSemaphoreInfoCount = 2,
                            .pSignalSemaphoreInfos = ssi};
        vkResetFences(g_vk.dev, 1, &fence[k]);
        VkPresentInfoKHR pr = {.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR, .waitSemaphoreCount = 1, .pWaitSemaphores = &done[img % 8],
                               .swapchainCount = 1, .pSwapchains = &g_vk.swapchain, .pImageIndices = &img};
        vk_queue_lock();
        VkResult sr = vkQueueSubmit2(g_vk.queue, 1, &si, fence[k]);
        r = sr == VK_SUCCESS ? vkQueuePresentKHR(g_vk.queue, &pr) : sr;
        vk_queue_unlock();
        n_pres++;
        if (rec && !wd.ring && wd.i == wd.count) wd_finish();
        if (rec && wd.ring) wd_ring_check();
        if (r == VK_ERROR_OUT_OF_DATE_KHR || r == VK_SUBOPTIMAL_KHR) {
            vk_queue_lock();
            vkDeviceWaitIdle(g_vk.dev);
            create_swapchain();
            vk_queue_unlock();
        } else if (r != VK_SUCCESS) {
            static int nerr;
            if (nerr++ < 5) LOG("[fpwarp] submit / present: %d\n", r);
        }
        if (now - t_log > 5000000000ull) {
            double sec = (now - t_log) / 1e9;
            if (log) LOG("[fpwarp] %.1f images/s presented (%.1f new from the game, %.0f%% turned, %.0f%% moved), delay %.1f ms\n",
                         n_pres / sec, n_new / sec, n_pres ? 100.0 * n_turned / n_pres : 0.0, n_pres ? 100.0 * n_moved / n_pres : 0.0,
                         atomic_load(&wp_delay) / 1e6);
            if (log && s.hasd) LOG("[fpwarp] depth %ux%u, its part %.3f x %.3f, terms %g %g %g\n", s.dimw, s.dimh, s.dk[0], s.dk[1],
                                   s.dk[2], s.dk[3], s.dk[4]);
            t_log = now, n_pres = n_turned = n_new = n_moved = 0;
        }
    }
    return NULL;
}

// id: frames acquired so far; secondary: the window image's rendering open now is the overlay's (vk_overlay_end), for
// secondary command buffers only
typedef struct { uint32_t image; VkSemaphore acquired; bool ok, secondary; uint64_t id; } Frame;
static Frame cur_frame;
// the image this frame is composed into, and its view: a presenter's slot, or the window's (or an offscreen) image
#define CUR_IMG (wp_cur >= 0 ? ws[wp_cur].img : g_vk.sc_images[cur_frame.image])
#define CUR_VIEW (wp_cur >= 0 ? ws[wp_cur].view : g_vk.sc_views[cur_frame.image])

// Acquire the next window image and begin rendering into it (cleared to `clear`). Returns false when there is
// nothing to draw into (minimised window).
bool vk_frame_begin(const float clear[4]) {
    cur_frame.ok = false;
    extern VkImageView g_vk_scene_view;  // (exec.c: the game image isn't drawn yet this frame)
    g_vk_scene_view = VK_NULL_HANDLE;
    if (rq_stopping()) return false;  // quitting: no window image after the window closed (#40)
    int w, h;
    if (offscreen()) w = g_off_w, h = g_off_h;
    else window_px(&w, &h);
    if (!w || !h) {  // minimised (a shell without a window: its swapchain let go, by the thread that presents)
        if (shell_window() && !wp_enabled() && g_vk.swapchain) { vk_queue_lock(); shell_window_drop(); vk_queue_unlock(); }
        return false;
    }
    // The last present (the recorder's, record.c) must be done before the swapchain is used again - acquire and present
    // on the same swapchain are not allowed at the same time - and it may have asked for a new swapchain
    WAITING(rec_wait_presents(rec_presents_queued()));
    if (rec_present_recreate()) recreate_swapchain();
    if (!offscreen()) atomic_store(&wp_win_w, (uint32_t)w), atomic_store(&wp_win_h, (uint32_t)h);
    if (wp_enabled() && g_vk.swapchain) {  // (the presenter's: a slot to compose into; the swapchain is its own)
        wp_slots(g_vk.sc_extent.width, g_vk.sc_extent.height);
        slot = (slot + 1) % FRAMES;
        WAITING_AT(0, vk_wait_seq(slot_seq[slot]));
        pthread_mutex_lock(&wp_lock);
        int lat = atomic_load(&wp_latest), pick = -1;
        for (int i = 1; i <= WSLOTS && pick < 0; i++) {
            int c = (wp_prev + i + WSLOTS) % WSLOTS;
            if (c != lat) pick = c;
        }
        uint64_t rs = ws[pick].read_seq;
        pthread_mutex_unlock(&wp_lock);
        if (rs) {
            VkSemaphoreWaitInfo wi = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO, .semaphoreCount = 1, .pSemaphores = &wp_tl, .pValues = &rs};
            WAITING_AT(1, vkWaitSemaphores(g_vk.dev, &wi, 1000000000ull));
        }
        wp_cur = pick;
        cur_frame.image = 0;
        cur_frame.acquired = VK_NULL_HANDLE;
        slot_seq[slot] = vk_pending_seq();
        VkCommandBuffer cb = vk_cmd();
        image_barrier(cb, ws[pick].img, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
        vk_gpu_timer_window_start();
        VkRenderingAttachmentInfo ca = {.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO, .imageView = ws[pick].view,
                                        .imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                                        .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR, .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
                                        .clearValue.color.float32 = {clear[0], clear[1], clear[2], clear[3]}};
        VkRenderingInfo ri = {.sType = VK_STRUCTURE_TYPE_RENDERING_INFO, .renderArea = {{0, 0}, g_vk.sc_extent}, .layerCount = 1,
                              .colorAttachmentCount = 1, .pColorAttachments = &ca};
        rec_CmdBeginRendering(cb, &ri);
        VkViewport vp = {0, 0, (float)g_vk.sc_extent.width, (float)g_vk.sc_extent.height, 0, 1};
        VkRect2D sc = {{0, 0}, g_vk.sc_extent};
        rec_CmdSetViewport(cb, 0, 1, &vp);
        rec_CmdSetScissor(cb, 0, 1, &sc);
        cur_frame.ok = true;
        return true;
    }
    // (a shell's window with the warp presenter: its thread alone makes the swapchain again - wp_thread)
    if (!offscreen() && !(shell_window() && wp_enabled()) &&
        (!g_vk.swapchain || (uint32_t)w != g_vk.sc_extent.width || (uint32_t)h != g_vk.sc_extent.height ||
         sc_mode_changed())) recreate_swapchain();
    if (!offscreen() && !g_vk.swapchain) return false;
    static int trace = -1;
    if (trace < 0) trace = getenv("LARECOMP_VK_TRACE") != NULL;
    if (trace) LOG("[vk] frame begin %dx%d (swapchain %ux%u)\n", w, h, g_vk.sc_extent.width, g_vk.sc_extent.height);
    slot = (slot + 1) % FRAMES;
    if (!acquire_sem[slot]) acquire_sem[slot] = binary_semaphore();
    WAITING_AT(0, vk_wait_seq(slot_seq[slot]));
    VkResult r = VK_SUCCESS;
#ifdef __ANDROID__
    if (!g_vk.swapchain) {  // (no window last time: try again)
        recreate_swapchain();
        if (!g_vk.swapchain) return false;
    }
#endif
    if (offscreen()) {  // (an OpenXR shell's swapchain image when it gives one: vk_xr.h acquire; else ours)
        int x = g_vk.sc_count > 2 ? g_vk_xr->acquire() : -1;
        cur_frame.image = x >= 0 && 2 + (uint32_t)x < g_vk.sc_count ? 2 + (uint32_t)x : (uint32_t)slot % 2;
    } else WAITING_AT(1, r = vkAcquireNextImageKHR(g_vk.dev, g_vk.swapchain, UINT64_MAX, acquire_sem[slot], VK_NULL_HANDLE, &cur_frame.image));
    if (r == VK_ERROR_OUT_OF_DATE_KHR || r == VK_ERROR_SURFACE_LOST_KHR) {
        recreate_swapchain();
#ifdef __ANDROID__
        if (!g_vk.swapchain) return false;
#endif
        WAITING_AT(1, r = vkAcquireNextImageKHR(g_vk.dev, g_vk.swapchain, UINT64_MAX, acquire_sem[slot], VK_NULL_HANDLE,
                                          &cur_frame.image));
    }
    if (r != VK_SUCCESS && r != VK_SUBOPTIMAL_KHR) { LOG("[vk] acquire failed: %d\n", r); return false; }
    cur_frame.acquired = offscreen() ? VK_NULL_HANDLE : acquire_sem[slot];
    cur_frame.id++;
    cur_frame.secondary = false;
    slot_seq[slot] = vk_pending_seq();
    VkCommandBuffer cb = vk_cmd();
    image_barrier(cb, g_vk.sc_images[cur_frame.image], VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    vk_gpu_timer_window_start();  // (the GPU timer: this submission's work starts after that barrier, which waits for the image)
    VkRenderingAttachmentInfo ca = {.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO, .imageView = g_vk.sc_views[cur_frame.image],
                                    .imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                                    .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR, .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
                                    .clearValue.color.float32 = {clear[0], clear[1], clear[2], clear[3]}};
    VkRenderingInfo ri = {.sType = VK_STRUCTURE_TYPE_RENDERING_INFO, .renderArea = {{0, 0}, g_vk.sc_phys}, .layerCount = 1,
                          .colorAttachmentCount = 1, .pColorAttachments = &ca};
    rec_CmdBeginRendering(cb, &ri);
    VkViewport vp = {0, 0, (float)g_vk.sc_phys.width, (float)g_vk.sc_phys.height, 0, 1};
    VkRect2D sc = {{0, 0}, g_vk.sc_phys};
    rec_CmdSetViewport(cb, 0, 1, &vp);
    rec_CmdSetScissor(cb, 0, 1, &sc);
    extern void vk_imgui_prerotation(int quarter);  // (imgui_glue.cpp: the UI drawn into this image turned like it)
    vk_imgui_prerotation(g_vk.sc_quarter);
    cur_frame.ok = true;
    return true;
}

// Passes of their own between vk_frame_begin and vk_frame_end (exec.c post passes): end the window image's
// rendering, and begin it again keeping what was drawn.
void vk_frame_suspend(void) {
    if (cur_frame.ok) rec_CmdEndRendering(vk_cmd());
}
void vk_frame_resume(void) {
    if (!cur_frame.ok) return;
    cur_frame.secondary = false;
    VkCommandBuffer cb = vk_cmd();
    VkRenderingAttachmentInfo ca = {.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO, .imageView = CUR_VIEW,
                                    .imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                                    .loadOp = VK_ATTACHMENT_LOAD_OP_LOAD, .storeOp = VK_ATTACHMENT_STORE_OP_STORE};
    VkRenderingInfo ri = {.sType = VK_STRUCTURE_TYPE_RENDERING_INFO, .renderArea = {{0, 0}, g_vk.sc_phys}, .layerCount = 1,
                          .colorAttachmentCount = 1, .pColorAttachments = &ca};
    rec_CmdBeginRendering(cb, &ri);
    VkViewport vp = {0, 0, (float)g_vk.sc_phys.width, (float)g_vk.sc_phys.height, 0, 1};
    VkRect2D sc = {{0, 0}, g_vk.sc_phys};
    rec_CmdSetViewport(cb, 0, 1, &vp);
    rec_CmdSetScissor(cb, 0, 1, &sc);
}

VkExtent2D vk_frame_extent(void) { return g_vk.sc_extent; }
// Pre-rotation (sc_quarter): window coordinates (sc_extent, y down) to the swapchain image's. A quarter turn of 1 is
// clip space (x, y) -> (-y, x), 3 is (y, -x) (the vertex shaders); these turn the rects to match.
VkRect2D vk_sc_rect(int32_t x, int32_t y, uint32_t w, uint32_t h) {
    int32_t W = (int32_t)g_vk.sc_extent.width, H = (int32_t)g_vk.sc_extent.height;
    if (g_vk.sc_quarter == 1) return (VkRect2D){{H - y - (int32_t)h, x}, {h, w}};
    if (g_vk.sc_quarter == 3) return (VkRect2D){{y, W - x - (int32_t)w}, {h, w}};
    return (VkRect2D){{x, y}, {w, h}};
}
VkViewport vk_sc_viewport(float x, float y, float w, float h) {
    float W = (float)g_vk.sc_extent.width, H = (float)g_vk.sc_extent.height;
    if (g_vk.sc_quarter == 1) return (VkViewport){H - y - h, x, h, w, 0, 1};
    if (g_vk.sc_quarter == 3) return (VkViewport){y, W - x - w, h, w, 0, 1};
    return (VkViewport){x, y, w, h, 0, 1};
}
// The window image's command buffer for direct recording (the start screen's ImGui): the recorder has recorded
// everything queued before it, and records what is queued after it.
void *vk_frame_cmd(void) {
    VkCommandBuffer cb = vk_cmd();
    rec_drain();
    return (void *)cb;
}

// ---- the overlay (menu.mm menu_render: ImGui's FPS counter, run timer, cards, the menu)
// ImGui's backend (imgui_impl_vulkan.cpp) makes its vkCmd* calls itself, on the thread that presents. With the recorder
// thread on (record.c), the window image's command buffer and its pool are the recorder's until it has caught up:
// ImGui recording into that buffer needed a full rec_drain first, every frame an overlay was up, which serialised the
// two threads (the render thread waited for the recorder to record the whole frame: ~5 ms a frame on a desktop, 20-30
// on a Steam Deck with only the FPS counter shown). Now ImGui records into a secondary command buffer from a pool of
// this file's - one per frame slot, never recorded by the recorder - and the recorder executes it in its place in the
// window image's commands: the window's rendering ends, a rendering of secondary command buffers begins on the same
// image (its contents loaded) and executes it. That rendering stays open for vk_frame_end to end: nothing is drawn
// into the window image inline after the overlay.
// A slot's buffer is recorded again (its pool reset) only once the GPU finished the submission that executed it - the
// slot comes round after vk_frame_begin waited for the window submission three frames back, so normally that is
// already so - which implies the recorder made that vkCmdExecuteCommands long before: the pool is this thread's alone
// whenever it touches it. ImGui's own objects (pipeline, buffers, descriptor sets) were only ever used by this thread.
// The recorder off, LARECOMP_VK_OVERLAY_DIRECT=1, or a second overlay in one frame: ImGui records into the window
// image's command buffer itself, after rec_drain (a no-op with the recorder off), as before.
static VkCommandPool ov_pool[FRAMES];
static VkCommandBuffer ov_cb[FRAMES];
static uint64_t ov_seq[FRAMES], ov_frame[FRAMES];  // the submission that executed the slot's buffer; the frame it was for
static VkCommandBuffer ov_open;                    // (handed out by vk_overlay_begin, until vk_overlay_end)
static int ov_direct = -1;

static bool ov_slot_ready(void) {  // (the slot's pool and buffer exist, and the buffer is free to record)
    if (!ov_pool[slot]) {
        VkCommandPoolCreateInfo pi = {.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, .flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT,
                                      .queueFamilyIndex = g_vk.qfam};
        if (vkCreateCommandPool(g_vk.dev, &pi, NULL, &ov_pool[slot]) != VK_SUCCESS) { ov_pool[slot] = VK_NULL_HANDLE; return false; }
        VkCommandBufferAllocateInfo ai = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, .commandPool = ov_pool[slot],
                                          .level = VK_COMMAND_BUFFER_LEVEL_SECONDARY, .commandBufferCount = 1};
        if (vkAllocateCommandBuffers(g_vk.dev, &ai, &ov_cb[slot]) != VK_SUCCESS) {
            vkDestroyCommandPool(g_vk.dev, ov_pool[slot], NULL);
            ov_pool[slot] = VK_NULL_HANDLE;
            return false;
        }
        return true;
    }
    if (ov_seq[slot] > vk_completed_seq()) vk_wait_seq(ov_seq[slot]);  // (normally done already: see above)
    VK_CHECK(vkResetCommandPool(g_vk.dev, ov_pool[slot], 0));
    return true;
}

// The command buffer for ImGui to record the overlay into (window_cmd: the window image's, inside its rendering), then
// vk_overlay_end with both
void *vk_overlay_begin(void *window_cmd) {
    if (ov_direct < 0) {
        const char *e = getenv("LARECOMP_VK_OVERLAY_DIRECT");
        ov_direct = !rec_on() || (e && atoi(e) != 0);
        LOG("[vk] overlay: %s\n", ov_direct ? "recorded into the window image's command buffer" : "a secondary command buffer the recorder executes");
    }
    if (ov_direct || !cur_frame.ok || ov_open || ov_frame[slot] == cur_frame.id || !ov_slot_ready()) {
        if (cur_frame.ok && cur_frame.secondary) {  // (a second overlay this frame: inline draws again, in a rendering of their own)
            vk_frame_suspend();
            vk_frame_resume();
        }
        rec_drain();  // (ImGui records into window_cmd itself: the recorder caught up and idle first)
        return window_cmd;
    }
    VkCommandBufferInheritanceRenderingInfo ir = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_INHERITANCE_RENDERING_INFO,
                                                  .colorAttachmentCount = 1, .pColorAttachmentFormats = &g_vk.sc_format,
                                                  .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT};
    VkCommandBufferInheritanceInfo ii = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_INHERITANCE_INFO, .pNext = &ir};
    VkCommandBufferBeginInfo bi = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
                                   .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT | VK_COMMAND_BUFFER_USAGE_RENDER_PASS_CONTINUE_BIT,
                                   .pInheritanceInfo = &ii};
    VK_CHECK(vkBeginCommandBuffer(ov_cb[slot], &bi));
    ov_frame[slot] = cur_frame.id;
    ov_open = ov_cb[slot];
    return (void *)ov_open;
}

void vk_overlay_end(void *window_cmd, void *cmd) {
    if (!cmd || cmd == window_cmd || (VkCommandBuffer)cmd != ov_open) return;  // (recorded into window_cmd directly)
    VkCommandBuffer sec = ov_open, cb = (VkCommandBuffer)window_cmd;
    ov_open = VK_NULL_HANDLE;
    VK_CHECK(vkEndCommandBuffer(sec));
    rec_CmdEndRendering(cb);
    VkRenderingAttachmentInfo ca = {.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO, .imageView = g_vk.sc_views[cur_frame.image],
                                    .imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                                    .loadOp = VK_ATTACHMENT_LOAD_OP_LOAD, .storeOp = VK_ATTACHMENT_STORE_OP_STORE};
    VkRenderingInfo ri = {.sType = VK_STRUCTURE_TYPE_RENDERING_INFO, .flags = VK_RENDERING_CONTENTS_SECONDARY_COMMAND_BUFFERS_BIT,
                          .renderArea = {{0, 0}, g_vk.sc_phys}, .layerCount = 1, .colorAttachmentCount = 1, .pColorAttachments = &ca};
    rec_CmdBeginRendering(cb, &ri);
    rec_CmdExecuteCommands(cb, 1, &sec);
    cur_frame.secondary = true;
    ov_seq[slot] = vk_pending_seq();  // (the submission window_cmd goes in)
}

static void save_window_image(VkCommandBuffer cb, VkSpan *out) {
    uint32_t w = g_vk.sc_phys.width, h = g_vk.sc_phys.height;
    *out = vk_readback((uint64_t)w * h * 4);
    image_barrier(cb, CUR_IMG, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    VkBufferImageCopy rg = {.bufferOffset = out->off, .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                            .imageExtent = {w, h, 1}};
    rec_CmdCopyImageToBuffer(cb, CUR_IMG, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, out->buf, 1, &rg);
    image_barrier(cb, CUR_IMG, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
}

// End rendering into the window image and present it. shot_path: also save the window image as PNG.
void vk_frame_end(const char *shot_path) {
    if (!cur_frame.ok) return;
    VkCommandBuffer cb = vk_cmd();
    rec_CmdEndRendering(cb);
    VkSpan shot = {0};
    if (shot_path) save_window_image(cb, &shot);
    uint32_t img = cur_frame.image;
    if (wp_cur >= 0) {  // (the presenter's slot: handed over with its view; the presenter shows it at the refreshes)
        WSlot *s = &ws[wp_cur];
        image_barrier(cb, s->img, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL);
        bool has3d = g_fp_has3d;
        g_fp_has3d = false;
        if (has3d) image_barrier(cb, s->img3d, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL);
        extern bool fp_warp_frame_view(float *yaw, float *pitch, float *tx, float *ty, uint64_t *ns);
        extern void vk_present_place(float *vr, float *crop);
        float yaw = 0, pitch = 0, tx = 0, ty = 0;
        uint64_t ns = 0;
        bool turn = fp_warp_frame_view(&yaw, &pitch, &tx, &ty, &ns);
        extern bool fp_warp_frame_eye(float *eye);
        extern bool fp_warp_frame_lens(float *tx, float *ty, float *ovs);
        float ovs = 1, ltx, lty;
        if (fp_warp_frame_lens(&ltx, &lty, &ovs) && !turn) tx = ltx, ty = lty;  // (shown at the screen's part, turned or not)
        float eye[3] = {0, 0, 0};
        bool has_eye = turn && fp_warp_frame_eye(eye);
        bool hasd = has3d && has_eye && wp_slot_depth(cb, s);
        float vr[4], crop[4];
        vk_present_place(vr, crop);
        uint64_t seq = vk_pending_seq();
        vk_submit_extra(VK_NULL_HANDLE, VK_NULL_HANDLE, VK_NULL_HANDLE);
        // (with the recorder thread, the submission is the recorder's to make: the slot is handed to the presenter only
        //  once it is in the queue, or the presenter's own submission could come before it and show it half drawn)
        if (rec_on()) rec_wait_submitted(seq);
        pthread_mutex_lock(&wp_lock);
        s->turn = turn, s->yaw = yaw, s->pitch = pitch, s->tx = tx, s->ty = ty, s->ns = ns, s->shown = false, s->has3d = has3d;
        s->hasd = hasd, s->has_eye = has_eye, memcpy(s->eye, eye, sizeof eye), s->ovs = ovs;
        memcpy(s->vr, vr, sizeof vr), memcpy(s->crop, crop, sizeof crop);
        atomic_store(&wp_latest, wp_cur);
        pthread_mutex_unlock(&wp_lock);
        wp_prev = wp_cur, wp_cur = -1;
        if (!wp_started) {
            // the view drawn this much wider than the screen (firstperson.c): the presenter turns into it - measured
            // 22/10 at the stick's full turn, 0.033 of a radian between a frame's view and a refresh's: 7.5 % more at
            // the edge of a 92-degree field. LARECOMP_FP_WARP_OVERSCAN=1: none (the edge stretched)
            extern void fp_warp_overscan_set(float k);
            const char *e = getenv("LARECOMP_FP_WARP_OVERSCAN");
            fp_warp_overscan_set(e ? (float)atof(e) : 1.1f);
            wp_pipeline();
            pthread_t t;
            pthread_create(&t, NULL, wp_thread, NULL);
            pthread_detach(t);
            wp_started = true;
        }
        cur_frame.ok = false;
        if (shot_path) {
            vk_wait_seq(seq);
            uint32_t w = g_vk.sc_extent.width, h = g_vk.sc_extent.height;
            uint8_t *px = shot.ptr;
            bool bgr = g_vk.sc_format == VK_FORMAT_B8G8R8A8_UNORM || g_vk.sc_format == VK_FORMAT_B8G8R8A8_SRGB;
            for (uint64_t i = 0; bgr && i < (uint64_t)w * h; i++) { uint8_t t = px[i * 4]; px[i * 4] = px[i * 4 + 2]; px[i * 4 + 2] = t; }
            for (uint64_t i = 0; i < (uint64_t)w * h; i++) px[i * 4 + 3] = 255;
            vk_save_png(shot_path, px, w, h);
        }
        return;
    }
    if (offscreen()) {  // (no present: the image stays for whoever reads it - a shot, an OpenXR app)
        extern void vk_gp_mark(VkCommandBuffer cb, const char *what);
        vk_gp_mark(cb, img >= 2 ? "present: in the headset's swapchain" : "present: the copy into the headset's swapchain");
        if (g_vk_xr) g_vk_xr->frame(cb, g_vk.sc_images[img], g_vk.sc_extent);
        vk_gp_mark(cb, "present: end");
        uint64_t seq = vk_pending_seq();
        vk_submit_extra(VK_NULL_HANDLE, VK_NULL_HANDLE, VK_NULL_HANDLE);
        if (g_vk_xr) g_vk_xr->frame_submitted();
        extern void vk_xr_compare_finish(uint64_t seq);
        vk_xr_compare_finish(seq);  // only explicit same-frame stereo comparison requests block here
        cur_frame.ok = false;
        if (shot_path) {
            vk_wait_seq(seq);
            uint32_t w = g_vk.sc_extent.width, h = g_vk.sc_extent.height;
            uint8_t *px = shot.ptr;
            for (uint64_t i = 0; i < (uint64_t)w * h; i++) px[i * 4 + 3] = 255;
            vk_save_png(shot_path, px, w, h);
        }
        return;
    }
    image_barrier(cb, g_vk.sc_images[cur_frame.image], VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);
    if (!done_sem[img]) done_sem[img] = binary_semaphore();
    uint64_t seq = vk_pending_seq();
    vk_submit_extra(cur_frame.acquired, done_sem[img], VK_NULL_HANDLE);
    // the present follows the submission in the recorder's order (its wait for the display is the recorder's, counted in
    // g_vk_frame_waits[2]; an out-of-date result makes the next vk_frame_begin recreate the swapchain)
    rec_QueuePresentKHR(done_sem[img], g_vk.swapchain, img);
    cur_frame.ok = false;
    if (shot_path) {
        vk_wait_seq(seq);
        uint32_t w = g_vk.sc_extent.width, h = g_vk.sc_extent.height;
        uint8_t *px = shot.ptr, *turned = NULL;
        if (g_vk.sc_quarter) {  // (the pre-rotated image turned back into the window's shape: W x H from H x W)
            turned = malloc((uint64_t)w * h * 4);
            for (uint32_t y = 0; y < h && turned; y++)
                for (uint32_t x = 0; x < w; x++) {
                    uint32_t qx = g_vk.sc_quarter == 1 ? h - 1 - y : y, qy = g_vk.sc_quarter == 1 ? x : w - 1 - x;
                    memcpy(turned + ((uint64_t)y * w + x) * 4, px + ((uint64_t)qy * h + qx) * 4, 4);
                }
            if (turned) px = turned;
        }
        bool bgr = g_vk.sc_format == VK_FORMAT_B8G8R8A8_UNORM || g_vk.sc_format == VK_FORMAT_B8G8R8A8_SRGB;
        for (uint64_t i = 0; bgr && i < (uint64_t)w * h; i++) { uint8_t t = px[i * 4]; px[i * 4] = px[i * 4 + 2]; px[i * 4 + 2] = t; }
        for (uint64_t i = 0; i < (uint64_t)w * h; i++) px[i * 4 + 3] = 255;
        vk_save_png(shot_path, px, w, h);
        free(turned);
    }
}

#ifdef __ANDROID__
#include <zlib.h>
// SDL_SavePNG reaches Java on Android (SDL_IOFromFile) and crashes in the Quest app's render threads, absolute path or
// not: the PNG written here with zlib (filter 0 rows, RGBA 8-bit).
static void png_chunk(FILE *f, const char *type, const uint8_t *d, uint32_t n) {
    uint8_t be[4] = {(uint8_t)(n >> 24), (uint8_t)(n >> 16), (uint8_t)(n >> 8), (uint8_t)n};
    fwrite(be, 1, 4, f);
    fwrite(type, 1, 4, f);
    if (n) fwrite(d, 1, n, f);
    uLong c = crc32(crc32(0, (const Bytef *)type, 4), d, n);
    uint8_t cb[4] = {(uint8_t)(c >> 24), (uint8_t)(c >> 16), (uint8_t)(c >> 8), (uint8_t)c};
    fwrite(cb, 1, 4, f);
}
static bool png_write_zlib(const char *path, const uint8_t *rgba, uint32_t w, uint32_t h) {
    size_t row = (size_t)w * 4 + 1, raw_n = row * h;
    uint8_t *raw = malloc(raw_n);
    uLongf zn = compressBound((uLong)raw_n);
    uint8_t *z = malloc(zn);
    bool ok = false;
    if (raw && z) {
        for (uint32_t y = 0; y < h; y++) { raw[y * row] = 0; memcpy(raw + y * row + 1, rgba + (size_t)y * w * 4, (size_t)w * 4); }
        FILE *f = compress2(z, &zn, raw, (uLong)raw_n, 3) == Z_OK ? fopen(path, "wb") : NULL;
        if (f) {
            static const uint8_t sig[8] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n'};
            uint8_t ihdr[13] = {(uint8_t)(w >> 24), (uint8_t)(w >> 16), (uint8_t)(w >> 8), (uint8_t)w,
                                (uint8_t)(h >> 24), (uint8_t)(h >> 16), (uint8_t)(h >> 8), (uint8_t)h, 8, 6, 0, 0, 0};
            fwrite(sig, 1, 8, f);
            png_chunk(f, "IHDR", ihdr, 13);
            png_chunk(f, "IDAT", z, (uint32_t)zn);
            png_chunk(f, "IEND", NULL, 0);
            ok = fclose(f) == 0;
        }
    }
    free(raw);
    free(z);
    return ok;
}
#endif

void vk_save_png(const char *path, const uint8_t *rgba, uint32_t w, uint32_t h) {
    char abs[1200];  // (SDL resolves a relative path against Android's internal storage, which needs the Java side:
    if (path[0] != '/') {  //  a native process on the Quest crashed there - made absolute from the working directory)
        char cwd[1024];
        if (getcwd(cwd, sizeof cwd)) snprintf(abs, sizeof abs, "%s/%s", cwd, path), path = abs;
    }
#ifdef __ANDROID__
    if (png_write_zlib(path, rgba, w, h)) LOG("[vk] saved %s (%ux%u)\n", path, w, h);
    else LOG("[vk] saving %s failed\n", path);
    return;
#endif
    SDL_Surface *s = SDL_CreateSurfaceFrom((int)w, (int)h, SDL_PIXELFORMAT_ABGR8888, (void *)rgba, (int)w * 4);
    if (!s || !SDL_SavePNG(s, path)) LOG("[vk] saving %s failed: %s\n", path, SDL_GetError());
    else LOG("[vk] saved %s (%ux%u)\n", path, w, h);
    SDL_DestroySurface(s);
}

// ================================================================== game presentation (metal.m backend_present)
// Draws the game's image into the window with the widescreen crop, then the overlay menu.
extern void vk_present_draw(VkCommandBuffer cb, TexBackend *b, float vx, float vy, float vw, float vh, const float pv[8]);
extern bool g_debug_frame;  // (exec.c; set in present_hooks, read in the render thread's present)
void renderer_submit(void);
void renderer_set_scale(float sx, float sy);
uint64_t renderer_present_count(void);
void renderer_save_texture_tex(TexBackend *b, const char *path);

static uint64_t frames, total_frames, g_frame_counter;
uint64_t renderer_present_count(void) { return total_frames; }  // (the game thread's frame: the HLE hooks')
// The frame whose work is being translated (rqueue.h rq_frame): "once per frame" state of the translation. Without the
// render thread, total_frames.
uint64_t renderer_exec_frame(void) { return (rq_current_item() ? rq_frame() : total_frames) + interp_replay_salt(); }
uint64_t vk_frame_counter(void) { return renderer_exec_frame(); }  // (the translation's: g_frame_counter == total_frames)
static float g_shot_crop[4] = {0, 0, 1, 1};
static double last_fps_time;
static NvnTexture *g_last_presented;

const float *vk_shot_crop(void) { return g_shot_crop; }

void backend_present(NvnTexture *t);
void *backend_pool_push(void) { return NULL; }
void backend_pool_pop(void *p) { (void)p; }
static bool direct_repeat;  // (render thread off) backend_represent's present
void backend_represent(void) {
    if (!g_last_presented) { SDL_Delay(16); return; }
    if (rq_on()) rq_present(g_last_presented, total_frames, false);
    else { direct_repeat = true; backend_present_now(g_last_presented, total_frames); direct_repeat = false; }
}

// (LARECOMP_VK_PROF) the present's time by part, per frame in the fps line ("present/frame"): submit = the frame's
// last work submitted, acquire = frame slot + window image (waits included), scale = output scale / widescreen /
// dynamic resolution, blit = the game image into the window (FSR / blit / AA recording), overlay = menu_render (ImGui),
// end = the window image's submission + vkQueuePresentKHR, rest = test hooks and the fps line
enum { PP_SUBMIT, PP_ACQUIRE, PP_SCALE, PP_BLIT, PP_OVERLAY, PP_END, PP_REST, PP_N };
static int pp_on = -1;
static uint64_t pp_ns[PP_N], pp_frames, pp_t;
static void pp_mark(int part) {
    if (!pp_on) return;
    uint64_t t = SDL_GetTicksNS();
    pp_ns[part] += t - pp_t;
    pp_t = t;
}
static const char *pp_text(void) {
    static char s[192];
    s[0] = 0;
    if (!pp_on || !pp_frames) return s;
    double f = (double)pp_frames * 1e6;
    snprintf(s, sizeof s, " | present/frame submit %.3f acquire %.3f scale %.3f blit %.3f overlay %.3f end %.3f rest %.3f ms",
             pp_ns[PP_SUBMIT] / f, pp_ns[PP_ACQUIRE] / f, pp_ns[PP_SCALE] / f, pp_ns[PP_BLIT] / f, pp_ns[PP_OVERLAY] / f,
             pp_ns[PP_END] / f, pp_ns[PP_REST] / f);
    memset(pp_ns, 0, sizeof pp_ns);
    pp_frames = 0;
    return s;
}

// ---- frame interpolation's in-betweens (runtime/nvn/interp.c; Metal: metal.m ip_present, with a presenter thread)
// Inline: before a game frame's image, its in-betweens (between the previous frame and this one) are made - each a
// replay of the frame's lists with blended uniforms into the game's own targets - and presented, each with the
// previous image's placement; FIFO (present_mode) shows each for one refresh. The frame's image is set aside first
// (a copy) and put back before it is presented. In-betweens per frame: the refresh rate over the game's 60, less one
// (120 Hz: 1, 240 Hz: 3), from 100 Hz.
static struct { bool ok; float vx, vy, vw, vh, pv[8]; } ip_place;  // (the last image's placement in the window)
static VkImage ip_held;
static VmaAllocation ip_held_mem;
static uint32_t ip_held_w, ip_held_h;
static VkFormat ip_held_fmt;
static bool ip_hold(TexBackend *b, bool restore) {  // false: no copy (no memory)
    if (!restore && (!ip_held || ip_held_w != b->width || ip_held_h != b->height || ip_held_fmt != b->format)) {
        if (ip_held) { rec_drain(); vkDeviceWaitIdle(g_vk.dev); vmaDestroyImage(g_vk.vma, ip_held, ip_held_mem); ip_held = VK_NULL_HANDLE; }
        VkImageCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D, .format = b->format,
                                .extent = {b->width, b->height, 1}, .mipLevels = 1, .arrayLayers = 1, .samples = VK_SAMPLE_COUNT_1_BIT,
                                .tiling = VK_IMAGE_TILING_OPTIMAL, .usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                                .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED};
        VmaAllocationCreateInfo ai = {.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE};
        if (vmaCreateImage(g_vk.vma, &ci, &ai, &ip_held, &ip_held_mem, NULL) != VK_SUCCESS) { ip_held = VK_NULL_HANDLE; return false; }
        ip_held_w = b->width, ip_held_h = b->height, ip_held_fmt = b->format;
        VkCommandBuffer cb = vk_cmd();
        VkImageMemoryBarrier2 ib = {.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2, .srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                                    .dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, .dstAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT,
                                    .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED, .newLayout = VK_IMAGE_LAYOUT_GENERAL, .image = ip_held,
                                    .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
        VkDependencyInfo di = {.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &ib};
        rec_CmdPipelineBarrier2(cb, &di);
    }
    if (!ip_held) return false;
    vk_end_pass();
    VkCommandBuffer cb = vk_cmd();
    vk_full_barrier(cb);
    VkImageCopy r = {.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, .dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                     .extent = {b->width, b->height, 1}};
    if (restore) rec_CmdCopyImage(cb, ip_held, VK_IMAGE_LAYOUT_GENERAL, b->image, VK_IMAGE_LAYOUT_GENERAL, 1, &r);
    else rec_CmdCopyImage(cb, b->image, VK_IMAGE_LAYOUT_GENERAL, ip_held, VK_IMAGE_LAYOUT_GENERAL, 1, &r);
    vk_full_barrier(cb);
    return true;
}
// Presents one refresh period apart (Frame Generation on): an in-between and its frame's image presented back to back
// - within a millisecond - left their spacing to FIFO, and through the compositor (and Parsec on top) some were merged
// or dropped: 120 presents a second showed as jerky motion. Each waits until a period after the one before
// (LARECOMP_INTERP_SPACING=0: not, research).
static double ip_hz;            // (ip_mids: the refresh rate the in-betweens are made for; 0: none being made)
static double ip_rate;          // (stats: the refresh rate in use, learned or the display's - with in-betweens or not)
static bool ip_restart;         // (Frame Generation was off: nothing learned or counted before counts)
static bool ip_new_interval, ip_new_rate;  // (ip_mids, after a restart: the game's interval and the display's rate measured again)
static uint64_t ip_log_last;
static uint64_t ip_last_present;
// (stats, ip_log: where the window's presents fell short of the refresh rate - gaps over 1.5 refresh periods, waits
// that overslept by over half a period, and in-betweens not made, by reason)
enum { IP_NOT_ALLOWED, IP_LOCKED, IP_NO_HOLD, IP_NOT_PREPARED, IP_CAPPED, IP_NO_BLEND, IP_NO_GPU, IP_NREASONS };
static uint64_t ip_presents, ip_gaps, ip_overslept, ip_not_made[IP_NREASONS];
static double ip_gap_max;
static void ip_space(void) {
    static int on = -1;
    if (on < 0) on = !getenv("LARECOMP_INTERP_SPACING") || atoi(getenv("LARECOMP_INTERP_SPACING"));
    uint64_t now = plat_now_ns();
    if (on && ip_hz > 0 && ip_last_present) {
        uint64_t target = ip_last_present + (uint64_t)(0.97e9 / ip_hz);
        if (now < target && target - now < 50000000ull) {
            WAITING(SDL_DelayPrecise(target - now));
            now = plat_now_ns();
            ip_overslept += now > target && now - target > 0.5e9 / ip_hz;
        }
    }
    if (ip_hz > 0 && ip_last_present && now > ip_last_present) {
        double gap = (now - ip_last_present) / 1e6;
        ip_gaps += gap > 1.5e3 / ip_hz;
        ip_gap_max = fmax(ip_gap_max, gap);
    }
    ip_presents++;
    ip_last_present = now;
}
static void ip_log(void) {
    uint64_t now = plat_now_ns();
    if (!ip_log_last) ip_log_last = now;
    if (now - ip_log_last < 2000000000ull) return;
    double dt = (now - ip_log_last) / 1e9;
    static const char *why[IP_NREASONS] = {"cut", "busy", "no copy", "not prepared", "lower display rate", "no blend", "no GPU time"};
    char nm[200] = "";
    int o = 0;
    for (int i = 0; i < IP_NREASONS; i++)
        if (ip_not_made[i]) o += snprintf(nm + o, sizeof nm - (size_t)o, " %s %llu", why[i], (unsigned long long)ip_not_made[i]);
    if (ip_rate > 0 || o)
        LOG("[interp] presents %.1f/s for %.0f Hz, gaps over 1.5 refreshes %llu (longest %.1f ms), waits overslept %llu | frames without in-betweens:%s\n",
            ip_presents / dt, ip_rate, (unsigned long long)ip_gaps, ip_gap_max, (unsigned long long)ip_overslept, o ? nm : " none");
    ip_presents = ip_gaps = ip_overslept = 0, ip_gap_max = 0;
    memset(ip_not_made, 0, sizeof ip_not_made);
    ip_log_last = now;
}
static void ip_present_mid(TexBackend *b) {  // an in-between in b (the game's image), placed as the last image
    static const float black[4] = {0, 0, 0, 1};
    extern bool vk_ip_mid;  // (exec.c: its GPU time is the in-between's)
    vk_ip_mid = true;
    if (vk_frame_begin(black)) {
        VkCommandBuffer cb = vk_cmd();
        vk_present_draw(cb, b, ip_place.vx, ip_place.vy, ip_place.vw, ip_place.vh, ip_place.pv);
        menu_render((void *)cb, g_vk.sc_extent.width, g_vk.sc_extent.height);
        ip_space();
        vk_frame_end(NULL);
    }
    vk_ip_mid = false;
}
static int ip_mids(TexBackend *b, uint64_t frame) {  // (the in-betweens shown)
    static uint64_t prev_ns;
    static double interval_ns;  // (smoothed: the game's frame interval)
    static double win_sum, win_max;  // (the raw intervals of the current 30-frame check)
    uint64_t now = plat_now_ns();
    if (ip_new_interval) prev_ns = 0, interval_ns = 0, ip_new_interval = false;
    if (prev_ns && now > prev_ns) {
        double d = (double)(now - prev_ns);
        interval_ns = interval_ns > 0 && d < 4 * interval_ns ? interval_ns + (d - interval_ns) * 0.1 : d;
        win_sum += d, win_max = fmax(win_max, d);
    }
    prev_ns = now;
    if (!ip_place.ok || interval_ns <= 0 || !b || !b->image || b->depth > 1 || b->layers > 1) return 0;
    // The refresh rate the window's presents really get: the display's own (SDL), unless the frames with in-betweens
    // showed fewer - a virtual display (Parsec: 120 Hz reported, presents paced at 60) held each image two refreshes
    // and the game fell to its 30 fps mode. Learned from those frames, kept while the display reports the same rate.
    // In-betweens only from 100 Hz: never at the cost of the game's own 60.
    // (gameplay only, three 30-frame checks in a row, each steady - no frame over 1.5x the check's average: a movie's,
    // a load's or a resolution change's hitches aren't the display. A cap is tried again after 10 s of the game at its
    // 60 - 20 s, 40 s... up to 5 min each time it comes back: an 8x resolution scale's slow frames learned 61 Hz once,
    // and nothing measured again.)
    // LARECOMP_INTERP_HZ=<hz> (tests): that refresh rate, nothing learned.
    static double hz_seen, hz_cap, hz_env = -1;
    static int mids_last, settle, slow_checks, full_rate, retry_after = 600;
    if (hz_env < 0) hz_env = getenv("LARECOMP_INTERP_HZ") ? atof(getenv("LARECOMP_INTERP_HZ")) : 0;
    if (ip_new_rate) hz_seen = 0, mids_last = 0, ip_new_rate = false;  // (a learned rate starts over: logged and measured again)
    SDL_DisplayID did = SDL_GetDisplayForWindow(window);
    const SDL_DisplayMode *dm = did ? SDL_GetCurrentDisplayMode(did) : NULL;
    double hz = dm && dm->refresh_rate > 1 ? dm->refresh_rate : 60;
    if (hz != hz_seen) {
        LOG("[interp] display %u: %.2f Hz (SDL)%s\n", (unsigned)did, hz, hz_env > 0 ? " - LARECOMP_INTERP_HZ overrides it" : "");
        hz_seen = hz, hz_cap = 0, settle = slow_checks = full_rate = 0, retry_after = 600;
    }
    if (hz_env > 0) hz = hz_env;
    extern int game_in_play(void);      // hle/warp.c
    extern bool world_on_screen(void);  // hle/mirror.c
    bool playing = hz_env <= 0 && game_in_play() && world_on_screen();
    if (playing && mids_last > 0 && ++settle >= 30) {  // (30 frames with in-betweens)
        double expect = (mids_last + 1) * 1e9 / (hz_cap > 0 ? fmin(hz, hz_cap) : hz), mean = win_sum / settle;
        bool steady = win_max < 1.5 * mean;
        settle = 0;
        slow_checks = steady && mean > 1.3 * expect ? slow_checks + 1 : 0;
        if (slow_checks >= 3) {
            hz_cap = (mids_last + 1) * 1e9 / mean;
            slow_checks = full_rate = 0;
            static int nlog;
            if (nlog++ < 10) LOG("[interp] the display shows %.0f images a second, not %.0f: in-betweens for %.0f Hz\n", hz_cap, hz, hz_cap);
        }
    }
    if (!playing || mids_last == 0 || settle == 0) win_sum = win_max = 0;  // (a check counts its own 30 frames)
    if (hz_cap > 0 && hz_cap < hz) {  // (trying the display's rate again)
        full_rate = playing && interval_ns < 1.1e9 / 60 ? full_rate + 1 : 0;
        if (full_rate >= retry_after) {
            LOG("[interp] trying %.0f Hz again (the game held 60 for %d s)\n", hz, retry_after / 60);
            hz_cap = 0, full_rate = settle = slow_checks = 0;
            retry_after = retry_after * 2 > 18000 ? 18000 : retry_after * 2;
        }
    }
    if (hz_env <= 0 && hz_cap > 0 && hz_cap < hz) hz = hz_cap;
    // (counted for the game's own 60, not the measured interval: a game slowed to its 30 fps mode got more in-betweens,
    // which held it at 30)
    int mids = hz >= 100 ? (int)lround(hz / 60.0) - 1 : 0;
    if (mids > 3) mids = 3;
    ip_rate = hz;
    if (mids < 1) { ip_not_made[IP_CAPPED] += hz_cap > 0; mids_last = 0; ip_hz = 0; return 0; }
    if (mids != mids_last) settle = slow_checks = 0;
    mids_last = mids;
    // The GPU's time goes to the game's frames first (as on Metal, metal.m): in-betweens only in what a game frame
    // leaves of its interval - the game's own GPU time (pace.c; the in-betweens' submissions aren't counted there,
    // exec.c) against an in-between's, replay and window image. Fewer than the refresh rate wants: evenly spaced, each
    // shown longer. LARECOMP_INTERP_GPU_BUDGET=0: all made (research).
    int allowed = mids;
    {
        static int budget_on = -1;
        if (budget_on < 0) { const char *e = getenv("LARECOMP_INTERP_GPU_BUDGET"); budget_on = !e || atoi(e); }
        extern double pace_gpu_frame_ms(void);
        double game = pace_gpu_frame_ms(), img = interp_image_gpu_ms();
        double D = fmin(fmax(interval_ns / 1e6, 16.667), 50);
        if (budget_on && game >= 0 && img > 0) {
            double left = 0.9 * D - game;
            allowed = left > 0 ? (int)(left / img) : 0;
            if (allowed > mids) allowed = mids;
        }
    }
    for (int m = allowed; m < mids; m++) interp_note_no_gpu();
    if (allowed < 1) { ip_not_made[IP_NO_GPU]++; ip_hz = 0; return 0; }
    ip_hz = hz * (allowed + 1) / (mids + 1);  // (presents spaced for the images made)
    if (!interp_replays_allowed(frame, total_frames, now)) { ip_not_made[IP_NOT_ALLOWED]++; return 0; }
    if (!interp_replay_lock()) { ip_not_made[IP_LOCKED]++; return 0; }
    int made = 0;
    if (ip_hold(b, false)) {
        extern void renderer_interp_replay(void);
        for (int m = 1; m <= allowed; m++) {
            if (!interp_prepare((float)m / (float)(allowed + 1))) break;
            renderer_interp_replay();
            ip_present_mid(b);
            made++;
        }
        ip_hold(b, true);
        if (!made) ip_not_made[IP_NOT_PREPARED]++;
    } else ip_not_made[IP_NO_HOLD]++;
    interp_replay_unlock();
    return made;
}

// The GPU side of a present (the render thread when it's on, rqueue.c): the present pass, the overlay, shots.
// frame: the number of this present (the game thread's frame when it was queued).
void backend_present_now(NvnTexture *t, uint64_t frame) {
    if (pp_on < 0) pp_on = getenv("LARECOMP_VK_PROF") != NULL;
    if (pp_on) pp_t = SDL_GetTicksNS();
    pc_save_now_and_then();
    TexBackend *b = vk_tex(t, true);
    b->presented = true;
    vk_end_pass();
    // the frame's work goes to the GPU now, in a submission of its own: it starts while we wait for the window's image.
    // Its GPU time is counted, and the window image's submission's from after its wait (exec.c gpu_timer_add)
    renderer_submit();
    pp_mark(PP_SUBMIT);
    {   // frame interpolation (runtime/nvn/interp.c): the frame's draws matched with the previous frame's and logged,
        // and with it on, its in-betweens presented (ip_mids)
        bool repeat = rq_on() ? rq_present_is_repeat() : direct_repeat;
        int want = wp_enabled() ? INTERP_OFF : interp_wanted();  // (the first-person presenter instead: wp_enabled)
        if (!repeat) {
            if (want == INTERP_ON && ip_restart) {  // (turned on again: the stats from now, and measured again)
                ip_restart = false, ip_new_interval = ip_new_rate = true;
                ip_last_present = ip_log_last = 0;
                ip_presents = ip_gaps = ip_overslept = 0, ip_gap_max = 0;
                memset(ip_not_made, 0, sizeof ip_not_made);
            }
            bool blend = interp_frame_end(want != INTERP_OFF);
            if (want == INTERP_ON) {
                if (blend) interp_note_images(ip_mids(b, frame));
                else ip_not_made[IP_NO_BLEND]++, interp_note_images(0);
            }
            interp_frame_rotate();
            if (want != INTERP_OFF) interp_log_stats();
            if (want == INTERP_ON) ip_log();
            else ip_restart = true, ip_hz = ip_rate = 0;  // (off: the next on starts fresh)
        }
    }
    static const float black[4] = {0, 0, 0, 1};
    uint64_t t0 = SDL_GetTicksNS();
    bool have = vk_frame_begin(black);
    pp_mark(PP_ACQUIRE);
    uint64_t nd = SDL_GetTicksNS() - t0;
    if (nd > 30000000) {
        static int warned;
        if (warned++ < 20) LOG("[vk] acquire blocked %llums\n", (unsigned long long)(nd / 1000000));
    }
    if (have) {
        double dw = g_vk.sc_extent.width, dh = g_vk.sc_extent.height;
        extern void widescreen_frame_end(uint64_t frame);
        widescreen_frame_end(frame);
        extern void widescreen_set_factor(float f);
        extern bool widescreen_allowed(void);
        // render scale + placement in the window: runtime/nvn/output_scale.c (Video tab "Internal resolution");
        // GPU time since the last frame (exec.c's GPU timer; -1 without one: dynamic resolution goes by frame times):
        // the game's submissions and the window image's (LARECOMP_DYNRES_COUNT_PRESENT=0: the game's only, as before)
        static int count_present = -1;
        if (count_present < 0) count_present = !getenv("LARECOMP_DYNRES_COUNT_PRESENT") || atoi(getenv("LARECOMP_DYNRES_COUNT_PRESENT"));
        static uint64_t sp_prev[4], wt_prev[4], cw_prev;
        static bool sp_have;
        uint64_t sp[4], wt[4];
        for (int i = 0; i < 4; i++) wt[i] = __atomic_load_n(&g_vk_frame_waits[i], __ATOMIC_RELAXED);  // ([2], [3]: the recorder's)
        double game_ms = -1, win_ms = -1;
        if (vk_gpu_split_ns(sp)) {
            uint64_t cw = vk_cum_wait_ns();
            if (sp_have) {
                double game = (sp[0] - sp_prev[0]) / 1e6, win = (sp[1] - sp_prev[1]) / 1e6;
                game_ms = game, win_ms = count_present ? win : -1;
                double slotw = (wt[0] - wt_prev[0]) / 1e6;
                DynSplit ds = {game, win, (sp[2] - sp_prev[2]) / 1e6, (double)(sp[3] - sp_prev[3]), slotw,
                               (wt[1] - wt_prev[1]) / 1e6, (wt[2] - wt_prev[2]) / 1e6,
                               fmax(0, (cw - cw_prev) / 1e6 - slotw), count_present != 0};
                output_scale_note_split(&ds);
            }
            memcpy(sp_prev, sp, sizeof sp);
            memcpy(wt_prev, wt, sizeof wt_prev);
            cw_prev = cw;
            sp_have = true;
        }
        OutScale os;
        // (a headset, the view from his eyes off - the view from above on its screen: the game at its own 16:9, the
        //  screen cropped to it. Widened to the window, an eye's shape - 1.19:1 - the game's camera made a flattened band
        //  with black above and below it: « tout est aplati, énorme marge en haut et en bas », Guillaume, 27/10)
        extern bool fp_view_on(void);
        bool wide = widescreen_allowed() && !(vk_xr_headset() && !fp_view_on());
        output_scale(dw, dh, wide, SDL_GetTicksNS() / 1e9, game_ms, win_ms, &os);
        double sx = os.sx, sy = os.sy, vx = os.vx, vy = os.vy, vw = os.vw, vh = os.vh, shape = os.shape;
        output_scale_log((unsigned long long)frame, dw, dh, &os);  // (diagnostics: [scale] on a change)
        // interiors / dungeons: their 16:9 centre, or the part inside a room wider than one screen
        extern void widescreen_present_crop(uint64_t, double, float *, double *, double *, double *, double *);
        float crop[4];  // uv origin and size
        widescreen_present_crop(frame, shape, crop, &vx, &vy, &vw, &vh);
        widescreen_set_factor((float)shape);
        extern void renderer_set_alloc_scale(float ax, float ay);
        renderer_set_alloc_scale((float)os.ax, (float)os.ay);
        renderer_set_scale((float)sx, (float)sy);
        pp_mark(PP_SCALE);
        if (getenv("LARECOMP_VK_RES_LOG") && total_frames % 120 == 0)
            LOG("[resolution] frame %llu guest %ux%u backing %ux%u crop %.3f %.3f %.3f %.3f viewport %.0fx%.0f output %.0fx%.0f scale %.3fx%.3f aa %d upscaler %d\n",
                (unsigned long long)total_frames, t->width, t->height, b->width, b->height,
                crop[0], crop[1], crop[2], crop[3], vw, vh, dw, dh, sx, sy,
                g_settings.antialias, g_settings.upscaler);
        VkCommandBuffer cb = vk_cmd();
        float pv[8] = {renderer_origin_lower_left() ? 1.0f : 0.0f, 0, 0, 0, crop[0], crop[1], crop[2], crop[3]};
        memcpy(g_shot_crop, crop, sizeof crop);
        ip_place.ok = true, ip_place.vx = (float)vx, ip_place.vy = (float)vy, ip_place.vw = (float)vw, ip_place.vh = (float)vh;
        memcpy(ip_place.pv, pv, sizeof ip_place.pv);
        extern bool fp_screen_black(void);  // (first person: black at a door, not the room from above - firstperson.c)
        if (vk_xr_headset() || !fp_screen_black()) vk_present_draw(cb, b, (float)vx, (float)vy, (float)vw, (float)vh, pv);
        pp_mark(PP_BLIT);
        static long menu_at = -1;
        if (menu_at < 0) menu_at = getenv("LARECOMP_MENU_AT") ? atol(getenv("LARECOMP_MENU_AT")) : 0;
        if (menu_at && (long)frame + 1 == menu_at) { extern void menu_test_open(void); menu_test_open(); }
        // (ImGui records through vk_overlay_begin / vk_overlay_end, when it draws anything)
        // (offscreen: drawn too where the app shows the image itself and says so - the Android tablet's shell sets
        //  LARECOMP_OFFSCREEN_MENU=1; the menu and its on-screen controller were never drawn there)
        static int off_menu = -1;
        if (off_menu < 0) off_menu = getenv("LARECOMP_OFFSCREEN_MENU") && atoi(getenv("LARECOMP_OFFSCREEN_MENU")) != 0;
        if (!offscreen() || off_menu) menu_render((void *)cb, g_vk.sc_extent.width, g_vk.sc_extent.height);
        pp_mark(PP_OVERLAY);
        static long win_shot = -1;
        if (win_shot < 0) win_shot = getenv("LARECOMP_WINDOW_SHOT") ? atol(getenv("LARECOMP_WINDOW_SHOT")) : 0;
        bool shot = win_shot && (long)frame + 1 == win_shot;
        ip_space();
        vk_frame_end(shot ? (getenv("LARECOMP_SHOT_PATH") ? getenv("LARECOMP_SHOT_PATH") : "build/window.png") : NULL);
        if (shot && getenv("LARECOMP_EXIT_AFTER_SHOT")) exit_after_shot();
    } else {
        renderer_submit();
    }
    pp_mark(PP_END);
    pp_frames++;
    extern void vk_prof_frame(uint64_t frame, uint64_t present_ns);
    vk_prof_frame(frame, SDL_GetTicksNS() - t0);
    frames++;
    static long shot_every = -1;
    if (shot_every < 0) { const char *e = getenv("LARECOMP_SCREENSHOT_EVERY"); shot_every = e ? atol(e) : 0; }
    {   // LARECOMP_GPUPROF_AT=<frame>[,<frame>...]: GPU time per pass of those frames ("[gpuprof]" in the log)
        extern bool g_gpuprof_frame;
        extern void vk_gpuprof_report(void);
        if (g_gpuprof_frame) { g_gpuprof_frame = false; vk_gpuprof_report(); }
        // LARECOMP_GPUPROF_FILE=<path>: on demand - when that file appears (checked once a second) it is removed and
        // 10 frames half a second apart are profiled (an A/B at one spot: touch the file while standing still)
        static const char *pf;
        static int pf_init;
        static uint64_t pf_next, pf_left;
        if (!pf_init) { pf_init = 1; pf = getenv("LARECOMP_GPUPROF_FILE"); }
        if (pf && !pf_left && frame % 60 == 0 && access(pf, F_OK) == 0) {
            unlink(pf);
            pf_left = 10, pf_next = frame + 1;
            LOG("[gpuprof] on demand: 10 frames from %llu\n", (unsigned long long)pf_next);
        }
        if (pf_left && frame == pf_next) { g_gpuprof_frame = true; pf_left--; pf_next = frame + 30; }
        const char *e = getenv("LARECOMP_GPUPROF_AT");
        for (const char *p = e; p && *p;) {
            long at = strtol(p, (char **)&p, 10);
            if (at == (long)frame) { g_gpuprof_frame = true; break; }
            if (*p == ',') p++; else break;
        }
    }
    if (g_debug_frame) LOG("===== debug frame %llu =====\n", (unsigned long long)frame);
    {   // LARECOMP_SHOT_AT=<frame>,<frame>,... (ascending): the game image at those frames -> build/frames/frame_N.png
        static const char *next;
        static int init;
        if (!init) { init = 1; next = getenv("LARECOMP_SHOT_AT"); if (next) mkdir("build", 0755), mkdir("build/frames", 0755); }
        while (next && *next) {
            long at = strtol(next, (char **)&next, 10);
            if ((long)frame < at) { while (next > getenv("LARECOMP_SHOT_AT") && next[-1] != ',') next--; break; }
            if ((long)frame == at) {
                char path[256];
                snprintf(path, sizeof path, "build/frames/frame_%05llu.png", (unsigned long long)frame);
                renderer_save_texture_tex(b, path);
            }
            if (*next == ',') next++;
        }
    }
    static long shot_from = -1;
    if (shot_from < 0) { const char *e = getenv("LARECOMP_SCREENSHOT_FROM"); shot_from = e ? atol(e) : 0; }
    if (shot_every && frame % shot_every == 0 && (long)frame >= shot_from) {
        char path[256];
        if (shot_from) mkdir("build/frames", 0755);
        snprintf(path, sizeof path, shot_from ? "build/frames/frame_%05llu.png" : "build/frame_%05llu.png",
                 (unsigned long long)frame);
        renderer_save_texture_tex(b, path);
        // a series (SCREENSHOT_FROM) runs on until LARECOMP_WINDOW_SHOT exits
        if (getenv("LARECOMP_EXIT_AFTER_SHOT") && !shot_from) exit_after_shot();
    }
    double now = SDL_GetTicks() / 1000.0;
    static double fps_iv = -1, prev_present, max_gap;
    if (fps_iv < 0) fps_iv = getenv("LARECOMP_FPS_INTERVAL") ? atof(getenv("LARECOMP_FPS_INTERVAL")) : 2.0;
    if (prev_present > 0 && now - prev_present > max_gap) max_gap = now - prev_present;
    static double hitch_ms = -1;  // LARECOMP_LOG_HITCH_MS=<ms>: every present gap over that (tests: hitches per run)
    if (hitch_ms < 0) hitch_ms = getenv("LARECOMP_LOG_HITCH_MS") ? atof(getenv("LARECOMP_LOG_HITCH_MS")) : 0;
    if (hitch_ms > 0 && prev_present > 0 && (now - prev_present) * 1000 > hitch_ms)
        LOG("[hitch] t=%.2f frame %llu gap %.0f ms\n", now, (unsigned long long)frame, (now - prev_present) * 1000);
    prev_present = now;
    pp_mark(PP_REST);  // (the fps line itself is not counted)
    if (now - last_fps_time >= fps_iv) {
        extern const char *game_level_name(void);
        if (last_fps_time > 0)
            LOG("[vk] t=%.1f frame %llu %.1f fps max %.0fms level %s (%s%s)\n", now, (unsigned long long)frame,
                frames / (now - last_fps_time), max_gap * 1000.0, game_level_name(), renderer_stats(), pp_text());
        max_gap = 0;
        frames = 0;
        last_fps_time = now;
    }
    static double mem_at = 60.0;  // (memory in steady state: a minute in, then every 10 minutes)
    if (now >= mem_at) { vk_log_memory("playing"); mem_at = now + 600.0; }
}

// The game thread's side of a present: the frame counters and the per-frame hooks (input, pacing, test hooks).
static void present_hooks(void) {
    total_frames++;
    g_frame_counter++;
    pace_frame();  // (pace.c: the game's 30 fps mode when frames don't fit 60)
    extern void input_frame(uint64_t frame);
    input_frame(total_frames);
    static long slow_ms = -1;  // LARECOMP_SIM_SLOW_FRAME_MS=<ms>: sleep per presented frame (simulate a slow machine)
    if (slow_ms < 0) slow_ms = getenv("LARECOMP_SIM_SLOW_FRAME_MS") ? atol(getenv("LARECOMP_SIM_SLOW_FRAME_MS")) : 0;
    static long slow_until = -1;  // LARECOMP_SLOW_FRAME_UNTIL=<frame>: the stall ends there (frame pacing recovery tests)
    if (slow_until < 0) slow_until = getenv("LARECOMP_SLOW_FRAME_UNTIL") ? atol(getenv("LARECOMP_SLOW_FRAME_UNTIL")) : 0;
    if (slow_ms > 0 && total_frames > 2600 && (!slow_until || (long)total_frames < slow_until)) SDL_Delay((Uint32)slow_ms);
    extern void fnprof_frame(uint64_t frame);
    fnprof_frame(total_frames);
    extern void warp_frame(uint64_t frame);
    warp_frame(total_frames);
    extern void reboot_test_frame(uint64_t frame);  // runtime/main.c (LARECOMP_REBOOT_AT)
    reboot_test_frame(total_frames);
    extern void save_editor_frame(uint64_t frame);
    save_editor_frame(total_frames);
    extern void vk_level_change_check(void);
    vk_level_change_check();
    static long debug_at = -1;
    if (debug_at < 0) { const char *e = getenv("LARECOMP_DEBUG_FRAME"); debug_at = e ? atol(e) : 0; }
    extern volatile int g_debug_frame_request;
    if (g_debug_frame_request) { g_debug_frame_request = 0; debug_at = (long)total_frames + 1; }
    // LARECOMP_DEBUG_FRAMES=<n> (with LARECOMP_DEBUG_FRAME / F9): dump n consecutive frames (build/pass_<frame>_NNN.png)
    static long debug_n = -1;
    if (debug_n < 0) { const char *e = getenv("LARECOMP_DEBUG_FRAMES"); debug_n = e && atol(e) > 1 ? atol(e) : 1; }
    g_debug_frame = debug_at && total_frames >= (uint64_t)debug_at && total_frames < (uint64_t)debug_at + (uint64_t)debug_n;
}

void backend_present(NvnTexture *t) {
    g_last_presented = t;
    uint64_t frame = total_frames, t0 = plat_now_ns();
    if (rq_on()) rq_present(t, frame, true);
    else backend_present_now(t, frame);
    uint64_t t1 = plat_now_ns();
    g_present_ns += t1 - t0;  // (LARECOMP_LOG_PACE, pace.c)
    present_hooks();
    g_hooks_ns += plat_now_ns() - t1;
}

// ================================================================== UI thread
void vk_exec_init(void);
void vk_shaders_init(void);

// The end of a validation run (LARECOMP_VK_VALIDATION, after backend_quit: the GPU idle, the pipeline cache kept):
// the debug messenger goes, then the process ends at once - no vkDestroyDevice / vkDestroyInstance, no atexit
// handlers, no DLL teardown. _exit() alone isn't enough on Windows: it ends in ExitProcess, which runs the layer's
// and the driver's DLL teardown after every other thread is gone, and with the Khronos layer on NVIDIA that left the
// process hung there and unkillable (one thread waiting, gigabytes of the layer's state).
static _Noreturn void validation_exit(void) {
    if (messenger) {
        PFN_vkDestroyDebugUtilsMessengerEXT f =
            (PFN_vkDestroyDebugUtilsMessengerEXT)vkGetInstanceProcAddr(g_vk.inst, "vkDestroyDebugUtilsMessengerEXT");
        VkDebugUtilsMessengerEXT m = messenger;
        messenger = VK_NULL_HANDLE;
        if (f) f(g_vk.inst, m, NULL);
    }
    LOG("[vk] validation run: ending without GPU teardown\n");
    fflush(stdout);
    fflush(stderr);
#if defined(_WIN32) || defined(__linux__)
    plat_terminate();
#endif
    _Exit(0);
}

// LARECOMP_EXIT_AFTER_SHOT: the test run ends with its shot. As it always did (_exit: no cache save), except under
// validation: backend_quit, then validation_exit.
static void exit_after_shot(void) {
    fflush(stderr);
    if (validating) {
        backend_quit();
        validation_exit();
    }
    _exit(0);
}

// Quitting, before exit() (the window closed; also from exit() itself, quick_exit_handler): the render thread stops
// between items, so no image is acquired or presented any more; the recorder thread stops making driver calls (the
// packets it still had are dropped, record.c rec_quit) and is idle; then the GPU finishes and the pipeline cache is
// kept. All of it before exit() runs the atexit handlers and the libraries' destructors: a Vulkan layer loaded with
// the instance (the Steam overlay on the Steam Deck, #40) tears its swapchain state down there, and a present from
// the render thread after that crashed in vkQueuePresentKHR. The swapchain, surface and window are never destroyed
// explicitly: the process ends with them.
void backend_quit(void) {
    static atomic_flag once = ATOMIC_FLAG_INIT;
    if (atomic_flag_test_and_set(&once)) return;
    static int trace = -1;
    if (trace < 0) trace = getenv("LARECOMP_VK_TRACE") != NULL;
    if (trace) LOG("[vk] quit: stopping the render thread\n");
    rq_stop();
    if (trace) LOG("[vk] quit: stopping the recorder\n");
    rec_quit();  // (what it still had is dropped, not run: no submission or present after this; the queue idle for the wait)
    if (!g_vk.dev) return;
    if (trace) LOG("[vk] quit: waiting for the GPU\n");
    vkDeviceWaitIdle(g_vk.dev);
    if (trace) LOG("[vk] quit: saving the pipeline cache\n");
    vk_pipeline_cache_save();
}

// exit() from any thread: backend_quit (when the window close didn't already), then end the process without the DLL
// teardown (driver / validation-layer threads can stall it for minutes while the game's threads still run).
static void quick_exit_handler(void) {
    backend_quit();
    if (validating) validation_exit();
    fflush(stdout);
    fflush(stderr);
#if defined(_WIN32) || defined(__linux__)
    plat_terminate();
#endif
}

// the app in the background with the game paused (phones; runtime/kernel.c's hang watchdog asks): never here
bool ui_in_background(void) { return false; }

void ui_window_init(void) {
    if (window || ((offscreen() || shell_window()) && g_vk.dev)) return;
    atexit(quick_exit_handler);
    settings_load();
    if (shell_window()) {  // (a flat shell's window, no SDL one: vk_xr.h surface)
        create_instance();
        g_vk.surface = g_vk_xr->surface(g_vk.inst);
        if (!g_vk.surface) FATAL("the shell gave no window surface\n");
        pick_device();
        create_device();
        create_swapchain();
        vk_exec_init();
        vk_shaders_init();
        return;
    }
    if (offscreen()) {
        create_instance();
        pick_device();
        create_device();
        create_offscreen();
        vk_exec_init();
        vk_shaders_init();
        // (Inside The Dream, PC VR: the PC's own controllers play too, and the menu's events from the headset's
        //  controllers - android/xr_app.c menu_pad - are read: no window, so no focus - their events in the background)
        SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");
        if (!SDL_InitSubSystem(SDL_INIT_GAMEPAD)) LOG("[vk] offscreen: no gamepads (%s)\n", SDL_GetError());
        return;
    }
    input_apply_background();  // controllers in the background: "Allow background inputs" (Controls tab)
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD)) FATAL("SDL_Init: %s\n", SDL_GetError());
    input_load_user_mappings();
    int win_w = 1280, win_h = 720;
    if (getenv("LARECOMP_WINDOW")) sscanf(getenv("LARECOMP_WINDOW"), "%dx%d", &win_w, &win_h);
    else rt_window_size(&win_w, &win_h);
    SDL_WindowFlags wflags = SDL_WINDOW_VULKAN | SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY;
#ifdef __ANDROID__
    wflags |= SDL_WINDOW_FULLSCREEN;  // (the whole screen, system bars hidden)
#endif
    if (getenv("LARECOMP_HIDDEN")) wflags |= SDL_WINDOW_HIDDEN;
    bool show_later = false;
#ifndef __ANDROID__
    // (opened hidden, shown once Vulkan, its swapchain and the shaders' index are ready, full screen if it's to be: an
    //  empty window with Windows's white frame sat on screen for a second before the logo, 12/11/2026)
    if (!(wflags & SDL_WINDOW_HIDDEN)) wflags |= SDL_WINDOW_HIDDEN, show_later = true;
#endif
    {   // scripted test runs (user input ignored: runtime/hle/input.c input_user_blocked) open without taking the
        // keyboard focus from whatever the player is doing
        const char *v = getenv("LARECOMP_NO_USER_INPUT");
        bool scripted = v ? atoi(v) != 0 : (getenv("LARECOMP_INPUT") || getenv("LARECOMP_DOORJUMP_TEST") || getenv("LARECOMP_PLACE") ||
                                            getenv("LARECOMP_HOST_INPUT") || getenv("LARECOMP_BIND_TEST"));
        if (scripted) SDL_SetHint(SDL_HINT_WINDOW_ACTIVATE_WHEN_SHOWN, "0");
    }
    window = SDL_CreateWindow("Inside The Dream", win_w, win_h, wflags);
#ifdef __ANDROID__
    // opened with the screen locked / off: no surface to make the window on yet; it comes once the activity shows.
    // SDL_PumpEvents sleeps while the app is paused, so the 5 s count only time in the foreground, and a window that
    // fails for another reason still shows the error below. (From mamcjr's LASRecompAndroid, MIT.)
    for (int tries = 0; !window && tries < 20 * 5; tries++) {
        if (tries == 0) LOG("[vk] waiting for the Android window (%s)\n", SDL_GetError());
        SDL_PumpEvents();
        SDL_Delay(50);
        window = SDL_CreateWindow("LAS Recomp", win_w, win_h, wflags);
    }
#endif
    if (!window) FATAL("SDL_CreateWindow: %s\n", SDL_GetError());
    rt_window_icon(window);
    rt_window_remember(window);
    // the Fullscreen setting applies from the start screen on (not only once the game runs); test windows stay windows
#ifdef __ANDROID__
    SDL_SetWindowFullscreen(window, true);  // (always: there is no window to go back to)
#else
    if (g_settings.fullscreen && !getenv("LARECOMP_WINDOW") && !getenv("LARECOMP_HIDDEN")) SDL_SetWindowFullscreen(window, true);
#endif
    create_instance();
    if (!SDL_Vulkan_CreateSurface(window, g_vk.inst, NULL, &g_vk.surface)) FATAL("SDL_Vulkan_CreateSurface: %s\n", SDL_GetError());
    pick_device();
    create_device();
    create_swapchain();
    vk_exec_init();
    vk_shaders_init();
    if (show_later) SDL_ShowWindow(window);
}

SDL_Window *ui_window(void) { return window; }
bool ui_window_focused(void) { return window && (SDL_GetWindowFlags(window) & SDL_WINDOW_INPUT_FOCUS); }  // (any thread, for a log)

void backend_init(void) {
    while (!atomic_load(&ui_ready)) SDL_Delay(1);
    LOG("[vk] %s, swapchain %ux%u\n", g_vk.props.deviceName, g_vk.sc_extent.width, g_vk.sc_extent.height);
}

// ---- in-place reboot (runtime/main.c teardown; main thread, guest threads stopped)
void backend_reset(void) {
    if (!g_vk.dev) return;
    rq_drain();  // (the render thread: everything queued ran; it idles while the game is down)
    rec_drain();  // (the recorder: everything the render thread queued is recorded and submitted)
    vkDeviceWaitIdle(g_vk.dev);
    extern void vk_textures_reset(void);
    extern void vk_exec_reset(void);
    vk_textures_reset();
    vk_exec_reset();
    g_last_presented = NULL;
}
void backend_guest_memory_reset(void) {}  // (exec.c re-imports the heap on first use)
void backend_clear_window(void) {
    if (rq_stopping()) return;
    rq_drain();
    static const float black[4] = {0, 0, 0, 1};
    if (g_vk.dev && vk_frame_begin(black)) vk_frame_end(NULL);
}

int ui_run(void) {
    ui_window_init();
    atomic_store(&ui_ready, true);
    for (;;) {
        if (rt_reboot_requested()) return rt_reboot_requested();  // (runtime/main.c tears the game down)
        if (shell_window()) { SDL_Delay(16); continue; }  // (no SDL window, no events: the shell's or scripted input)
        if (offscreen()) {  // (PC VR: no window - the PC's controllers and the menu's queued events, read here)
            SDL_Event e;
            while (SDL_WaitEventTimeout(&e, 16)) {
                if (e.type == SDL_EVENT_QUIT) { backend_quit(); exit(0); }
                extern bool input_user_blocked(const SDL_Event *e);
                if (input_user_blocked(&e)) continue;
                bool pad = e.type >= SDL_EVENT_GAMEPAD_AXIS_MOTION && e.type <= SDL_EVENT_GAMEPAD_STEAM_HANDLE_UPDATED;
                if (pad && !g_settings.itd_vr_pad && e.gbutton.which != 0) continue;  // (the start screen's "PC controller: no")
                if (menu_handle_event(&e)) continue;
                input_handle_event(&e);
            }
            input_rumble_poll();
            input_pad_poll();
            continue;
        }
        SDL_Event e;
        while (SDL_WaitEventTimeout(&e, 16)) {
            input_rumble_poll();
#ifdef __ANDROID__
            // the app may not come back from the background (the system ends it there): keep the pipeline cache and
            // the settings (options changed with the menu still open were lost). From LASRecomp-Android-Plus (MIT).
            if (e.type == SDL_EVENT_WILL_ENTER_BACKGROUND && g_vk.dev) {
                vk_pipeline_cache_save();
                settings_save();
            }
#endif
            if (e.type == SDL_EVENT_QUIT) {
                input_rumble_stop();
                input_led_stop();
                LOG("window closed\n");
                backend_quit();  // (before exit(): see there)
                exit(0);
            }
            extern bool input_user_blocked(const SDL_Event *e);
            if (input_user_blocked(&e)) continue;  // scripted test runs: the real keyboard / mouse / pads do nothing (as metal.m)
            input_cursor_event(&e);
            if (menu_handle_event(&e)) continue;
            input_handle_event(&e);
        }
        menu_apply_window_requests(window);
        extern void fp_mouse_window(SDL_Window *w);  // (first-person view: the mouse captured to look, as metal.m)
        fp_mouse_window(window);
        input_rumble_poll();
        input_led_poll();
        input_pad_poll();
    }
}
