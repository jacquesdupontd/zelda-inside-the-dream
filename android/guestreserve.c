// libguestreserve.so: preloaded by wrap.sh (LD_PRELOAD) into the tablet app's process, which Android then starts afresh
// instead of forking it from zygote (android:debuggable, lib/arm64-v8a/wrap.sh). Before anything else maps memory, the
// game's fixed range is held (PROT_NONE): Android's own libraries and allocations land elsewhere, and the runtime takes
// it over (runtime/platform/linux.c plat_reserve_guest, LARECOMP_GUEST_HELD). Forked from zygote, the range was already
// full of Android's own mappings on a Xiaomi Pad 7 (27/10/2026: MIUI's libmigl, the allocator's reserves, our libraries).
#include <android/log.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#define GUEST_LO 0x7000000000ull
#define GUEST_HI 0x7320000000ull

__attribute__((constructor(101))) static void hold_guest_range(void) {
    void *p = mmap((void *)(uintptr_t)GUEST_LO, GUEST_HI - GUEST_LO, PROT_NONE,
                   MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE | MAP_FIXED_NOREPLACE, -1, 0);
    if (p == (void *)(uintptr_t)GUEST_LO) {
        setenv("LARECOMP_GUEST_HELD", "1", 1);
        __android_log_print(ANDROID_LOG_INFO, "lasrecomp", "the guest range held at the process's start");
        return;
    }
    if (p != MAP_FAILED) munmap(p, GUEST_HI - GUEST_LO);
    // Taken already: the address layout this start drew put the allocator's reserves or the CFI shadow in it (a third of
    // the starts, by Gemini's reading). Nothing of the app has run yet (app_process's main() comes after): started again
    // with the same command line, the kernel draws another layout - 2 ms a try, at most 20 (LARECOMP_WRAP_TRIES)
    const char *t = getenv("LARECOMP_WRAP_TRIES");
    int n = t ? atoi(t) : 0;
    __android_log_print(ANDROID_LOG_WARN, "lasrecomp", "the guest range taken at the start (try %d): started again", n + 1);
    if (n >= 20) return;
    char buf[16];
    snprintf(buf, sizeof buf, "%d", n + 1);
    setenv("LARECOMP_WRAP_TRIES", buf, 1);
    static char cmd[8192];
    FILE *f = fopen("/proc/self/cmdline", "rb");
    size_t len = f ? fread(cmd, 1, sizeof cmd - 1, f) : 0;
    if (f) fclose(f);
    char *argv[128];
    int argc = 0;
    for (size_t i = 0; i < len && argc < 127; i += strlen(cmd + i) + 1) argv[argc++] = cmd + i;
    argv[argc] = NULL;
    if (argc) execv("/proc/self/exe", argv);
}
