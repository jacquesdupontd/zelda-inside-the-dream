// Hitch profiler. LARECOMP_HITCH_PROF=1: a thread looks at the game's frame thread (the one that presents) about
// 1000 times a second - suspends it, reads its pc and its frame-pointer chain, resumes it - and keeps the samples of
// the frame being built. When that frame comes out late (the [hitch] log, runtime/nvn/metal.m), its samples go to
// build/prof/hitch-<pid>.bin, untouched: tools/hitchprof.py names the functions afterwards (nm, atos) and says where
// those frames spent their time. `sample` stops the whole game for seconds to do the same (a 114 ms frame of its own
// in the player's game, 20/10) and averages over every frame; this keeps only the late ones.
// The sampler takes no lock and allocates nothing while the thread is stopped (it may hold the allocator's lock).
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "hle.h"

#ifdef __APPLE__
#include <mach-o/dyld.h>
#include <mach/mach.h>
#include <pthread.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define DEPTH 24
#define MAXS 512
typedef struct { uint64_t t; uint32_t n, pad; uint64_t a[DEPTH]; } Samp;
static Samp g_s[2][MAXS];  // two buffers: the sampler fills one, the frame's end swaps
static volatile int g_cur;
static volatile uint32_t g_n[2];
static thread_act_t g_target;
static uint64_t g_stack_lo, g_stack_hi;
static FILE *g_out;
static pthread_mutex_t g_swap = PTHREAD_MUTEX_INITIALIZER;

static uint64_t now_ns(void) { return clock_gettime_nsec_np(CLOCK_UPTIME_RAW); }

static void *sampler(void *arg) {
    (void)arg;
    for (;;) {
        usleep(900);
        pthread_mutex_lock(&g_swap);
        int b = g_cur;
        uint32_t i = g_n[b];
        if (i >= MAXS) { pthread_mutex_unlock(&g_swap); continue; }
        Samp *s = &g_s[b][i];
        arm_thread_state64_t st;
        mach_msg_type_number_t cnt = ARM_THREAD_STATE64_COUNT;
        if (thread_suspend(g_target) != KERN_SUCCESS) { pthread_mutex_unlock(&g_swap); continue; }
        uint32_t n = 0;
        if (thread_get_state(g_target, ARM_THREAD_STATE64, (thread_state_t)&st, &cnt) == KERN_SUCCESS) {
            s->a[n++] = arm_thread_state64_get_pc(st);
            s->a[n++] = arm_thread_state64_get_lr(st);
            uint64_t fp = arm_thread_state64_get_fp(st);
            while (n < DEPTH && fp >= g_stack_lo && fp + 16 <= g_stack_hi && !(fp & 7)) {
                uint64_t next = ((const uint64_t *)(uintptr_t)fp)[0], ret = ((const uint64_t *)(uintptr_t)fp)[1];
                if (ret) s->a[n++] = ret & 0x0000ffffffffffffull;  // (pointer authentication bits off)
                if (next <= fp) break;
                fp = next;
            }
        }
        thread_resume(g_target);
        s->n = n;
        s->t = now_ns();
        g_n[b] = i + 1;
        pthread_mutex_unlock(&g_swap);
    }
    return NULL;
}

static int prof_on(void) {
    static int on = -1;
    const char *e = getenv("LARECOMP_HITCH_PROF");
    if (on < 0) on = e && (atoi(e) != 0 || !strcmp(e, "game"));
    return on;
}

// LARECOMP_HITCH_PROF=game: the game's own thread is sampled instead (objects.c nvnQueuePresentTexture calls this on
// it) - with asynchronous submission the presenting thread is the translation worker, and a late frame whose time is
// all "rest" there was the game's (the frames still go by the worker's presents: the game is about a frame ahead).
static bool g_started, g_game_set;
void hitchprof_game_thread(void) {
    if (!prof_on() || g_game_set) return;
    const char *e = getenv("LARECOMP_HITCH_PROF");
    if (!e || strcmp(e, "game")) { g_game_set = true; return; }
    pthread_t me = pthread_self();
    g_stack_hi = (uint64_t)(uintptr_t)pthread_get_stackaddr_np(me);
    g_stack_lo = g_stack_hi - pthread_get_stacksize_np(me);
    g_target = mach_thread_self();
    g_game_set = true;
    LOG("[hitchprof] sampling the game's thread\n");
}

// The frame thread, at each present: the first call starts the sampler on this thread; then the samples since the
// last call are the frame just presented - kept when it was late (gap_ms over the [hitch] threshold).
void hitchprof_frame(uint64_t frame, double gap_ms, int late, const char *level) {
    if (!prof_on()) return;
    if (!g_started) {
        g_started = true;
        if (!g_target) {
            g_target = mach_thread_self();
            pthread_t me = pthread_self();
            g_stack_hi = (uint64_t)(uintptr_t)pthread_get_stackaddr_np(me);
            g_stack_lo = g_stack_hi - pthread_get_stacksize_np(me);
        }
        mkdir("build/prof", 0755);
        char path[128];
        snprintf(path, sizeof path, "build/prof/hitch-%d.bin", (int)getpid());
        g_out = fopen(path, "wb");
        if (!g_out) return;
        // header: the images and where they are loaded (so the offline names match this run)
        fprintf(g_out, "LAHP1\n");
        for (uint32_t i = 0; i < _dyld_image_count(); i++) {
            const struct mach_header *h = _dyld_get_image_header(i);
            fprintf(g_out, "image %llx %s\n", (unsigned long long)(uintptr_t)h, _dyld_get_image_name(i));
        }
        fprintf(g_out, "end\n");
        fflush(g_out);
        pthread_t t;
        pthread_create(&t, NULL, sampler, NULL);
        LOG("[hitchprof] sampling the frame thread, late frames to %s\n", path);
        return;
    }
    pthread_mutex_lock(&g_swap);
    int b = g_cur;
    uint32_t n = g_n[b];
    g_cur = b ^ 1;
    g_n[b ^ 1] = 0;
    pthread_mutex_unlock(&g_swap);
    if (late && g_out && n) {
        // record: "F" frame gap_ms(x100) nsamples level[16], then the samples
        uint32_t hdr[4] = {0x46524d46u, (uint32_t)frame, (uint32_t)(gap_ms * 100), n};
        char lv[16] = {0};
        if (level) strncpy(lv, level, sizeof lv - 1);
        fwrite(hdr, sizeof hdr, 1, g_out);
        fwrite(lv, sizeof lv, 1, g_out);
        for (uint32_t i = 0; i < n; i++) {
            Samp *s = &g_s[b][i];
            fwrite(&s->n, 4, 1, g_out);
            fwrite(s->a, 8, s->n, g_out);
        }
        static int k;
        if (++k % 8 == 0) fflush(g_out);
    }
}
#else
void hitchprof_frame(uint64_t frame, double gap_ms, int late, const char *level) { (void)frame, (void)gap_ms, (void)late, (void)level; }
void hitchprof_game_thread(void) {}
#endif
