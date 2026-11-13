// nn::vi (display/layer) and nv:: (graphics driver init), implemented on the host.
#include <stdlib.h>
#include <time.h>

#include "../hle.h"
#include "../kernel.h"
#include "../settings.h"

static uint32_t vsync_r, vsync_w;
static KEvent *vsync_event;
uint32_t vi_vsync_handle(void) { return vsync_r; }
volatile uint64_t g_vsync_count;  // vsyncs signalled so far (runtime/hle/framepace.c, runtime/nvn/pace.c)
volatile uint64_t g_vsync_ns;     // plat_now_ns() of the latest (stored after the count: count >= the vsync it's from;
                                  // also frame interpolation's game timeline)
uint64_t g_native_window;

// One vsync: the game's frame timer waits on this event. The source is the 60 Hz timer below, unless the display drives
// it (runtime/nvn/metal.m, a display link: every Nth refresh of a display at N x 60 Hz). A free-running 60 Hz beside a
// 120 Hz screen left each frame on screen for 1, 2 or 3 refreshes as the two clocks drifted - a judder at a steady
// 60 fps (Guillaume's 1080p 120 Hz screen, 20/10).
static volatile int g_vsync_from_display;
static volatile uint64_t g_vsync_last_ns;
void vi_vsync_signal(void) {
    if (!vsync_event) return;
    g_vsync_last_ns = plat_now_ns();
    __atomic_add_fetch(&g_vsync_count, 1, __ATOMIC_RELEASE);  // (the count first, then the time: see g_vsync_ns)
    __atomic_store_n(&g_vsync_ns, g_vsync_last_ns, __ATOMIC_RELEASE);
    pthread_mutex_lock(&g_klock);
    kernel_signal(vsync_event);
    pthread_mutex_unlock(&g_klock);
}
void vi_vsync_from_display(int on) { g_vsync_from_display = on; }

static void *vsync_thread(void *arg) {
    (void)arg;
    const uint64_t period = 16666667;
    plat_thread_name("vsync");
    plat_thread_realtime(period);
    plat_thread_perf_core(PLAT_PERF_VSYNC);
    uint64_t next = plat_now_ns();
    for (;;) {
        next += period;
        plat_sleep_until_ns(next);
        uint64_t now = plat_now_ns();
        if (now > next + period * 3) {
            // (~100 ms late every frame: macOS coalescing the timers of a thread that lost its real-time policy while
            // the app isn't frontmost - checked and set again here; the window's focus says whether it was frontmost)
            static int warned;
            int rt = plat_thread_realtime_check(period);
            extern bool ui_window_focused(void);
            if (warned++ < 40) LOG("[vi] vsync timer late by %llums (window focused %d, real-time policy %s)\n", (unsigned long long)((now - next) / 1000000),
                                    ui_window_focused(), rt == 0 ? "holds" : rt == 1 ? "was gone: set again" : "unknown");
            next = now;  // don't burst-signal to catch up
        }
        // Pause on focus lost (Video tab): no vsync while the window isn't in front, so the game waits on it - still
        // (test runs' windows don't take the focus: never there)
        extern bool ui_window_focused(void);
        static int test_run = -1;
        if (test_run < 0) test_run = getenv("LARECOMP_HIDDEN") || getenv("LARECOMP_WINDOW_SHOT");
        if (g_settings.pause_unfocused && !test_run && !ui_window_focused()) continue;
        // (the display's ticks stopped - a display link paused, the headset's frame loop not running: the timer
        //  takes over after 50 ms, the game doesn't stop on a missing vsync)
        if (!g_vsync_from_display || now - g_vsync_last_ns > 50000000ull) vi_vsync_signal();
    }
    return NULL;
}

HLE_FUNC("_ZN2nn2vi10InitializeEv", vi_initialize) { (void)c; }
HLE_FUNC("_ZN2nn2vi8FinalizeEv", vi_finalize) { (void)c; }

HLE_FUNC("_ZN2nn2vi18OpenDefaultDisplayEPPNS0_7DisplayE", vi_open_default_display) {
    *(uint64_t *)(uintptr_t)c->x[0] = (uint64_t)hle_alloc(0x100);
    c->x[0] = 0;
}

HLE_FUNC("_ZN2nn2vi12CloseDisplayEPNS0_7DisplayE", vi_close_display) { (void)c; }

HLE_FUNC("_ZN2nn2vi11CreateLayerEPPNS0_5LayerEPNS0_7DisplayE", vi_create_layer) {
    *(uint64_t *)(uintptr_t)c->x[0] = (uint64_t)hle_alloc(0x100);
    c->x[0] = 0;
}

HLE_FUNC("_ZN2nn2vi12DestroyLayerEPNS0_5LayerE", vi_destroy_layer) { (void)c; }
HLE_FUNC("_ZN2nn2vi12SetLayerCropEPNS0_5LayerEiiii", vi_set_layer_crop) { c->x[0] = 0; }

HLE_FUNC("_ZN2nn2vi15GetNativeWindowEPPvPNS0_5LayerE", vi_get_native_window) {
    if (!g_native_window) g_native_window = (uint64_t)hle_alloc(0x100);
    *(uint64_t *)(uintptr_t)c->x[0] = g_native_window;
    c->x[0] = 0;
}

// Reboot: the vsync timer and its event live on (the kernel's handle table doesn't): new handles at the next boot.
RT_ON_RESET(vi_reset) {
    vsync_r = vsync_w = 0;
    g_native_window = 0;  // (HLE heap)
}

HLE_FUNC("_ZN2nn2vi20GetDisplayVsyncEventEPNS_2os15SystemEventTypeEPNS0_7DisplayE", vi_get_vsync_event) {
    if (!vsync_event) {
        vsync_event = kernel_create_event_pair(&vsync_w, &vsync_r);
        pthread_t t;
        pthread_create(&t, NULL, vsync_thread, NULL);
    } else if (!vsync_r) {
        vsync_w = kernel_new_handle(vsync_event);
        vsync_r = kernel_new_handle(vsync_event);
    }
    // nn::os::AttachReadableHandleToSystemEvent(event, handle, managed=false, clear_mode=AutoClear)
    c->x[1] = vsync_r;
    c->x[2] = 0;
    c->x[3] = 1;
    guest_call(c, guest_sym("_ZN2nn2os33AttachReadableHandleToSystemEventEPNS0_15SystemEventTypeEjbNS0_14EventClearModeE"));
    c->x[0] = 0;
}

HLE_FUNC("_ZN2nv18InitializeGraphicsEPvm", nv_initialize_graphics) { (void)c; }
HLE_FUNC("_ZN2nv16FinalizeGraphicsEv", nv_finalize_graphics) { (void)c; }
HLE_FUNC("_ZN2nv20SetGraphicsAllocatorEPFPvmmS0_EPFvS0_S0_EPFS0_S0_mS0_ES0_", nv_set_allocator) { (void)c; }
HLE_FUNC("_ZN2nv28SetGraphicsDevtoolsAllocatorEPFPvmmS0_EPFvS0_S0_EPFS0_S0_mS0_ES0_", nv_set_dt_allocator) { (void)c; }
