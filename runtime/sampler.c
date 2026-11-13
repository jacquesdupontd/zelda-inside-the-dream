// In-process sampling profiler for Linux, Android and Windows: simpleperf needs perf_event access, which the Quest refuses even
// to a profileable app (and runtime/hitchprof.c samples through Mach, macOS only).
//   LARECOMP_SAMPLE=<from>:<to>[:game|worker|compile]  samples one thread every 0.5 ms between those seconds after its first
//       frame - the game's thread (the one presenting frames; default) or the asynchronous submission worker - then writes
//       samples.txt in the working directory: one line per distinct stack, "<count> <module>+<offset hex> ...", the
//       interrupted PC first, then the link register, then the return addresses up the frame-pointer chain.
//   tools/samples.py <samples.txt> <unstripped libraries...> symbolizes and sums them (self and inclusive).
//   With the game's thread, samples_slow.txt beside it holds only the samples taken in frames whose busy time
//   (exec.c vk_pace_frame: present to present, minus the waits) was over LARECOMP_SAMPLE_SLOW ms (default 16.2):
//   what the frames that miss 60 fps do that the others don't.
// The sampled thread is interrupted by SIGPROF (SA_RESTART: its waits go on); the handler only copies registers and
// reads its own stack.
#define _GNU_SOURCE
#include <stdbool.h>
#include <stdint.h>

void sampler_thread(int kind);
void sampler_frame_end(double busy_ms);

#if defined(__linux__)
#include <dlfcn.h>
#include <link.h>
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/syscall.h>
#include <sys/ucontext.h>
#include <time.h>
#include <unistd.h>

#include "hle.h"

#define SMAX 80000  // 40 s at 2 kHz
#define DEPTH 16
static uint64_t (*g_s)[DEPTH];
static uint32_t *g_sframe_of;      // each sample's frame
static _Atomic uint32_t g_sframe;  // the game thread's frame in progress
static float g_fbusy[65536];       // each finished frame's busy ms, by its number & 0xFFFF
static _Atomic int g_ns;
static int g_kind = -1;  // the thread asked for: 0 game, 1 worker
static double g_from, g_to;
static pid_t g_tid;
static uint64_t g_lo, g_hi;  // its stack
static _Atomic bool g_started;

static void on_prof(int sig, siginfo_t *si, void *ctx) {
    (void)sig; (void)si;
    ucontext_t *uc = ctx;
    int i = atomic_fetch_add(&g_ns, 1);
    if (i >= SMAX) return;
    g_sframe_of[i] = g_sframe;
    uint64_t *s = g_s[i];
    int d = 0;
#if defined(__aarch64__)
    s[d++] = uc->uc_mcontext.pc;
    s[d++] = uc->uc_mcontext.regs[30];
    uint64_t fp = uc->uc_mcontext.regs[29];
#else
    s[d++] = (uint64_t)uc->uc_mcontext.gregs[REG_RIP];
    s[d++] = 0;
    uint64_t fp = (uint64_t)uc->uc_mcontext.gregs[REG_RBP];
#endif
    while (d < DEPTH && fp >= g_lo && fp + 16 <= g_hi && !(fp & 7)) {
        uint64_t next = ((uint64_t *)(uintptr_t)fp)[0], ret = ((uint64_t *)(uintptr_t)fp)[1];
        s[d++] = ret;
        if (next <= fp) break;
        fp = next;
    }
    while (d < DEPTH) s[d++] = 0;
}

typedef struct { uint64_t lo, hi, base; char name[64]; } Mod;
static Mod g_mods[4096];
static int g_nmods;
static int mod_cb(struct dl_phdr_info *in, size_t sz, void *arg) {
    (void)sz; (void)arg;
    for (int k = 0; k < in->dlpi_phnum && g_nmods < 4096; k++) {
        const ElfW(Phdr) *ph = &in->dlpi_phdr[k];
        if (ph->p_type != PT_LOAD || !(ph->p_flags & PF_X)) continue;
        Mod *m = &g_mods[g_nmods++];
        m->lo = in->dlpi_addr + ph->p_vaddr;
        m->hi = m->lo + ph->p_memsz;
        m->base = in->dlpi_addr;
        const char *n = in->dlpi_name && *in->dlpi_name ? strrchr(in->dlpi_name, '/') : NULL;
        snprintf(m->name, sizeof m->name, "%s", n ? n + 1 : in->dlpi_name && *in->dlpi_name ? in->dlpi_name : "main");
    }
    return 0;
}

void sampler_frame_end(double busy_ms) {  // (the game's thread, each frame: exec.c vk_pace_frame)
    uint32_t f = g_sframe;
    g_fbusy[f & 0xFFFF] = (float)busy_ms;
    g_sframe = f + 1;
}

static int cmp_stack(const void *a, const void *b) {
    int r = memcmp(g_s[*(const int *)a], g_s[*(const int *)b], sizeof(uint64_t) * DEPTH);
    return r ? r : *(const int *)a - *(const int *)b;
}

// One file: the stacks of the samples in ord[0..n) (sorted by stack) for which keep[] is set.
static void write_stacks(const char *path, const int *ord, const bool *keep, int n, int nkept, const char *what) {
    FILE *f = fopen(path, "w");
    if (!f) { LOG("[sample] can't write %s\n", path); return; }
    fprintf(f, "# %d samples of the %s thread, %.1f-%.1f s%s\n", nkept, g_kind == 2 ? "compile" : g_kind ? "worker" : "game", g_from, g_to, what);
    for (int i = 0; i < n;) {
        int j = i + 1, c = keep[ord[i]];
        while (j < n && !memcmp(g_s[ord[i]], g_s[ord[j]], sizeof g_s[0])) c += keep[ord[j++]];
        if (!c) { i = j; continue; }
        fprintf(f, "%d", c);
        for (int d = 0; d < DEPTH && (d < 2 || g_s[ord[i]][d]); d++) {
            uint64_t a = g_s[ord[i]][d];
            int m = 0;
            while (m < g_nmods && !(a >= g_mods[m].lo && a < g_mods[m].hi)) m++;
            if (m < g_nmods) fprintf(f, " %s+%llx", g_mods[m].name, (unsigned long long)(a - g_mods[m].base));
            else fprintf(f, " ?+%llx", (unsigned long long)a);
        }
        fputc('\n', f);
        i = j;
    }
    fclose(f);
}

static void write_report(int n) {
    dl_iterate_phdr(mod_cb, NULL);
    int *ord = malloc(sizeof(int) * (size_t)(n ? n : 1));
    bool *all = malloc((size_t)(n ? n : 1)), *slow = malloc((size_t)(n ? n : 1));
    if (!ord || !all || !slow) return;
    for (int i = 0; i < n; i++) ord[i] = i;
    qsort(ord, (size_t)n, sizeof *ord, cmp_stack);
    const char *e = getenv("LARECOMP_SAMPLE_SLOW");
    double over = e ? atof(e) : 16.2;
    uint32_t last = g_sframe, f0 = n ? g_sframe_of[0] : 0, nslowf = 0;
    int nslow = 0;
    for (int i = 0; i < n; i++) {
        all[i] = true;
        uint32_t fr = g_sframe_of[i];
        slow[i] = !g_kind && fr < last && last - fr < 65536 && g_fbusy[fr & 0xFFFF] > over;
        nslow += slow[i];
    }
    for (uint32_t fr = f0; fr < last && last - fr < 65536; fr++) nslowf += g_fbusy[fr & 0xFFFF] > over;
    write_stacks("samples.txt", ord, all, n, n, "");
    char what[96];
    snprintf(what, sizeof what, ", in the %u frames (of %u) busy over %.1f ms", nslowf, last - f0, over);
    if (!g_kind) write_stacks("samples_slow.txt", ord, slow, n, nslow, what);
    LOG("[sample] %d samples of the %s thread written to samples.txt%s", n, g_kind == 2 ? "compile" : g_kind ? "worker" : "game", g_kind ? "\n" : "");
    if (!g_kind) LOG(", %d of them in samples_slow.txt (%u slow frames of %u)\n", nslow, nslowf, last - f0);
    free(ord), free(all), free(slow);
}

static double now_s(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec + t.tv_nsec / 1e9;
}

static void *sampler_main(void *arg) {
    (void)arg;
    double t0 = now_s();
    struct timespec iv = {0, 500000};
    for (;;) {
        double t = now_s() - t0;
        if (t >= g_to) break;
        if (t >= g_from) syscall(SYS_tgkill, getpid(), g_tid, SIGPROF);
        nanosleep(&iv, NULL);
    }
    int n = atomic_load(&g_ns);
    if (n > SMAX) n = SMAX;
    usleep(20000);  // (a signal in flight)
    write_report(n);
    return NULL;
}

// The thread of `kind` (0: the game's, at its first present; 1: the submission worker, at its start) calls this.
void sampler_thread(int kind) {
    if (g_kind == -1) {
        const char *e = getenv("LARECOMP_SAMPLE");
        g_kind = -2;
        if (e && *e) {
            char *p;
            g_from = strtod(e, &p);
            g_to = *p == ':' ? strtod(p + 1, &p) : g_from + 10;
            g_kind = *p != ':' ? 0 : !strcmp(p + 1, "worker") ? 1 : !strcmp(p + 1, "compile") ? 2 : 0;
        }
    }
    if (kind != g_kind || atomic_exchange(&g_started, true)) return;
    g_tid = (pid_t)syscall(SYS_gettid);
    pthread_attr_t a;
    void *addr = NULL;
    size_t sz = 0;
    if (pthread_getattr_np(pthread_self(), &a) == 0) {
        pthread_attr_getstack(&a, &addr, &sz);
        pthread_attr_destroy(&a);
    }
    g_lo = (uint64_t)(uintptr_t)addr;
    g_hi = g_lo + sz;
    g_s = calloc(SMAX, sizeof *g_s);
    g_sframe_of = calloc(SMAX, sizeof *g_sframe_of);
    if (!g_s || !g_sframe_of) return;
    struct sigaction sa = {0};
    sa.sa_sigaction = on_prof;
    sa.sa_flags = SA_SIGINFO | SA_RESTART;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGPROF, &sa, NULL);
    pthread_t th;
    pthread_create(&th, NULL, sampler_main, NULL);
    pthread_detach(th);
    LOG("[sample] sampling the %s thread (tid %d) from %.1f to %.1f s (stack %llx-%llx)\n", kind == 2 ? "compile" : kind ? "worker" : "game",
        (int)g_tid, g_from, g_to, (unsigned long long)g_lo, (unsigned long long)g_hi);
}
#elif defined(_WIN32)
// Windows: the thread is suspended (SuspendThread), its context read, its stack unwound through the modules' unwind
// tables (RtlVirtualUnwind: x64 code keeps no frame pointer), then resumed - about 1000 times a second. The same files
// (samples.txt: "<count> <module>+<offset hex> ..."); offsets from the module's load address: add its preferred base
// (lasrecomp.exe 0x140000000; objdump -p for lasgame.dll) for addr2line. Nothing is allocated while it is stopped.
#include <windows.h>
#include <mmsystem.h>  // (timeBeginPeriod: WIN32_LEAN_AND_MEAN leaves it out)
#include <tlhelp32.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "hle.h"

#define SMAX 80000
#define DEPTH 16
static uint64_t (*g_s)[DEPTH];
static _Atomic int g_ns;
static int g_kind = -1;
static double g_from, g_to;
static HANDLE g_th;
static _Atomic bool g_started;

void sampler_frame_end(double busy_ms) { (void)busy_ms; }

static void take(uint64_t *s) {
    CONTEXT c;
    memset(&c, 0, sizeof c);
    c.ContextFlags = CONTEXT_CONTROL | CONTEXT_INTEGER;
    int d = 0;
    if (SuspendThread(g_th) == (DWORD)-1) return;
    if (GetThreadContext(g_th, &c)) {
        s[d++] = c.Rip;
        s[d++] = 0;
        for (int k = 0; k < DEPTH - 2 && c.Rip; k++) {
            DWORD64 base = 0;
            PRUNTIME_FUNCTION fe = RtlLookupFunctionEntry(c.Rip, &base, NULL);
            if (fe) {
                PVOID hd = NULL;
                DWORD64 est = 0;
                RtlVirtualUnwind(UNW_FLAG_NHANDLER, base, c.Rip, fe, &c, &hd, &est, NULL);
            } else {  // (a leaf: its return address on top of the stack)
                if (!c.Rsp || (c.Rsp & 7)) break;
                c.Rip = *(const DWORD64 *)(uintptr_t)c.Rsp;
                c.Rsp += 8;
            }
            if (!c.Rip) break;
            s[d++] = c.Rip;
        }
    }
    ResumeThread(g_th);
    while (d < DEPTH) s[d++] = 0;
}

typedef struct { uint64_t lo, hi; char name[64]; } Mod;
static Mod g_mods[1024];
static int g_nmods;
static void list_modules(void) {
    HANDLE h = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE, GetCurrentProcessId());
    if (h == INVALID_HANDLE_VALUE) return;
    MODULEENTRY32 me;
    me.dwSize = sizeof me;
    for (BOOL ok = Module32First(h, &me); ok && g_nmods < 1024; ok = Module32Next(h, &me)) {
        Mod *m = &g_mods[g_nmods++];
        m->lo = (uint64_t)(uintptr_t)me.modBaseAddr, m->hi = m->lo + me.modBaseSize;
        snprintf(m->name, sizeof m->name, "%s", me.szModule);
    }
    CloseHandle(h);
}

static int cmp_stack(const void *a, const void *b) {
    int r = memcmp(g_s[*(const int *)a], g_s[*(const int *)b], sizeof(uint64_t) * DEPTH);
    return r ? r : *(const int *)a - *(const int *)b;
}

static void write_report(int n) {
    list_modules();
    int *ord = malloc(sizeof(int) * (size_t)(n ? n : 1));
    if (!ord) return;
    for (int i = 0; i < n; i++) ord[i] = i;
    qsort(ord, (size_t)n, sizeof *ord, cmp_stack);
    FILE *f = fopen("samples.txt", "w");
    if (!f) { LOG("[sample] can't write samples.txt\n"); free(ord); return; }
    fprintf(f, "# %d samples of the %s thread, %.1f-%.1f s\n", n, g_kind == 2 ? "compile" : g_kind ? "worker" : "game", g_from, g_to);
    for (int i = 0; i < n;) {
        int j = i + 1;
        while (j < n && !memcmp(g_s[ord[i]], g_s[ord[j]], sizeof g_s[0])) j++;
        fprintf(f, "%d", j - i);
        for (int d = 0; d < DEPTH && (d < 2 || g_s[ord[i]][d]); d++) {
            uint64_t a = g_s[ord[i]][d];
            int m = 0;
            while (m < g_nmods && !(a >= g_mods[m].lo && a < g_mods[m].hi)) m++;
            if (m < g_nmods) fprintf(f, " %s+%llx", g_mods[m].name, (unsigned long long)(a - g_mods[m].lo));
            else fprintf(f, " ?+%llx", (unsigned long long)a);
        }
        fputc('\n', f);
        i = j;
    }
    fclose(f);
    free(ord);
    LOG("[sample] %d samples of the %s thread written to samples.txt\n", n, g_kind == 2 ? "compile" : g_kind ? "worker" : "game");
}

static DWORD WINAPI sampler_main(LPVOID arg) {
    (void)arg;
    timeBeginPeriod(1);
    LARGE_INTEGER fq, t0, t;
    QueryPerformanceFrequency(&fq);
    QueryPerformanceCounter(&t0);
    for (;;) {
        QueryPerformanceCounter(&t);
        double s = (double)(t.QuadPart - t0.QuadPart) / (double)fq.QuadPart;
        if (s >= g_to) break;
        if (s >= g_from) {
            int i = atomic_fetch_add(&g_ns, 1);
            if (i < SMAX) take(g_s[i]);
        }
        Sleep(1);
    }
    timeEndPeriod(1);
    int n = atomic_load(&g_ns);
    if (n > SMAX) n = SMAX;
    write_report(n);
    return 0;
}

void sampler_thread(int kind) {
    if (g_kind == -1) {
        const char *e = getenv("LARECOMP_SAMPLE");
        g_kind = -2;
        if (e && *e) {
            char *p;
            g_from = strtod(e, &p);
            g_to = *p == ':' ? strtod(p + 1, &p) : g_from + 10;
            g_kind = *p != ':' ? 0 : !strcmp(p + 1, "worker") ? 1 : !strcmp(p + 1, "compile") ? 2 : 0;
        }
    }
    if (kind != g_kind || atomic_exchange(&g_started, true)) return;
    g_th = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, GetCurrentThreadId());
    g_s = calloc(SMAX, sizeof *g_s);
    if (!g_th || !g_s) return;
    HANDLE h = CreateThread(NULL, 0, sampler_main, NULL, 0, NULL);
    if (h) SetThreadPriority(h, THREAD_PRIORITY_TIME_CRITICAL), CloseHandle(h);
    LOG("[sample] sampling the %s thread from %.1f to %.1f s\n", kind == 2 ? "compile" : kind ? "worker" : "game", g_from, g_to);
}
#else
void sampler_thread(int kind) { (void)kind; }
void sampler_frame_end(double busy_ms) { (void)busy_ms; }
#endif
