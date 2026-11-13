// xr_app.c: the Quest 3 app - LASRecomp in the headset, as a NativeActivity (no Java of its own).
//
// The game runs exactly as on the desktop, in the Vulkan backend's offscreen mode (LARECOMP_OFFSCREEN): this shell
// creates the Vulkan instance and device through the OpenXR runtime (runtime/nvn/vulkan/vk_xr.h), copies each
// finished frame into an OpenXR swapchain shown as a screen floating in front of the player (a quad layer, which the
// compositor keeps steady at the display's rate whatever the game's), and turns the Touch controllers into the
// Switch pad; the sound goes out through AAudio (android/aaudio_sdl.c). The first-person view follows the head: each
// frame is rendered from the head's pose (fp_xr_view) and shown as a projection layer, the compositor turning it with
// the head at the display's rate; one image for both eyes, no depth yet (A-FAIRE/lasrecomp-quest3-appsw.md). Both
// sticks clicked together switch to the floating screen and back.
//
// The game files are the player's own dump, pushed over adb to the app's folder
// (/sdcard/Android/data/com.lasrecomp.quest/files): extracted/exefs, extracted/romfs.bin,
// extracted/romfs/region_common/movie/*.webm, Arial.ttf, and optionally save/. The log goes to lasrecomp.log there,
// and env.txt there (KEY=VALUE lines) sets runtime options for a test without rebuilding.
//
// Threads: the game (lasrecomp_main, its own thread with a large stack) renders and calls the hooks below at the end
// of each frame; this activity's thread runs the OpenXR frame loop (xrWaitFrame / xrBeginFrame / xrEndFrame at the
// display's rate) and reads the controllers. They share the Vulkan queue under vk_queue_lock.
//
// LARECOMP_XR_TURNIP=1 (env.txt; the driver pushed to <files>/turnip/vulkan.ad07xx.so): the game draws with Mesa Turnip
// (the Quest 3 build of github.com/Jbbrack03/Quest-3-Turnip-Driver, loaded by libadrenotools; every Vulkan command of
// the backend goes through runtime/nvn/vulkan/vk_dispatch.h, loaded from it). Meta's compositor reads swapchain images
// in the layout Qualcomm's driver writes (Turnip's showed scattered dashes - android/turnip_test), so OpenXR keeps a
// device of the system driver: each finished frame is copied by the game, on Turnip, into one of three shared
// AHardwareBuffers (CPU-readable usage: a linear layout both drivers agree on), and a bridge thread copies it into the
// swapchain image with the system device once Turnip's timeline says the frame is done - the game's threads never wait
// for it.

#ifdef __ANDROID__
#include <android/log.h>
#include <android_native_app_glue.h>
#include <dlfcn.h>
#include <libgen.h>
#endif
#include <math.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define XR_USE_GRAPHICS_API_VULKAN
#ifdef __ANDROID__
#define XR_USE_PLATFORM_ANDROID
#define VK_USE_PLATFORM_ANDROID_KHR
#include <android/hardware_buffer.h>
#include <fcntl.h>
#include <jni.h>
#endif
#include <vulkan/vulkan.h>
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>

#include "nvn/vulkan/vk_xr.h"
// (record.h: what this file records into the render thread's command buffer - declared here, its header brings
//  win_compat.h's setenv, which this file has its own of)
void rec_CmdPipelineBarrier2(VkCommandBuffer cb, const VkDependencyInfo *di);
void rec_CmdCopyImage(VkCommandBuffer cb, VkImage src, VkImageLayout sl, VkImage dst, VkImageLayout dl, uint32_t n, const VkImageCopy *r);
#ifdef __ANDROID__
#include "adrenotools/driver.h"
#define LR_VK_NO_MACROS  // (this file's own vk* calls stay the system driver's; the game's go through lr_vk*)
#include "nvn/vulkan/vk_dispatch.h"
#define ALOG(...) __android_log_print(ANDROID_LOG_INFO, "lasrecomp", __VA_ARGS__)
#else
// A PC (PCVR: SteamVR, Quest Link, Virtual Desktop... any OpenXR runtime): the same shell, started by
// `lasrecomp.exe --vr` (runtime/main.c). No Turnip, no Android thread settings; the log is the game's.
#define ALOG(...) ((void)0)
#define g_turnip false
#ifdef _WIN32
static int setenv(const char *k, const char *v, int overwrite) {
    if (!overwrite && getenv(k)) return 0;
    return _putenv_s(k, v);
}
#endif
#endif
#define XLOG(...) (ALOG(__VA_ARGS__), fprintf(stderr, "[xr] " __VA_ARGS__), fputc('\n', stderr))

extern int lasrecomp_main(int argc, char **argv);
extern void input_host_pad(uint64_t buttons, int32_t lx, int32_t ly, int32_t rx, int32_t ry);
extern void fp_xr_view(const float *q, const float *pos, const float *tan);  // runtime/hle/firstperson.c
extern void fp_xr_off(void);
extern void fp_xr_recenter(void);  // (the head's place taken as his eyes' again)

// Switch buttons (runtime/hle/input.c BTN_*)
enum { BTN_A, BTN_B, BTN_X, BTN_Y, BTN_LSTICK, BTN_RSTICK, BTN_L, BTN_R, BTN_ZL, BTN_ZR, BTN_PLUS, BTN_MINUS };

static XrInstance g_inst;
static XrSystemId g_sys;
static XrSession g_session;
static XrSpace g_space;
// The swapchain. It has three images on the Quest, and at 72 or 90 Hz an image released at 60 fps is free again only
// once the GPU has drawn it, the compositor has shown it and then shown the next one - about 34 ms, when the third
// frame after it wants it back (33 ms): xrWaitSwapchainImage held the submission thread 2 to 16 ms, one frame in seven
// (609 times in 75 s at 72 Hz, 523 at 90; none at 120 Hz, where each frame is shown twice - measured 21/10).
// LARECOMP_XR_SWAPCHAINS=2 (test, not to keep): two swapchains used in turn, six images - no more waits, but the
// headset then showed some frames half drawn (blocks of the world missing, or all of it), the compositor reading an
// image of the other swapchain before the GPU had finished it.
static XrSwapchain g_sc[2];
static XrSwapchainImageVulkan2KHR g_images[2][8];
static uint32_t g_nimages;  // (in each)
static int g_nsc;           // swapchains made
static int g_next_sc;       // (game thread) the one the next image is taken from
static int g_acq_sc;        // (game thread) the one the image taken for this frame is from
static int g_shown_sc;      // the one the last released image is from (read by the frame loop, under g_shown_lock)
// The game's image (LARECOMP_OFFSCREEN) and the swapchain: the aspect of both eyes' fields together (tangents
// -1.38 1.38 0.97 -1.43 on the Quest 3, about 1.15), so the head view's pixels are square - 1280x720 stretched it
// (the game's own buffers follow the output's size; its 2D overlay keeps a centred 16:9 region)
static int g_w = 1408, g_h = 1216;
static _Atomic int g_session_made;  // device_ready done: the frame loop may start
static _Atomic int g_running;       // between xrBeginSession and xrEndSession
static _Atomic int g_released;      // an image was released at least once: the screen can be shown
static int g_acquired = -1;         // (game thread) the swapchain image taken for this frame
static bool g_direct;               // its images can be viewed as RGBA8 UNORM (mutable format): drawn into directly
static bool g_perf_ext;
// The display's refresh rate (XR_FB_display_refresh_rate): 72 Hz by default, where a 60 fps game shows one frame in
// five twice. LARECOMP_XR_HZ=<hz> (a multiple of 60: 120 on the Quest 3) asks for it: each frame two refreshes, the
// game's vsync driven by the frame loop (every hz/60th xrWaitFrame: runtime/hle/vi.c vi_vsync_from_display), as the
// Mac's display link does for a 120 Hz screen. Not by default: at 120 Hz the compositor takes more of the GPU the game
// is short of, and the beach went from 55-58 to 39-46 fps (21/10).
// Meta's Super Resolution (XR_FB_composition_layer_settings): the compositor scales the game's image to the display
// with its edge-aware filter instead of a plain one - the image being 0.7-1.0 of the headset's size with the dynamic
// resolution. LARECOMP_XR_LAYER_FILTER=sr (default) | sharpen | auto | none.
static bool g_layer_ext;
static XrCompositionLayerSettingsFlagsFB layer_flags(void) {
    static int f = -1;
    if (f < 0) {
        const char *e = getenv("LARECOMP_XR_LAYER_FILTER");
        f = !e || !strcmp(e, "sr") ? 1 : !strcmp(e, "sharpen") ? 2 : !strcmp(e, "auto") ? 3 : 0;
        if (g_layer_ext) XLOG("layer filter: %s", f == 1 ? "super resolution" : f == 2 ? "sharpening" : f == 3 ? "the system's choice" : "none");
    }
    if (!g_layer_ext) return 0;
    return f == 1 ? XR_COMPOSITION_LAYER_SETTINGS_QUALITY_SUPER_SAMPLING_BIT_FB
         : f == 2 ? XR_COMPOSITION_LAYER_SETTINGS_QUALITY_SHARPENING_BIT_FB
         : f == 3 ? XR_COMPOSITION_LAYER_SETTINGS_AUTO_LAYER_FILTER_BIT_META : 0;
}
static bool g_hz_ext;
static float g_hz;
static PFN_xrEnumerateDisplayRefreshRatesFB p_enum_hz;
static PFN_xrRequestDisplayRefreshRateFB p_request_hz;
static PFN_xrGetDisplayRefreshRateFB p_get_hz;
extern void vi_vsync_signal(void);
extern void vi_vsync_from_display(int on);
static bool fresh_only(void) {
    const char *e = getenv("LARECOMP_XR_FRESH_ONLY");
    return e && *e == '1';
}
// Let the simulation keep its 60-Hz clock while producing at most one new
// stereo/MV pair per compositor request. Otherwise a 60-Hz producer at45->90
// replaces some pairs without displaying them, breaking the MV history.
static _Atomic bool g_eye_requested = true;
static bool demand_frames(void) {
    const char *e = getenv("LARECOMP_XR_DEMAND_FRAMES");
    return e && *e == '1';
}
static void choose_refresh_rate(void) {
    if (!g_hz_ext || !p_enum_hz || !p_request_hz) return;
    const char *e = getenv("LARECOMP_XR_HZ");
    float want = e ? (float)atof(e) : 0.0f;
    if (!isfinite(want) || want <= 0) return;
    float rates[16];
    uint32_t n = 0;
    if (XR_FAILED(p_enum_hz(g_session, 16, &n, rates))) n = 0;
    for (uint32_t i = 0; i < n; i++) {
        XLOG("display refresh rate available: %.1f Hz", rates[i]);
    }
    // Enumeration can omit rates supported by newer runtimes. Ask exactly what was
    // requested and observe the event; neither the list nor request success is proof.
    XrResult r = p_request_hz(g_session, want);
    XLOG("display refresh rate %.1f Hz requested: %d", want, r);
    float actual = 0;
    if (p_get_hz && XR_SUCCEEDED(p_get_hz(g_session, &actual))) {
        bool clock = !fresh_only() && actual > 0 && fabsf(actual / 60 - roundf(actual / 60)) < 0.001f;
        g_hz = clock ? actual : 0;
        vi_vsync_from_display(clock);
        XLOG("display refresh rate currently %.1f Hz (change may still be pending)", actual);
    }
}

// The view: 1 = it follows the head (each frame rendered from the head's pose over both eyes' fields, shown as a
// projection layer the compositor turns with the head at the display's rate - one image for both eyes: no depth yet),
// 0 = the floating screen. Both sticks clicked together switch.
static _Atomic int g_vr = 1;
// The 3D screen (LARECOMP_XR_SCREEN=1, the default since 22/10): the game's own first-person camera, turned by the stick
// exactly as on the Mac - the head moves nothing in the game - on a screen fixed in the room, each eye given its own
// image made from the scene's depth (stereo2_frame): a 3D cinema. Guillaume, 22/10, after the view that follows the head
// jerked as soon as the head or the stick moved: "je voudrais que bouger le casque ne fasse rien, qu'on ait besoin du
// stick exactement comme sur le Mac" - he had clicked both sticks by accident, found the floating screen "kinda nice and
// playable". The image is the screen's size (LARECOMP_XR_SCREEN_SIZE, 1920x1080): far less than both eyes of the view
// that follows the head - the game in its 60 fps mode. Both sticks clicked: the relief on / off.
// LARECOMP_XR_SCREEN_W / _DIST: its width and distance (metres, 2.4 and 2.2); LARECOMP_XR_SCREEN_DEPTH: the two eyes'
// gap on the screen for what is infinitely far (metres, 0.03), LARECOMP_XR_SCREEN_CONV: the distance shown on the
// screen's plane (game units, 2.0) - nearer comes out of it, farther goes in.
static int g_screen;
static _Atomic int g_screen3d = 1;
static _Atomic bool g_screen_two;  // (the view off in the headset: its last image made for each eye - in relief)
static _Atomic XrTime g_display_time;  // the frame loop's latest predicted display time
typedef struct {
    int vr, eye;
    XrPosef pose;
    XrFovf fov;
    // (stereo 2) each eye: its pose, its field with the margin (and as tangents: left, right, up, down), its offset
    // along the head's x from the head's centre (metres); the frame's own field as tangents
    XrPosef epose[2];
    XrFovf efov[2];
    float etan[2][4], eoff[2], ctan[4];
    // the body's turn of the view it was drawn from (radians), and when the game published that view (CLOCK_MONOTONIC):
    // the frame loop turns it by the stick's turn since (fp_xr_turn); look 0: not turned
    int look;
    float yaw, pitch;
    int64_t ns;
    uint64_t frame_id;   // actual rendered image, independent of camera publication
    float place[3];      // (where the view's eye was in the world, game units: AppSW's motion)
    int sw;              // (AppSW) this eye's motion and depth images written with it
    float nearz, farz;   // (AppSW) its depth's range: the distances of depth 0 and 1, metres
} Shot;
// AppSW (XR_FB_space_warp, LARECOMP_XR_APPSW=1 - Guillaume, 22/10: "ça fait 10 fois que je veux l'AppSW et la super
// résolution"): the game in its 60 fps mode, the headset at 120, the compositor making every other image from each eye's
// motion vectors and depth. The world is still in the game: what moves it on screen is Link (his eye's place) and the
// stick (the body's turn), known for every image - the motion vectors are that motion, per pixel, from the depth
// (exec.c vk_xr_appsw, shaders.c xr_appsw.frag); the head's own motion is the headset's. Low resolution, as Meta
// recommends (the runtime's size x2). Never on a session's first frames nor right after the images stopped being written:
// the compositor crashed on it, and stayed broken until a reboot (TCVR, 01/10): 30 frames focused and 30 consecutive
// frames written first.
static bool g_sw_ext;
static bool g_synth_ext;  // XR_EXT_frame_synthesis, explicit opt-in; never chain both APIs
static int g_appsw;  // its swapchains made
static uint32_t g_mvw, g_mvh;
static XrSwapchain g_mv_sc[2], g_dp_sc[2];
static XrSwapchainImageVulkan2KHR g_mv_img[2][8], g_dp_img[2][8];
static VkFormat g_dp_fmt;
static int g_swacq[2];      // (game thread) this frame's motion images, per eye: written
static _Atomic int g_focused;
// Stereo (LARECOMP_XR_STEREO, on by default since 21/10 - Guillaume: "full res, stereoscopic, then the ms lever after
// lever"): each frame is rendered from one eye in turn, at the size the headset recommends for an eye, into that eye's
// own swapchain; each eye is shown its own latest image, the compositor turning it with the head at the display's
// rate. Real depth at no GPU cost; each eye is updated at half the game's rate (the next lever: both eyes a frame).
static int g_stereo;  // 0: one image for both eyes; 1: one eye a frame in turn; 2: both eyes each frame, made from one
                      // image and the scene's depth (LARECOMP_XR_STEREO, 2 by default since 21/10 evening)
static int g_ew, g_eh;  // (stereo 2) an eye's image
static int g_sw, g_sh;  // the swapchains' images: the frame's size, or an eye's (stereo 2)
static int g_acq2[2] = {-1, -1};  // (stereo 2, game thread) each eye's swapchain image taken for this frame
static int g_eye_next;                                    // (game thread) the eye the next frame is rendered for
static Shot g_shown_eye[2];                               // what each eye's latest released image was rendered with
static int g_released_eye[2];                             // (under g_shown_lock)
// An image is released once the GPU has finished it (a thread waits for its submission): with two swapchains read in
// turn the compositor showed frames half drawn when they were released at submission (LARECOMP_XR_SWAPCHAINS=2, 21/10)
static pthread_mutex_t g_rel_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_rel_cond = PTHREAD_COND_INITIALIZER;
static int g_rel_pending[2];                              // a swapchain's image waiting for its release
static struct { int sc, sw; uint64_t seq; Shot shot; } g_rel_q[4];
static int g_rel_n;
static void *release_thread(void *arg);
static Shot g_rendering;  // (game thread) what the frame in progress is rendered with
static Shot g_shown;      // what the last released image was rendered with (read by the frame loop)
static pthread_mutex_t g_shown_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_shown_cond = PTHREAD_COND_INITIALIZER;

static PFN_xrGetVulkanGraphicsRequirements2KHR p_requirements;
static PFN_xrCreateVulkanInstanceKHR p_create_instance;
static PFN_xrGetVulkanGraphicsDevice2KHR p_graphics_device;
static PFN_xrCreateVulkanDeviceKHR p_create_device;
static PFN_xrPerfSettingsSetPerformanceLevelEXT p_perf_level;

// XR_KHR_android_thread_settings: threads told to the runtime. The Quest gives an immersive app three cores (3-5) for
// all its threads; a thread told is set to nice -20 (the others are at -10), and once an application main thread is
// told the cores run at 2.36 GHz instead of 2.21 (measured 21/10). Told: this frame loop's thread as the application's
// main one (it calls xrWaitFrame), and the submission worker as the renderer - 75% of a core. The game's thread
// (79%) is not: at nice -20 it crashed the game three times out of three - the woken game thread preempts, on its
// core, the job thread that has just signalled the end of a job and is about to call its "done" callback, and the
// job is reset meanwhile (on the Switch each thread has its core of its own). LARECOMP_XR_THREADS=0: none told; =2:
// the game's thread as the main one (test).
#ifdef __ANDROID__
static bool g_thread_ext;
static PFN_xrSetAndroidApplicationThreadKHR p_set_thread;
static _Atomic int g_role_tid[2];  // the game's thread, the worker (0: not started yet)
static bool g_role_done[2];        // (the frame loop's)
#endif
extern void (*g_thread_role_hook)(int kind);  // runtime/nvn/objects.c
static void thread_role(int kind) {
#ifndef __ANDROID__
    (void)kind;
    return;
#else
    const char *e = getenv("LARECOMP_XR_THREADS");
    if (kind == 0 && !(e && *e == '2')) return;
    if (kind >= 0 && kind < 2) g_role_tid[kind] = (int)gettid();
#endif
}
static void apply_thread_roles(void) {  // (the frame loop, once the session exists)
#ifdef __ANDROID__
    if (!p_set_thread || !g_session) return;
    const char *e = getenv("LARECOMP_XR_THREADS");
    if (!g_role_tid[0] && e && *e == '3') g_role_tid[0] = (int)gettid();
    static const XrAndroidThreadTypeKHR type[2] = {XR_ANDROID_THREAD_TYPE_APPLICATION_MAIN_KHR, XR_ANDROID_THREAD_TYPE_RENDERER_MAIN_KHR};
    for (int k = 0; k < 2; k++) {
        int tid = g_role_tid[k];
        if (!tid || g_role_done[k]) continue;
        g_role_done[k] = true;
        XrResult r = p_set_thread(g_session, type[k], (uint32_t)tid);
        XLOG("thread %d (%s) told to the runtime as its %s: %d", tid, tid == (int)gettid() ? "the frame loop's" : k ? "the submission worker" : "the game's",
             k ? "renderer" : "application main thread", r);
    }
#endif
}

// LARECOMP_XR_CPU / LARECOMP_XR_GPU = boost: XR_EXT_performance_settings' boost level instead of sustained high.
static XrPerfSettingsLevelEXT perf_level(const char *var) {
    const char *e = getenv(var);
    return e && !strcmp(e, "boost") ? XR_PERF_SETTINGS_LEVEL_BOOST_EXT : XR_PERF_SETTINGS_LEVEL_SUSTAINED_HIGH_EXT;
}

// ------------------------------------------------------------------ controllers -> Switch pad
static XrActionSet g_aset;
enum { A_A, A_B, A_X, A_Y, A_MENU, A_LCLICK, A_RCLICK, A_LTRIG, A_RTRIG, A_LGRIP, A_RGRIP, A_LSTICK, A_RSTICK, A_COUNT };
static XrAction g_act[A_COUNT];
static const struct { const char *name; XrActionType type; const char *path; } k_actions[A_COUNT] = {
    {"a", XR_ACTION_TYPE_BOOLEAN_INPUT, "/user/hand/right/input/a/click"},
    {"b", XR_ACTION_TYPE_BOOLEAN_INPUT, "/user/hand/right/input/b/click"},
    {"x", XR_ACTION_TYPE_BOOLEAN_INPUT, "/user/hand/left/input/x/click"},
    {"y", XR_ACTION_TYPE_BOOLEAN_INPUT, "/user/hand/left/input/y/click"},
    {"menu", XR_ACTION_TYPE_BOOLEAN_INPUT, "/user/hand/left/input/menu/click"},
    {"lclick", XR_ACTION_TYPE_BOOLEAN_INPUT, "/user/hand/left/input/thumbstick/click"},
    {"rclick", XR_ACTION_TYPE_BOOLEAN_INPUT, "/user/hand/right/input/thumbstick/click"},
    {"ltrig", XR_ACTION_TYPE_FLOAT_INPUT, "/user/hand/left/input/trigger/value"},
    {"rtrig", XR_ACTION_TYPE_FLOAT_INPUT, "/user/hand/right/input/trigger/value"},
    {"lgrip", XR_ACTION_TYPE_FLOAT_INPUT, "/user/hand/left/input/squeeze/value"},
    {"rgrip", XR_ACTION_TYPE_FLOAT_INPUT, "/user/hand/right/input/squeeze/value"},
    {"lstick", XR_ACTION_TYPE_VECTOR2F_INPUT, "/user/hand/left/input/thumbstick"},
    {"rstick", XR_ACTION_TYPE_VECTOR2F_INPUT, "/user/hand/right/input/thumbstick"},
};

static XrPath path_of(const char *s) {
    XrPath p = XR_NULL_PATH;
    xrStringToPath(g_inst, s, &p);
    return p;
}

static void make_actions(void) {
    XrActionSetCreateInfo si = {.type = XR_TYPE_ACTION_SET_CREATE_INFO};
    strcpy(si.actionSetName, "game");
    strcpy(si.localizedActionSetName, "Game");
    if (XR_FAILED(xrCreateActionSet(g_inst, &si, &g_aset))) { XLOG("no action set"); return; }
    XrActionSuggestedBinding b[A_COUNT];
    for (int i = 0; i < A_COUNT; i++) {
        XrActionCreateInfo ai = {.type = XR_TYPE_ACTION_CREATE_INFO, .actionType = k_actions[i].type};
        strcpy(ai.actionName, k_actions[i].name);
        strcpy(ai.localizedActionName, k_actions[i].name);
        xrCreateAction(g_aset, &ai, &g_act[i]);
        b[i] = (XrActionSuggestedBinding){g_act[i], path_of(k_actions[i].path)};
    }
    XrInteractionProfileSuggestedBinding sb = {.type = XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING,
                                               .interactionProfile = path_of("/interaction_profiles/oculus/touch_controller"),
                                               .countSuggestedBindings = A_COUNT, .suggestedBindings = b};
    XrResult r = xrSuggestInteractionProfileBindings(g_inst, &sb);
    if (XR_FAILED(r)) XLOG("bindings refused: %d", r);
}

static bool held(int a) {
    XrActionStateGetInfo gi = {.type = XR_TYPE_ACTION_STATE_GET_INFO, .action = g_act[a]};
    XrActionStateBoolean st = {.type = XR_TYPE_ACTION_STATE_BOOLEAN};
    return XR_SUCCEEDED(xrGetActionStateBoolean(g_session, &gi, &st)) && st.isActive && st.currentState;
}
static float value(int a) {
    XrActionStateGetInfo gi = {.type = XR_TYPE_ACTION_STATE_GET_INFO, .action = g_act[a]};
    XrActionStateFloat st = {.type = XR_TYPE_ACTION_STATE_FLOAT};
    return XR_SUCCEEDED(xrGetActionStateFloat(g_session, &gi, &st)) && st.isActive ? st.currentState : 0;
}
static XrVector2f stick(int a) {
    XrActionStateGetInfo gi = {.type = XR_TYPE_ACTION_STATE_GET_INFO, .action = g_act[a]};
    XrActionStateVector2f st = {.type = XR_TYPE_ACTION_STATE_VECTOR2F};
    if (XR_SUCCEEDED(xrGetActionStateVector2f(g_session, &gi, &st)) && st.isActive) return st.currentState;
    return (XrVector2f){0, 0};
}
static int32_t axis(float v) { return (int32_t)(v < -1 ? -32767 : v > 1 ? 32767 : v * 32767); }

#ifndef __ANDROID__
// LAS Recomp's menu (runtime/ui/menu.mm) from the Touch controllers. They reach no SDL pad, so the menu's own chords
// (Minus + Plus, the Guide button) never came from them: in a headset it could not be opened at all (27/10). Its
// events are made here and queued for the window's thread, which hands them to the menu as a pad's (which 0).
#include <SDL3/SDL.h>
#include "settings.h"
extern bool menu_is_open(void);
extern int input_menu_family(SDL_JoystickID which);
static void menu_pad(int button, bool down) {
    SDL_Event e;
    SDL_zero(e);
    e.type = down ? SDL_EVENT_GAMEPAD_BUTTON_DOWN : SDL_EVENT_GAMEPAD_BUTTON_UP;
    e.gbutton.timestamp = SDL_GetTicksNS(), e.gbutton.which = 0, e.gbutton.button = (Uint8)button, e.gbutton.down = down;
    SDL_PushEvent(&e);
}
// While the menu is open the controllers drive it - A confirms and B goes back (in the menu's own convention: game
// layout or the controller's, runtime/ui/pad_nav.mm), the left stick moves, the grips change tabs - and the game gets
// nothing. b: the buttons as the game would have had them, l: the left stick.
static void menu_drive(uint64_t b, XrVector2f l) {
    static uint64_t prev;
    static bool dir[4];
    bool sc = g_settings.menu_confirm == MENU_CONFIRM_CONTROLLER && input_menu_family(0) != 0;
    const struct { int bit, button; } map[] = {
        {BTN_A, sc ? SDL_GAMEPAD_BUTTON_SOUTH : SDL_GAMEPAD_BUTTON_EAST}, {BTN_B, sc ? SDL_GAMEPAD_BUTTON_EAST : SDL_GAMEPAD_BUTTON_SOUTH},
        {BTN_X, SDL_GAMEPAD_BUTTON_NORTH}, {BTN_Y, SDL_GAMEPAD_BUTTON_WEST},
        {BTN_L, SDL_GAMEPAD_BUTTON_LEFT_SHOULDER}, {BTN_R, SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER},
        {BTN_ZL, SDL_GAMEPAD_BUTTON_LEFT_SHOULDER}, {BTN_ZR, SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER}};
    for (size_t i = 0; i < sizeof map / sizeof map[0]; i++) {
        uint64_t m = 1ull << map[i].bit;
        if ((b ^ prev) & m) menu_pad(map[i].button, (b & m) != 0);
    }
    prev = b;
    // (the stick as the pad's cross: pressed past 0.6, let go under 0.4)
    const float v[4] = {l.y, -l.y, -l.x, l.x};
    const int button[4] = {SDL_GAMEPAD_BUTTON_DPAD_UP, SDL_GAMEPAD_BUTTON_DPAD_DOWN, SDL_GAMEPAD_BUTTON_DPAD_LEFT, SDL_GAMEPAD_BUTTON_DPAD_RIGHT};
    for (int k = 0; k < 4; k++) {
        bool now = dir[k] ? v[k] > 0.4f : v[k] > 0.6f;
        if (now != dir[k]) menu_pad(button[k], now), dir[k] = now;
    }
}
#endif

// The buttons carry the Switch's letters (A B X Y); the triggers are ZL / ZR, the grips L / R, the left menu button
// Plus, the left stick's click Minus, the sticks the sticks.
static void read_controllers(void) {
    if (!g_aset) return;
    XrActiveActionSet as = {g_aset, XR_NULL_PATH};
    XrActionsSyncInfo si = {.type = XR_TYPE_ACTIONS_SYNC_INFO, .countActiveActionSets = 1, .activeActionSets = &as};
    if (xrSyncActions(g_session, &si) != XR_SUCCESS) { input_host_pad(0, 0, 0, 0, 0); return; }  // (not focused: let go)
    uint64_t b = 0;
    if (held(A_A)) b |= 1ull << BTN_A;
    if (held(A_B)) b |= 1ull << BTN_B;
    if (held(A_X)) b |= 1ull << BTN_X;
    if (held(A_Y)) b |= 1ull << BTN_Y;
#ifndef __ANDROID__
    {   // the left menu button: a tap is Plus (the game's inventory), sent once it is let go; held half a second it opens
        // or closes LAS Recomp's menu (menu_pad: the Guide button's toggle)
        static int64_t down_at;
        static bool was, long_done;
        static int tap;
        bool m = held(A_MENU);
        extern uint64_t plat_now_ns(void);
        int64_t now = (int64_t)plat_now_ns();
        if (m && !was) down_at = now, long_done = false;
        if (m && !long_done && now - down_at > 500000000) {
            long_done = true;
            menu_pad(SDL_GAMEPAD_BUTTON_GUIDE, true), menu_pad(SDL_GAMEPAD_BUTTON_GUIDE, false);
            XLOG("menu: %s from the controller", menu_is_open() ? "closed" : "opened");
        }
        if (!m && was && !long_done && !menu_is_open()) tap = 4;
        if (tap > 0) b |= 1ull << BTN_PLUS, tap--;
        was = m;
    }
    {   // X and Y held together half a second: the same toggle - through Steam Link the left menu button opens Steam's own
        // menu and never reaches the game (Guillaume, 12/11: « le bouton menu du contrôleur gauche ouvre le menu Steam Link »)
        static int64_t both_at;
        static bool done;
        bool both = held(A_X) && held(A_Y);
        extern uint64_t plat_now_ns(void);
        int64_t now = (int64_t)plat_now_ns();
        if (!both) both_at = 0, done = false;
        else if (!both_at) both_at = now;
        else if (!done && now - both_at > 500000000) {
            done = true;
            menu_pad(SDL_GAMEPAD_BUTTON_GUIDE, true), menu_pad(SDL_GAMEPAD_BUTTON_GUIDE, false);
            XLOG("menu: %s from the controller (X + Y)", menu_is_open() ? "closed" : "opened");
        }
        if (both) b &= ~((1ull << BTN_X) | (1ull << BTN_Y));  // (the chord isn't the game's X and Y)
    }
#else
    if (held(A_MENU)) b |= 1ull << BTN_PLUS;
#endif
    // the left stick's click: L3 (the view from behind / his eyes, as on a screen), held half a second the map (minus)
    // - the Touch controllers have no button left for it (it was minus alone: no view from behind in the headset)
    {
        static int64_t down_at;
        static bool was, minus;
        bool l = held(A_LCLICK);
        extern uint64_t plat_now_ns(void);
        int64_t now = (int64_t)plat_now_ns();
        if (l && !was) down_at = now, minus = false;
        if (l && now - down_at > 500000000) minus = true;
        if (l && minus) b |= 1ull << BTN_MINUS;
        static int tap;  // (a tap's L3, sent for a few polls once released)
        if (!l && was && !minus) tap = 4;
        if (tap > 0) b |= 1ull << BTN_LSTICK, tap--;
        was = l;
    }
    if (held(A_RCLICK)) b |= 1ull << BTN_RSTICK;
    if (value(A_LTRIG) > 0.5f) b |= 1ull << BTN_ZL;
    if (value(A_RTRIG) > 0.5f) b |= 1ull << BTN_ZR;
    if (value(A_LGRIP) > 0.5f) b |= 1ull << BTN_L;
    if (value(A_RGRIP) > 0.5f) b |= 1ull << BTN_R;
    XrVector2f l = stick(A_LSTICK), r = stick(A_RSTICK);
    static bool both_was;
    bool both = held(A_LCLICK) && held(A_RCLICK);
    if (both && !both_was && g_screen) { g_screen3d = !g_screen3d; XLOG("screen: %s", g_screen3d ? "in relief" : "flat"); }
    else if (both && !both_was) { g_vr = !g_vr; XLOG("view: %s", g_vr ? "follows the head" : "floating screen"); }
    both_was = both;
    if (both) b &= ~(1ull << BTN_MINUS | 1ull << BTN_RSTICK | 1ull << BTN_LSTICK);
#ifndef __ANDROID__
    static bool used;  // (once a controller button is pressed or a stick leans: from then on, as on the Quest)
    if (!used && (b || fabsf(l.x) > 0.3f || fabsf(l.y) > 0.3f || fabsf(r.x) > 0.3f || fabsf(r.y) > 0.3f)) used = true;
    if (!used) return;
    if (menu_is_open()) {
        menu_drive(b, l);
        input_host_pad(0, 0, 0, 0, 0);
        return;
    }
    menu_drive(0, (XrVector2f){0, 0});  // (let go of whatever was held when it closed)
#endif
    input_host_pad(b, axis(l.x), axis(l.y), axis(r.x), axis(r.y));
}

#ifdef __ANDROID__
// ------------------------------------------------------------------ Turnip: the game's driver, and the bridge
static bool g_turnip;  // the game draws with Turnip (load_vulkan); OpenXR keeps the system driver's device below
static VkInstance s_inst;  // (the system driver's, made through OpenXR: the session and the swapchain copies)
static VkPhysicalDevice s_pd;
static VkDevice s_dev, g_tdev;
static uint32_t s_qfam, g_tqfam;
static VkQueue s_queue;
static VkCommandPool s_pool;
static VkCommandBuffer s_cb;
static VkFence s_fence;
#define NSLOT 3
static AHardwareBuffer *g_ahb[NSLOT];
static VkImage g_slot_t[NSLOT], g_slot_s[NSLOT];  // each shared image in the game's (Turnip) device and in the system's
static int g_slot_state[NSLOT];                    // 0 free, 1 the game's frame copied into it, 2 queued for the bridge
static uint64_t g_slot_seq[NSLOT];
static Shot g_slot_shot[NSLOT];
static int g_cur_slot = -1;  // (game thread) the slot of the frame being submitted
static int g_bq[NSLOT * 2], g_bq_head, g_bq_n;
static pthread_mutex_t g_bl = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_bc = PTHREAD_COND_INITIALIZER;
static const char *const k_bridge_exts[] = {"VK_ANDROID_external_memory_android_hardware_buffer", "VK_EXT_queue_family_foreign",
                                            "VK_KHR_external_memory", "VK_KHR_dedicated_allocation", "VK_KHR_get_memory_requirements2",
                                            "VK_KHR_sampler_ycbcr_conversion", "VK_KHR_maintenance1", "VK_KHR_bind_memory2"};

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

// The backend's Vulkan commands (vk_dispatch.h) from Turnip when asked and loadable, else from the system's loader.
static void load_vulkan(struct android_app *app) {
    const char *t = getenv("LARECOMP_XR_TURNIP");
    if (t && *t == '1') {
        char src[700], dir[700], tmp[700], dst[800], hook[1024] = "";
        snprintf(src, sizeof src, "%s/turnip/vulkan.ad07xx.so", app->activity->externalDataPath);
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
            g_turnip = true;
            setenv("LARECOMP_VK_NO_HOST_IMPORT", "1", 0);  // (the guest heap by the page cache first: device.c)
            XLOG("Vulkan for the game: Mesa Turnip (%s), the headset's swapchain through the system driver", src);
            return;
        }
        XLOG("Turnip asked for but not loaded (%s: %s): the system driver", src, copied ? "adrenotools failed" : "no driver file");
    }
    lr_vk_load_global(vkGetInstanceProcAddr);
}

static void add_exts(const char *const *have, uint32_t nhave, VkExtensionProperties *sup, uint32_t nsup, const char **out, uint32_t *n) {
    for (uint32_t i = 0; i < nhave; i++) out[(*n)++] = have[i];
    for (unsigned k = 0; k < sizeof k_bridge_exts / sizeof *k_bridge_exts; k++) {
        bool there = false, ok = false;
        for (uint32_t i = 0; i < *n; i++) there |= !strcmp(out[i], k_bridge_exts[k]);
        for (uint32_t i = 0; i < nsup; i++) ok |= !strcmp(sup[i].extensionName, k_bridge_exts[k]);
        if (!there && ok) out[(*n)++] = k_bridge_exts[k];
    }
}

// The system driver's device for OpenXR (the session, the swapchain copies), made once the game's device exists.
static bool system_device(void) {
    XrGraphicsRequirementsVulkan2KHR req = {.type = XR_TYPE_GRAPHICS_REQUIREMENTS_VULKAN2_KHR};
    p_requirements(g_inst, g_sys, &req);
    VkApplicationInfo ai = {.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO, .pApplicationName = "LASRecomp bridge", .apiVersion = VK_API_VERSION_1_1};
    VkInstanceCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pApplicationInfo = &ai};
    XrVulkanInstanceCreateInfoKHR xi = {.type = XR_TYPE_VULKAN_INSTANCE_CREATE_INFO_KHR, .systemId = g_sys,
                                        .pfnGetInstanceProcAddr = vkGetInstanceProcAddr, .vulkanCreateInfo = &ci};
    VkResult vr = VK_ERROR_INITIALIZATION_FAILED;
    XrResult r = p_create_instance(g_inst, &xi, &s_inst, &vr);
    XLOG("system Vulkan instance through OpenXR: xr %d vk %d", r, vr);
    if (XR_FAILED(r) || vr) return false;
    XrVulkanGraphicsDeviceGetInfoKHR gi = {.type = XR_TYPE_VULKAN_GRAPHICS_DEVICE_GET_INFO_KHR, .systemId = g_sys, .vulkanInstance = s_inst};
    p_graphics_device(g_inst, &gi, &s_pd);
    uint32_t nq = 8;
    VkQueueFamilyProperties qp[8];
    vkGetPhysicalDeviceQueueFamilyProperties(s_pd, &nq, qp);
    for (uint32_t i = 0; i < nq; i++) if (qp[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) { s_qfam = i; break; }
    uint32_t ns = 0;
    vkEnumerateDeviceExtensionProperties(s_pd, NULL, &ns, NULL);
    VkExtensionProperties *sup = calloc(ns ? ns : 1, sizeof *sup);
    vkEnumerateDeviceExtensionProperties(s_pd, NULL, &ns, sup);
    const char *exts[16];
    uint32_t ne = 0;
    add_exts(NULL, 0, sup, ns, exts, &ne);
    free(sup);
    float prio = 1;
    VkDeviceQueueCreateInfo qci = {.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, .queueFamilyIndex = s_qfam, .queueCount = 1, .pQueuePriorities = &prio};
    VkDeviceCreateInfo dci = {.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, .queueCreateInfoCount = 1, .pQueueCreateInfos = &qci,
                              .enabledExtensionCount = ne, .ppEnabledExtensionNames = exts};
    XrVulkanDeviceCreateInfoKHR xd = {.type = XR_TYPE_VULKAN_DEVICE_CREATE_INFO_KHR, .systemId = g_sys, .pfnGetInstanceProcAddr = vkGetInstanceProcAddr,
                                      .vulkanPhysicalDevice = s_pd, .vulkanCreateInfo = &dci};
    vr = VK_ERROR_INITIALIZATION_FAILED;
    r = p_create_device(g_inst, &xd, &s_dev, &vr);
    XLOG("system Vulkan device through OpenXR: xr %d vk %d (%u extensions)", r, vr, ne);
    if (XR_FAILED(r) || vr) return false;
    vkGetDeviceQueue(s_dev, s_qfam, 0, &s_queue);
    VkCommandPoolCreateInfo pci = {.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT, .queueFamilyIndex = s_qfam};
    vkCreateCommandPool(s_dev, &pci, NULL, &s_pool);
    VkCommandBufferAllocateInfo cai = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, .commandPool = s_pool, .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1};
    vkAllocateCommandBuffers(s_dev, &cai, &s_cb);
    VkFenceCreateInfo fci = {.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    vkCreateFence(s_dev, &fci, NULL, &s_fence);
    return true;
}

// An AHardwareBuffer as an image of a device: Turnip's (lr_vk*, the game's) or the system's (vk*).
static bool import_ahb(bool turnip, VkDevice dev, AHardwareBuffer *ahb, VkImage *img) {
    PFN_vkGetAndroidHardwareBufferPropertiesANDROID props = turnip ? lr_vkGetAndroidHardwareBufferPropertiesANDROID
        : (PFN_vkGetAndroidHardwareBufferPropertiesANDROID)vkGetDeviceProcAddr(dev, "vkGetAndroidHardwareBufferPropertiesANDROID");
    PFN_vkCreateImage create = turnip ? lr_vkCreateImage : vkCreateImage;
    PFN_vkAllocateMemory alloc = turnip ? lr_vkAllocateMemory : vkAllocateMemory;
    PFN_vkBindImageMemory bind = turnip ? lr_vkBindImageMemory : vkBindImageMemory;
    VkAndroidHardwareBufferFormatPropertiesANDROID fp = {.sType = VK_STRUCTURE_TYPE_ANDROID_HARDWARE_BUFFER_FORMAT_PROPERTIES_ANDROID};
    VkAndroidHardwareBufferPropertiesANDROID pp = {.sType = VK_STRUCTURE_TYPE_ANDROID_HARDWARE_BUFFER_PROPERTIES_ANDROID, .pNext = &fp};
    VkResult r = props ? props(dev, ahb, &pp) : VK_ERROR_EXTENSION_NOT_PRESENT;
    if (r) { XLOG("bridge: buffer properties (%s): %d", turnip ? "Turnip" : "system", r); return false; }
    VkExternalMemoryImageCreateInfo emi = {.sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO,
                                           .handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_ANDROID_HARDWARE_BUFFER_BIT_ANDROID};
    VkImageCreateInfo ici = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .pNext = &emi, .imageType = VK_IMAGE_TYPE_2D,
                             .format = fp.format ? fp.format : VK_FORMAT_R8G8B8A8_UNORM, .extent = {(uint32_t)g_w, (uint32_t)g_h, 1},
                             .mipLevels = 1, .arrayLayers = 1, .samples = VK_SAMPLE_COUNT_1_BIT, .tiling = VK_IMAGE_TILING_OPTIMAL,
                             .usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED};
    if ((r = create(dev, &ici, NULL, img))) { XLOG("bridge: image (%s): %d", turnip ? "Turnip" : "system", r); return false; }
    VkImportAndroidHardwareBufferInfoANDROID imp = {.sType = VK_STRUCTURE_TYPE_IMPORT_ANDROID_HARDWARE_BUFFER_INFO_ANDROID, .buffer = ahb};
    VkMemoryDedicatedAllocateInfo ded = {.sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO, .pNext = &imp, .image = *img};
    VkMemoryAllocateInfo mai = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .pNext = &ded, .allocationSize = pp.allocationSize,
                                .memoryTypeIndex = (uint32_t)__builtin_ctz(pp.memoryTypeBits ? pp.memoryTypeBits : 1)};
    VkDeviceMemory mem;
    if ((r = alloc(dev, &mai, NULL, &mem)) || (r = bind(dev, *img, mem, 0))) {
        XLOG("bridge: memory (%s): %d", turnip ? "Turnip" : "system", r);
        return false;
    }
    return true;
}

static void *bridge_thread(void *arg);
static bool bridge_init(void) {
    for (int k = 0; k < NSLOT; k++) {
        AHardwareBuffer_Desc ad = {.width = (uint32_t)g_w, .height = (uint32_t)g_h, .layers = 1, .format = AHARDWAREBUFFER_FORMAT_R8G8B8A8_UNORM,
                                   .usage = AHARDWAREBUFFER_USAGE_GPU_SAMPLED_IMAGE | AHARDWAREBUFFER_USAGE_GPU_COLOR_OUTPUT |
                                            AHARDWAREBUFFER_USAGE_CPU_READ_RARELY};
        if (AHardwareBuffer_allocate(&ad, &g_ahb[k]) || !import_ahb(true, g_tdev, g_ahb[k], &g_slot_t[k]) ||
            !import_ahb(false, s_dev, g_ahb[k], &g_slot_s[k])) {
            XLOG("bridge: shared image %d failed", k);
            return false;
        }
    }
    pthread_t t;
    pthread_create(&t, NULL, bridge_thread, NULL);
    XLOG("bridge ready: %d shared images of %dx%d", NSLOT, g_w, g_h);
    return true;
}

static void qbarrier(bool turnip, VkCommandBuffer cb, VkImage img, VkImageLayout from, VkImageLayout to, uint32_t src_q, uint32_t dst_q,
                     VkAccessFlags sa, VkAccessFlags da) {
    VkImageMemoryBarrier b = {.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, .srcAccessMask = sa, .dstAccessMask = da, .oldLayout = from,
                              .newLayout = to, .srcQueueFamilyIndex = src_q, .dstQueueFamilyIndex = dst_q, .image = img,
                              .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
    (turnip ? lr_vkCmdPipelineBarrier : vkCmdPipelineBarrier)(cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                                                              0, 0, NULL, 0, NULL, 1, &b);
}

// (game thread) the finished frame into a free shared image, in the frame's own command buffer (Turnip)
static void bridge_frame(VkCommandBuffer cb, VkImage src, VkExtent2D ext) {
    pthread_mutex_lock(&g_bl);
    if (g_cur_slot >= 0) g_slot_state[g_cur_slot] = 0;  // (the last frame copied but never submitted)
    g_cur_slot = -1;
    int k = -1;
    while (k < 0) {
        for (int i = 0; i < NSLOT && k < 0; i++) if (!g_slot_state[i]) k = i;
        if (k < 0) pthread_cond_wait(&g_bc, &g_bl);  // (all three on their way to the headset: the bridge is behind)
    }
    g_slot_state[k] = 1;
    pthread_mutex_unlock(&g_bl);
    g_slot_shot[k] = g_rendering;
    uint32_t w = ext.width < (uint32_t)g_w ? ext.width : (uint32_t)g_w, h = ext.height < (uint32_t)g_h ? ext.height : (uint32_t)g_h;
    qbarrier(true, cb, src, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_QUEUE_FAMILY_IGNORED,
             VK_QUEUE_FAMILY_IGNORED, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT);
    qbarrier(true, cb, g_slot_t[k], VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_QUEUE_FAMILY_FOREIGN_EXT, g_tqfam, 0,
             VK_ACCESS_TRANSFER_WRITE_BIT);
    VkImageCopy rg = {.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, .dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, .extent = {w, h, 1}};
    lr_vkCmdCopyImage(cb, src, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, g_slot_t[k], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &rg);
    qbarrier(true, cb, g_slot_t[k], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL, g_tqfam, VK_QUEUE_FAMILY_FOREIGN_EXT,
             VK_ACCESS_TRANSFER_WRITE_BIT, 0);
    qbarrier(true, cb, src, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_QUEUE_FAMILY_IGNORED,
             VK_QUEUE_FAMILY_IGNORED, VK_ACCESS_TRANSFER_READ_BIT, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT);
    g_cur_slot = k;
}

// (game thread) the frame is on Turnip's queue: the bridge copies it once Turnip's timeline passes its submission
static void bridge_submitted(void) {
    extern uint64_t renderer_submitted_seq(void);
    int k = g_cur_slot;
    if (k < 0) return;
    g_cur_slot = -1;
    pthread_mutex_lock(&g_bl);
    g_slot_seq[k] = renderer_submitted_seq();
    g_slot_state[k] = 2;
    g_bq[(g_bq_head + g_bq_n++) % (NSLOT * 2)] = k;
    pthread_cond_broadcast(&g_bc);
    pthread_mutex_unlock(&g_bl);
}

static int acquire_image(void);
static VkImage image_at(int k);
static double now_ms(void);
static void *bridge_thread(void *arg) {
    (void)arg;
    extern int vk_wait_seq_quiet(uint64_t seq, uint64_t timeout_ns);
    extern void plat_thread_name(const char *name);
    plat_thread_name("xr-bridge");
    for (;;) {
        pthread_mutex_lock(&g_bl);
        while (!g_bq_n) pthread_cond_wait(&g_bc, &g_bl);
        int k = g_bq[g_bq_head];
        g_bq_head = (g_bq_head + 1) % (NSLOT * 2), g_bq_n--;
        uint64_t seq = g_slot_seq[k];
        pthread_mutex_unlock(&g_bl);
        double b0 = now_ms();
        int r = vk_wait_seq_quiet(seq, 2000000000ull);
        double b1 = now_ms(), b2 = b1, b3 = b1;
        static int nlog;
        if (r && nlog++ < 5) XLOG("bridge: Turnip's submission %llu not done (%d)", (unsigned long long)seq, r);
        int got = r ? -1 : acquire_image();
        b2 = now_ms();
        if (got >= 0) {
            VkImage dst = image_at(got);
            VkCommandBufferBeginInfo bi = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
            vkResetCommandBuffer(s_cb, 0);
            vkBeginCommandBuffer(s_cb, &bi);
            qbarrier(false, s_cb, g_slot_s[k], VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_QUEUE_FAMILY_FOREIGN_EXT, s_qfam, 0,
                     VK_ACCESS_TRANSFER_READ_BIT);
            qbarrier(false, s_cb, dst, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED,
                     0, VK_ACCESS_TRANSFER_WRITE_BIT);
            VkImageCopy rg = {.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, .dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                              .extent = {(uint32_t)g_w, (uint32_t)g_h, 1}};
            vkCmdCopyImage(s_cb, g_slot_s[k], VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &rg);
            qbarrier(false, s_cb, dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_QUEUE_FAMILY_IGNORED,
                     VK_QUEUE_FAMILY_IGNORED, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_COLOR_ATTACHMENT_READ_BIT);
            qbarrier(false, s_cb, g_slot_s[k], VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL, s_qfam, VK_QUEUE_FAMILY_FOREIGN_EXT,
                     VK_ACCESS_TRANSFER_READ_BIT, 0);
            vkEndCommandBuffer(s_cb);
            VkSubmitInfo si = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &s_cb};
            vk_queue_lock();
            vkQueueSubmit(s_queue, 1, &si, s_fence);
            vk_queue_unlock();
            vkWaitForFences(s_dev, 1, &s_fence, VK_TRUE, 1000000000ull);
            vkResetFences(s_dev, 1, &s_fence);
            b3 = now_ms();
            XrSwapchainImageReleaseInfo ri = {.type = XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
            vk_queue_lock();
            xrReleaseSwapchainImage(g_sc[g_acq_sc], &ri);
            vk_queue_unlock();
            pthread_mutex_lock(&g_shown_lock);
            g_shown = g_slot_shot[k];
            g_shown_sc = g_acq_sc;
            pthread_mutex_unlock(&g_shown_lock);
            g_released = 1;
        }
        pthread_mutex_lock(&g_bl);
        g_slot_state[k] = 0;
        pthread_cond_broadcast(&g_bc);
        pthread_mutex_unlock(&g_bl);
        // (LARECOMP_LOG_PACE) where the bridge's time goes, every second: Turnip's frame awaited, the swapchain image,
        // the system driver's copy, the release
        static double acc[4], t_last;
        static int nb;
        double b4 = now_ms();
        acc[0] += b1 - b0, acc[1] += b2 - b1, acc[2] += b3 - b2, acc[3] += b4 - b3, nb++;
        if (b4 - t_last > 1000) {
            if (getenv("LARECOMP_LOG_PACE"))
                fprintf(stderr, "[bridge] %d frames/%.0f ms: turnip wait %.2f, acquire %.2f, copy %.2f, release %.2f ms each\n", nb, b4 - t_last,
                        acc[0] / nb, acc[1] / nb, acc[2] / nb, acc[3] / nb);
            acc[0] = acc[1] = acc[2] = acc[3] = 0, nb = 0, t_last = b4;
        }
    }
    return NULL;
}

#else
static void load_vulkan(void *app) { (void)app; }  // (the system's Vulkan loader, linked)
static void bridge_submitted(void) {}
#endif

// ------------------------------------------------------------------ Vulkan through OpenXR (vk_xr.h hooks)
static VkResult hk_create_instance(const VkInstanceCreateInfo *ci, VkInstance *out) {
#ifdef __ANDROID__
    if (g_turnip) {  // (the game's: Turnip's own; OpenXR gets the system's in hk_device_ready)
        VkResult r = lr_vkCreateInstance(ci, NULL, out);
        XLOG("Vulkan instance (Turnip): %d", r);
        return r;
    }
#endif
    XrGraphicsRequirementsVulkan2KHR req = {.type = XR_TYPE_GRAPHICS_REQUIREMENTS_VULKAN2_KHR};
    p_requirements(g_inst, g_sys, &req);  // (required before the instance)
    XrVulkanInstanceCreateInfoKHR xi = {.type = XR_TYPE_VULKAN_INSTANCE_CREATE_INFO_KHR, .systemId = g_sys,
                                        .pfnGetInstanceProcAddr = vkGetInstanceProcAddr, .vulkanCreateInfo = ci};
    VkResult vr = VK_ERROR_INITIALIZATION_FAILED;
    XrResult r = p_create_instance(g_inst, &xi, out, &vr);
    XLOG("Vulkan instance through OpenXR: xr %d vk %d", r, vr);
    return XR_SUCCEEDED(r) ? vr : VK_ERROR_INITIALIZATION_FAILED;
}

static VkPhysicalDevice hk_physical_device(VkInstance vi) {
#ifdef __ANDROID__
    if (g_turnip) {
        uint32_t n = 1;
        VkPhysicalDevice pd = VK_NULL_HANDLE;
        lr_vkEnumeratePhysicalDevices(vi, &n, &pd);
        return pd;
    }
#endif
    XrVulkanGraphicsDeviceGetInfoKHR gi = {.type = XR_TYPE_VULKAN_GRAPHICS_DEVICE_GET_INFO_KHR, .systemId = g_sys, .vulkanInstance = vi};
    VkPhysicalDevice pd = VK_NULL_HANDLE;
    p_graphics_device(g_inst, &gi, &pd);
    return pd;
}

static VkResult hk_create_device(VkPhysicalDevice pd, const VkDeviceCreateInfo *ci, VkDevice *out) {
#ifdef __ANDROID__
    if (g_turnip) {  // (the game's, with what the shared images need)
        uint32_t ns = 0;
        lr_vkEnumerateDeviceExtensionProperties(pd, NULL, &ns, NULL);
        VkExtensionProperties *sup = calloc(ns ? ns : 1, sizeof *sup);
        lr_vkEnumerateDeviceExtensionProperties(pd, NULL, &ns, sup);
        const char **exts = calloc(ci->enabledExtensionCount + 16, sizeof *exts);
        uint32_t ne = 0;
        add_exts(ci->ppEnabledExtensionNames, ci->enabledExtensionCount, sup, ns, exts, &ne);
        VkDeviceCreateInfo c2 = *ci;
        c2.enabledExtensionCount = ne, c2.ppEnabledExtensionNames = exts;
        VkResult r = lr_vkCreateDevice(pd, &c2, NULL, out);
        XLOG("Vulkan device (Turnip): %d, %u extensions", r, ne);
        free(sup), free(exts);
        return r;
    }
#endif
    XrVulkanDeviceCreateInfoKHR xi = {.type = XR_TYPE_VULKAN_DEVICE_CREATE_INFO_KHR, .systemId = g_sys,
                                      .pfnGetInstanceProcAddr = vkGetInstanceProcAddr, .vulkanPhysicalDevice = pd,
                                      .vulkanCreateInfo = ci};
    VkResult vr = VK_ERROR_INITIALIZATION_FAILED;
    XrResult r = p_create_device(g_inst, &xi, out, &vr);
    XLOG("Vulkan device through OpenXR: xr %d vk %d", r, vr);
    return XR_SUCCEEDED(r) ? vr : VK_ERROR_INITIALIZATION_FAILED;
}

// The session, its space and the screen's swapchain, once the game's device exists (the game thread).
static void session_make(VkInstance vi, VkPhysicalDevice pd, VkDevice dev, uint32_t qfam);
static void hk_device_ready(VkInstance vi, VkPhysicalDevice pd, VkDevice dev, uint32_t qfam) {
#ifdef __ANDROID__
    if (g_turnip) {  // (the session on the system driver's device; the frames through the shared images)
        g_tdev = dev, g_tqfam = qfam;
        if (!system_device()) { XLOG("no system device for OpenXR: no headset image"); return; }
        session_make(s_inst, s_pd, s_dev, s_qfam);
        g_direct = false;
        if (g_session_made && !bridge_init()) XLOG("no bridge: no headset image");
        return;
    }
#endif
    session_make(vi, pd, dev, qfam);
}
static void session_make(VkInstance vi, VkPhysicalDevice pd, VkDevice dev, uint32_t qfam) {
    extern uint32_t vk_xr_queue_index(void);  // (runtime/nvn/vulkan/device.c: a queue of its own when the family has two)
    XrGraphicsBindingVulkan2KHR gb = {.type = XR_TYPE_GRAPHICS_BINDING_VULKAN2_KHR, .instance = vi, .physicalDevice = pd,
                                      .device = dev, .queueFamilyIndex = qfam, .queueIndex = g_turnip ? 0 : vk_xr_queue_index()};
    XrSessionCreateInfo sci = {.type = XR_TYPE_SESSION_CREATE_INFO, .next = &gb, .systemId = g_sys};
    XrResult r = xrCreateSession(g_inst, &sci, &g_session);
    if (XR_FAILED(r)) { XLOG("xrCreateSession failed: %d", r); return; }
    XrReferenceSpaceCreateInfo rs = {.type = XR_TYPE_REFERENCE_SPACE_CREATE_INFO, .referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL,
                                     .poseInReferenceSpace = {{0, 0, 0, 1}, {0, 0, 0}}};
    xrCreateReferenceSpace(g_session, &rs, &g_space);
    // (the Quest offers 70 formats, measured 20/10: asked by count first, never with a fixed array)
    int64_t fmt = 0;
    uint32_t nf = 0;
    xrEnumerateSwapchainFormats(g_session, 0, &nf, NULL);
    int64_t *fmts = calloc(nf ? nf : 1, sizeof *fmts);
    if (XR_FAILED(xrEnumerateSwapchainFormats(g_session, nf, &nf, fmts))) nf = 0;
    for (uint32_t i = 0; i < nf && !fmt; i++) if (fmts[i] == VK_FORMAT_R8G8B8A8_SRGB) fmt = fmts[i];
    for (uint32_t i = 0; i < nf && !fmt; i++) if (fmts[i] == VK_FORMAT_R8G8B8A8_UNORM) fmt = fmts[i];
    free(fmts);
    if (!fmt) { XLOG("no RGBA8 swapchain format (%u offered)", nf); return; }
    // (the game's image is RGBA8 holding sRGB-encoded values: written bit for bit into an sRGB swapchain - copied, or
    // drawn through an RGBA8 UNORM view of it, hence its mutable format - the compositor reads them as they were meant)
    XrSwapchainCreateInfo sc = {.type = XR_TYPE_SWAPCHAIN_CREATE_INFO,
                                .usageFlags = XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT | XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT |
                                              XR_SWAPCHAIN_USAGE_TRANSFER_SRC_BIT | XR_SWAPCHAIN_USAGE_MUTABLE_FORMAT_BIT,
                                .format = fmt, .sampleCount = 1, .width = (uint32_t)g_sw, .height = (uint32_t)g_sh,
                                .faceCount = 1, .arraySize = 1, .mipCount = 1};
    r = xrCreateSwapchain(g_session, &sc, &g_sc[0]);
    g_direct = XR_SUCCEEDED(r) || fmt == VK_FORMAT_R8G8B8A8_UNORM;
    if (XR_FAILED(r)) {  // (no mutable format: the copy only)
        sc.usageFlags = XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT | XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT;
        r = xrCreateSwapchain(g_session, &sc, &g_sc[0]);
    }
    if (XR_FAILED(r)) { XLOG("xrCreateSwapchain failed: %d", r); return; }
    const char *se = getenv("LARECOMP_XR_SWAPCHAINS");
    g_nsc = ((se && *se == '2') || g_stereo) && XR_SUCCEEDED(xrCreateSwapchain(g_session, &sc, &g_sc[1])) ? 2 : 1;
    if (g_stereo && g_nsc < 2) { g_stereo = false; XLOG("no second swapchain: stereo off"); }
    if (g_stereo) {
        pthread_t rt;
        pthread_create(&rt, NULL, release_thread, NULL);
    }
    uint32_t counts[2] = {0, 0};
    for (int s = 0; s < g_nsc; s++) {
        for (int i = 0; i < 8; i++) g_images[s][i] = (XrSwapchainImageVulkan2KHR){.type = XR_TYPE_SWAPCHAIN_IMAGE_VULKAN2_KHR};
        xrEnumerateSwapchainImages(g_sc[s], 8, &counts[s], (XrSwapchainImageBaseHeader *)g_images[s]);
    }
    if (g_nsc == 2 && counts[1] != counts[0]) g_nsc = 1;  // (a flat index needs the same count in both)
    g_nimages = counts[0];
    if (g_aset) {
        XrSessionActionSetsAttachInfo at = {.type = XR_TYPE_SESSION_ACTION_SETS_ATTACH_INFO, .countActionSets = 1, .actionSets = &g_aset};
        xrAttachSessionActionSets(g_session, &at);
    }
    if (g_sw_ext && g_stereo == 2 && g_direct) {  // AppSW's motion and depth swapchains, an eye each
        XrSystemSpaceWarpPropertiesFB swp = {.type = XR_TYPE_SYSTEM_SPACE_WARP_PROPERTIES_FB};
        XrSystemProperties sp = {.type = XR_TYPE_SYSTEM_PROPERTIES, .next = &swp};
        if (g_synth_ext) {
            XrFrameSynthesisConfigViewEXT cfg[2] = {{.type = XR_TYPE_FRAME_SYNTHESIS_CONFIG_VIEW_EXT},
                                                   {.type = XR_TYPE_FRAME_SYNTHESIS_CONFIG_VIEW_EXT}};
            XrViewConfigurationView cv[2] = {{.type = XR_TYPE_VIEW_CONFIGURATION_VIEW, .next = &cfg[0]},
                                               {.type = XR_TYPE_VIEW_CONFIGURATION_VIEW, .next = &cfg[1]}};
            uint32_t count = 0;
            XrResult r = xrEnumerateViewConfigurationViews(g_inst, g_sys, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, 2, &count, cv);
            if (XR_SUCCEEDED(r) && count == 2) {
                swp.recommendedMotionVectorImageRectWidth = cfg[0].recommendedMotionVectorImageRectWidth;
                swp.recommendedMotionVectorImageRectHeight = cfg[0].recommendedMotionVectorImageRectHeight;
            }
            XLOG("EXT frame synthesis configuration: %d, %u views", r, count);
        } else xrGetSystemProperties(g_inst, g_sys, &sp);
        // Meta explicitly recommends the queried size: supersampling motion vectors
        // adds bandwidth without improving AppSW. Keep an override for controlled A/B.
        const char *mve = getenv("LARECOMP_XR_MV_SCALE");
        float mvscale = mve ? strtof(mve, NULL) : 1;
        if (!isfinite(mvscale) || mvscale < 0.5f || mvscale > 2) mvscale = 1;
        g_mvw = (uint32_t)lroundf(swp.recommendedMotionVectorImageRectWidth * mvscale);
        g_mvh = (uint32_t)lroundf(swp.recommendedMotionVectorImageRectHeight * mvscale);
        if (!g_mvw || !g_mvh) g_mvw = (uint32_t)g_sw / 4, g_mvh = (uint32_t)g_sh / 4;
        uint32_t nf2 = 0;
        xrEnumerateSwapchainFormats(g_session, 0, &nf2, NULL);
        int64_t *f2 = calloc(nf2 ? nf2 : 1, sizeof *f2);
        if (XR_FAILED(xrEnumerateSwapchainFormats(g_session, nf2, &nf2, f2))) nf2 = 0;
        bool rgba16f = false;
        g_dp_fmt = VK_FORMAT_UNDEFINED;
        static const VkFormat want[] = {VK_FORMAT_D32_SFLOAT, VK_FORMAT_D16_UNORM, VK_FORMAT_D24_UNORM_S8_UINT};
        for (uint32_t i = 0; i < nf2; i++) if (f2[i] == VK_FORMAT_R16G16B16A16_SFLOAT) rgba16f = true;
        for (int k = 0; k < 3 && !g_dp_fmt; k++) for (uint32_t i = 0; i < nf2; i++) if (f2[i] == want[k]) g_dp_fmt = want[k];
        free(f2);
        bool ok = rgba16f && g_dp_fmt;
        for (int e = 0; e < 2 && ok; e++) {
            XrSwapchainCreateInfo mc = {.type = XR_TYPE_SWAPCHAIN_CREATE_INFO,
                                        .usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_SAMPLED_BIT,
                                        .format = VK_FORMAT_R16G16B16A16_SFLOAT, .sampleCount = 1, .width = g_mvw, .height = g_mvh,
                                        .faceCount = 1, .arraySize = 1, .mipCount = 1};
            XrSwapchainCreateInfo dc = mc;
            dc.usageFlags = XR_SWAPCHAIN_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_SAMPLED_BIT, dc.format = g_dp_fmt;
            ok = XR_SUCCEEDED(xrCreateSwapchain(g_session, &mc, &g_mv_sc[e])) && XR_SUCCEEDED(xrCreateSwapchain(g_session, &dc, &g_dp_sc[e]));
            uint32_t n1 = 0, n2 = 0;
            for (int i = 0; i < 8; i++) g_mv_img[e][i] = g_dp_img[e][i] = (XrSwapchainImageVulkan2KHR){.type = XR_TYPE_SWAPCHAIN_IMAGE_VULKAN2_KHR};
            if (ok) xrEnumerateSwapchainImages(g_mv_sc[e], 8, &n1, (XrSwapchainImageBaseHeader *)g_mv_img[e]);
            if (ok) xrEnumerateSwapchainImages(g_dp_sc[e], 8, &n2, (XrSwapchainImageBaseHeader *)g_dp_img[e]);
            ok = ok && n1 && n2;
        }
        g_appsw = ok;
        XLOG("AppSW: %s (motion %ux%u, recommended %ux%u; RGBA16F %s, depth format %d)", ok ? "ready" : "unavailable", g_mvw, g_mvh,
             swp.recommendedMotionVectorImageRectWidth, swp.recommendedMotionVectorImageRectHeight, rgba16f ? "yes" : "no", g_dp_fmt);
    }
    XLOG("session ready: screen %dx%d, format %lld, %d swapchain%s of %u images", g_w, g_h, (long long)fmt, g_nsc, g_nsc > 1 ? "s" : "", g_nimages);
    g_session_made = 1;
}

// Barriers in the render thread's command buffer (the frame's hooks below run there: vk_frame_end): through the
// recorder thread like the rest of it (record.h), as synchronization2 - the recorder records nothing else. Recorded
// straight, they landed beside the recorder's own work in a command buffer it was filling: the driver read 0x10 and
// the game stopped in the headset (Beta 6's recorder on, 12/11/2026). The flags keep their bits in the 2 forms.
static void barriers2(VkCommandBuffer cb, VkPipelineStageFlags src_stage, VkPipelineStageFlags dst_stage, uint32_t n,
                      const VkImageMemoryBarrier *b) {
    VkImageMemoryBarrier2 b2[4];
    if (n > 4) n = 4;
    for (uint32_t i = 0; i < n; i++)
        b2[i] = (VkImageMemoryBarrier2){.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2, .srcStageMask = src_stage,
                                        .srcAccessMask = b[i].srcAccessMask, .dstStageMask = dst_stage, .dstAccessMask = b[i].dstAccessMask,
                                        .oldLayout = b[i].oldLayout, .newLayout = b[i].newLayout,
                                        .srcQueueFamilyIndex = b[i].srcQueueFamilyIndex, .dstQueueFamilyIndex = b[i].dstQueueFamilyIndex,
                                        .image = b[i].image, .subresourceRange = b[i].subresourceRange};
    VkDependencyInfo di = {.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .imageMemoryBarrierCount = n, .pImageMemoryBarriers = b2};
    rec_CmdPipelineBarrier2(cb, &di);
}
static void barrier(VkCommandBuffer cb, VkImage img, VkImageLayout from, VkImageLayout to, VkAccessFlags src_access,
                    VkAccessFlags dst_access, VkPipelineStageFlags src_stage, VkPipelineStageFlags dst_stage) {
    VkImageMemoryBarrier b = {.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, .srcAccessMask = src_access, .dstAccessMask = dst_access,
                              .oldLayout = from, .newLayout = to, .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                              .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .image = img,
                              .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
    barriers2(cb, src_stage, dst_stage, 1, &b);
}

// A finished frame (the game thread): take a swapchain image and copy the game's image into it.
// The next frame's view (game thread, at the end of a frame: the next frame's scene is drawn before its end): the head's
// pose and both eyes' fields put together, given to the first-person camera.
static _Atomic bool g_hold_black;  // (next_view: the view from his eyes wanted, not there for a moment - shown black)
static void next_view(void) {
    XrTime t = g_display_time;
    g_rendering.vr = 0;
    // (the first-person view off - R3, as on a screen: the game's own camera, on the floating screen; its image shown as
    //  from the head was the view from above glued to the face)
    extern bool fp_view_on(void);
    // (wanted but not there for a moment - a door, a level loading: black meanwhile, stereo2_frame - his head's place and
    //  the stick's tilt kept, or the view jumped after every door)
    extern bool fp_view_held(void);
    {   // (each switch between the view from his eyes and the floating screen, and why: a talk in a house showed the
        //  screen with nothing in the first-person log, 27/10)
        extern uint64_t renderer_present_count(void);
        int why = !g_vr ? 1 : !g_running ? 2 : !t ? 3 : !fp_view_on() ? (fp_view_held() ? 5 : 4) : 0;
#ifndef __ANDROID__
        if (why == 0 && menu_is_open()) why = 6;  // (LAS Recomp's menu: read flat on the screen, not rebuilt in depth)
#endif
        static int was = -1;
        static const char *const whys[] = {"from his eyes", "floating screen (both sticks clicked)", "screen: session not running",
                                           "screen: no display time", "screen: first-person view off", "black: the view held",
                                           "screen: LAS Recomp's menu"};
        // (to the screen only after 3 frames of the view off: as it came back, a single frame of the floating screen
        //  showed before the view from his eyes - at the play's start and after each door, 27/10 - black meanwhile)
        static int off_n, on_n;
        off_n = why == 4 ? off_n + 1 : 0;
        on_n = why == 0 ? on_n + 1 : 0;
        if (why == 4 && off_n < 3 && was >= 0 && was != 4) why = 5;
        // (back from black: 12 frames of the view on first - the new level's first frame, made before its camera was
        //  ours, showed the room from above, flattened, then a fragment of a frame at the door and the sky black a
        //  moment, 27/10 replay and Guillaume)
        if (why == 0 && was == 5 && on_n < 12) why = 5;
        if (why != was) XLOG("headset: %s (frame %llu)", whys[why], (unsigned long long)renderer_present_count()), was = why;
        g_hold_black = why == 5;
    }
    bool menu = false;
#ifndef __ANDROID__
    menu = menu_is_open();
#endif
    if (!g_vr || !g_running || !t || !fp_view_on() || g_hold_black || menu) { if (!g_hold_black) fp_xr_off(); return; }
    XrViewLocateInfo li = {.type = XR_TYPE_VIEW_LOCATE_INFO, .viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO,
                           .displayTime = t, .space = g_space};
    XrViewState vs = {.type = XR_TYPE_VIEW_STATE};
    XrView v[2] = {{.type = XR_TYPE_VIEW}, {.type = XR_TYPE_VIEW}};
    uint32_t n = 0;
    // A moment without the head's pose (tracking lost - the headset set down, its cameras covered): the last pose is
    // held. Switching to the flat view for those frames changed the camera, its field and what is culled from one frame
    // to the next: the world flickered between two views, often half of it missing (21/10, recorded in the headset).
    static Shot held;
    static float held_q[4], held_tan[4];
    static uint32_t nheld;
    if (XR_FAILED(xrLocateViews(g_session, &li, &vs, 2, &n, v)) || n < 2 ||
        !(vs.viewStateFlags & XR_VIEW_STATE_ORIENTATION_VALID_BIT)) {
        if (!held.vr) { fp_xr_off(); return; }
        if (nheld++ % 300 == 0) XLOG("head pose not located (%u frames so far): the last one held", nheld);
        static const float zero[3];
        fp_xr_view(held_q, zero, held_tan);
        g_rendering = held;
        return;
    }
    // Optional benchmark pose: keep the source scene identical between A/B runs.
    // Normal play always uses the tracked poses above. Preserve the measured IPD.
    // LARECOMP_XR_BENCH_POSE=2 (recordings with nobody in the headset): the game still sees from a head looking straight
    // ahead, but each eye's image is handed to the compositor at the headset's real pose - shown straight in front of
    // it, whole, however the headset lies on the desk (with 1 the compositor put the image where the locked head
    // looked: half the recording black, the rest tilted, 27/10)
    static int bench_pose = -1;
    if (bench_pose < 0) {
        const char *e = getenv("LARECOMP_XR_BENCH_POSE");
        bench_pose = e && *e >= '1' && *e <= '3' ? *e - '0' : 0;
    }
    XrPosef real[2] = {v[0].pose, v[1].pose};
    if (bench_pose) {
        float dx = v[1].pose.position.x - v[0].pose.position.x;
        float dy = v[1].pose.position.y - v[0].pose.position.y;
        float dz = v[1].pose.position.z - v[0].pose.position.z;
        float half_ipd = 0.5f * sqrtf(dx * dx + dy * dy + dz * dz);
        // 3: as 2, but a head that looks around by itself - left and right 70 degrees over 6 s, up and down 20 degrees
        // over 4.3 s - a player's head without a player (12/11/2026: the lags came with his head moving, never on a
        // replay with the headset on the desk). Each eye along the turned head's x axis.
        XrQuaternionf q = {.w = 1};
        if (bench_pose == 3) {
            double sec = t * 1e-9;
            float yaw = 1.2217f * sinf((float)(6.283185307 * sec / 6.0)), pitch = 0.3491f * sinf((float)(6.283185307 * sec / 4.3));
            float cy = cosf(yaw / 2), sy = sinf(yaw / 2), cp = cosf(pitch / 2), sp = sinf(pitch / 2);
            q = (XrQuaternionf){.x = cy * sp, .y = sy * cp, .z = -sy * sp, .w = cy * cp};  // (yaw, then pitch)
        }
        float hx[3] = {1 - 2 * (q.y * q.y + q.z * q.z), 2 * (q.x * q.y + q.z * q.w), 2 * (q.x * q.z - q.y * q.w)};
        v[0].pose = (XrPosef){.orientation = q, .position = {-half_ipd * hx[0], -half_ipd * hx[1], -half_ipd * hx[2]}};
        v[1].pose = (XrPosef){.orientation = q, .position = {half_ipd * hx[0], half_ipd * hx[1], half_ipd * hx[2]}};
        static bool said;
        if (!said) { said = true; XLOG("benchmark pose %s, IPD %.1f mm", bench_pose == 3 ? "looking around by itself" : "locked", half_ipd * 2000); }
    }
    if (g_stereo == 2) {  // (one image from the head's centre over both eyes' fields and a margin; each eye made from it)
        static float M = -1;
        if (M < 0) { const char *e = getenv("LARECOMP_XR_MARGIN"); M = e ? (float)atof(e) : 0.10f; }
        Shot s = {.vr = 1, .pose = v[0].pose};
        float c[3], xa[3];
        for (int i = 0; i < 3; i++) c[i] = ((&v[0].pose.position.x)[i] + (&v[1].pose.position.x)[i]) / 2, (&s.pose.position.x)[i] = c[i];
        {   // (the head's x axis, from its orientation)
            float x = v[0].pose.orientation.x, y = v[0].pose.orientation.y, z = v[0].pose.orientation.z, w = v[0].pose.orientation.w;
            xa[0] = 1 - 2 * (y * y + z * z), xa[1] = 2 * (x * y + z * w), xa[2] = 2 * (x * z - y * w);
        }
        for (int e = 0; e < 2; e++) {
            float t[4] = {tanf(v[e].fov.angleLeft) - M, tanf(v[e].fov.angleRight) + M, tanf(v[e].fov.angleUp) + M,
                          tanf(v[e].fov.angleDown) - M};
            memcpy(s.etan[e], t, sizeof t);
            s.efov[e] = (XrFovf){atanf(t[0]), atanf(t[1]), atanf(t[2]), atanf(t[3])};
            s.epose[e] = bench_pose >= 2 ? real[e] : v[e].pose;
            s.eoff[e] = 0;
            for (int i = 0; i < 3; i++) s.eoff[e] += ((&v[e].pose.position.x)[i] - c[i]) * xa[i];
            // (LARECOMP_XR_IPD_SCALE: the eyes' shift from the depth scaled - 0: both the same image, nothing rebuilt; a
            //  test of what the rebuilding itself does, the sword's ghost, 27/10)
            static float ipd_k = -1;
            if (ipd_k < 0) ipd_k = getenv("LARECOMP_XR_IPD_SCALE") ? (float)atof(getenv("LARECOMP_XR_IPD_SCALE")) : 1;
            s.eoff[e] *= ipd_k;
        }
        {   // (what he holds drawn from each eye: firstperson.c viewmodel)
            extern void fp_xr_eye_offsets(float left, float right);
            fp_xr_eye_offsets(s.eoff[0], s.eoff[1]);
        }
        // (an eye's source reaches a little past its own field: its shift - 0.04, a point 0.32 game units away)
        s.ctan[0] = fminf(s.etan[0][0], s.etan[1][0]) - 0.04f, s.ctan[1] = fmaxf(s.etan[0][1], s.etan[1][1]) + 0.04f;
        s.ctan[2] = fmaxf(s.etan[0][2], s.etan[1][2]), s.ctan[3] = fminf(s.etan[0][3], s.etan[1][3]);
        s.fov = (XrFovf){atanf(s.ctan[0]), atanf(s.ctan[1]), atanf(s.ctan[2]), atanf(s.ctan[3])};
        const float q[4] = {v[0].pose.orientation.x, v[0].pose.orientation.y, v[0].pose.orientation.z, v[0].pose.orientation.w};
        // The head's place too (LARECOMP_XR_6DOF=0: its turn only): leaning moves the view. Without it the world stayed
        // glued to the face (« bizarre quand je bouge la tête », 22/10), and with AppSW - the compositor given each eye's
        // depth, moving the image with the head between two game images - it jumped back at every new image: jerks as
        // soon as the head moved (22/10, 72 Hz). The image is now made from where the layer's pose says it was.
        static int six = -1;
        if (six < 0) { const char *e6 = getenv("LARECOMP_XR_6DOF"); six = !e6 || *e6 != '0'; }
        static const float zero[3];
        fp_xr_view(q, six ? c : zero, s.ctan);
        static int slogged;
        if (!slogged++)
            XLOG("stereo from depth: frame tangents %.2f %.2f %.2f %.2f, eyes %.1f and %.1f mm from the head's centre",
                 s.ctan[0], s.ctan[1], s.ctan[2], s.ctan[3], 1000 * s.eoff[0], 1000 * s.eoff[1]);
        g_rendering = held = s;
        memcpy(held_q, q, sizeof held_q), memcpy(held_tan, s.ctan, sizeof held_tan);
        return;
    }
    if (g_stereo == 1) {  // (one eye a frame, in turn: its own pose, its place from the head's centre, its own field)
        int e = g_eye_next;
        g_eye_next ^= 1;
        Shot s = {.vr = 1, .eye = e, .pose = v[e].pose, .fov = v[e].fov};
        const float q[4] = {v[e].pose.orientation.x, v[e].pose.orientation.y, v[e].pose.orientation.z, v[e].pose.orientation.w};
        float pos[3];
        for (int i = 0; i < 3; i++) pos[i] = (&v[e].pose.position.x)[i] - ((&v[0].pose.position.x)[i] + (&v[1].pose.position.x)[i]) / 2;
        const float tan[4] = {tanf(v[e].fov.angleLeft), tanf(v[e].fov.angleRight), tanf(v[e].fov.angleUp), tanf(v[e].fov.angleDown)};
        fp_xr_view(q, pos, tan);
        static int slogged;
        if (slogged++ < 2)
            XLOG("eye %d: tangents %.2f %.2f %.2f %.2f, %.1f mm from the head's centre", e, tan[0], tan[1], tan[2], tan[3],
                 1000 * sqrtf(pos[0] * pos[0] + pos[1] * pos[1] + pos[2] * pos[2]));
        g_rendering = held = s;
        memcpy(held_q, q, sizeof held_q), memcpy(held_tan, tan, sizeof held_tan);
        return;
    }
    Shot s = {.vr = 1, .pose = v[0].pose};
    for (int i = 0; i < 3; i++) (&s.pose.position.x)[i] = ((&v[0].pose.position.x)[i] + (&v[1].pose.position.x)[i]) / 2;
    s.fov.angleLeft = fminf(v[0].fov.angleLeft, v[1].fov.angleLeft);
    s.fov.angleRight = fmaxf(v[0].fov.angleRight, v[1].fov.angleRight);
    s.fov.angleUp = fmaxf(v[0].fov.angleUp, v[1].fov.angleUp);
    s.fov.angleDown = fminf(v[0].fov.angleDown, v[1].fov.angleDown);
    const float q[4] = {s.pose.orientation.x, s.pose.orientation.y, s.pose.orientation.z, s.pose.orientation.w};
    const float pos[3] = {0, 0, 0};  // (from the head's centre: one view for both eyes)
    const float tan[4] = {tanf(s.fov.angleLeft), tanf(s.fov.angleRight), tanf(s.fov.angleUp), tanf(s.fov.angleDown)};
    fp_xr_view(q, pos, tan);
    static int logged;
    if (!logged++) XLOG("head view: tangents %.2f %.2f %.2f %.2f", tan[0], tan[1], tan[2], tan[3]);
    g_rendering = held = s;
    memcpy(held_q, q, sizeof held_q), memcpy(held_tan, tan, sizeof held_tan);
}

static Shot g_finishing;  // (game thread) what the frame being copied was rendered with

// The next swapchain image, taken and waited for: its index over both swapchains (g_nimages each), or -1.
static double now_ms(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec * 1e3 + t.tv_nsec / 1e6;
}
static int acquire_image_sc(int s);
static int acquire_image(void) {
    if (!g_running || !g_nsc) return -1;
    int s = g_next_sc;
    g_next_sc = (s + 1) % g_nsc;
    return acquire_image_sc(s);
}
static int acquire_image_sc(int s) {
    if (!g_running || s >= g_nsc) return -1;
    if (g_stereo) {  // (one image of a swapchain waited for at a time: the last one released first)
        pthread_mutex_lock(&g_rel_lock);
        while (g_rel_pending[s]) pthread_cond_wait(&g_rel_cond, &g_rel_lock);
        pthread_mutex_unlock(&g_rel_lock);
    }
    static int log = -1;  // LARECOMP_LOG_PACE=1: an acquisition that took over 1 ms, and where
    if (log < 0) log = getenv("LARECOMP_LOG_PACE") != NULL;
    double t0 = log ? now_ms() : 0;
    uint32_t idx = 0;
    XrSwapchainImageAcquireInfo ai = {.type = XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
    vk_queue_lock();
    double t1 = log ? now_ms() : 0;
    XrResult r = xrAcquireSwapchainImage(g_sc[s], &ai, &idx);
    vk_queue_unlock();
    if (XR_FAILED(r)) return -1;
    double t2 = log ? now_ms() : 0;
    XrSwapchainImageWaitInfo wi = {.type = XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO, .timeout = XR_INFINITE_DURATION};
    xrWaitSwapchainImage(g_sc[s], &wi);
    if (log) {
        double t3 = now_ms();
        if (t3 - t0 > 1) fprintf(stderr, "[xr] acquire %.2f ms: queue lock %.2f, acquire %.2f, wait %.2f (swapchain %d image %u)\n", t3 - t0, t1 - t0, t2 - t1, t3 - t2, s, idx);
    }
    g_acq_sc = s;
    return s * (int)g_nimages + (int)idx;
}
static VkImage image_at(int k) { return g_images[k / (int)g_nimages][k % (int)g_nimages].image; }

// (stereo) the release thread: each image released once the GPU has finished its frame, then shown to its eye
static void *release_thread(void *arg) {
    (void)arg;
    extern int vk_wait_seq_quiet(uint64_t seq, uint64_t timeout_ns);
    extern void plat_thread_name(const char *name);
    plat_thread_name("xr-release");
    XLOG("release: stereo pairs and their poses published together");
    for (;;) {
        pthread_mutex_lock(&g_rel_lock);
        while (!g_rel_n) pthread_cond_wait(&g_rel_cond, &g_rel_lock);
        uint64_t seq = g_rel_q[0].seq;
        int count = g_stereo == 2 && g_rel_n >= 2 && g_rel_q[1].seq == seq &&
                    g_rel_q[0].sc != g_rel_q[1].sc &&
                    g_rel_q[0].shot.frame_id == g_rel_q[1].shot.frame_id ? 2 : 1;
        int sc[2], sw[2]; Shot shot[2];
        for (int i = 0; i < count; i++) sc[i] = g_rel_q[i].sc, sw[i] = g_rel_q[i].sw, shot[i] = g_rel_q[i].shot;
        pthread_mutex_unlock(&g_rel_lock);
        int r = vk_wait_seq_quiet(seq, 1000000000ull);
        static int nlog;
        if (r) {
            if (nlog++ < 5) XLOG("release: submission %llu not done (%d); unfinished images retained", (unsigned long long)seq, r);
            if (r == VK_TIMEOUT) continue;
            return NULL;  // device failure: never expose unfinished images
        }
        XrSwapchainImageReleaseInfo ri = {.type = XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
        vk_queue_lock();
        for (int i = 0; i < count; i++) {
            xrReleaseSwapchainImage(g_sc[sc[i]], &ri);
            if (sw[i]) xrReleaseSwapchainImage(g_mv_sc[sc[i]], &ri), xrReleaseSwapchainImage(g_dp_sc[sc[i]], &ri);
        }
        pthread_mutex_lock(&g_shown_lock);
        for (int i = 0; i < count; i++) {
            g_shown = shot[i]; g_shown_sc = sc[i];
            int ee = g_stereo == 2 ? sc[i] : shot[i].eye;
            if (shot[i].vr) g_shown_eye[ee] = shot[i], g_released_eye[ee] = 1;
            else if (g_screen && g_stereo == 2) g_released_eye[ee] = 1;
        }
        pthread_cond_broadcast(&g_shown_cond);
        pthread_mutex_unlock(&g_shown_lock);
        vk_queue_unlock();
        g_released = 1;
        pthread_mutex_lock(&g_rel_lock);
        g_rel_n -= count;
        memmove(&g_rel_q[0], &g_rel_q[count], (size_t)g_rel_n * sizeof g_rel_q[0]);
        for (int i = 0; i < count; i++) g_rel_pending[sc[i]] = 0;
        pthread_cond_broadcast(&g_rel_cond);
        pthread_mutex_unlock(&g_rel_lock);
    }
    return NULL;
}

// The swapchain's images, which the backend draws its final pass into (vk_xr.h images / acquire): no copy.
static uint32_t hk_images(VkImage *out, uint32_t max) {
    uint32_t n = g_nsc && g_direct && !g_turnip && !g_stereo ? g_nsc * g_nimages : 0;
    if (n > max) n = max;
    for (uint32_t i = 0; i < n; i++) out[i] = image_at((int)i);
    return n;
}
static int hk_acquire(void) {
    g_acquired = acquire_image();
    return g_acquired;
}

// (AppSW) an eye's motion and depth images for this frame (game thread), after its colour: w the warp's 8 floats.
// M, T: the previous image's view from this one's, in the head's frame - the body's turn and Link's step since.
static void appsw_eye(VkCommandBuffer cb, VkImage src, int e, const float *w, const Shot *s, float p10, float p11, int zo, float unit) {
    extern bool vk_xr_appsw(VkCommandBuffer cb, VkImage frame, uint32_t w, uint32_t h, const float *pc, VkImage mv, VkImage dp, VkFormat dfmt);
    static float M[9], T[3];
    static uint64_t done_frame;
    if (s->frame_id != done_frame) {  // (once per image: both eyes share it)
        static uint64_t previous_frame;
        if (getenv("LARECOMP_XR_HISTORY_LOG"))
            XLOG("MV history: frame %llu previous %llu", (unsigned long long)s->frame_id,
                 (unsigned long long)previous_frame);
        previous_frame = s->frame_id;
        static int have;
        static float pyaw, ppitch, peye[3];
        float h[9], B0[9], B1[9];
        {   float x = s->pose.orientation.x, y = s->pose.orientation.y, z = s->pose.orientation.z, q = s->pose.orientation.w;
            float r[9] = {1 - 2 * (y * y + z * z), 2 * (x * y - z * q), 2 * (x * z + y * q),
                          2 * (x * y + z * q), 1 - 2 * (x * x + z * z), 2 * (y * z - x * q),
                          2 * (x * z - y * q), 2 * (y * z + x * q), 1 - 2 * (x * x + y * y)};
            memcpy(h, r, sizeof r); }
        #define BODY(m, yw, pt) do { float cy = cosf(yw), sy = sinf(yw), cp = cosf(pt), sp = sinf(pt); \
            float b_[9] = {cy, -sy * sp, -sy * cp, 0, cp, -sp, sy, cy * sp, cy * cp}; memcpy(m, b_, sizeof b_); } while (0)
        BODY(B1, s->yaw, s->pitch);
        BODY(B0, have ? pyaw : s->yaw, have ? ppitch : s->pitch);
        #undef BODY
        float d[3] = {0, 0, 0}, dy = have ? s->yaw - pyaw : 0;
        if (have) for (int i = 0; i < 3; i++) d[i] = s->place[i] - peye[i];
        while (dy > 3.14159265f) dy -= 6.2831853f;
        while (dy < -3.14159265f) dy += 6.2831853f;
        bool cut = !have || fabsf(dy) > 0.5f || d[0] * d[0] + d[1] * d[1] + d[2] * d[2] > 1.0f;  // (a door, R3: no motion)
        if (cut) memcpy(B0, B1, sizeof B0), d[0] = d[1] = d[2] = 0;
        // M = h^T B0^T B1 h, T = h^T B0^T d
        float A[9], C[9], v[3];
        for (int r = 0; r < 3; r++) for (int c = 0; c < 3; c++) {
            A[r * 3 + c] = 0;
            for (int k = 0; k < 3; k++) A[r * 3 + c] += B0[k * 3 + r] * B1[k * 3 + c];  // B0^T B1
        }
        for (int r = 0; r < 3; r++) for (int c = 0; c < 3; c++) {
            C[r * 3 + c] = 0;
            for (int k = 0; k < 3; k++) C[r * 3 + c] += A[r * 3 + k] * h[k * 3 + c];  // (B0^T B1) h
        }
        for (int r = 0; r < 3; r++) for (int c = 0; c < 3; c++) {
            M[r * 3 + c] = 0;
            for (int k = 0; k < 3; k++) M[r * 3 + c] += h[k * 3 + r] * C[k * 3 + c];  // h^T (...)
        }
        for (int r = 0; r < 3; r++) { v[r] = 0; for (int k = 0; k < 3; k++) v[r] += B0[k * 3 + r] * d[k]; }  // B0^T d
        for (int r = 0; r < 3; r++) { T[r] = 0; for (int k = 0; k < 3; k++) T[r] += h[k * 3 + r] * v[k]; }  // h^T (...)
        have = 1, pyaw = s->yaw, ppitch = s->pitch, memcpy(peye, s->place, sizeof peye);
        done_frame = s->frame_id;
    }
    uint32_t ai = 0, di = 0;
    XrSwapchainImageAcquireInfo aq = {.type = XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
    XrSwapchainImageWaitInfo wi = {.type = XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO, .timeout = XR_INFINITE_DURATION};
    vk_queue_lock();
    XrResult r1 = xrAcquireSwapchainImage(g_mv_sc[e], &aq, &ai), r2 = xrAcquireSwapchainImage(g_dp_sc[e], &aq, &di);
    vk_queue_unlock();
    if (XR_FAILED(r1) || XR_FAILED(r2)) { XLOG("AppSW: acquire failed (%d, %d): off", r1, r2); g_appsw = 0; return; }
    xrWaitSwapchainImage(g_mv_sc[e], &wi);
    xrWaitSwapchainImage(g_dp_sc[e], &wi);
    VkImage mv = g_mv_img[e][ai].image, dp = g_dp_img[e][di].image;
    bool stencil = g_dp_fmt == VK_FORMAT_D24_UNORM_S8_UINT || g_dp_fmt == VK_FORMAT_D32_SFLOAT_S8_UINT;
    VkImageAspectFlags da = VK_IMAGE_ASPECT_DEPTH_BIT | (stencil ? VK_IMAGE_ASPECT_STENCIL_BIT : 0);
    VkImageMemoryBarrier b[2] = {
        {.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, .dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
         .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED, .newLayout = VK_IMAGE_LAYOUT_GENERAL, .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
         .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .image = mv, .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}},
        {.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, .dstAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
         .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED, .newLayout = VK_IMAGE_LAYOUT_GENERAL, .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
         .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .image = dp, .subresourceRange = {da, 0, 1, 0, 1}}};
    barriers2(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT, 2, b);
    const float *c = s->ctan, *t = s->etan[e];
    static float flip = -2;
    if (flip < -1) { const char *fe = getenv("LARECOMP_XR_MV_FLIPY"); flip = fe && *fe == '1' ? -1.0f : 1.0f; }
    float pc[32] = {
        (t[0] - c[0]) / (c[1] - c[0]), (t[1] - t[0]) / (c[1] - c[0]) / (float)g_mvw,
        (c[2] - t[2]) / (c[2] - c[3]), (t[2] - t[3]) / (c[2] - c[3]) / (float)g_mvh,
        w[4], p10, p11, (float)(zo ? 1 : 0),
        0, 0, 1, 1,
        M[0], M[1], M[2], T[0], M[3], M[4], M[5], T[1], M[6], M[7], M[8], T[2],
        t[0], t[1], t[2], t[3],
        s->eoff[e] / unit, 1.0f / (float)g_mvw, 1.0f / (float)g_mvh, flip};
    bool ok = vk_xr_appsw(cb, src, g_mvw, g_mvh, pc, mv, dp, g_dp_fmt);
    b[0].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, b[0].dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT;
    b[0].oldLayout = VK_IMAGE_LAYOUT_GENERAL, b[0].newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    b[1].srcAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT, b[1].dstAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT;
    b[1].oldLayout = VK_IMAGE_LAYOUT_GENERAL, b[1].newLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
    barriers2(cb, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 2, b);
    g_swacq[e] = ok ? 1 : 0;
    if (!ok) {  // (taken: released with the eye's image all the same, not chained)
        g_swacq[e] = 2;
    }
}

// (stereo 2) both eyes from this frame (game thread): each eye's image made from the frame and the scene's depth
// (exec.c vk_xr_eye_warp), copied into that eye's swapchain image. Flat screen (vr 0): the frame into the first one.
static void stereo2_frame(VkCommandBuffer cb, VkImage src) {
    extern VkImage vk_xr_eye_warp(VkCommandBuffer cb, VkImage frame, int eye, uint32_t w, uint32_t h, const float *pc, VkImage target);
    extern bool fp_xr_depth(float *p10, float *p11, int *zero_one);
    if (demand_frames() && g_appsw && g_running && g_focused && g_released) {
        static unsigned built, skipped;
        bool requested = atomic_exchange(&g_eye_requested, false);
        if (requested) built++; else skipped++;
        if ((built + skipped) % 300 == 0)
            XLOG("eye demand: built %u skipped %u of300 game frames", built, skipped), built = skipped = 0;
        if (!requested) return;
    }
    static int flags = -1;
    extern float fp_xr_unit(void);
    const float unit = fp_xr_unit();  // (metres per game unit, the world's size setting included: firstperson.c)
    if (flags < 0) {
        const char *e = getenv("LARECOMP_XR_WARP_SHOW");  // =depth: the scene's depth drawn in each eye (test)
        flags = e && !strcmp(e, "depth") ? 2 : 0;
        e = getenv("LARECOMP_XR_DEPTH_FLIP");  // =1: the depth read upside down (test)
        if (e && *e == '1') flags |= 4;
    }
    const Shot *s = &g_finishing;
    float p10 = 0, p11 = 1;
    int zo = 1;
    extern const void *fp_scene_depth(uint64_t *);
    extern uint64_t renderer_present_count(void);
    uint64_t depth_frame = ~0ull;
    bool current_depth = fp_scene_depth(&depth_frame) && depth_frame == renderer_present_count();
    // (the view off in the headset - the view from above on its screen - in relief too: the game camera's projection,
    //  firstperson.c fp_xr_depth; « en vue de haut tout est aplati », Guillaume, 27/10)
    // Off by default (LARECOMP_XR_TOP_RELIEF=1): each eye saw the view from above turned the other way, too flat to be
    // seen as one - the Quest's view from above, in its immersive mode, is the flat screen (checked in the headset, 27/10)
    static int top_relief = -1;
    if (top_relief < 0) top_relief = getenv("LARECOMP_XR_TOP_RELIEF") && atoi(getenv("LARECOMP_XR_TOP_RELIEF"));
    bool relief = g_screen ? g_screen3d : top_relief && g_screen3d;
    bool depth = current_depth && (s->vr || relief) && fp_xr_depth(&p10, &p11, &zo);
    if (depth) {  // (AppSW: the distances of depth 0 and 1, metres)
        g_finishing.nearz = (zo ? p11 / p10 : p11 / (p10 - 1)) * unit;
        g_finishing.farz = p11 / (1 + p10) * unit;
    }
    static int dlog;
    if (s->vr && !depth && dlog++ < 3) XLOG("stereo from depth: no projection this frame, the eyes unshifted");
    static float sdepth = -1, sconv, swidth;
    if (sdepth < 0) {
        const char *e1 = getenv("LARECOMP_XR_SCREEN_DEPTH"), *e2 = getenv("LARECOMP_XR_SCREEN_CONV"), *e3 = getenv("LARECOMP_XR_SCREEN_W");
        sdepth = e1 ? (float)atof(e1) : 0.03f, sconv = e2 ? (float)atof(e2) : 2.0f, swidth = e3 ? (float)atof(e3) : g_screen ? 2.4f : 3.2f;
    }
    int neyes = s->vr ? 2 : relief && depth ? 2 : 1;
    if (!s->vr) atomic_store(&g_screen_two, neyes == 2);
    for (int e = 0; e < neyes; e++) {
        float pc[8];
        if (!s->vr && neyes == 2) {  // (the screen in relief: an eye's shift per 1 / distance, none at the screen's plane)
            float K = (e ? 0.5f : -0.5f) * sdepth * sconv / swidth;
            pc[0] = -K / sconv, pc[1] = 1.0f / (float)g_sw, pc[2] = 0, pc[3] = 1.0f / (float)g_sh, pc[4] = K;
        } else if (s->vr) {
            const float *c = s->ctan, *t = s->etan[e];
            pc[0] = (t[0] - c[0]) / (c[1] - c[0]), pc[1] = (t[1] - t[0]) / (c[1] - c[0]) / (float)g_sw;
            pc[2] = (c[2] - t[2]) / (c[2] - c[3]), pc[3] = (t[2] - t[3]) / (c[2] - c[3]) / (float)g_sh;
            pc[4] = depth ? s->eoff[e] / unit / (c[1] - c[0]) : 0;
        } else {
            pc[0] = 0, pc[1] = 1.0f / (float)g_sw, pc[2] = 0, pc[3] = 1.0f / (float)g_sh, pc[4] = 0;
        }
        pc[5] = p10, pc[6] = p11, pc[7] = (float)((zo ? 1 : 0) | flags);
        // (the view from his eyes wanted, not there for a moment: black, not the screen with the game's camera - a door,
        //  a level loading, the play starting looked like a bug, Guillaume, 27/10; xr_warp.frag 4096)
        if (!s->vr && g_hold_black) pc[7] = (float)((int)pc[7] | 4096);
        if (g_direct) {  // (drawn straight into the eye's swapchain image: its sRGB bytes through an RGBA8 UNORM view)
            int got = acquire_image_sc(e);
            if (got < 0) continue;
            VkImage dst = image_at(got);
            barrier(cb, dst, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL, 0, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
                    VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT);
            if (vk_xr_eye_warp(cb, src, e, (uint32_t)g_sw, (uint32_t)g_sh, pc, dst)) {
                barrier(cb, dst, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
                        VK_ACCESS_COLOR_ATTACHMENT_READ_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                        VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT);
            }
            g_acq2[e] = got;
            if (g_appsw && s->vr && depth && s->look) {
                extern void vk_gp_mark(VkCommandBuffer, const char *);
                vk_gp_mark(cb, e ? "AppSW eye 1 motion and depth" : "AppSW eye 0 motion and depth");
                appsw_eye(cb, src, e, pc, s, p10, p11, zo, unit);
            }
            continue;
        }
        VkImage ei = vk_xr_eye_warp(cb, src, e, (uint32_t)g_sw, (uint32_t)g_sh, pc, VK_NULL_HANDLE);
        if (!ei) continue;
        int got = acquire_image_sc(e);
        if (got < 0) continue;
        VkImage dst = image_at(got);
        barrier(cb, ei, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
                VK_ACCESS_TRANSFER_READ_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
        barrier(cb, dst, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0, VK_ACCESS_TRANSFER_WRITE_BIT,
                VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
        VkImageCopy rg = {.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, .dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                          .extent = {(uint32_t)g_sw, (uint32_t)g_sh, 1}};
        rec_CmdCopyImage(cb, ei, VK_IMAGE_LAYOUT_GENERAL, dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &rg);
        barrier(cb, dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_ACCESS_TRANSFER_WRITE_BIT,
                VK_ACCESS_COLOR_ATTACHMENT_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT);
        g_acq2[e] = got;
    }
    if (g_acq2[0] >= 0 || g_acq2[1] >= 0) g_acquired = 1;
}

static void hk_frame(VkCommandBuffer cb, VkImage src, VkExtent2D ext) {
#ifdef __ANDROID__
    if (g_turnip) {
        if (s_dev) bridge_frame(cb, src, ext);
        next_view();
        return;
    }
#endif
    g_finishing = g_rendering;
    extern uint64_t renderer_present_count(void);
    g_finishing.frame_id = renderer_present_count() + 1;
    {
        extern bool fp_xr_frame_look(float *yaw, float *pitch, int64_t *ns);
        g_finishing.look = g_finishing.vr && fp_xr_frame_look(&g_finishing.yaw, &g_finishing.pitch, &g_finishing.ns);
        extern bool fp_xr_frame_eye(float *eye);
        if (g_finishing.look && !fp_xr_frame_eye(g_finishing.place)) g_finishing.look = 0;
    }
    next_view();
    if (g_stereo == 2) { stereo2_frame(cb, src); return; }
    if (g_acquired >= 0 && src == image_at(g_acquired)) return;  // (drawn into it: hk_acquire)
    g_acquired = -1;
    int got = g_stereo ? acquire_image_sc(g_finishing.vr ? g_finishing.eye : 0) : acquire_image();
    if (got < 0) return;
    VkImage dst = image_at(got);
    barrier(cb, src, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
            VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT,
            VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
    barrier(cb, dst, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0, VK_ACCESS_TRANSFER_WRITE_BIT,
            VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
    uint32_t w = ext.width < (uint32_t)g_w ? ext.width : (uint32_t)g_w, h = ext.height < (uint32_t)g_h ? ext.height : (uint32_t)g_h;
    VkImageCopy rg = {.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, .dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                      .extent = {w, h, 1}};
    rec_CmdCopyImage(cb, src, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &rg);
    barrier(cb, dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_ACCESS_TRANSFER_WRITE_BIT,
            VK_ACCESS_COLOR_ATTACHMENT_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT);
    barrier(cb, src, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_ACCESS_TRANSFER_READ_BIT,
            VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT);
    g_acquired = got;
}

static void hk_frame_submitted(void) {
    if (g_turnip) { bridge_submitted(); return; }
    if (g_acquired < 0) return;
    if (g_stereo) {
        extern uint64_t renderer_submitted_seq(void);
        uint64_t seq = renderer_submitted_seq();
        pthread_mutex_lock(&g_rel_lock);
        for (int e = 0; e < 2; e++) {  // (stereo 2: both eyes' images; 1: the one taken)
            int sc = g_stereo == 2 ? (g_acq2[e] >= 0 ? e : -1) : e == 0 ? g_acq_sc : -1;
            if (sc < 0) continue;
            while (g_rel_n == 4) pthread_cond_wait(&g_rel_cond, &g_rel_lock);
            g_rel_q[g_rel_n].sc = sc, g_rel_q[g_rel_n].seq = seq, g_rel_q[g_rel_n].shot = g_finishing;
            g_rel_q[g_rel_n].sw = g_stereo == 2 && g_swacq[e];
            g_rel_q[g_rel_n].shot.sw = g_stereo == 2 && g_swacq[e] == 1;
            g_rel_n++;
            g_rel_pending[sc] = 1;
        }
        pthread_cond_broadcast(&g_rel_cond);
        pthread_mutex_unlock(&g_rel_lock);
        g_acq2[0] = g_acq2[1] = -1;
        g_swacq[0] = g_swacq[1] = 0;
        g_acquired = -1;
        return;
    }
    XrSwapchainImageReleaseInfo ri = {.type = XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
    vk_queue_lock();
    xrReleaseSwapchainImage(g_sc[g_acq_sc], &ri);
    vk_queue_unlock();
    pthread_mutex_lock(&g_shown_lock);
    g_shown = g_finishing;
    g_shown_sc = g_acq_sc;
    pthread_mutex_unlock(&g_shown_lock);
    g_acquired = -1;
    g_released = 1;
}

static VkXrHooks g_hooks = {hk_create_instance, hk_physical_device, hk_create_device, hk_device_ready, hk_frame, hk_frame_submitted,
                            hk_images, hk_acquire};

// ------------------------------------------------------------------ the OpenXR frame loop (this activity's thread)
static void session_events(void) {
    XrEventDataBuffer ev = {.type = XR_TYPE_EVENT_DATA_BUFFER};
    while (xrPollEvent(g_inst, &ev) == XR_SUCCESS) {
        if (ev.type == XR_TYPE_EVENT_DATA_DISPLAY_REFRESH_RATE_CHANGED_FB) {
            const XrEventDataDisplayRefreshRateChangedFB *r = (const void *)&ev;
            bool clock = !fresh_only() && fabsf(r->toDisplayRefreshRate / 60 - roundf(r->toDisplayRefreshRate / 60)) < 0.001f;
            g_hz = clock ? r->toDisplayRefreshRate : 0;
            vi_vsync_from_display(clock);
            XLOG("display refresh rate confirmed %.1f -> %.1f Hz, game clock %s", r->fromDisplayRefreshRate,
                 r->toDisplayRefreshRate, clock ? "display / 60" : "independent 60 Hz");
        } else if (ev.type == XR_TYPE_EVENT_DATA_REFERENCE_SPACE_CHANGE_PENDING) {
            // recentred (the Meta / Steam button held): his eyes at the head again (the screen follows the runtime's origin)
            fp_xr_recenter();
            XLOG("recentred: his eyes put where the head is");
        } else if (ev.type == XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED) {
            XrSessionState st = ((XrEventDataSessionStateChanged *)&ev)->state;
            XLOG("session state %d", st);
            g_focused = st == XR_SESSION_STATE_FOCUSED;
            if (st == XR_SESSION_STATE_READY) {
                XrSessionBeginInfo bi = {.type = XR_TYPE_SESSION_BEGIN_INFO,
                                         .primaryViewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO};
                if (XR_SUCCEEDED(xrBeginSession(g_session, &bi))) g_running = 1;
                if (g_running) choose_refresh_rate();
                if (g_perf_ext) {  // the game is CPU-bound on the Quest (measured 20/10): the highest sustained levels
                    p_perf_level(g_session, XR_PERF_SETTINGS_DOMAIN_CPU_EXT, perf_level("LARECOMP_XR_CPU"));
                    p_perf_level(g_session, XR_PERF_SETTINGS_DOMAIN_GPU_EXT, perf_level("LARECOMP_XR_GPU"));
                }
            } else if (st == XR_SESSION_STATE_STOPPING) {
                g_running = 0;
                vk_queue_lock();
                xrEndSession(g_session);
                vk_queue_unlock();
            } else if (st == XR_SESSION_STATE_EXITING || st == XR_SESSION_STATE_LOSS_PENDING) {
                XLOG("session over: leaving");
                fflush(stderr);
                _exit(0);
            }
        }
        ev = (XrEventDataBuffer){.type = XR_TYPE_EVENT_DATA_BUFFER};
    }
}

// The stick's turn at the display's rate (LARECOMP_XR_TURN=0: off; runtime/hle/firstperson.c fp_xr_turn): each refresh,
// the image shown is given the pose it would have had with the body turned as it is at that moment - B = Ry(-yaw)
// Rx(pitch), the world from the head B h: the pose B1^-1 B0 h0, about the head's centre. The moment is now minus the
// measured delay from a view's publication to its image's first refresh: an image first shows as it was made, then
// turns with the views published since (interpolated, the game being ahead) - the same delay as without, at 120 a
// second instead of 30. The 2D overlay drawn into the image turns with it, up to a frame's turn (to separate).
static XrQuaternionf qmul(XrQuaternionf a, XrQuaternionf b) {
    return (XrQuaternionf){a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y, a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
                           a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w, a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z};
}
static XrQuaternionf body_q(float yaw, float pitch) {
    XrQuaternionf y = {0, sinf(-yaw / 2), 0, cosf(-yaw / 2)}, p = {sinf(pitch / 2), 0, 0, cosf(pitch / 2)};
    return qmul(y, p);
}
static void turn_pose(XrPosef *p, XrVector3f c, const Shot *s, float yaw, float pitch) {
    XrQuaternionf b1 = body_q(yaw, pitch), r = qmul((XrQuaternionf){-b1.x, -b1.y, -b1.z, b1.w}, body_q(s->yaw, s->pitch));
    p->orientation = qmul(r, p->orientation);
    XrQuaternionf d = {p->position.x - c.x, p->position.y - c.y, p->position.z - c.z, 0};
    d = qmul(qmul(r, d), (XrQuaternionf){-r.x, -r.y, -r.z, r.w});
    p->position = (XrVector3f){c.x + d.x, c.y + d.y, c.z + d.z};
}
static int64_t mono_ns(void) {  // (the clock of the views' publication, firstperson.c: plat_now_ns)
    extern uint64_t plat_now_ns(void);
    return (int64_t)plat_now_ns();
}

static void await_fresh_pair(uint64_t submitted_frame) {
    if (fresh_only() && g_appsw && g_stereo == 2 && g_focused && submitted_frame) {
        struct timespec limit;
        clock_gettime(CLOCK_REALTIME, &limit);
        limit.tv_nsec += 100000000;
        if (limit.tv_nsec >= 1000000000) limit.tv_nsec -= 1000000000, limit.tv_sec++;
        pthread_mutex_lock(&g_shown_lock);
        while (g_shown_eye[0].frame_id == submitted_frame || g_shown_eye[0].frame_id != g_shown_eye[1].frame_id) {
            if (pthread_cond_timedwait(&g_shown_cond, &g_shown_lock, &limit)) break;
        }
        pthread_mutex_unlock(&g_shown_lock);
    }
}

static void xr_frame(void) {
    static uint64_t submitted_frame;
    // Legacy path preserved. Demand rendering starts AFTER xrWaitFrame has
    // granted the next frame's time budget, not before that synchronization.
    if (!demand_frames()) await_fresh_pair(submitted_frame);
    XrFrameState fs = {.type = XR_TYPE_FRAME_STATE};
    XrFrameWaitInfo wi = {.type = XR_TYPE_FRAME_WAIT_INFO};
    static int llog = -1, nl;  // LARECOMP_LOG_LOOP=1: where each refresh's time goes (3000 refreshes)
    if (llog < 0) llog = getenv("LARECOMP_LOG_LOOP") != NULL;
    int64_t l0 = mono_ns();
    if (XR_FAILED(xrWaitFrame(g_session, &wi, &fs))) return;
    int64_t l1 = mono_ns();
    g_display_time = fs.predictedDisplayTime;
    apply_thread_roles();
    if (g_hz > 0) {  // (the game's vsync: every 1/60 s of display time - every other refresh at 120 Hz, every one when
                     // AppSW has the runtime pace the loop at half the rate)
        static XrTime last;
        const XrTime per = 16666667;
        XrTime t = fs.predictedDisplayTime;
        if (!last || t - last >= per - 2000000) {
            vi_vsync_signal();
            last = last && t - last < 2 * per ? last + per : t;
        }
    }
    XrFrameBeginInfo bi = {.type = XR_TYPE_FRAME_BEGIN_INFO};
    vk_queue_lock();
    int64_t l2 = mono_ns();
    xrBeginFrame(g_session, &bi);
    int64_t l3 = mono_ns();
    vk_queue_unlock();
    read_controllers();
    if (demand_frames()) {
        atomic_store(&g_eye_requested, true);
        await_fresh_pair(submitted_frame);  // queue lock is released while GPU work finishes
    }
    // The screen: 2.4 m wide, 2.2 m ahead of where the head was at the start (the system's recentering moves it)
    XrCompositionLayerSettingsFB lset = {.type = XR_TYPE_COMPOSITION_LAYER_SETTINGS_FB, .layerFlags = layer_flags()};
    const void *lnext = lset.layerFlags ? &lset : NULL;
    // A release updates images and metadata under this same lock. Keep the
    // selected poses paired with the runtime's latest released images through
    // xrEndFrame, not merely through the metadata snapshot.
    int64_t select_before = mono_ns();
    vk_queue_lock();
    int64_t select_after = mono_ns();
    pthread_mutex_lock(&g_shown_lock);
    Shot shown = g_shown;
    XrSwapchain shown_sc = g_sc[g_shown_sc];
    pthread_mutex_unlock(&g_shown_lock);
    XrCompositionLayerQuad quad = {.type = XR_TYPE_COMPOSITION_LAYER_QUAD, .next = lnext, .space = g_space,
                                   .eyeVisibility = XR_EYE_VISIBILITY_BOTH,
                                   .subImage = {shown_sc, {{0, 0}, {g_sw, g_sh}}, 0},
                                   .pose = {{0, 0, 0, 1}, {0, 0, -2.2f}}, .size = {2.4f, 2.4f * g_h / g_w}};
    // (or the image rendered from the head, for both eyes: the compositor turns it to where the head is now)
    XrCompositionLayerProjectionView pv[2];
    for (int i = 0; i < 2; i++)
        pv[i] = (XrCompositionLayerProjectionView){.type = XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW, .pose = shown.pose,
                                                   .fov = shown.fov, .subImage = {shown_sc, {{0, 0}, {g_sw, g_sh}}, 0}};
    Shot es[2] = {shown, shown};  // (what each eye's image was made with)
    if (g_stereo && shown.vr) {  // (each eye its own latest image; an eye with none yet is shown the other's)
        pthread_mutex_lock(&g_shown_lock);
        for (int i = 0; i < 2; i++) {
            int k = g_released_eye[i] ? i : !i;
            if (!g_released_eye[k]) continue;
            es[i] = g_shown_eye[k];
            if (g_stereo == 2) pv[i].pose = g_shown_eye[k].epose[k], pv[i].fov = g_shown_eye[k].efov[k];
            else pv[i].pose = g_shown_eye[k].pose, pv[i].fov = g_shown_eye[k].fov;
            pv[i].subImage.swapchain = g_sc[k];
        }
        pthread_mutex_unlock(&g_shown_lock);
    }
    XrCompositionLayerSpaceWarpInfoFB swi[2];
    XrFrameSynthesisInfoEXT synth[2];
    bool swon = false;
    if (g_appsw) {  // AppSW: each eye's motion and depth, once the session has been focused 30 frames and the images
                    // written 30 frames in a row
        static uint32_t nfoc, nsw;
        static uint64_t warm_frame;
        nfoc = g_focused ? nfoc + 1 : 0;
        bool w = g_stereo == 2 && shown.vr && es[0].sw == 1 && es[1].sw == 1 && g_released_eye[0] && g_released_eye[1];
        if (!w && nsw > 30) XLOG("AppSW off: images not written (after %u frames)", nsw - 30);
        if (!w || !g_focused) nsw = 0, warm_frame = 0;
        else if (es[0].frame_id != warm_frame && es[0].frame_id == es[1].frame_id) {
            warm_frame = es[0].frame_id;
            if (++nsw == 31) XLOG("AppSW on: 30 distinct stereo frames written in a row");
        }
        if (w && nfoc > 30 && nsw > 30) {
            for (int i = 0; i < 2; i++) {
                swi[i] = (XrCompositionLayerSpaceWarpInfoFB){.type = XR_TYPE_COMPOSITION_LAYER_SPACE_WARP_INFO_FB,
                    .motionVectorSubImage = {g_mv_sc[i], {{0, 0}, {(int32_t)g_mvw, (int32_t)g_mvh}}, 0},
                    .appSpaceDeltaPose = {{0, 0, 0, 1}, {0, 0, 0}},
                    .depthSubImage = {g_dp_sc[i], {{0, 0}, {(int32_t)g_mvw, (int32_t)g_mvh}}, 0},
                    .minDepth = 0, .maxDepth = 1, .nearZ = es[i].nearz, .farZ = es[i].farz};
                pv[i].next = &swi[i];
                if (g_synth_ext) {
                    const char *relax = getenv("LARECOMP_XR_SYNTHESIS_RELAXED");
                    synth[i] = (XrFrameSynthesisInfoEXT){.type = XR_TYPE_FRAME_SYNTHESIS_INFO_EXT,
                        .layerFlags = relax && *relax == '1' ? XR_FRAME_SYNTHESIS_INFO_REQUEST_RELAXED_FRAME_INTERVAL_BIT_EXT : 0,
                        .motionVectorSubImage = swi[i].motionVectorSubImage, .motionVectorScale = {1, 1, 1, 1},
                        .appSpaceDeltaPose = swi[i].appSpaceDeltaPose, .depthSubImage = swi[i].depthSubImage,
                        .minDepth = 0, .maxDepth = 1, .nearZ = es[i].nearz, .farZ = es[i].farz};
                    pv[i].next = &synth[i];
                }
            }
            swon = true;
        }
    }
    if (getenv("LARECOMP_XR_HISTORY_LOG")) {
        static uint64_t previous_frame;
        if (es[0].frame_id && es[0].frame_id != previous_frame) {
            XLOG("display history: frame %llu previous %llu AppSW %d", (unsigned long long)es[0].frame_id,
                 (unsigned long long)previous_frame, swon);
            previous_frame = es[0].frame_id;
        }
    }
    if (getenv("LARECOMP_XR_CADENCE_LOG")) {
        // A compositor counter alone cannot tell whether the app repeats game images.
        static int64_t at, previous_ns;
        static unsigned loops, fresh, repeated, active, focused, images, split;
        static uint64_t previous_frame;
        static double age_sum, age_max;
        int64_t now = mono_ns();
        if (!at) at = now;
        loops++, active += swon, focused += !!g_focused;
        if (es[0].frame_id && es[0].frame_id != previous_frame) images++, previous_frame = es[0].frame_id;
        split += es[0].frame_id != es[1].frame_id;
        if (es[0].ns && es[0].ns != previous_ns) fresh++, previous_ns = es[0].ns;
        else repeated++;
        if (es[0].ns) {
            double age = (now - es[0].ns) / 1e6;
            age_sum += age;
            if (age > age_max) age_max = age;
        }
        if (now - at >= 5000000000LL) {
            double sec = (now - at) / 1e9;
            XLOG("cadence: loop %.2f/s distinct %.2f/s repeated %u/%u AppSW %u/%u focused %u/%u source age avg %.1f max %.1f ms",
                 loops / sec, fresh / sec, repeated, loops, active, loops, focused, loops, age_sum / loops, age_max);
            XLOG("image cadence: rendered images sampled %.2f/s, camera views %.2f/s, mismatched eye frame IDs %u/%u",
                 images / sec, fresh / sec, split, loops);
            at = now; loops = fresh = repeated = active = focused = 0; age_sum = age_max = 0;
            images = split = 0;
        }
    }
    submitted_frame = swon ? es[0].frame_id : 0;
    if (shown.vr && !swon) {  // the stick's turn since each eye's image (above)
        static int turn = -1;
        static double delay = -1;  // (ns: from a view's publication to its image's first refresh)
        static int64_t last_ns;
        static uint32_t nloop, nturned;
        if (turn < 0) {
            const char *e = getenv("LARECOMP_XR_TURN");
            turn = !e || atoi(e) != 0;
            XLOG("the stick's turn at the display's rate: %s", turn ? "on" : "off (LARECOMP_XR_TURN=0)");
        }
        int64_t now = mono_ns();
        if (es[0].look && es[0].ns != last_ns) {
            double d = (double)(now - es[0].ns);
            if (d > 0 && d < 5e8) delay = delay < 0 ? d : delay + (d - delay) * 0.05;
            last_ns = es[0].ns;
        }
        float ang = 0;
        if (turn && delay > 0) {
            extern bool fp_xr_turn(float yaw0, float pitch0, int64_t ns0, int64_t t, float *yaw, float *pitch);
            for (int i = 0; i < 2; i++) {
                float y, p;
                if (!es[i].look || !fp_xr_turn(es[i].yaw, es[i].pitch, es[i].ns, now - (int64_t)delay, &y, &p)) continue;
                turn_pose(&pv[i].pose, es[i].pose.position, &es[i], y, p);
                if (i == 0) ang = (y - es[0].yaw) * 57.29578f, nturned++;
                static int tlog = -1, nt;  // LARECOMP_LOG_TURN=1: each refresh while turning (2000 at most)
                if (tlog < 0) tlog = getenv("LARECOMP_LOG_TURN") != NULL;
                if (tlog && i == 0 && fabsf(ang) > 0.01f && nt++ < 2000)
                    fprintf(stderr, "[turn] t %.1f img %.1f shown %.1f yaw %.2f by %.2f\n", (double)(now % 100000000000LL) / 1e6,
                            (double)(es[0].ns % 100000000000LL) / 1e6, (double)((now - (int64_t)delay) % 100000000000LL) / 1e6,
                            y * 57.29578f, ang);
            }
        }
        static int plog = -1;
        if (plog < 0) plog = getenv("LARECOMP_LOG_PACE") != NULL;
        if (++nloop % 600 == 0 && (plog || nloop == 600))
            XLOG("turn: delay %.1f ms, %u of the last 600 refreshes turned (now %.2f degrees)", delay / 1e6, nturned, ang), nturned = 0;
    }
    XrCompositionLayerProjection proj = {.type = XR_TYPE_COMPOSITION_LAYER_PROJECTION, .next = lnext, .space = g_space, .viewCount = 2, .views = pv};
    const XrCompositionLayerBaseHeader *layers[2] = {
        shown.vr ? (const XrCompositionLayerBaseHeader *)&proj : (const XrCompositionLayerBaseHeader *)&quad};
    uint32_t nlayers = 1;
    if (!g_screen && !shown.vr) {
        // The game on a screen (not in the headset's view: the view from above, the menus): the game image's part of the
        // window only - the bars above and below it were half the screen - LARECOMP_XR_SCREEN_W wide (3.2 m) at
        // LARECOMP_XR_SCREEN_DIST (2.4 m) ahead of the runtime's origin, which the player's own recentring (the Meta /
        // Steam button) puts where the head is. (Put where the head looked as it appeared, it overrode that: « tu dois
        // avoir une commande pour mettre l'écran devant toi et elle prend le dessus sur la mienne », Guillaume, 27/10)
        static float qw = -1, qd;
        if (qw < 0) {
            const char *e3 = getenv("LARECOMP_XR_SCREEN_W"), *e4 = getenv("LARECOMP_XR_SCREEN_DIST");
            qw = e3 ? (float)atof(e3) : 3.2f, qd = e4 ? (float)atof(e4) : 2.4f;
        }
        extern void vk_present_place(float *vr, float *crop);
        float pl[4], cr[4];
        vk_present_place(pl, cr);
        static float said[4];
        if (memcmp(said, pl, sizeof said)) {
            memcpy(said, pl, sizeof said);
            XLOG("screen: the game's image at %.0f,%.0f %.0fx%.0f of %dx%d (crop %.3f,%.3f %.3fx%.3f), swapchain %dx%d", pl[0], pl[1],
                 pl[2], pl[3], g_w, g_h, cr[0], cr[1], cr[2], cr[3], g_sw, g_sh);
        }
        quad.pose = (XrPosef){{0, 0, 0, 1}, {0, 0, -qd}};
        quad.size = (XrExtent2Df){qw, qw * (float)g_h / (float)g_w};
        bool whole = false;  // (the menu, drawn over the whole window's height: all of it, not the game's 16:9 band - in the
#ifndef __ANDROID__  //  headset only "the start of something" showed, Guillaume 12/11)
        whole = menu_is_open();
#endif
        if (pl[2] > 8 && pl[3] > 8 && !whole) {
            float kx = (float)g_sw / (float)g_w, ky = (float)g_sh / (float)g_h;
            quad.subImage.imageRect = (XrRect2Di){{(int32_t)(pl[0] * kx), (int32_t)(pl[1] * ky)}, {(int32_t)(pl[2] * kx), (int32_t)(pl[3] * ky)}};
            quad.size = (XrExtent2Df){qw, qw * pl[3] / pl[2]};
        }
    }
    XrCompositionLayerQuad sq[2];
    if (!g_screen && !shown.vr && atomic_load(&g_screen_two)) {  // (in relief: an eye each)
        for (int i = 0; i < 2; i++) {
            sq[i] = quad;
            sq[i].eyeVisibility = i ? XR_EYE_VISIBILITY_RIGHT : XR_EYE_VISIBILITY_LEFT;
            sq[i].subImage.swapchain = g_sc[i];
            layers[i] = (const XrCompositionLayerBaseHeader *)&sq[i];
        }
        nlayers = 2;
    }
    if (g_screen && !shown.vr) {  // the screen: an eye each (its own image in relief; flat, the first eye's for both)
        static float sw_ = -1, sd_;
        if (sw_ < 0) {
            const char *e3 = getenv("LARECOMP_XR_SCREEN_W"), *e4 = getenv("LARECOMP_XR_SCREEN_DIST");
            sw_ = e3 ? (float)atof(e3) : 2.4f, sd_ = e4 ? (float)atof(e4) : 2.2f;
        }
        bool two = g_screen3d && g_released_eye[0] && g_released_eye[1];
        for (int i = 0; i < 2; i++) {
            sq[i] = quad;
            sq[i].eyeVisibility = i ? XR_EYE_VISIBILITY_RIGHT : XR_EYE_VISIBILITY_LEFT;
            sq[i].subImage.swapchain = g_sc[two ? i : 0];
            sq[i].pose = (XrPosef){{0, 0, 0, 1}, {0, 0, -sd_}};
            sq[i].size = (XrExtent2Df){sw_, sw_ * (float)g_h / (float)g_w};
            layers[i] = (const XrCompositionLayerBaseHeader *)&sq[i];
        }
        nlayers = 2;
    }
    XrFrameEndInfo fe = {.type = XR_TYPE_FRAME_END_INFO, .displayTime = fs.predictedDisplayTime,
                         .environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE,
                         .layerCount = fs.shouldRender && g_released ? nlayers : 0, .layers = layers};
    int64_t l4 = mono_ns();
    XrResult r = xrEndFrame(g_session, &fe);
    int64_t l6 = mono_ns();
    vk_queue_unlock();
    if (llog && nl++ < 3000) {
        static int64_t prev;
        fprintf(stderr, "[loop] %.2f: wait %.2f, lock %.2f, begin %.2f, work %.2f, lock %.2f, end %.2f (since last %.2f)\n",
                (double)(l0 % 100000000000LL) / 1e6, (l1 - l0) / 1e6, (l2 - l1) / 1e6, (l3 - l2) / 1e6,
                (select_before - l3 + l4 - select_after) / 1e6, (select_after - select_before) / 1e6,
                (l6 - l4) / 1e6, prev ? (l0 - prev) / 1e6 : 0.0);
        prev = l0;
    }
    static int logged;
    if (XR_FAILED(r) && logged++ < 5) XLOG("xrEndFrame: %d", r);
}

// ------------------------------------------------------------------ start
#ifdef __ANDROID__
static bool xr_init(struct android_app *app) {
    PFN_xrInitializeLoaderKHR init_loader = NULL;
    xrGetInstanceProcAddr(XR_NULL_HANDLE, "xrInitializeLoaderKHR", (PFN_xrVoidFunction *)&init_loader);
    if (init_loader) {
        XrLoaderInitInfoAndroidKHR li = {.type = XR_TYPE_LOADER_INIT_INFO_ANDROID_KHR, .applicationVM = app->activity->vm,
                                         .applicationContext = app->activity->clazz};
        init_loader((const XrLoaderInitInfoBaseHeaderKHR *)&li);
    }
#else
static bool xr_init(void *app) {
    (void)app;
#endif
    uint32_t n = 0;
    xrEnumerateInstanceExtensionProperties(NULL, 0, &n, NULL);
    XrExtensionProperties *props = calloc(n ? n : 1, sizeof *props);
    for (uint32_t i = 0; i < n; i++) props[i].type = XR_TYPE_EXTENSION_PROPERTIES;
    xrEnumerateInstanceExtensionProperties(NULL, n, &n, props);
    if (getenv("LARECOMP_XR_CADENCE_LOG"))
        for (uint32_t i = 0; i < n; i++) XLOG("runtime extension: %s v%u", props[i].extensionName, props[i].extensionVersion);
    for (uint32_t i = 0; i < n; i++) if (!strcmp(props[i].extensionName, XR_EXT_PERFORMANCE_SETTINGS_EXTENSION_NAME)) g_perf_ext = true;
    for (uint32_t i = 0; i < n; i++) if (!strcmp(props[i].extensionName, XR_FB_DISPLAY_REFRESH_RATE_EXTENSION_NAME)) g_hz_ext = true;
    for (uint32_t i = 0; i < n; i++) if (!strcmp(props[i].extensionName, XR_FB_COMPOSITION_LAYER_SETTINGS_EXTENSION_NAME)) g_layer_ext = true;
    const char *swe = getenv("LARECOMP_XR_APPSW");
    const char *sye = getenv("LARECOMP_XR_SYNTHESIS");
    for (uint32_t i = 0; i < n && sye && *sye == '1'; i++)
        if (!strcmp(props[i].extensionName, XR_EXT_FRAME_SYNTHESIS_EXTENSION_NAME)) g_synth_ext = g_sw_ext = true;
    for (uint32_t i = 0; i < n && swe && *swe == '1'; i++) if (!strcmp(props[i].extensionName, XR_FB_SPACE_WARP_EXTENSION_NAME)) g_sw_ext = true;
    const char *te = getenv("LARECOMP_XR_THREADS");
#ifdef __ANDROID__
    for (uint32_t i = 0; i < n && !(te && *te == '0'); i++)
        if (!strcmp(props[i].extensionName, XR_KHR_ANDROID_THREAD_SETTINGS_EXTENSION_NAME)) g_thread_ext = true;
#else
    (void)te;
#endif
    free(props);
#ifdef __ANDROID__
    const char *exts[8] = {XR_KHR_ANDROID_CREATE_INSTANCE_EXTENSION_NAME, XR_KHR_VULKAN_ENABLE2_EXTENSION_NAME};
    uint32_t next = 2;
#else
    const char *exts[8] = {XR_KHR_VULKAN_ENABLE2_EXTENSION_NAME};
    uint32_t next = 1;
#endif
    if (g_perf_ext) exts[next++] = XR_EXT_PERFORMANCE_SETTINGS_EXTENSION_NAME;
    if (g_hz_ext) exts[next++] = XR_FB_DISPLAY_REFRESH_RATE_EXTENSION_NAME;
    if (g_layer_ext) exts[next++] = XR_FB_COMPOSITION_LAYER_SETTINGS_EXTENSION_NAME;
#ifdef __ANDROID__
    if (g_thread_ext) exts[next++] = XR_KHR_ANDROID_THREAD_SETTINGS_EXTENSION_NAME;
#endif
    if (g_sw_ext) exts[next++] = g_synth_ext ? XR_EXT_FRAME_SYNTHESIS_EXTENSION_NAME : XR_FB_SPACE_WARP_EXTENSION_NAME;
#ifdef __ANDROID__
    XrInstanceCreateInfoAndroidKHR ai = {.type = XR_TYPE_INSTANCE_CREATE_INFO_ANDROID_KHR, .applicationVM = app->activity->vm,
                                         .applicationActivity = app->activity->clazz};
    XrInstanceCreateInfo ci = {.type = XR_TYPE_INSTANCE_CREATE_INFO, .next = &ai, .enabledExtensionCount = next,
                               .enabledExtensionNames = exts};
#else
    XrInstanceCreateInfo ci = {.type = XR_TYPE_INSTANCE_CREATE_INFO, .enabledExtensionCount = next, .enabledExtensionNames = exts};
#endif
    strcpy(ci.applicationInfo.applicationName, "LASRecomp");
    strcpy(ci.applicationInfo.engineName, "LASRecomp");
    ci.applicationInfo.applicationVersion = 1;
    ci.applicationInfo.apiVersion = XR_MAKE_VERSION(1, 0, 0);
    XrResult r = xrCreateInstance(&ci, &g_inst);
    if (XR_FAILED(r)) { XLOG("xrCreateInstance failed: %d", r); return false; }
    XrSystemGetInfo si = {.type = XR_TYPE_SYSTEM_GET_INFO, .formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY};
    r = xrGetSystem(g_inst, &si, &g_sys);
    if (XR_FAILED(r)) { XLOG("xrGetSystem failed: %d", r); return false; }
    {   // which runtime and headset: the sound goes to the headset's output by them (runtime/services/audio.c)
        XrInstanceProperties ip = {.type = XR_TYPE_INSTANCE_PROPERTIES};
        XrSystemProperties sp = {.type = XR_TYPE_SYSTEM_PROPERTIES};
        xrGetInstanceProperties(g_inst, &ip);
        xrGetSystemProperties(g_inst, g_sys, &sp);
        char rs[600];
        snprintf(rs, sizeof rs, "%s | %s", ip.runtimeName, sp.systemName);
        setenv("LARECOMP_XR_RUNTIME", rs, 1);
        XLOG("runtime %s %u.%u.%u, system %s", ip.runtimeName, XR_VERSION_MAJOR(ip.runtimeVersion), XR_VERSION_MINOR(ip.runtimeVersion),
             XR_VERSION_PATCH(ip.runtimeVersion), sp.systemName);
    }
    xrGetInstanceProcAddr(g_inst, "xrGetVulkanGraphicsRequirements2KHR", (PFN_xrVoidFunction *)&p_requirements);
    xrGetInstanceProcAddr(g_inst, "xrCreateVulkanInstanceKHR", (PFN_xrVoidFunction *)&p_create_instance);
    xrGetInstanceProcAddr(g_inst, "xrGetVulkanGraphicsDevice2KHR", (PFN_xrVoidFunction *)&p_graphics_device);
    xrGetInstanceProcAddr(g_inst, "xrCreateVulkanDeviceKHR", (PFN_xrVoidFunction *)&p_create_device);
    if (g_perf_ext) xrGetInstanceProcAddr(g_inst, "xrPerfSettingsSetPerformanceLevelEXT", (PFN_xrVoidFunction *)&p_perf_level);
#ifdef __ANDROID__
    if (g_thread_ext) xrGetInstanceProcAddr(g_inst, "xrSetAndroidApplicationThreadKHR", (PFN_xrVoidFunction *)&p_set_thread);
#endif
    if (g_hz_ext) {
        xrGetInstanceProcAddr(g_inst, "xrEnumerateDisplayRefreshRatesFB", (PFN_xrVoidFunction *)&p_enum_hz);
        xrGetInstanceProcAddr(g_inst, "xrRequestDisplayRefreshRateFB", (PFN_xrVoidFunction *)&p_request_hz);
        xrGetInstanceProcAddr(g_inst, "xrGetDisplayRefreshRateFB", (PFN_xrVoidFunction *)&p_get_hz);
    }
    if (!p_requirements || !p_create_instance || !p_graphics_device || !p_create_device) { XLOG("no XR_KHR_vulkan_enable2"); return false; }
    make_actions();
    XLOG("OpenXR ready (performance settings: %s)", g_perf_ext ? "yes" : "no");
    return true;
}

// The frame's size (stereo from the depth: both eyes and the margin, at an eye's recommended density) and each eye's.
static void xr_sizes(char *size, bool own_size) {
    if (g_stereo == 2 && !own_size) {  // (the frame over both eyes and the margin, at an eye's recommended density)
        XrViewConfigurationView cv[2] = {{.type = XR_TYPE_VIEW_CONFIGURATION_VIEW}, {.type = XR_TYPE_VIEW_CONFIGURATION_VIEW}};
        uint32_t n = 0;
        if (XR_SUCCEEDED(xrEnumerateViewConfigurationViews(g_inst, g_sys, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, 2, &n, cv)) && n) {
            const char *es = getenv("LARECOMP_XR_EYE_SCALE"), *em = getenv("LARECOMP_XR_MARGIN");
            float k = es ? (float)atof(es) : 1.0f, M = em ? (float)atof(em) : 0.10f;
            // (the Quest 3's fields, for the sizes only - the views say the real ones each frame: an eye -1.38..0.84 or
            //  -0.84..1.38 across, both 0.97..-1.43 up and down, the two together -1.38..1.38)
            float ppx = cv[0].recommendedImageRectWidth * k / 2.22f, ppy = cv[0].recommendedImageRectHeight * k / 2.40f;
            g_w = ((int)(ppx * (2.76f + 2 * M + 0.08f)) + 7) & ~7, g_h = ((int)(ppy * (2.40f + 2 * M)) + 7) & ~7;
            snprintf(size, 32, "%dx%d", g_w, g_h);
            setenv("LARECOMP_OFFSCREEN", size, 1);
            XLOG("stereo from depth: the frame %dx%d, each eye %dx%d (recommended %ux%u, scale %.2f, margin %.2f)", g_w, g_h,
                 ((int)(ppx * (2.22f + 2 * M)) + 7) & ~7, g_h, cv[0].recommendedImageRectWidth, cv[0].recommendedImageRectHeight, k, M);
        }
    }
    if (g_stereo == 1 && !own_size) {  // (an eye's image: the size the headset recommends, LARECOMP_XR_EYE_SCALE times)
        XrViewConfigurationView cv[2] = {{.type = XR_TYPE_VIEW_CONFIGURATION_VIEW}, {.type = XR_TYPE_VIEW_CONFIGURATION_VIEW}};
        uint32_t n = 0;
        if (XR_SUCCEEDED(xrEnumerateViewConfigurationViews(g_inst, g_sys, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, 2, &n, cv)) && n) {
            const char *es = getenv("LARECOMP_XR_EYE_SCALE");
            float k = es ? (float)atof(es) : 1.0f;
            g_w = ((int)(cv[0].recommendedImageRectWidth * k) + 7) & ~7, g_h = ((int)(cv[0].recommendedImageRectHeight * k) + 7) & ~7;
            snprintf(size, 32, "%dx%d", g_w, g_h);
            setenv("LARECOMP_OFFSCREEN", size, 1);
            XLOG("stereo: each eye %dx%d (recommended %ux%u, max %ux%u, scale %.2f)", g_w, g_h, cv[0].recommendedImageRectWidth,
                 cv[0].recommendedImageRectHeight, cv[0].maxImageRectWidth, cv[0].maxImageRectHeight, k);
        }
    }
    {   // (stereo 2: an eye's image, from the frame's size - its share of the frame's width)
        const char *em = getenv("LARECOMP_XR_MARGIN");
        float M = em ? (float)atof(em) : 0.10f;
        g_ew = ((int)(g_w * (2.22f + 2 * M) / (2.76f + 2 * M + 0.08f)) + 7) & ~7, g_eh = g_h;
        const char *output = getenv("LARECOMP_XR_OUTPUT_SCALE"), *source = getenv("LARECOMP_XR_EYE_SCALE");
        float os = output ? strtof(output, NULL) : 0, ss = source ? strtof(source, NULL) : 0;
        if (g_stereo == 2 && !g_screen && isfinite(os) && os >= 0.5f && os <= 2 && ss > 0) {
            g_ew = ((int)lroundf(g_ew * os / ss) + 7) & ~7;
            g_eh = ((int)lroundf(g_eh * os / ss) + 7) & ~7;
            XLOG("separate eye output scale %.2f: source %dx%d, eyes %dx%d (resampling, not added source detail)",
                 os, g_w, g_h, g_ew, g_eh);
        }
        g_sw = g_stereo == 2 && !g_screen ? g_ew : g_w, g_sh = g_stereo == 2 && !g_screen ? g_eh : g_h;
    }
    XLOG("view: %s, %dx%d", g_stereo == 2 ? "stereo, both eyes each frame from the depth" : g_stereo ? "stereo, one eye a frame in turn"
                                                                                                   : "one image for both eyes", g_w, g_h);
}

// env.txt in the data folder: KEY=VALUE per line, for tests (LARECOMP_INPUT, LARECOMP_WARP_ON_LOAD...)
// The player's Headset resolution (the in-game menu's Video tab: vr_scale in the settings file) as the eye scale, before
// the shell's own default and after nothing else: env.txt, read later, still wins. The settings file where the game
// will look for it (runtime/settings.c, runtime/paths.c): LARECOMP_SETTINGS, else larecomp.ini in the data root.
static void xr_settings_eye_scale(void) {
    if (getenv("LARECOMP_XR_EYE_SCALE")) return;
    char path[2048];
    const char *set = getenv("LARECOMP_SETTINGS"), *root = getenv("LARECOMP_DATA_ROOT");
    struct stat st;
    if (set && *set) snprintf(path, sizeof path, "%s", set);
    else if ((root && *root) || (stat("extracted", &st) == 0 && S_ISDIR(st.st_mode))) snprintf(path, sizeof path, "%s/larecomp.ini", root && *root ? root : ".");
    else {
#ifdef _WIN32
        extern bool plat_exe_path(char *buf, size_t size);
        char exe[1800];
        if (!plat_exe_path(exe, sizeof exe)) return;
        char *sl = strrchr(exe, '\\'), *fs = strrchr(exe, '/');
        if (fs > sl) sl = fs;
        if (sl) *sl = 0;
        snprintf(path, sizeof path, "%s\\larecomp.ini", exe);
#elif defined(__ANDROID__)
        snprintf(path, sizeof path, "settings.ini");
#else
        const char *xdg = getenv("XDG_DATA_HOME"), *home = getenv("HOME");
        if (xdg && *xdg == '/') snprintf(path, sizeof path, "%s/lasrecomp/larecomp.ini", xdg);
        else snprintf(path, sizeof path, "%s/.local/share/lasrecomp/larecomp.ini", home && *home ? home : "/tmp");
#endif
    }
    FILE *f = fopen(path, "r");
    if (!f) return;
    char line[256];
    float v = 0;
    while (fgets(line, sizeof line, f))
        if (sscanf(line, " vr_scale = %f", &v) == 1) break;
    fclose(f);
    if (v >= 0.4f && v <= 2.0f) {
        char b[16];
        snprintf(b, sizeof b, "%.2f", v);
        setenv("LARECOMP_XR_EYE_SCALE", b, 0);
        XLOG("Headset resolution from %s: %s", path, b);
    }
}

static void read_env_file(void) {
    FILE *f = fopen("env.txt", "r");
    if (!f) return;
    char line[2048];
    while (fgets(line, sizeof line, f)) {
        line[strcspn(line, "\r\n")] = 0;
        char *eq = strchr(line, '=');
        if (line[0] == '#' || !eq) continue;
        *eq = 0;
        setenv(line, eq + 1, 1);
        fprintf(stderr, "[xr] env %s=%s\n", line, eq + 1);
    }
    fclose(f);
}

// The game files' folder (the start screen's PLAY IN VR passes it: <data root>/extracted, which an installed copy keeps in
// the platform's data folder, not next to the exe); "extracted" when started without one (the VR shortcut, a folder)
static char g_game_dir[1024] = "extracted";
static void *game_thread(void *arg) {
    (void)arg;
    char *argv[] = {"lasrecomp", g_game_dir, "--play", NULL};
    int r = lasrecomp_main(3, argv);
    XLOG("the game returned %d", r);
    fflush(stderr);
    _exit(r);
    return NULL;
}

#ifdef __ANDROID__
static void on_cmd(struct android_app *app, int32_t cmd) {
    (void)app;
    if (cmd == APP_CMD_DESTROY) { fflush(stderr); _exit(0); }
}

void android_main(struct android_app *app) {
    app->onAppCmd = on_cmd;
    const char *dir = app->activity->externalDataPath;
    if (!dir || chdir(dir) != 0) { ALOG("no data folder (%s)", dir ? dir : "null"); return; }
    if (freopen("lasrecomp.log", "w", stderr)) { setvbuf(stderr, NULL, _IOLBF, 0); dup2(2, 1); }
    ALOG("data folder %s", dir);
    // the game code ships in the app (lib/arm64-v8a/liblasgame.so), next to this library
    Dl_info di;
    char lib[1024] = "";
    if (dladdr((void *)android_main, &di) && di.dli_fname) {
        char tmp[1024];
        snprintf(tmp, sizeof tmp, "%s", di.dli_fname);
        snprintf(lib, sizeof lib, "%s/liblasgame.so", dirname(tmp));
        setenv("LARECOMP_GAME_LIB", lib, 0);
    }
    char size[32];
    snprintf(size, sizeof size, "%dx%d", g_w, g_h);
    setenv("LARECOMP_OFFSCREEN", size, 0);
    setenv("LARECOMP_FONT", "Arial.ttf", 0);
    setenv("LARECOMP_FP", "1", 0);
    {   // the whole RomFS image when there is one: the headset holds it with only part of a romfs/ folder beside it
        // (region_common), which the runtime would serve instead - the game then crashed at boot reading files that
        // weren't there (runtime/services/fs.c base_open; set by a test's env.txt until 27/10, gone with it)
        struct stat rs;
        if (stat("extracted/romfs.bin", &rs) == 0) setenv("LARECOMP_ROMFS_IMAGE", "1", 0);
    }
    setenv("LARECOMP_SAVE_DIR", "save", 0);
    setenv("LARECOMP_SETTINGS", "settings.ini", 0);
    setenv("LARECOMP_NO_LAUNCHER", "1", 0);
    // dynamic resolution's floor: 1.0, the full size always (Guillaume, 21/10 evening: "go to full res, stereoscopic,
    // then we'll earn the ms lever after lever"). It was 0.7 - the beach sat there, 986x851, the image he called rotten.
    // the right stick turns only, the head looks up and down: the world always level. Its tilt, kept once pushed, leaned
    // the world as soon as the head turned (« si je bouge la tête ça me décale en pivot par rapport au sol, dans le sens
    // d'une horloge ... il faut toujours rester parallèle au sol », Guillaume 27/10 evening - a second time: commit
    // 646b61b had put it back)
    setenv("LARECOMP_FP_XR_STICK_PITCH", "0", 0);
    // (the compositor's turn between game frames stays on here, LARECOMP_XR_TURN: the stick's turn at 120 with the game at
    //  30 - off, it turned at 30; with what he holds in its own layer below, « le meilleur compromis », Guillaume, 28/10)
    // what he holds in a layer of its own, drawn once per eye (runtime/nvn/vulkan/exec.c), and the first-person view on
    // from the start, as in PC VR (27/10): the sword's ghost was still there in the headset (« il faut la dessiner
    // qu'une fois », Guillaume, 28/10)
    // one submission a frame, not one every 8 passes: half the translation's time in the village (570 against 1340 ms
    // every 2 s at scale 1.0, 28/10)
    setenv("LARECOMP_CHUNK_PASSES", "0", 0);
    setenv("LARECOMP_XR_HELD_LAYER", "1", 0);
    setenv("LARECOMP_XR_HELD_SPLIT", "1", 0);
    setenv("LARECOMP_XR_HELD_EYES", "1", 0);
    setenv("LARECOMP_FP_START_ON", "1", 0);
    setenv("LARECOMP_DYNRES_MIN", "1.0", 0);
    setenv("LARECOMP_XR_STEREO", "2", 0);
    setenv("LARECOMP_XR_SCREEN", "0", 0);  // immersive by default (Guillaume, 25/10)
    setenv("LARECOMP_XR_DEPTH_BOUNDS", "2", 0);  // conservative bounds per row, full-resolution colour
    setenv("LARECOMP_XR_WARP_REFINE", "1", 0);  // finer silhouette reconstruction, 24 steps / adaptive refinement
    // (off it saved 2 ms of 40 at scale 1.0, but the image deformed when the head moved: Guillaume, 28/10, back on)
    // the game's own GPU counters frozen (runtime/nvn/vulkan/exec.c): it reads them to drop its internal resolution and
    // to take its 30 fps mode - with both eyes at full size the GPU times grew, and its image went soft (« bourré de
    // pixels », 21/10); our own resolution is fixed at 1.0 and the 30 fps mode is off (LARECOMP_PACE=0)
    setenv("LARECOMP_VK_COUNTER", "frozen", 0);
    // the game in its 30 fps mode (two steps of its world a frame: full speed at 30 images a second - in its 60 fps mode
    // at the 30-35 the stereo frame allows, the world ran at ~55% speed) and the display at 120 Hz: exactly four
    // refreshes a game image, the head turned at 120 by the compositor (21/10)
    // (LARECOMP_GAME_30: after env.txt, below - AppSW at 120 needs the game at 60)
    setenv("LARECOMP_XR_HZ", "120", 0);
    setenv("LARECOMP_XR_MARGIN", "0.05", 0);
    // 0.85 (1496x1560 per eye): 32 in the village, the heaviest place measured, with the two shader switches below - 0.9
    // gave 29.7-30.5 there, 0.8 33-34, 0.75 without them 27.5-30 walking (28/10). 1.5 (« quality first », 25/10) gave
    // 11-16. Guillaume, 28/10: « la qualité est acceptable pour un tel portage ». LARECOMP_XR_EYE_SCALE=<s> to choose.
    xr_settings_eye_scale();  // (the player's Headset resolution first, above)
    setenv("LARECOMP_XR_EYE_SCALE", "0.85", 0);
    // saturate / fmin / fmax without their NaN tests, and fp16 decoded by unpackHalf2x16 in fragment and compute shaders
    // (runtime/nvn/maxwell/translate.c): at scale 1.0 in the village 38.5 ms of GPU against 47.8, 25-26 fps against
    // 20-21; the same image from the same pose (LARECOMP_BENCH_POSE captures, the differences in the grass's wind, 28/10)
    setenv("LARECOMP_VK_FAST_NAN", "1", 0);
    setenv("LARECOMP_VK_NATIVE_H2F", "1", 0);
    // the frame ahead and the asynchronous submission (runtime/nvn/objects.c), as the Mac's launcher sets them: the
    // game's thread, the translation and the GPU each work on their own frame (beach, 21/10: 30 -> 50-58 fps)
    setenv("LARECOMP_FRAME_AHEAD", "1", 0);
    setenv("LARECOMP_ASYNC_SUBMIT", "1", 0);
    // the game always in its 60 fps mode: the frame pacing (runtime/nvn/vulkan/exec.c vk_pace_frame) put it in its 30
    // fps mode 8 s out of every 30-40 in play (21/10, his test) - in the headset a stutter far worse than a game running
    // a few percent slow when a frame is late
    setenv("LARECOMP_PACE", "0", 0);
    // first person's far scenery (runtime/hle/firstperson.c detail_tick): late from 16.9 ms on average, not the Mac's
    // 17.8 - at 55-58 fps it never gave anything up. Every level starts with all its detail (27/10: a start at the first
    // coarse step took the small blocks out of far walls).
    setenv("LARECOMP_FP_DETAIL_LATE_MS", "16.9", 0);
    read_env_file();
    {   // AppSW at a multiple of 60 Hz: the runtime makes every other image, the app at half the rate - the game in its 60
        // fps mode at 120 Hz; at 72 or 90 (the app at 36 or 45) and without AppSW, its 30 fps mode, at full speed
        const char *asw = getenv("LARECOMP_XR_APPSW"), *synth = getenv("LARECOMP_XR_SYNTHESIS"), *scr = getenv("LARECOMP_XR_SCREEN");
        int hz = atoi(getenv("LARECOMP_XR_HZ"));
        // (the 3D screen too in its 30 fps mode: at 1920x1080 the 60 fps mode ran 29-38 images a second at the beach -
        //  the processor, ~1000 draws an image - the world at 50-60% of its speed, 22/10)
        (void)scr;
        bool synthesis = (asw && *asw == '1') || (synth && *synth == '1');
        setenv("LARECOMP_GAME_30", synthesis && hz >= 120 && hz % 60 == 0 ? "0" : "1", 0);
    }
    load_vulkan(app);
    bool own_size = strcmp(getenv("LARECOMP_OFFSCREEN"), size) != 0;  // (set in env.txt: kept)
    g_stereo = g_turnip ? 0 : atoi(getenv("LARECOMP_XR_STEREO"));
    {
        const char *scr = getenv("LARECOMP_XR_SCREEN");
        g_screen = !g_turnip && scr && *scr == '1';
        if (g_screen) {
            g_vr = 0, g_stereo = 2;
            if (!own_size) {
                const char *ss = getenv("LARECOMP_XR_SCREEN_SIZE");
                setenv("LARECOMP_OFFSCREEN", ss ? ss : "1920x1080", 1);
                own_size = true;
            }
            XLOG("the 3D screen: %s, the game's own camera", getenv("LARECOMP_OFFSCREEN"));
        }
    }
    sscanf(getenv("LARECOMP_OFFSCREEN"), "%dx%d", &g_w, &g_h);
    fprintf(stderr, "[xr] data %s, game code %s\n", dir, lib);
    if (!xr_init(app)) { fflush(stderr); return; }
    xr_sizes(size, own_size);
    g_vk_xr = &g_hooks;
    g_thread_role_hook = thread_role;
    pthread_t t;
    pthread_attr_t at;
    pthread_attr_init(&at);
    pthread_attr_setstacksize(&at, 64 << 20);
    pthread_create(&t, &at, game_thread, NULL);
    for (;;) {
        int events;
        struct android_poll_source *src;
        while (ALooper_pollOnce(g_running ? 0 : 20, NULL, &events, (void **)&src) >= 0)
            if (src) src->process(app, src);
        if (!g_session_made) continue;
        session_events();
        if (g_running) xr_frame();
    }
}
#endif

#ifndef __ANDROID__
// A PC: `lasrecomp.exe --vr` (runtime/main.c), from the game's folder - the screen's settings, save and log as they are.
// The Quest's choices where they hold for any headset (immersive, stereo from the depth, the full resolution, the
// game's counters frozen, the frame ahead); the processor's (its 30 fps mode, the scenery's coarser steps) are the
// Quest's own: a PC runs the game in its 60 fps mode. No display-rate extension under SteamVR: the game keeps its own
// 60 Hz clock and the runtime reprojects. env.txt in the folder still sets anything for a test.
int xr_desktop_main(int argc, char **argv) {
    for (int i = 1; i < argc; i++)  // (the first argument that isn't an option: the game files' folder)
        if (argv[i][0] != '-') { snprintf(g_game_dir, sizeof g_game_dir, "%s", argv[i]); break; }
    extern void plat_init(void);
    plat_init();  // (the log, lasrecomp.log next to the exe, from the start: the game's own plat_init appends to it)
    char size[32];
    snprintf(size, sizeof size, "%dx%d", g_w, g_h);
    setenv("LARECOMP_OFFSCREEN", size, 0);
    setenv("LARECOMP_FP_START_ON", "1", 0);  // (on at the start; R3 switches it, as on a screen - firstperson.c)
    setenv("LARECOMP_NO_LAUNCHER", "1", 0);
    // (the menu drawn into the headset's image: offscreen, device.c draws it only when asked - in the headset it opened,
    //  read the controllers, and was never seen; Guillaume, 12/11: « X + Y ne marche pas »)
    setenv("LARECOMP_OFFSCREEN_MENU", "1", 0);
    // the shaders prepared before the game starts, as the start screen this skips does (runtime/nvn/shader_prep.c):
    // without it every variant compiled at first use - 550 a run, the first steps slow, 30 fps at the first sword
    // swings (Raccoon, 26/10)
    setenv("LARECOMP_SHADER_PREP", "1", 0);
    setenv("LARECOMP_DYNRES_MIN", "1.0", 0);
    setenv("LARECOMP_XR_STEREO", "2", 0);
    setenv("LARECOMP_XR_SCREEN", "0", 0);
    setenv("LARECOMP_XR_DEPTH_BOUNDS", "2", 0);
    setenv("LARECOMP_XR_WARP_REFINE", "1", 0);
    setenv("LARECOMP_VK_COUNTER", "frozen", 0);
    setenv("LARECOMP_XR_MARGIN", "0.05", 0);
    xr_settings_eye_scale();  // (the player's Headset resolution first)
    setenv("LARECOMP_XR_EYE_SCALE", "1.0", 0);  // (the size the headset recommends: the PC's GPU has room - to raise)
    setenv("LARECOMP_FRAME_AHEAD", "1", 0);
    setenv("LARECOMP_ASYNC_SUBMIT", "1", 0);
    setenv("LARECOMP_PACE", "0", 0);
    setenv("LARECOMP_GAME_30", "0", 0);
    setenv("LARECOMP_XR_HELD_LAYER", "1", 0);  // (what he holds: a layer of its own in each eye, exec.c vk_xr_eye_warp)
    // What he holds drawn apart from the scene, once per eye (exec.c LARECOMP_XR_HELD_EYES): made from one image and its
    // depth, the sword - thin, near - was doubled in the headset; with both eyes given that one image it was clean
    // (LARECOMP_XR_IPD_SCALE=0, Guillaume, 27/10). The layer alone, rebuilt from its depth, changed nothing.
    setenv("LARECOMP_XR_HELD_SPLIT", "1", 0);
    setenv("LARECOMP_XR_HELD_EYES", "1", 0);
    // (the stick's turn at the display's rate off: the compositor turned the whole image, the sword and shield in it,
    //  between two game frames - they jumped and doubled at each turn; at 60 frames a second the turn is smooth enough
    //  without it - Guillaume, 27/10, the consultant's 4A)
    setenv("LARECOMP_XR_TURN", "0", 0);
    read_env_file();
    load_vulkan(NULL);
    bool own_size = strcmp(getenv("LARECOMP_OFFSCREEN"), size) != 0;
    g_stereo = atoi(getenv("LARECOMP_XR_STEREO"));
    {
        const char *scr = getenv("LARECOMP_XR_SCREEN");
        g_screen = scr && *scr == '1';
        if (g_screen) {
            g_vr = 0, g_stereo = 2;
            if (!own_size) {
                const char *ss = getenv("LARECOMP_XR_SCREEN_SIZE");
                setenv("LARECOMP_OFFSCREEN", ss ? ss : "1920x1080", 1);
                own_size = true;
            }
        }
    }
    sscanf(getenv("LARECOMP_OFFSCREEN"), "%dx%d", &g_w, &g_h);
#ifdef _WIN32
    {   // the game code, built by the start screen's setup (runtime/gamebuild.c) next to the exe: without it, said
        // before the headset is taken (it was a crash box: "the game code hasn't been built", a fresh unzip, 27/10)
        extern bool plat_exe_path(char *buf, size_t size);
        char exe[1024], lib[1100];
        // (beta 6 builds it into the game files' folder - gamelib.c candidates - before, next to the exe)
        extern bool gamelib_present(const char *game_dir);
        bool have = getenv("LARECOMP_GAME_LIB") || access("lasgame.dll", 0) == 0 || gamelib_present(g_game_dir);
        if (!have && g_game_dir[0] != '/' && g_game_dir[0] != '\\' && g_game_dir[1] != ':') {  // (relative: in the data folder,
            extern bool rt_data_root_early(char *out, size_t n);                               //  as the game resolves it)
            char root[1024], dir[1100];
            if (rt_data_root_early(root, sizeof root)) snprintf(dir, sizeof dir, "%s/%s", root, g_game_dir), have = gamelib_present(dir);
        }
        if (!have && plat_exe_path(exe, sizeof exe)) {
            char *s = strrchr(exe, '\\'), *f = strrchr(exe, '/');
            if (f > s) s = f;
            if (s) *s = 0;
            snprintf(lib, sizeof lib, "%s\\lasgame.dll", exe);
            have = access(lib, 0) == 0;
        }
        if (!have) {
            __declspec(dllimport) int __stdcall MessageBoxA(void *w, const char *text, const char *title, unsigned type);
            MessageBoxA(NULL, "The game isn't set up yet.\n\nStart Inside The Dream once and set up your game files on its "
                        "start screen, then play in VR from there (PLAY IN VR).", "Inside The Dream VR", 0x40);
            return 1;
        }
    }
#endif
    if (!xr_init(NULL)) {
        fprintf(stderr, "[xr] no headset: is SteamVR (or another OpenXR runtime) running, the headset connected?\n");
        fflush(stderr);
#ifdef _WIN32
        __declspec(dllimport) int __stdcall MessageBoxA(void *w, const char *text, const char *title, unsigned type);
        MessageBoxA(NULL, "No headset found.\n\nStart SteamVR (or Quest Link, Virtual Desktop...) with the headset "
                    "connected, then start Inside The Dream VR again.", "Inside The Dream VR", 0x30);
#endif
        return 1;
    }
    xr_sizes(size, own_size);
    g_vk_xr = &g_hooks;
    g_thread_role_hook = thread_role;
    pthread_t t;
    pthread_attr_t at;
    pthread_attr_init(&at);
    pthread_attr_setstacksize(&at, 64 << 20);
    pthread_create(&t, &at, game_thread, NULL);
    for (;;) {
        if (!g_session_made) { usleep(20000); continue; }
        session_events();
        if (g_running) xr_frame();
        else usleep(10000);
    }
}
#endif
