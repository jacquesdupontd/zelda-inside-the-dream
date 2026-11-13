// Recording a play session and replaying it (Guillaume, 20/10: « la logique voudrait que tu records mes inputs en me
// faisant partir de tel endroit, et tu peux le rejouer autant de fois que tu veux »). Measured runs that stand still
// and turn say nothing of a fight or a walk across screens; his own route, replayed, does.
//   LARECOMP_INPUT_RECORD=<dir>  copies the save folder and larecomp.ini into <dir> at start, then from the first frame
//                                in play writes <dir>/inputs.bin: per presented frame, what the game's pad gets
//                                (buttons and sticks, before the first-person and mirror steps), the mouse buttons and
//                                the first-person view's yaw and pitch (whatever turned it: stick, mouse).
//   LARECOMP_INPUT_REPLAY=<dir>  from the first frame in play, the same frames given again (the real keyboard, mouse
//                                and pads are ignored); title screen and file select are the caller's (LARECOMP_INPUT),
//                                with LARECOMP_SAVE_DIR on a copy of <dir>/save. The log says when the recording ends.
// Frames count from the first presented frame with a game in play (warp.c game_in_play), in both modes.
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "../hle.h"

typedef struct {
    uint32_t f, mbtn;
    uint64_t b;
    int32_t lx, ly, rx, ry;
    float yaw, pitch;
} Rec;  // 40 bytes a frame: ~140 KB a minute

static int g_mode = -1;  // 0 off, 1 record, 2 replay
static char g_dir[512];
static FILE *g_out;
static Rec *g_recs, g_cur;
static size_t g_n;
static uint64_t g_start = ~0ull, g_last_frame = ~0ull;
static bool g_ended;

extern uint64_t renderer_present_count(void);
extern int game_in_play(void);
extern const char *g_save_dir;

static void init(void) {
    if (g_mode >= 0) return;
    const char *r = getenv("LARECOMP_INPUT_RECORD"), *p = getenv("LARECOMP_INPUT_REPLAY");
    g_mode = r && *r ? 1 : p && *p ? 2 : 0;
    if (!g_mode) return;
    snprintf(g_dir, sizeof g_dir, "%s", g_mode == 1 ? r : p);
    if (g_mode == 1) {
        char cmd[1400];
#ifdef _WIN32  // (the shell's commands are cmd.exe's: mkdir -p and cp made nothing, and nothing was recorded, 27/10)
        snprintf(cmd, sizeof cmd, "(if not exist \"%s\" mkdir \"%s\") && (if exist \"%s\\save\" rmdir /s /q \"%s\\save\") && "
                 "xcopy /e /i /q /y \"%s\" \"%s\\save\" >nul && copy /y larecomp.ini \"%s\\\" >nul",
                 g_dir, g_dir, g_dir, g_dir, g_save_dir, g_dir, g_dir);
#else
        snprintf(cmd, sizeof cmd, "mkdir -p '%s' && rm -rf '%s/save' && cp -R '%s' '%s/save' && cp larecomp.ini '%s/' 2>/dev/null",
                 g_dir, g_dir, g_save_dir, g_dir, g_dir);
#endif
        if (system(cmd) != 0) LOG("[inputrec] could not copy the save into %s\n", g_dir);
        char path[600];
        snprintf(path, sizeof path, "%s/inputs.bin", g_dir);
        g_out = fopen(path, "wb");
        if (g_out) LOG("[inputrec] recording to %s\n", path);
        else LOG("[inputrec] cannot write %s: nothing recorded\n", path), g_mode = 0;
    } else {
        char path[600];
        snprintf(path, sizeof path, "%s/inputs.bin", g_dir);
        FILE *f = fopen(path, "rb");
        if (!f) { LOG("[inputrec] no %s\n", path); g_mode = 0; return; }
        fseek(f, 0, SEEK_END);
        long sz = ftell(f);
        fseek(f, 0, SEEK_SET);
        g_n = sz > 0 ? (size_t)sz / sizeof(Rec) : 0;
        g_recs = malloc(g_n ? g_n * sizeof(Rec) : 1);
        g_n = fread(g_recs, sizeof(Rec), g_n, f);
        fclose(f);
        LOG("[inputrec] replaying %s: %zu frames (%.1f s)\n", path, g_n, g_n / 60.0);
    }
}

bool inputrec_replaying(void) { init(); return g_mode == 2; }  // (input.c: the player's own input is then ignored)

// LARECOMP_INPUTREC_SHOTS=<frame>,<frame>...: window shots at those frames in play, <dir>/<record|replay>_<frame>.png
// (a recording and its replay compared at the same moment of play)
static void shots(int64_t rf) {
    static int64_t want[32], last = -1;
    static int n = -1;
    if (n < 0) {
        n = 0;
        for (const char *p = getenv("LARECOMP_INPUTREC_SHOTS"); p && *p && n < 32;) {
            want[n++] = strtoll(p, (char **)&p, 10);
            while (*p == ',') p++;
        }
    }
    if (rf == last) return;
    last = rf;
    for (int i = 0; i < n; i++)
        if (want[i] == rf) {
            extern char g_window_shot_request[256];
            snprintf(g_window_shot_request, sizeof g_window_shot_request, "%s/%s_%lld.png", g_dir, g_mode == 1 ? "record" : "replay",
                     (long long)rf);
        }
}

// The frame in play (relative), or -1: started at the first presented frame with a game in play
static int64_t rel_frame(void) {
    uint64_t f = renderer_present_count();
    if (g_start == ~0ull) {
        if (!game_in_play()) return -1;
        g_start = f;
        LOG("[inputrec] in play at presented frame %llu: frame 0\n", (unsigned long long)f);
    }
    shots((int64_t)(f - g_start));
    return (int64_t)(f - g_start);
}

static const Rec *replay_rec(int64_t rf) {
    if (rf < 0 || (size_t)rf >= g_n) {
        if (rf >= 0 && !g_ended && g_n) { g_ended = true; LOG("[inputrec] replay over at frame %lld\n", (long long)rf); }
        return NULL;
    }
    return &g_recs[rf];
}

// runtime/hle/input.c fill_state: what the pad gives, before the mirror and first-person steps (every poll of a frame)
void inputrec_pad(uint64_t *b, int32_t *lx, int32_t *ly, int32_t *rx, int32_t *ry) {
    init();
    if (!g_mode) return;
    int64_t rf = rel_frame();
    if (rf < 0) return;
    if (g_mode == 2) {
        const Rec *r = replay_rec(rf);
        if (r) *b = r->b, *lx = r->lx, *ly = r->ly, *rx = r->rx, *ry = r->ry;
        return;
    }
    if ((uint64_t)rf != g_last_frame) {  // a new frame: the last one is complete
        if (g_last_frame != ~0ull && g_out) {
            fwrite(&g_cur, sizeof g_cur, 1, g_out);
            // frames the game did not read the pad in (loading): nothing held, the view as it was - one record a frame,
            // so that the replay can index by frame
            for (uint64_t k = g_last_frame + 1; k < (uint64_t)rf; k++) {
                Rec gap = g_cur;
                gap.f = (uint32_t)k, gap.b = 0, gap.lx = gap.ly = gap.rx = gap.ry = 0, gap.mbtn = 0;
                fwrite(&gap, sizeof gap, 1, g_out);
            }
            if ((g_cur.f & 255) == 0) fflush(g_out);
        }
        g_last_frame = (uint64_t)rf;
        memset(&g_cur, 0, sizeof g_cur);
        g_cur.f = (uint32_t)rf;
        g_cur.b = *b, g_cur.lx = *lx, g_cur.ly = *ly, g_cur.rx = *rx, g_cur.ry = *ry;  // (the frame's first poll)
    }
}

// runtime/hle/firstperson.c fp_input, once a presented frame before the view is published: its yaw, pitch and the
// mouse buttons - recorded, or given back
void inputrec_view(float *yaw, float *pitch, uint32_t *mbtn) {
    init();
    if (!g_mode) return;
    int64_t rf = rel_frame();
    if (rf < 0) return;
    if (g_mode == 2) {
        const Rec *r = replay_rec(rf);
        if (r) *yaw = r->yaw, *pitch = r->pitch, *mbtn = r->mbtn;
        return;
    }
    if ((uint64_t)rf == g_last_frame) g_cur.yaw = *yaw, g_cur.pitch = *pitch, g_cur.mbtn = *mbtn;
}
