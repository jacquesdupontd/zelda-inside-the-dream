// First-person view (Gameplay tab, g_settings.first_person; LARECOMP_FP=0/1 overrides it, for tests): the camera at
// Link's eyes, the right stick looks around, the left stick walks relative to where he looks.
//
// Stage 1 (this file), on the render side: the game keeps its own top-down camera, and each draw's snapshot of its
// uniform blocks (runtime/nvn/renderer.m) gets the first-person camera instead. The game's camera of the moment is
// read from its scene block (slot 5, 320 bytes: view, projection, view-projection as rows); every block carrying copies
// of its values (view-projection, view, projection, inverse rotation, position, direction back to it, depth range) is
// found by content, per (stage, slot, size), and rewritten there: the scene block itself, the 1024-byte view block in
// slot 1, and whatever else turns out to hold them (LARECOMP_LOG_FP lists what was found where). Measured 20/10 in
// Mabe Village: the scene block alone reached ~25 draws of ~400 - most geometry reads the slot-1 copy. The game still
// culls with its own camera: the culling pass's fovy / aspect loads (tools/patches.py, rt_cull_fovy / rt_cull_aspect)
// are widened while the view is on, so what lies around Link is drawn.
// Stage 2, with the game's code in hand: replace the camera's eye and target where the game builds its view
// (main camera: fovy at +0x564, aspect at +0x570), so culling, shadows and every pass follow by themselves.
//
// World axes (to check on the first runs, LARECOMP_LOG_FP): y up, x to the right of the game's screen, z towards the
// game's camera (south). Yaw 0 looks north (-z); positive turns clockwise seen from above (towards +x).
// Link's position: runtime/hle/doorjump.c (PlayerCollision +0x80).
//
// Tuning while it's young (env): LARECOMP_FP_EYE eye height above that position (units, default 1.1; a tile is 1.5),
// LARECOMP_FP_FWD eye ahead of it (0.15), LARECOMP_FP_FOV vertical degrees (60), LARECOMP_FP_NEAR (0.1),
// LARECOMP_FP_FAR far plane at least (400), LARECOMP_FP_YAW start yaw in degrees, LARECOMP_FP_INVERT_Y=1.
#include <math.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "../game_addresses.h"
#include "../hle.h"
#include "../settings.h"

uint64_t renderer_present_count(void);
int game_in_play(void);
uint64_t doorjump_player(uint64_t frame);
uint32_t nvn_depth_mode(void);
bool widescreen_side_view_zone(void);

static float envf(const char *name, float def) {
    const char *e = getenv(name);
    return e && *e ? (float)atof(e) : def;
}

static bool fp_setting(void) {
    static int env = -1;
    if (env < 0) {
        const char *e = getenv("LARECOMP_FP");
        env = e ? atoi(e) != 0 : 2;
        // LARECOMP_FP_START_ON=1 (the headset, android/xr_app.c): on at the start, then R3 switches it as on a screen -
        // LARECOMP_FP=1 there kept it on for good (« le click du stick droit ne me refait pas passer par la vue de haut,
        // ni le stick gauche en vue derrière Link, alors que c'est partout comme ça », Guillaume, 27/10)
        // - unless the view was saved: the play starts in the one he left (« j'aimerais être directement dans la vue
        // précédente enregistrée », 27/10)
        extern bool settings_first_person_saved(void);
        if (env == 2 && ENV("LARECOMP_FP_START_ON") && !settings_first_person_saved()) g_settings.first_person = true;
    }
    return env == 2 ? g_settings.first_person : env;
}

// ------------------------------------------------------------------ the view (game thread)

static _Atomic bool g_view_held;
// R3 into his eyes asked before his place is known (fp_buttons): the frame it was asked at. The flight waits on the
// game's camera - not black - and starts once the view is on (the trailer's dive, 28/10: 2.75 s of black after R3 in
// Marin and Tarin's house, his place looked for only from the click on, and that much of the flight gone by unseen)
static _Atomic uint64_t g_fl_wait;
static void fl_begin(void);
static _Atomic uint64_t g_hide_from = UINT64_MAX;  // (what he holds hidden from a door's request: fp_held_hidden)
// The eye kept out of the people: their bones (fp_draw_class - the bone palettes of the drawn characters, Link's aside)
// closer to it than FP_PUSH_R across push it away on the next frame. Their bounding sphere, first, was far too big to
// push by, and leaving the whole person out when the eye entered it made them vanish - « le personnage disparaît quand
// on rentre dedans, ou s'accroche à nous si on est à côté », Guillaume, 27/10. Pots and bushes have no bones.
// A capsule per person (their bones' middle across, reach, lowest and highest), from the last frame drawn; the push
// itself is made in publish, on the game thread, from the eye as it stands there without it: first made per bone where
// they were drawn, a frame late and from another thread, it jumped from bone to bone and the view shook (« tout vibre à
// fond », 27/10). The head's own motion is left to the head: pushing it back made the world follow it.
typedef struct { float x, z, reach, y0, y1; } Cap;
#define NCAP 16
static Cap g_cap_acc[NCAP], g_cap_last[NCAP];
static _Atomic int g_ncap_acc, g_ncap_last;
static float g_push[2], g_push_pub[2];
static uint64_t g_push_frame;
static float g_cull_eye[3];
static float g_yaw, g_pitch;  // radians
static bool g_on;             // the view is on (the game thread's answer, for the stick and the 360 movement)

// What the renderer needs, published once per presented frame into a ring: a reader takes the last one written - except
// the asynchronous submission thread (the Quest), which takes the one that was the last when the game queued the
// commands it is translating (fp_view_pin, runtime/nvn/objects.c). It read the latest instead, so the frame it was
// translating could take the next frame's view part way through: its depth pre-pass drawn from one eye position and
// its colour pass from another (the eye follows Link's head, smoothed: it moves a little every frame), the colour pass's
// equal-depth test failing - the world black, or half of it missing, for a frame or several (21/10, recorded in the
// headset).
// (ns: when it was published, CLOCK_MONOTONIC; ryaw, rpitch: the stick's turn then, radians a second - a headset's
// frame loop turns the image between two game frames by it: fp_xr_turn; tilt: a headset's, look)
typedef struct { float eye[3], link[3], yaw, pitch; uint64_t frame; bool on; int64_t ns; float ryaw, rpitch; float tilt[9]; } View;
#define NPUB 8
static View g_pub[NPUB];
static atomic_uint g_pub_gen;
static __thread uint32_t t_pin;  // (the submission thread) the generation of the view of the commands it translates, + 1

RT_ON_RESET(fp_reset) {
    g_yaw = envf("LARECOMP_FP_YAW", 0) * (float)M_PI / 180.0f, g_pitch = 0, g_on = false;
    memset(g_pub, 0, sizeof g_pub);
}

static float stick_axis(int32_t v) {  // -1..1 past a dead zone, squared for fine aim near the centre
    const float dead = 5000;
    float a = fabsf((float)v);
    if (a <= dead) return 0;
    float x = fminf(1.0f, (a - dead) / (32767 - dead));
    return copysignf(x * x, (float)v);
}

static float g_ryaw, g_rpitch;  // (the stick's turn at the latest frame, radians a second)
static uint32_t g_xr_serial;  // 0: no headset view
// In a headset the stick's forward / back tilts the view about the head's right axis AT THAT MOMENT, the tilt kept as it
// is (rows: tracking space from tilted): the view is Y(yaw) T head. Tilted about the body's axis it was a sideways lean
// with the head turned (21/10 - 27/10); about the head's axis taken again each frame, the world turned round Link
// whenever the head moved (« pencher dans le sens des aiguilles d'une montre, ultra bizarre », Guillaume, 27/10).
static float g_tilt[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1};
static void quat_basis(const float *q, float *m);
static void xr_head_q(float *q);  // (the head's latest orientation: g_xr, below)
static void tilt_reset(void) { static const float id[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1}; memcpy(g_tilt, id, sizeof id); }
static void tilt_by(const float *a, float t) {  // (T = T R(a, t), a unit, in tracking space)
    float c = cosf(t), s = sinf(t), k = 1 - c, R[9] = {
        c + a[0] * a[0] * k, a[0] * a[1] * k - a[2] * s, a[0] * a[2] * k + a[1] * s,
        a[1] * a[0] * k + a[2] * s, c + a[1] * a[1] * k, a[1] * a[2] * k - a[0] * s,
        a[2] * a[0] * k - a[1] * s, a[2] * a[1] * k + a[0] * s, c + a[2] * a[2] * k}, T[9];
    for (int r = 0; r < 3; r++)
        for (int q = 0; q < 3; q++) T[r * 3 + q] = g_tilt[r * 3] * R[q] + g_tilt[r * 3 + 1] * R[3 + q] + g_tilt[r * 3 + 2] * R[6 + q];
    float ang = acosf(fmaxf(-1, fminf(1, (T[0] + T[4] + T[8] - 1) / 2)));
    if (ang < 1.4f) memcpy(g_tilt, T, sizeof T);  // (80 degrees at most from level)
}
static void look(int32_t rx, int32_t ry, uint64_t frames) {
    // In a headset LARECOMP_FP_XR_STICK_PITCH=0 (PC VR's default, android/xr_app.c): the stick turns only, the head looks
    // up and down - its pitch, from any slant of the stick, tilted the view under the head turned sideways (27/10)
    static int xr_pitch = -1;
    if (xr_pitch < 0) xr_pitch = envf("LARECOMP_FP_XR_STICK_PITCH", 1) != 0;
    bool inv = ENV("LARECOMP_FP_INVERT_Y") != NULL || g_settings.fp_invert_y;
    float sp = (g_settings.fp_sens > 0 ? g_settings.fp_sens : 5) / 5.0f;
    // (a frame of the game's 30 fps mode lasts two 60ths: counted as one, the view turned at half speed there - the
    //  tablet and the Quest, held at 30 - and the warp presenter, turning at the stick's rate between frames, went past
    //  it and came back each frame: « pour tourner, pivoter, c'est lent, c'est bizarre », Guillaume, 27/10)
    extern bool pace_is_slow(void);
    float dt = (float)frames * (pace_is_slow() ? 2.0f : 1.0f) / 60.0f;
    g_ryaw = stick_axis(rx) * 3.0f * sp, g_rpitch = stick_axis(ry) * (inv ? -2.0f : 2.0f) * sp;
    if (g_xr_serial) {  // (a headset: the turn about the world's up, the tilt about the head's right axis now, kept)
        float dp = stick_axis(ry) * (inv ? -2.0f : 2.0f) * sp * dt;
        g_pitch = 0, g_rpitch = 0;  // (the frame loop's turn between game frames: the turn only)
        if (xr_pitch && dp != 0) {
            float h[9], hq[4];
            xr_head_q(hq);
            quat_basis(hq, h);  // (columns: the head's right, up, back in tracking space)
            const float a[3] = {h[0], h[3], h[6]};
            tilt_by(a, dp);
        }
        g_yaw += stick_axis(rx) * 3.0f * sp * dt;
        if (g_yaw > (float)M_PI) g_yaw -= 2 * (float)M_PI;
        if (g_yaw < -(float)M_PI) g_yaw += 2 * (float)M_PI;
        return;
    }
    g_yaw += stick_axis(rx) * 3.0f * sp * dt;  // full tilt, look speed 5: about 170 degrees a second
    g_pitch += stick_axis(ry) * (inv ? -2.0f : 2.0f) * sp * dt;
    g_pitch = fmaxf(-1.4f, fminf(1.4f, g_pitch));  // (80 degrees up / down)
    if (g_yaw > (float)M_PI) g_yaw -= 2 * (float)M_PI;
    if (g_yaw < -(float)M_PI) g_yaw += 2 * (float)M_PI;
}

// Link's head, from his skeleton: his body (the draw of 4419 indices - hat and body, measured 20/10) reads his bone
// palette (2256 bytes); its bone 46 is his head (0.62 above his feet standing). The eye follows it (smoothed: no walking
// bob), so it is where his head is even when his model isn't at his collision - waking up in bed, jumping, falling,
// scenes. The palette's address also tells his parts apart from Marin's (the same size) when he isn't by his collision.
// runtime/nvn/renderer.m calls fp_link_body for that draw.
static uint64_t g_link_pal[2], g_head_frame;
static float g_head[3], g_face[2];  // (his head; where he faces: his root bone's forward axis, x and z)
static bool g_upright = true;
void fp_link_body(const uint64_t *addr, const uint64_t *size, int n) {
    for (int i = 0; i < n; i++) {
        if (!addr[i] || size[i] != 2256) continue;
        const float *f = (const float *)(uintptr_t)addr[i];
        if (!(fabsf(f[0]) <= 1.5f && fabsf(f[5]) <= 1.5f && f[0] * f[0] + f[1] * f[1] + f[2] * f[2] > 0.25f)) continue;
        if (addr[i] != g_link_pal[0]) g_link_pal[1] = g_link_pal[0], g_link_pal[0] = addr[i];
        g_head[0] = f[46 * 12 + 3], g_head[1] = f[46 * 12 + 7], g_head[2] = f[46 * 12 + 11];
        g_upright = f[5] > 0.8f;  // (his root's up axis still up)
        g_face[0] = f[2], g_face[1] = f[10];  // (the model's z axis in the world: the way he faces)
        g_head_frame = renderer_present_count();
        return;
    }
}
bool fp_link_upright(void) { return g_upright; }

// The door of a house: where Link stands when he comes in (the mat by the wall that was never modelled). The mirrored
// shell puts a wall there too (fp_mirror_draw); the sky pass darkens that wall the size of a door, so the way out shows
// (Guillaume's idea, 20/10). Unknown until he walks in (waking up in bed: no door).
static char g_door_level[48];
static float g_door[2];
static bool g_door_known;
static float ceil_height(const char *lv, bool *stone);
// the level he is in and the one before (loads, "-", left out), followed every frame whatever the view (fp_input)
static char g_lv_cur[48], g_lv_prev[48];
static void track_level(void) {
    extern const char *game_level_name(void);
    const char *lv = game_level_name();
    if (!strcmp(lv, "-") || !strncmp(g_lv_cur, lv, sizeof g_lv_cur - 1)) return;
    snprintf(g_lv_prev, sizeof g_lv_prev, "%s", g_lv_cur), snprintf(g_lv_cur, sizeof g_lv_cur, "%s", lv);
}
static void note_door(const float *p) {
    extern const char *game_level_name(void);
    const char *lv = game_level_name();
    if (!strcmp(lv, "-") || !strncmp(g_door_level, lv, sizeof g_door_level - 1)) return;
    // a house he walks into from outside: by its door, wherever it stands in its cell - Dampe's is 3.4 from the cell's
    // south edge, where the spawn-by-the-edge test (kept for the rest: waking up in bed, a save loaded) wanted 3; the
    // south wall wasn't built and stayed dark (« le mur sud noir des maisons », Guillaume, 22/10 and 26/10)
    bool from_outside = !strncmp(g_lv_cur, lv, sizeof g_lv_cur - 1) && !strcmp(g_lv_prev, "Field"), stone;
    ceil_height(lv, &stone);
    strncpy(g_door_level, lv, sizeof g_door_level - 1);
    g_door[0] = p[0], g_door[1] = p[2];
    float south = (floorf(p[2] / 12) + 1) * 12;  // (his cell's south edge: a spawn by it is a door)
    g_door_known = strcmp(lv, "Field") && strncmp(lv, "Lv", 2) && !(p[0] == 0 && p[2] == 0) &&
                   (south - p[2] < 3.0f || (from_outside && !stone));
    LOG("[fp] entered %s at (%.2f, %.2f)%s: %s\n", lv, p[0], p[2], from_outside ? " from outside" : "",
        g_door_known ? "the door" : "not by the door");
}

#define NLOOK 64
typedef struct { uint64_t ns; float yaw, pitch, eye[3]; bool eye_ok; } Look;  // (eye: where the view was, when on)
static Look g_looks[NLOOK];
static atomic_uint g_nlooks;
// How high above the floor we close a room (the sky pass's ceiling): stone (dungeons, caves) 3, a house 2.2 - on its walls'
// tops, 3.2 left a gap the walls were carried up into (20/10). LARECOMP_FP_CEIL.
static float ceil_height(const char *lv, bool *stone) {
    *stone = !strncmp(lv, "Lv", 2) || strstr(lv, "Cave") || strstr(lv, "Tunnel") || strstr(lv, "Shrine") || strstr(lv, "Well") ||
             strstr(lv, "Grave") || strstr(lv, "Tower") || strstr(lv, "Egg");
    return envf("LARECOMP_FP_CEIL", *stone ? 3.0f : 2.2f);
}
static float g_floor_y;  // (below: the floor he last stood still on)
static void fp_state_log(uint64_t pc, uint64_t frame, bool on);
// Indoors, the eye above the ceiling we close the room with: a boss that lifts him (Bottle Grotto: « on voyait plus que
// notre plafond, que du noir », Guillaume, 26/10 - he had to switch views himself), a ledge climbed without stopping.
// The game's view from above until he is back under it for 3/4 of a second. LARECOMP_FP_LIFT=1 for now: one guess of three
// at what he met (the view lost, or the scene's depth target changing, would show the same), not to be on by default
// before his log shows it.
static bool lifted_out(uint64_t pc, uint64_t frame) {
    extern const char *game_level_name(void);
    static uint64_t over_since, under_since;
    static bool lifted;
    const char *lv = game_level_name();
    if (!pc || !strcmp(lv, "Field") || !strcmp(lv, "-") || g_floor_y < -1e8f || ENV("LARECOMP_FP_NO_CEIL") ||
        !ENV("LARECOMP_FP_LIFT")) return lifted = false;  // (opt-in: the grab's cause not seen yet, 26/10)
    const float *p = (const float *)(uintptr_t)(pc + 0x80);
    bool stone;
    float eye_h = envf("LARECOMP_FP_EYE", g_settings.fp_eye > 0 ? g_settings.fp_eye / 100.0f : 1.1f);
    bool over = p[1] + eye_h > g_floor_y + ceil_height(lv, &stone) - 0.15f;
    if (over) {
        under_since = 0;
        if (!over_since) over_since = frame;
        if (!lifted && frame - over_since >= 2) {
            lifted = true;
            LOG("[fp] the eye above the room's ceiling (feet %.2f, floor %.2f): the view from above meanwhile\n", p[1], g_floor_y);
        }
    } else {
        over_since = 0;
        if (!under_since) under_since = frame;
        if (lifted && frame - under_since >= 45) {
            lifted = false;
            LOG("[fp] back under the ceiling: the first-person view again\n");
        }
    }
    return lifted;
}
static void publish(bool on, uint64_t pc, uint64_t frame) {
    uint32_t gen = atomic_load(&g_pub_gen) + 1;
    View *v = &g_pub[gen % NPUB];
    v->on = on, v->frame = frame, v->yaw = g_yaw, v->pitch = g_pitch;
    {   // (one clock for the headset's frame loop and the desktop presenter: plat_now_ns, CLOCK_MONOTONIC on Linux and
        //  Android, the performance counter on Windows - android/xr_app.c mono_ns reads the same)
        v->ns = (int64_t)plat_now_ns(), v->ryaw = g_ryaw, v->rpitch = g_rpitch;
        static int tlog = -1, nt;  // (LARECOMP_LOG_TURN=1: each view published while the stick turns)
        if (tlog < 0) tlog = ENV("LARECOMP_LOG_TURN") != NULL;
        if (tlog && g_ryaw != 0 && nt++ < 600)
            LOG("[turn] published %.1f yaw %.2f frame %llu\n", (double)(v->ns % 100000000000LL) / 1e6, g_yaw * 57.29578f,
                (unsigned long long)frame);
    }
    memcpy(v->tilt, g_tilt, sizeof v->tilt);
    if (on) {
        const float *p = (const float *)(uintptr_t)(pc + 0x80);
        {   // Stairs, a hole, a warp, a passage seen from the side: Link is elsewhere since the view was last on - it
            // turns the way he faces there, as R3 does on the way in (a dungeon's stairs kept the view the way he went
            // down them and showed his back on the way out: « on arrive par derrière au lieu de devant », Guillaume,
            // 26/10). His facing comes from his skeleton's next draw (fp_link_body).
            static float lp[3];
            static bool seen;
            static uint64_t align_from;
            float jx = p[0] - lp[0], jy = p[1] - lp[1], jz = p[2] - lp[2];
            if (seen && jx * jx + jy * jy + jz * jz > 4.0f && !ENV("LARECOMP_FP_NO_ALIGN")) align_from = frame;
            memcpy(lp, p, sizeof lp), seen = true;
            if (align_from && g_head_frame > align_from) {
                float fx = g_face[0], fz = g_face[1];
                if (fx * fx + fz * fz > 1e-4f) {
                    g_yaw = atan2f(fx, -fz), g_pitch = 0;
                    v->yaw = g_yaw, v->pitch = g_pitch;
                    if (ENV("LARECOMP_LOG_FP")) LOG("[fp] elsewhere at once: the view turned the way Link faces, %.0f degrees\n",
                                                    g_yaw * 180 / M_PI);
                }
                align_from = 0;
            }
            if (align_from && frame - align_from > 120) align_from = 0;  // (his skeleton not seen: left as it was)
        }
        static float fwd = -1e9f;
        if (fwd < -1e8f) fwd = envf("LARECOMP_FP_FWD", 0.15f);
        float eye_h = envf("LARECOMP_FP_EYE", g_settings.fp_eye > 0 ? g_settings.fp_eye / 100.0f : 1.1f);
        float t[3] = {p[0], p[1] + eye_h, p[2]};
        if (frame - g_head_frame <= 3 && !ENV("LARECOMP_FP_NO_HEAD")) {  // (his head, raised to the eye height we use)
            float dx = g_head[0] - p[0], dz = g_head[2] - p[2];
            if (dx * dx + dz * dz < 25) t[0] = g_head[0], t[1] = g_head[1] + eye_h - 0.62f, t[2] = g_head[2];
        }
        static float e[3];
        static bool started;
        float dd = (t[0] - e[0]) * (t[0] - e[0]) + (t[1] - e[1]) * (t[1] - e[1]) + (t[2] - e[2]) * (t[2] - e[2]);
        float k = !started || dd > 9 ? 1.0f : 0.35f;  // (smoothed; a jump of 3 units or more: there at once)
        for (int i = 0; i < 3; i++) e[i] += (t[i] - e[i]) * k;
        started = true;
        {   // (out of the people's capsules - LARECOMP_FP_NO_PUSH=1: off; LARECOMP_FP_PUSH_R=<min>,<max>: their radius)
            static float rmin = -1, rmax;
            if (rmin < 0) {
                rmin = 0.45f, rmax = 0.75f;
                const char *pe = getenv("LARECOMP_FP_PUSH_R");
                if (pe) sscanf(pe, "%f,%f", &rmin, &rmax);
            }
            float ux = e[0] + sinf(g_yaw) * fwd, uz = e[2] - cosf(g_yaw) * fwd, uy = e[1], tx = 0, tz = 0, best = 0;
            int nc = ENV("LARECOMP_FP_NO_PUSH") ? 0 : atomic_load(&g_ncap_last);
            for (int c = 0; c < nc; c++) {
                const Cap *q = &g_cap_last[c];
                if (uy < q->y0 - 0.3f || uy > q->y1 + 0.5f) continue;
                float r = fminf(rmax, fmaxf(rmin, q->reach * 0.5f + 0.2f));
                float dx = ux - q->x, dz = uz - q->z, d = sqrtf(dx * dx + dz * dz), need = fminf(r - d, 0.45f);
                if (need <= best) continue;
                // (the way out: from him to Link's feet, which the game's collision keeps steady - from him to the eye
                //  it turned as Link slid along him, and the view shook: « tout vibre à fond », 27/10)
                float lx = p[0] - q->x, lz = p[2] - q->z, ld = sqrtf(lx * lx + lz * lz);
                if (ld < 1e-3f) lx = -sinf(g_yaw), lz = cosf(g_yaw), ld = 1;
                best = need, tx = lx / ld * need, tz = lz / ld * need;
            }
            g_push[0] += (tx - g_push[0]) * 0.3f, g_push[1] += (tz - g_push[1]) * 0.3f;
            if (fabsf(g_push[0]) < 1e-4f && fabsf(g_push[1]) < 1e-4f) g_push[0] = g_push[1] = 0;
            g_push_pub[0] = g_push[0], g_push_pub[1] = g_push[1];
            static uint64_t said;
            if (ENV("LARECOMP_LOG_PUSH") && nc && frame - said >= 10) {
                said = frame;
                LOG("[fp] push: frame %llu, %d people, nearest need %.2f, push %.2f %.2f", (unsigned long long)frame, nc, best,
                    g_push[0], g_push[1]);
                for (int c = 0; c < nc && c < 3; c++)
                    LOG(" | (%.2f, %.2f) reach %.2f y %.2f..%.2f d %.2f", g_cap_last[c].x, g_cap_last[c].z, g_cap_last[c].reach,
                        g_cap_last[c].y0, g_cap_last[c].y1, sqrtf((ux - g_cap_last[c].x) * (ux - g_cap_last[c].x) +
                                                                  (uz - g_cap_last[c].z) * (uz - g_cap_last[c].z)));
                LOG("\n");
            }
        }
        v->eye[0] = e[0] + sinf(g_yaw) * fwd + g_push[0];
        v->eye[1] = e[1];
        v->eye[2] = e[2] - cosf(g_yaw) * fwd + g_push[1];
        memcpy(v->link, p, sizeof v->link);
        // Unsmoothed position + monotonic time for cross-refresh gameplay-speed
        // checks. No writes to the player, and disabled in normal profiles.
        if (ENV("LARECOMP_LOG_WALK"))
            LOG("[walk] ns %lld frame %llu pos %.6f %.6f %.6f\n", (long long)v->ns,
                (unsigned long long)frame, p[0], p[1], p[2]);
        note_door(p);
        if (ENV("LARECOMP_LOG_FP") && frame % 120 == 0)
            LOG("[fp] f%llu Link (%.2f, %.2f, %.2f) yaw %.0f pitch %.0f\n", (unsigned long long)frame, p[0], p[1], p[2],
                g_yaw * 180 / M_PI, g_pitch * 180 / M_PI);
    }
    {   // (the presenters: fp_warp_view_at, fp_warp_eye_at - the eye once it is known)
        uint32_t n = atomic_load(&g_nlooks);
        g_looks[n % NLOOK] = (Look){v->ns, g_yaw, g_pitch, {v->eye[0], v->eye[1], v->eye[2]}, on};
        atomic_store(&g_nlooks, n + 1);
    }
    atomic_store(&g_pub_gen, gen);
}

// A mouse or trackpad (runtime/hle/input.c captures it with a click in the window): its motion turns and tilts the view
// (LARECOMP_FP_MOUSE radians a pixel, 0.0025), left click = B (sword), right click = R (shield). Main thread -> game thread.
static atomic_int g_mdx, g_mdy;  // (sixteenths of a pixel, summed until the next frame)
static atomic_uint g_mbtn;
void fp_mouse_motion(float dx, float dy) {
    atomic_fetch_add(&g_mdx, (int)lroundf(dx * 16));
    atomic_fetch_add(&g_mdy, (int)lroundf(dy * 16));
}
void fp_mouse_button(int b, bool down) {
    unsigned bit = b == 1 ? 1u << 1 : b == 3 ? 1u << 7 : 0;  // (SDL left, right -> BTN_B, BTN_R)
    if (!bit) return;
    if (down) atomic_fetch_or(&g_mbtn, bit);
    else atomic_fetch_and(&g_mbtn, ~bit);
}

// Actions go where the view looks: the game swings the sword, throws, lifts or talks the way Link faces, which is the
// way he last walked. When A (lift, talk), B (sword), X or Y (items) goes down with the view on, the press is held back
// 3 frames while the left stick leans gently (a third) toward where the view looks - Link turns that way - and then
// let through (a tap shorter than that is sent for 2 frames after). fp_buttons sets it, fp_input leans the stick.
static uint64_t g_turn_until, g_emit_until;
static uint64_t g_emit_mask;

// runtime/hle/input.c, at each NpadState fill (the game polls several times a frame): look with the right stick (once
// per presented frame), hand the game the left stick turned into the world's frame, keep the right stick from it.
static int32_t g_rs_raw;  // the right stick's lean at this poll (fp_buttons: a click while it leans is not a toggle)
static float head_yaw(void);
bool fp_input(int32_t *lx, int32_t *ly, int32_t *rx, int32_t *ry) {
    static uint64_t last, pc;
    g_rs_raw = abs(*rx) > abs(*ry) ? abs(*rx) : abs(*ry);
    uint64_t frame = renderer_present_count();
    if (frame != last) {
        track_level();
        uint64_t n = frame > last && frame - last < 5 ? frame - last : 1;
        last = frame;
        bool want = fp_setting() && game_in_play();
        // (looked for in play with the view off too, a slice of the heap a frame after each level change: found by the
        //  time R3 is clicked, not 165 frames after it)
        uint64_t found = doorjump_player(frame);
        pc = want ? found : 0;
        bool side = pc && widescreen_side_view_zone(), lifted = pc && !side && lifted_out(pc, frame);
        bool on = pc && !side && !lifted;  // (side-view passages, lifted out of the room: the game's camera)
        if (on != g_on)  // (each switch, and why: a door, a jump - what the headset showed then, 27/10)
            LOG("[fp] view %s at frame %llu%s\n", on ? "on" : "off", (unsigned long long)frame,
                on ? "" : !fp_setting() ? " (R3)" : !want ? " (not in play)" : !pc ? " (his place not found)"
                : side ? " (a side-view passage)" : " (lifted out of the room)");
        if (on && !g_on && atomic_load(&g_hide_from) != UINT64_MAX && frame > atomic_load(&g_hide_from) + 10)
            atomic_store(&g_hide_from, UINT64_MAX);  // (the view on again after the door: what he holds back)
        g_on = on;
        uint64_t wait = atomic_load(&g_fl_wait);
        if (wait && (on || !fp_setting() || frame - wait > 600)) {
            if (on) fl_begin();
            atomic_store(&g_fl_wait, 0), wait = 0;
        } else if (wait) fl_begin();  // (held at its start: the game's camera)
        // Wanted but not there - a door, a level loading, the play starting, his place not found for a moment: the
        // headset shows black, not the floating screen with the game's camera for those frames (« ça fait vraiment
        // bugué », Guillaume, 27/10). R3, the start screen, a side-view passage: the screen, as before.
        static uint64_t on_frame;
        if (on) on_frame = frame;
        atomic_store(&g_view_held, !on && fp_setting() && !pc && !wait && (want || (on_frame && frame - on_frame < 180)));
        extern bool world_on_screen(void);
        bool world = world_on_screen();
        g_ryaw = g_rpitch = 0;
        {   // (a test: LARECOMP_FP_TEST_RX=<from>:<to>:<value> - the right stick held that far over those frames)
            const char *e = ENV("LARECOMP_FP_TEST_RX");
            unsigned long a, b;
            int v;
            if (e && sscanf(e, "%lu:%lu:%d", &a, &b, &v) == 3 && frame >= a && frame < b) *rx = v;
        }
        if (on && world) look(*rx, *ry, n);  // (the map and the pause menu: the right stick is theirs)
        extern void fp_warp_note_look(int32_t rx, int32_t ry);
        fp_warp_note_look(on && world ? *rx : 0, on && world ? *ry : 0);
        if (ENV("LARECOMP_LOG_LOOK") && (*rx > 12000 || *rx < -12000) && !(on && world)) {  // (why the view did not turn)
            extern uint64_t g_world_frame_dbg(void);
            LOG("[fp] look refused at frame %llu: view %s, world on screen %d (composite last at %llu)\n", (unsigned long long)frame,
                on ? "on" : "off", world, (unsigned long long)g_world_frame_dbg());
        }
        {   // the mouse (and LARECOMP_FP_TEST_MOUSE="frame:dx:dy", a test: that much motion at that frame)
            static float sens = -1;
            if (sens < 0) sens = envf("LARECOMP_FP_MOUSE", 0.0025f);
            const char *t = ENV("LARECOMP_FP_TEST_MOUSE");
            unsigned long tf;
            float tx, ty;
            if (t && sscanf(t, "%lu:%f:%f", &tf, &tx, &ty) == 3 && tf == frame) fp_mouse_motion(tx, ty);
            // (a test: "frame:degrees[+frame:degrees...]", the view turned there - 0 north, 90 east)
            const char *ty_ = ENV("LARECOMP_FP_TEST_YAW");
            for (const char *q = ty_; q && *q; q = strchr(q, '+') ? strchr(q, '+') + 1 : NULL)
                if (sscanf(q, "%lu:%f", &tf, &tx) == 2 && tf == frame) g_yaw = tx * (float)M_PI / 180.0f;
            const char *tp_ = ENV("LARECOMP_FP_TEST_PITCH");  // (the same, up and down: 0 level, 60 up, -60 down)
            for (const char *q = tp_; q && *q; q = strchr(q, '+') ? strchr(q, '+') + 1 : NULL)
                if (sscanf(q, "%lu:%f", &tf, &tx) == 2 && tf == frame) g_pitch = tx * (float)M_PI / 180.0f;
            // (the trailer's camera moves, 28/10: "frame:yaw:pitch+..." in degrees, keys in frame order - the view eased
            //  from each key to the next, smootherstep, from the first key's frame to the last's; two keys a frame
            //  apart are a cut. Each yaw as written: 0 to 400 turns past north one way, 0 to -40 the other)
            static struct { unsigned long f; float yaw, pitch; } key[256];
            static int nkey = -1;
            if (nkey < 0) {
                nkey = 0;
                for (const char *q = ENV("LARECOMP_FP_CAM"); q && *q && nkey < 256; q = strchr(q, '+') ? strchr(q, '+') + 1 : NULL)
                    if (sscanf(q, "%lu:%f:%f", &key[nkey].f, &key[nkey].yaw, &key[nkey].pitch) == 3) nkey++;
            }
            for (int k = 0; k + 1 < nkey; k++)
                if (frame >= key[k].f && frame <= key[k + 1].f && key[k + 1].f > key[k].f) {
                    float u = (float)(frame - key[k].f) / (float)(key[k + 1].f - key[k].f), e = u * u * u * (u * (u * 6 - 15) + 10);
                    g_yaw = (key[k].yaw + (key[k + 1].yaw - key[k].yaw) * e) * (float)M_PI / 180.0f;
                    g_pitch = (key[k].pitch + (key[k + 1].pitch - key[k].pitch) * e) * (float)M_PI / 180.0f;
                    break;
                }
            int dx = atomic_exchange(&g_mdx, 0), dy = atomic_exchange(&g_mdy, 0);
            if (on) {
                float sp = (g_settings.fp_sens > 0 ? g_settings.fp_sens : 5) / 5.0f * sens;
                g_yaw += dx / 16.0f * sp;
                g_pitch = fmaxf(-1.4f, fminf(1.4f, g_pitch + (g_settings.fp_invert_y ? dy : -dy) / 16.0f * sp));
            }
        }
        {   // a play session recorded or replayed (runtime/hle/inputrec.c): the view and the mouse buttons this frame
            extern void inputrec_view(float *yaw, float *pitch, uint32_t *mbtn);
            uint32_t mb = atomic_load(&g_mbtn);
            inputrec_view(&g_yaw, &g_pitch, &mb);
            atomic_store(&g_mbtn, mb);
        }
        publish(on, pc, frame);
        fp_state_log(pc, frame, on);
    }
    if (!g_on) return false;
    extern bool world_on_screen(void);  // (runtime/hle/mirror.c: the pause menu and the map keep the stick as it is)
    if (!world_on_screen()) return false;
    float yaw = g_yaw + head_yaw();  // (where the view looks: the body's turn, and the head's on top in a headset)
    if (frame < g_turn_until) {  // an action is held back: Link turns to where the view looks first (fp_buttons)
        *lx = (int32_t)(sinf(yaw) * 12000), *ly = (int32_t)(cosf(yaw) * 12000);
        *rx = *ry = 0;
        return true;
    }
    float s = sinf(yaw), c = cosf(yaw), x = (float)*lx, y = (float)*ly;
    float wx = y * s + x * c, wy = y * c - x * s;  // stick up = forward, right = his right; back to screen axes
    *lx = (int32_t)fmaxf(-32767, fminf(32767, wx));
    *ly = (int32_t)fmaxf(-32767, fminf(32767, wy));
    *rx = *ry = 0;
    return true;
}

bool fp_input_on(void) { return g_on; }  // (the game thread: runtime/hle/qol.c forces 360 movement and no blur)

// The switch with R3 is a flight, not a cut (Guillaume, 20/10: "on plonge d'en haut directement dans le personnage, et
// inversement on s'envole au ciel"): over LARECOMP_FP_FLIGHT seconds (2) the camera goes from the game's, above and
// behind, down an arc into Link's eyes - keeping him in view, turning to where the view looks only on the last third -
// and back up the same way. Only for R3: a level loading, a side-view passage or a cutscene still cut. rebuild() draws
// the camera along the way; Link's body is left out, and his hands shown, only once the camera is in his head.
static _Atomic double g_fl_t0 = -100;  // when the last flight began (monotonic seconds)
static _Atomic int g_fl_to = 1;        // where to: 1 into the eyes, 0 up to the game's camera
static double fl_now(void) { struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts); return ts.tv_sec + ts.tv_nsec * 1e-9; }
static double fl_len(void) { static double l = -1; if (l < 0) l = envf("LARECOMP_FP_FLIGHT", 2.0f); return l; }
static bool fl_flying(void) { return fl_now() - g_fl_t0 < fl_len(); }
static void fl_begin(void) { g_fl_t0 = fl_now(); }
// 1 = first person, 0 = the game's camera, eased (smootherstep). One value per presented frame: read off the clock at
// each draw, it moved within a frame - the scene drawn from one place, its lighting rebuilt from the depth as seen from
// another: everything black under a green sky (20/10)
float fp_flight(void) {
    static _Atomic uint64_t at = ~0ull;
    static _Atomic float val = 1;
    uint64_t f = renderer_present_count();
    if (atomic_load(&at) == f) return atomic_load(&val);
    double t = (fl_now() - g_fl_t0) / fl_len();
    float v = 1;  // (no flight: the view is what fp_view_on says)
    if (t < 1 && t >= 0) {
        float e = (float)(t * t * t * (t * (t * 6 - 15) + 10));
        v = g_fl_to ? e : 1 - e;
    }
    atomic_store(&val, v), atomic_store(&at, f);
    return v;
}

static View view_now(void) {
    uint32_t gen = atomic_load(&g_pub_gen);
    if (t_pin && gen - (t_pin - 1) < NPUB - 2) gen = t_pin - 1;  // (pinned, and not about to be written over)
    return g_pub[gen % NPUB];
}
uint32_t fp_view_gen(void) { return atomic_load(&g_pub_gen); }  // (the game thread, queueing commands)
void fp_view_pin(uint32_t gen) { t_pin = gen + 1; }            // (the submission thread, translating them)

// Any thread: the view from his eyes wanted, not there for a moment (fp_input: android/xr_app.c shows black meanwhile)
bool fp_view_held(void) { return atomic_load(&g_view_held); }
// A flat screen's presenter, once a frame (runtime/nvn/vulkan/device.c): black while the view is wanted but not there
// (fp_view_held: a door, a level loading, the play starting) and the 12 frames after it is back - its first frames can
// still be the game's own camera, the room from above (the headset's rule, android/xr_app.c next_view; on a screen the
// room showed from above a moment at each door, Guillaume 27/10: « tous les correctifs du casque devront être appliqués
// sur le plat »). R3, the start screen, a side-view passage: the game's camera, as before.
bool fp_view_on(void);
bool fp_screen_black(void) {
    static uint64_t last = UINT64_MAX;
    static bool black, held_before;
    static int on_n;
    uint64_t f = renderer_present_count();
    if (f == last) return black;
    last = f;
    bool held = fp_view_held(), on = fp_view_on();
    on_n = on ? on_n + 1 : 0;
    if (held) held_before = true;
    else if (!on || on_n >= 12) held_before = false;
    black = held || (held_before && on);
    return black;
}
// What he holds hidden from a level change's request (a door: runtime/hle/chamber.c rt_level_request) until the view is
// on again in the new place, 10 s at most - drawn over the frame, the sword and shield stayed lit while the doorway
// went dark (« pour l'épée ça doit être instantané quand on rentre quelque part », Guillaume, 27/10)
void fp_level_requested(void) { atomic_store(&g_hide_from, renderer_present_count()); }
bool fp_held_hidden(void) {
    uint64_t f = atomic_load(&g_hide_from), now = renderer_present_count();
    return f != UINT64_MAX && now >= f && now - f < 600;
}
// Any thread: the view is on for what's being drawn now.
bool fp_view_on(void) {
    View v = view_now();
    uint64_t f = renderer_present_count();
    // (the game can skip polling the pad a few frames; in a headset 10 s - after a talk in a house it stopped reading
    //  player 1's pad for a while and the view went to the one from above unannounced, 27/10: input.c logs those stops)
    bool recent = f >= v.frame && f - v.frame <= (g_xr_serial ? 600u : 20u);
    return recent && (v.on || (!g_fl_to && fl_flying()));  // (flying up out of his eyes: still our camera)
}

// ------------------------------------------------------------------ culling (tools/patches.py)

// The game culls with its own camera, high above and behind Link: widened while the view is on.
// (LARECOMP_FP_CULL_FOV degrees, LARECOMP_FP_CULL_ASPECT: measured 20/10 the game has more of the island loaded than its
// view from above shows - culling, not loading, is what stops the first-person view)
float fp_cull_fovy(float orig) {
    if (!fp_view_on() || !(orig > 0)) return orig;
    static float fov = -1;
    if (fov < 0) fov = envf("LARECOMP_FP_CULL_FOV", 150);
    return orig > 3.2f ? fov : fov * (float)M_PI / 180.0f;  // (degrees or radians, as the field holds)
}
// The reach: how many of the cells it has loaded the game draws. It draws the cells its camera's view covers (measured
// 20/10 at Mabe Village: 13 of the 49 it keeps loaded with its own field of view, 24 with 45 degrees, 43 with 120), so in
// the first-person view the camera's field of view is widened - the game's own scenery further away, nothing invented.
// Each cell brings its draws (about 1,200 a frame at 45 degrees, 690 with the game's view) and the frame is serial: the
// widening follows the frame rate, as the dynamic resolution does - wider while frames hold 60, narrower when they are
// late and the GPU has room (late frames with a busy GPU are the resolution's to fix). LARECOMP_FP_CAM_FOV=<degrees> sets
// a fixed one; LARECOMP_FP_REACH_MAX (44: a try at 46 cost windows at 40 fps on the M1 Pro) bounds it, LARECOMP_FP_REACH_START (38).
#include <time.h>
extern const char *game_level_name(void);  // (runtime/hle/widescreen.c; "-" out of play)
static bool eye_culling_here(void);
static float g_reach = -1;  // degrees, 0 until the game's own is known
float fp_cam_fovy(float orig) {
    static float fixed = -1, top;
    if (fixed < 0) fixed = envf("LARECOMP_FP_CAM_FOV", 0), top = envf("LARECOMP_FP_REACH_MAX", 44);
    if (!fp_view_on() || !(orig > 0)) return orig;
    static int eye_culling = -1;
    if (eye_culling < 0) eye_culling = envf("LARECOMP_FP_PLANES", 2) == 2;
    if (eye_culling && !fixed && eye_culling_here()) return orig;  // (the eye's frustum chooses outdoors: fp_frustum_ready)
    bool deg = orig > 3.2f;
    float own = deg ? orig : orig * 180.0f / (float)M_PI;
    float fov = fixed;
    if (!fixed) {  // (a cell seen for the first time brings shader builds: one late window is not a verdict - two are;
                   //  a widening that fails within 4 s doubles the wait before the next, as the dynamic resolution does)
        static double last, sum, raised_at = -100;
        static int n, good, slow, need = 8;
        struct timespec ts;
        clock_gettime(CLOCK_MONOTONIC, &ts);
        double now = ts.tv_sec + ts.tv_nsec * 1e-9;
        if (g_reach < 0) g_reach = fmaxf(own, fminf(top, envf("LARECOMP_FP_REACH_START", 38)));
        if (g_reach < own) g_reach = own;
        if (last > 0 && now - last < 0.25) sum += now - last, n++;
        last = now;
        if (n >= 30) {
            extern double output_scale_dynamic_factor(void);
            double avg = sum / n * 1000;
            sum = 0, n = 0;
            float prev = g_reach;
            float late = envf("LARECOMP_FP_REACH_LATE_MS", 17.8f);
            slow = avg > late ? slow + 1 : 0;
            if (slow >= 2) {  // late twice: narrower
                g_reach = fmaxf(own, g_reach - (avg > 22 ? 6 : 3)), good = 0, slow = 0;
                if (now - raised_at < 4) need = need < 64 ? need * 2 : 64;
            } else if (!slow && ++good >= need && output_scale_dynamic_factor() >= 1.0) {
                g_reach = fminf(top, g_reach + 2), good = 0, raised_at = now;
            }
            if (g_reach != prev && ENV("LARECOMP_LOG_FP"))
                LOG("[fp] reach %.0f -> %.0f degrees (frame %.1f ms, the game's own %.0f)\n", prev, g_reach, avg, own);
        }
        fov = g_reach;
    }
    return deg ? fov : fov * (float)M_PI / 180.0f;
}
// ------------------------------------------------------------------ the far scenery (tools/patches.py FAR_QUERY_SITE)
// The game keeps 7 x 7 cells of the island loaded around Link (its tree queries, measured 27/10: 0x700085c6ec's box is
// that block, 0x700085c51c's 5 x 5 inside it) - all its camera from above ever shows. Through his eyes the island goes
// on, so the game is asked for ONE larger block of whole cells: centred on the cell Link is in, the same whichever way
// he looks, and moved only once he is half a cell past that cell. Everything loaded is drawn by the eye's frustum
// (fp_frustum_ready) and nothing is taken out of it piece by piece; what is not loaded is beyond the block's edge,
// whole cells at a time. (27/10, « il faut revoir tout ton système d'affichage de ce qui est au loin ... ça doit être
// cohérent », Guillaume. What it replaces: a box 80 units ahead of the body, which loaded far cells and none between
// them and the game's own - Mt Tamaranch's egg in the air without its mountain, cliffs missing whole parts, and a step
// forward or back, its edges crossing cells, brought parts back or took them away.)
// The island: 16 x 16 cells, Field_01A (x 0-15, z 0-12) to Field_16P, 15 units wide (x) and 12 deep (z).
// LARECOMP_FP_REGION=<rings> beyond Link's cell at most (6: 13 x 13 cells; 3 = the game's own 7 x 7, nothing more),
// LARECOMP_FP_REGION_START at the start (4); the frame controller (detail_tick) gives rings while frames hold and takes
// them off while they are late (measured 27/10 on the RX 7800 XT: about 3,900 draws a frame at 6 rings, 52-58 fps).
// LARECOMP_FP_LOG_FAR=1 logs the game's queries and each move of the block.
#define CELL_W 15.0f
#define CELL_D 12.0f
#define FIELD_CELLS 16
static int g_region = -1, g_region_max = -1;  // rings of cells loaded beyond Link's own
static int g_rc = -1, g_rr = -1;              // the block's centre cell (column, row)
static int region_max(void) {
    if (g_region_max < 0) g_region_max = (int)envf("LARECOMP_FP_REGION", 6);
    return g_region_max;
}
static int region_start(void) {  // LARECOMP_FP_REGION_START (4): rings at the start, more while frames hold (detail_tick)
    int s = (int)envf("LARECOMP_FP_REGION_START", 4);
    return s < region_max() ? s : region_max();
}
uint64_t rt_far_query(uint64_t lr, uint64_t query) {
    bool log = ENV("LARECOMP_FP_LOG_FAR") != NULL;
    if (log) {
        static uint64_t seen[8], said[8];
        static int n;
        int i = 0;
        while (i < n && seen[i] != lr) i++;
        if (i == n && n < 8) seen[n++] = lr;
        if (i < 8 && renderer_present_count() / 120 != said[i] / 120) {
            said[i] = renderer_present_count();
            const float *b = (const float *)(uintptr_t)query;
            LOG("[fp] tree query from %llx: box (%.1f %.1f %.1f) - (%.1f %.1f %.1f)\n", (unsigned long long)lr, b[0], b[1], b[2],
                b[4], b[5], b[6]);
        }
    }
    // (the query whose boxes make the game load cells, measured 20/10 - 0x700085c6ec's load nothing more; one box, the
    //  whole block: the game loads the cells it touches, and keeps them while it does)
    static uint64_t asker, *box;
    if (!asker) {
        const char *e = ENV("LARECOMP_FP_FAR_ASKER");
        asker = e ? strtoull(e, NULL, 16) : 0x700085c51cull;
    }
    if (lr != asker || !fp_view_on() || strcmp(game_level_name(), "Field")) return 0;
    if (g_region < 0) g_region = region_start();
    if (g_region <= 3) return 0;  // (the game's own block)
    View w = view_now();
    float cx = w.link[0] / CELL_W, cz = w.link[2] / CELL_D;
    if (g_rc < 0 || fabsf(cx - (g_rc + 0.5f)) > 1.0f || fabsf(cz - (g_rr + 0.5f)) > 1.0f) {
        g_rc = (int)floorf(cx), g_rr = (int)floorf(cz);
        if (log) LOG("[fp] far block: centre %02d%c, %d rings (Link %.1f %.1f)\n", g_rr + 1, 'A' + g_rc, g_region, w.link[0], w.link[2]);
    }
    int c0 = g_rc - g_region, c1 = g_rc + g_region, r0 = g_rr - g_region, r1 = g_rr + g_region;
    c0 = c0 < 0 ? 0 : c0, r0 = r0 < 0 ? 0 : r0, c1 = c1 >= FIELD_CELLS ? FIELD_CELLS - 1 : c1, r1 = r1 >= FIELD_CELLS ? FIELD_CELLS - 1 : r1;
    if (!box) box = hle_alloc(32);
    const float *g = (const float *)(uintptr_t)query;
    // (half a unit inside the block's edges: a box on a cell's border would touch the next cell too)
    float b[8] = {c0 * CELL_W + 0.5f, g[1], r0 * CELL_D + 0.5f, 0, (c1 + 1) * CELL_W - 0.5f, g[5], (r1 + 1) * CELL_D - 0.5f, 0};
    memcpy(box, b, sizeof b);
    return (uint64_t)(uintptr_t)box;
}

// The game hides the loaded cells - their models and their actors - that its own culling frustum misses: the camera from
// above's (x27 + 0x610 of the island's update, f_70008529b0), rebuilt only when that camera moves, looking down at Link
// from behind (tools/patches.py CELLS_UPDATE_SITE; f_700085c280 read by Gemini, 27/10). Through his eyes that hid cells
// in front of him: Mt Tamaranch's summit, loaded, showed only with Link next to it, and the egg on it stood in the air.
// In first person on the Field the update is handed the eye's frustum instead (fp_frustum_ready's, the frame before):
// the cells he sees are shown whole, with their actors. LARECOMP_FP_CELLS_FRUSTUM: 2 the eye's (default), 1 one taking
// everything (measured 27/10: every loaded cell's actors woken, 30 fps for seconds after a warp), 0 the game's own.
static uint64_t g_eye_frustum, g_eye_frustum_frame = UINT64_MAX;  // (a copy of the eye's, 0xe8 bytes, guest memory)
uint64_t rt_cells_frustum(uint64_t frustum) {
    static uint64_t all;
    static int mode = -1;
    if (mode < 0) mode = (int)envf("LARECOMP_FP_CELLS_FRUSTUM", 2);
    if (!mode || frustum < 0x7000000000ull || !fp_view_on() || strcmp(game_level_name(), "Field")) return 0;
    if (mode == 2) {
        uint64_t f = renderer_present_count();
        return g_eye_frustum && g_eye_frustum_frame != UINT64_MAX && f - g_eye_frustum_frame <= 2 ? g_eye_frustum : 0;
    }
    if (!all) all = (uint64_t)(uintptr_t)hle_alloc(0x100);
    memcpy((void *)(uintptr_t)all, (const void *)(uintptr_t)frustum, 0xe8);
    float *box = (float *)(uintptr_t)all, *pl = box + 8;  // (a world box, min +0 and max +0x10; planes from +0x20, 0x20
                                                         //  apart - normal, distance at +0x10; their count at +0xe0)
    for (int k = 0; k < 3; k++) box[k] = -1e6f, box[4 + k] = 1e6f;
    int n = *(const int32_t *)(uintptr_t)(all + 0xe0);
    for (int i = 0; i < n && i < 6; i++) pl[i * 8] = pl[i * 8 + 1] = pl[i * 8 + 2] = 0, pl[i * 8 + 4] = -1e9f;
    return all;
}

// The sky and sea past the north edge (map/SkySea.bfres, tools/patches.py SKYSEA_BOX_SITES): pieces 60 units wide, each
// drawn only while Link stands in its box (its centre +-27 deep). Mt Tamaranch's summit is one of them - the rocks and
// the clouds under the egg, the piece at x 90 - so from three rows off the egg stood in the air above nothing (measured
// 27/10: the summit's five models drawn with Link at z 22, gone at z 42). In first person on the Field a piece shows
// while the island's block of cells loaded (rt_far_query; the game's own 7 x 7 without it) reaches the north edge under
// it: the summit with the mountain's foot and the egg, all three or none. b: the bound loaded (max: 1, else min).
void fp_skysea_box(uint64_t piece, int max, float *b) {
    if (!fp_view_on() || strcmp(game_level_name(), "Field") || piece < 0x7000000000ull) return;
    View w = view_now();
    bool block = g_region > 3 && g_rc >= 0;
    int ring = block ? g_region : 3;
    int cc = block ? g_rc : (int)floorf(w.link[0] / CELL_W), cr = block ? g_rr : (int)floorf(w.link[2] / CELL_D);
    if (cr - ring > 0) return;  // (the block stops short of the north edge)
    const float *mn = (const float *)(uintptr_t)(piece + 0x2d0), *mx = (const float *)(uintptr_t)(piece + 0x2e0);
    float x = (mn[0] + mx[0]) / 2, x0 = (cc - ring) * CELL_W, x1 = (cc + ring + 1) * CELL_W;
    if (x + 30 < x0 || x - 30 > x1) return;  // (the piece is not over the block's columns)
    for (int k = 0; k < 3; k++) b[k] = max ? 1e6f : -1e6f;
}

float fp_cull_aspect_scale(void) {
    static float k = -1;
    if (k < 0) k = envf("LARECOMP_FP_CULL_ASPECT", 3);
    return fp_view_on() ? k : 1.0f;
}

// ------------------------------------------------------------------ the camera blocks (render thread)

static void mul4(const float *a, const float *b, float *o) {  // rows: o = a b
    for (int i = 0; i < 4; i++)
        for (int j = 0; j < 4; j++)
            o[i * 4 + j] = a[i * 4] * b[j] + a[i * 4 + 1] * b[4 + j] + a[i * 4 + 2] * b[8 + j] + a[i * 4 + 3] * b[12 + j];
}

static bool inv4(const float *m, float *o) {  // Gauss-Jordan with partial pivoting; false: singular
    double a[4][8];
    for (int i = 0; i < 4; i++)
        for (int j = 0; j < 8; j++) a[i][j] = j < 4 ? m[i * 4 + j] : (double)(j - 4 == i);
    for (int c = 0; c < 4; c++) {
        int p = c;
        for (int i = c + 1; i < 4; i++) if (fabs(a[i][c]) > fabs(a[p][c])) p = i;
        if (fabs(a[p][c]) < 1e-12) return false;
        if (p != c) for (int j = 0; j < 8; j++) { double t = a[c][j]; a[c][j] = a[p][j]; a[p][j] = t; }
        double d = a[c][c];
        for (int j = 0; j < 8; j++) a[c][j] /= d;
        for (int i = 0; i < 4; i++)
            if (i != c && a[i][c] != 0) { double f = a[i][c]; for (int j = 0; j < 8; j++) a[i][j] -= f * a[c][j]; }
    }
    for (int i = 0; i < 4; i++)
        for (int j = 0; j < 4; j++) o[i * 4 + j] = (float)a[i][j + 4];
    return true;
}

static bool near_eq(const float *a, const float *b, int n) {
    for (int i = 0; i < n; i++)
        if (!(fabsf(a[i] - b[i]) <= 1e-3f * (1 + fabsf(b[i])))) return false;
    return true;
}

static void view_matrix(const View *w, float *v) {
    float cp = cosf(w->pitch), f[3] = {sinf(w->yaw) * cp, sinf(w->pitch), -cosf(w->yaw) * cp};
    float r[3] = {cosf(w->yaw), 0, sinf(w->yaw)};
    float u[3] = {-r[2] * f[1], r[2] * f[0] - r[0] * f[2], r[0] * f[1]};  // r x f
    const float *e = w->eye;
    float m[16] = {r[0], r[1], r[2], -(r[0] * e[0] + r[1] * e[1] + r[2] * e[2]),
                   u[0], u[1], u[2], -(u[0] * e[0] + u[1] * e[1] + u[2] * e[2]),
                   -f[0], -f[1], -f[2], f[0] * e[0] + f[1] * e[1] + f[2] * e[2],
                   0, 0, 0, 1};
    memcpy(v, m, sizeof m);
}

// ------------------------------------------------------------------ a headset's views (VR: OpenXR)
// An OpenXR view in place of the look and the lens: the eye's orientation in the headset's space (quaternion x, y, z, w;
// x right, y up, -z ahead, as ours), its position from the head's centre in metres, and its field's four tangents (left,
// right, up, down; left and down negative: a headset's fields are asymmetric). The body still turns with the stick
// (yaw); the head does the rest, and the stick's pitch is left out. LARECOMP_FP_XR_UNIT: metres per game unit (2.5:
// Link's eye, 0.62 above his feet, stands for a person's 1.55 m). Nothing calls fp_xr_view yet: the Quest's frame loop
// will, once per eye, on the render thread (A-FAIRE/lasrecomp-portage-3d-feuille-de-route.md, point 19).
// LARECOMP_FP_XR_TEST=<ipd in metres>[,<roll in degrees>[,<pitch in degrees>[,1]]]: on the Mac, the eyes in turn, one
// per frame, with a Quest-like pair of asymmetric fields - two consecutive frames side by side are a stereo pair; a
// last 1: the Quest app's single view over both eyes (tangents -1.38 1.38 0.97 -1.43).
typedef struct { float q[4], pos[3], tan[4]; } XrView;
static XrView g_xr;
// Metres per game unit in a headset: LARECOMP_FP_XR_UNIT (2.5) times the world's size setting (Gameplay tab, "World size
// (headset)", fp_world percent) - at 80 the eyes are 1.25 times further apart in the island and the head moves 1.25
// times as far: everything looks smaller and further off (« tout semble grand et collé à nous », Guillaume 27/10).
// The eye's shift in the stereo rebuilding (android/xr_app.c) and what he holds (viewmodel) read it too.
float fp_xr_unit(void) {
    static float base = -1;
    if (base < 0) base = envf("LARECOMP_FP_XR_UNIT", 2.5f);
    int w = g_settings.fp_world >= 50 && g_settings.fp_world <= 200 ? g_settings.fp_world : 100;
    return base * (float)w / 100.0f;
}
// The head's place from where it was when the view began (LARECOMP_FP_XR_NO_RECENTER=1: as the runtime gives it). The
// runtime's own origin is its seated position, wherever that was set: half a metre from it, his eyes were 0.2 units
// to his side - facing a door in the view, he pushed against the wall beside it (Guillaume, PC VR, 27/10). Again after
// the view was off (fp_xr_off: the headset taken off, the session left) or the head 1.5 m from it (a new place).
static float g_xr_p0[3];
static bool g_xr_have0;
void fp_xr_view(const float *q, const float *pos, const float *tan) {
    static int keep = -1;
    if (keep < 0) keep = ENV("LARECOMP_FP_XR_NO_RECENTER") != NULL;
    float p[3] = {pos[0], pos[1], pos[2]};
    static float last[3];
    if (!keep && (pos[0] || pos[1] || pos[2])) {
        // a jump of 8 cm or more from one view to the next is the runtime recentring (no event from SteamVR when the
        // player holds the Meta button: measured 27/10), not the head: absorbed, his eyes stay at the head
        float jx = pos[0] - last[0], jy = pos[1] - last[1], jz = pos[2] - last[2];
        if (g_xr_have0 && jx * jx + jy * jy + jz * jz > 0.08f * 0.08f)
            g_xr_p0[0] += jx, g_xr_p0[1] += jy, g_xr_p0[2] += jz, LOG("[fp] headset: recentred (a %.2f m jump)\n", sqrtf(jx * jx + jy * jy + jz * jz));
        memcpy(last, pos, 12);
        float dx = pos[0] - g_xr_p0[0], dy = pos[1] - g_xr_p0[1], dz = pos[2] - g_xr_p0[2];
        if (!g_xr_have0 || dx * dx + dy * dy + dz * dz > 1.5f * 1.5f) {
            memcpy(g_xr_p0, pos, 12), g_xr_have0 = true;
            LOG("[fp] headset: the head's place taken as his eyes' (%.2f, %.2f, %.2f m from the runtime's origin)\n", pos[0], pos[1], pos[2]);
        }
        for (int i = 0; i < 3; i++) p[i] -= g_xr_p0[i];
    }
    memcpy(g_xr.q, q, 16), memcpy(g_xr.pos, p, 12), memcpy(g_xr.tan, tan, 16);
    if (!++g_xr_serial) g_xr_serial = 1;
}
void fp_xr_off(void) { g_xr_serial = 0, g_xr_have0 = false; tilt_reset(); }
static void xr_head_q(float *q) { memcpy(q, g_xr.q, 16); }
void fp_xr_recenter(void) { g_xr_have0 = false; tilt_reset(); }  // (the runtime recentred: android/xr_app.c)
// The head view's field (tangents left, right, up, down), while the view follows the head (the Vulkan backend's
// foveation: runtime/nvn/vulkan/exec.c vrs_*).
bool fp_xr_tangents(float *tan) {
    if (!g_xr_serial) return false;
    memcpy(tan, g_xr.tan, 16);
    return true;
}

static void quat_basis(const float *q, float *m);
static const XrView *xr_now(uint32_t *serial);

// The stick's turn at the display's rate (a headset: android/xr_app.c). The game makes 30 images a second; the frame
// loop shows each of them four times at 120 Hz, the compositor turning it with the head - and the body, turned by the
// stick, moved only from one image to the next: the view turned in 30 jerks a second (Guillaume's headset, 21/10).
// Each refresh, the image is given the pose it would have had with the body's turn of the moment: the compositor turns
// it by the difference, the stick at the display's rate. fp_xr_frame_look: the turn of the view the image is drawn from
// (the submission thread, its view pinned: at the frame's end). fp_xr_turn: the turn at t, from the views published
// (interpolated between two; past the latest, it and the stick's turn since, 50 ms at most); false when the image is
// not to be turned - no view, the view off, or a turn the stick cannot make (a door, R3: a jump, not a turn).
static float wrap_pi(float a) {
    while (a > (float)M_PI) a -= 2 * (float)M_PI;
    while (a < -(float)M_PI) a += 2 * (float)M_PI;
    return a;
}
bool fp_xr_frame_look(float *yaw, float *pitch, int64_t *ns) {
    View v = view_now();
    if (!v.on || !v.ns || !g_xr_serial) return false;
    *yaw = v.yaw, *pitch = v.pitch, *ns = v.ns;
    return true;
}
bool fp_xr_frame_eye(float *eye) {  // (the same view's eye in the world, game units: AppSW, xr_app.c)
    View v = view_now();
    if (!v.on || !v.ns || !g_xr_serial) return false;
    memcpy(eye, v.eye, sizeof v.eye);
    return true;
}
bool fp_xr_turn(float yaw0, float pitch0, int64_t ns0, int64_t t, float *yaw, float *pitch) {
    float most = 3.0f * (g_settings.fp_sens > 0 ? g_settings.fp_sens : 5) / 5.0f * 1.25f;  // (the stick's fastest, and a margin)
    uint32_t gen = atomic_load(&g_pub_gen);
    View b = g_pub[gen % NPUB];
    if (!b.on || !b.ns) return false;
    float y, p;
    if (t >= b.ns) {
        float dt = fminf((float)(t - b.ns) * 1e-9f, 0.05f);
        y = b.yaw + b.ryaw * dt, p = fmaxf(-1.4f, fminf(1.4f, b.pitch + b.rpitch * dt));
    } else {
        uint32_t k = 1;
        for (; k < NPUB - 2; k++) {  // (the entry about to be written over left alone)
            View a = g_pub[(gen - k) % NPUB];
            if (!a.on || !a.ns || a.ns >= b.ns) return false;
            if (t >= a.ns) {
                float f = (float)(t - a.ns) / (float)(b.ns - a.ns), dy = wrap_pi(b.yaw - a.yaw), dp = b.pitch - a.pitch;
                float lim = most * (float)(b.ns - a.ns) * 1e-9f + 0.01f;
                if (fabsf(dy) > lim || fabsf(dp) > lim) f = 0;  // (a jump between them: the view before it)
                y = a.yaw + dy * f, p = a.pitch + dp * f;
                break;
            }
            b = a;
        }
        if (k == NPUB - 2) return false;
    }
    *yaw = wrap_pi(y), *pitch = p;
    float lim = most * fabsf((float)(t - ns0)) * 1e-9f + 0.01f;
    return fabsf(wrap_pi(y - yaw0)) <= lim && fabsf(p - pitch0) <= lim;
}
// The head's turn about the vertical from the body's (a headset; 0 without). The stick moves Link where the view looks:
// turned by the body's yaw alone, it moved him sideways or backwards once he had turned round in his chair - "the stick
// to the right makes me go back" (Guillaume, 21/10, in the headset).
static float head_yaw(void) {
    uint32_t xs;
    const XrView *x = xr_now(&xs);
    if (!x) return 0;
    float q[4], m[9];
    memcpy(q, x->q, sizeof q);
    quat_basis(q, m);
    float f0[3] = {-m[2], -m[5], -m[8]};  // (the head's forward, -z ...)
    float fx = g_tilt[0] * f0[0] + g_tilt[1] * f0[1] + g_tilt[2] * f0[2];  // (... tilted by the stick: where the view looks)
    float fz = g_tilt[6] * f0[0] + g_tilt[7] * f0[1] + g_tilt[8] * f0[2];
    if (fx * fx + fz * fz < 1e-4f) return 0;  // (straight up or down: no heading)
    return atan2f(fx, -fz);
}

static void quat_basis(const float *q, float *m) {  // columns: where the head's x, y, z axes point
    float x = q[0], y = q[1], z = q[2], w = q[3];
    float r[9] = {1 - 2 * (y * y + z * z), 2 * (x * y - z * w), 2 * (x * z + y * w),
                  2 * (x * y + z * w), 1 - 2 * (x * x + z * z), 2 * (y * z - x * w),
                  2 * (x * z - y * w), 2 * (y * z + x * w), 1 - 2 * (x * x + y * y)};
    memcpy(m, r, sizeof r);  // (rows of the rotation matrix: m[r * 3 + c])
}

static const XrView *xr_now(uint32_t *serial) {
    static int test = -1;
    static float ipd, roll, pitch, wide;
    if (test < 0) {
        const char *e = getenv("LARECOMP_FP_XR_TEST");
        test = e && sscanf(e, "%f,%f,%f,%f", &ipd, &roll, &pitch, &wide) >= 1;
    }
    if (test) {
        static XrView t;
        uint64_t f = renderer_present_count();
        bool right = f & 1;
        float a = roll * (float)M_PI / 360.0f, b = pitch * (float)M_PI / 360.0f, m[9];
        // (about x a pitch - negative looks down - then about z a roll: qx * qz)
        t.q[0] = sinf(b) * cosf(a), t.q[1] = sinf(b) * sinf(a), t.q[2] = cosf(b) * sinf(a), t.q[3] = cosf(b) * cosf(a);
        quat_basis(t.q, m);
        float h = right ? ipd / 2 : -ipd / 2;
        t.pos[0] = m[0] * h, t.pos[1] = m[3] * h, t.pos[2] = m[6] * h;  // (along the rolled head's x)
        static const float tl[4] = {-1.0f, 0.6f, 0.5f, -0.4f}, tr[4] = {-0.6f, 1.0f, 0.5f, -0.4f};  // (16:9 each, the
        static const float tw[4] = {-1.38f, 1.38f, 0.97f, -1.43f};  // (wide: both Quest 3 eyes together, the app's view)
        memcpy(t.tan, wide ? tw : right ? tr : tl, 16);                                               //  nose side narrower)
        *serial = (uint32_t)(f % 0x7fffffff) * 2 + 1;
        return &t;
    }
    *serial = g_xr_serial;
    return g_xr_serial ? &g_xr : NULL;
}

// The view of an eye: the body's turn (yaw) times the head's orientation, at the eye's place in the world.
// The body: turned by the right stick about the world's up, and tilted by its forward / back - the head's own
// orientation on top. Lying down, the stick lifts or lowers the view without bending the neck (Guillaume, 21/10).
// The tilt is T (look: about the head's right axis when the stick was pushed, then kept as it is): B = Y(yaw) X(pitch) T
// (pitch: 0 in a headset). The head stays free: nothing here follows it.
static void body_basis(const View *w, float *B) {
    float cp = cosf(w->pitch), sp = sinf(w->pitch), cy = cosf(w->yaw), sy = sinf(w->yaw);
    float YX[9] = {cy, -sy * sp, -sy * cp, 0, cp, -sp, sy, cy * sp, cy * cp};  // (rows; columns: right, up, back)
    static const float id[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1};
    const float *T = w->tilt[0] + w->tilt[4] + w->tilt[8] != 0 ? w->tilt : id;  // (a view published before any tilt: level)
    for (int r = 0; r < 3; r++)
        for (int c = 0; c < 3; c++) B[r * 3 + c] = YX[r * 3] * T[c] + YX[r * 3 + 1] * T[3 + c] + YX[r * 3 + 2] * T[6 + c];
}
// Where the view looks, as the view matrix has it: in a headset the head's forward through the body (B h). From the
// body's turn alone, a head turned south looked for room to the south - the fence he had walked up to, in front of his
// eyes - and the view from behind went to his eyes (Mabe, PC VR, 27/10).
static void look_dir(const View *w, float *f) {
    float cp = cosf(w->pitch);
    f[0] = sinf(w->yaw) * cp, f[1] = sinf(w->pitch), f[2] = -cosf(w->yaw) * cp;
    uint32_t xs;
    const XrView *x = xr_now(&xs);
    if (!x) return;
    float B[9], h[9];
    body_basis(w, B);
    quat_basis(x->q, h);
    for (int r = 0; r < 3; r++) f[r] = -(B[r * 3] * h[2] + B[r * 3 + 1] * h[5] + B[r * 3 + 2] * h[8]);
}
static void view_matrix_xr(const View *w, const XrView *x, float *v, float *eye) {
    const float unit = fp_xr_unit();
    float h[9], B[9];
    quat_basis(x->q, h);
    body_basis(w, B);
    float M[9];  // world from head: B h
    for (int r = 0; r < 3; r++)
        for (int c = 0; c < 3; c++) M[r * 3 + c] = B[r * 3] * h[c] + B[r * 3 + 1] * h[3 + c] + B[r * 3 + 2] * h[6 + c];
    for (int r = 0; r < 3; r++)
        eye[r] = w->eye[r] + (B[r * 3] * x->pos[0] + B[r * 3 + 1] * x->pos[1] + B[r * 3 + 2] * x->pos[2]) / unit;
    float m[16] = {M[0], M[3], M[6], 0, M[1], M[4], M[7], 0, M[2], M[5], M[8], 0, 0, 0, 0, 1};  // (rows: the head's axes)
    for (int r = 0; r < 3; r++) m[r * 4 + 3] = -(m[r * 4] * eye[0] + m[r * 4 + 1] * eye[1] + m[r * 4 + 2] * eye[2]);
    memcpy(v, m, sizeof m);
}

static float g_overscan = 1;  // (the presenter's: how much wider than the screen the view is drawn)
void fp_warp_overscan_set(float k) { g_overscan = k > 1 ? k : 1; }

// The game's projection rebuilt with our field of view and near / far planes, in its own depth convention (NVN depth
// mode 0: -1..1, else 0..1); false for a form we don't know (then nothing is rewritten).
static bool proj_matrix(const float *gp, float *p, const float *tan, float ovs) {
    bool zero_one = nvn_depth_mode() != 0;
    float a = gp[10], b = gp[11];
    float n0 = zero_one ? b / a : b / (a - 1), f0 = b / (a + 1);
    if (!(n0 > 0 && f0 > n0) || gp[14] != -1 || gp[0] == 0) return false;
    static float n = -1, far;
    // (near 0.1, not 0.05: depth keeps twice the precision far off - flat layers fought there, « z-flipping », 20/10)
    if (n < 0) n = envf("LARECOMP_FP_NEAR", 0.1f), far = envf("LARECOMP_FP_FAR", 400);
    float fov = envf("LARECOMP_FP_FOV", g_settings.fp_fov > 0 ? (float)g_settings.fp_fov : 60) * (float)M_PI / 180.0f;
    float f = fmaxf(f0, far), t = 1.0f / (tanf(fov / 2) * ovs), aspect = gp[5] / gp[0];  // (the game's own, widened, aspect)
    memset(p, 0, 64);
    p[0] = t / aspect, p[2] = gp[2];
    p[5] = t, p[6] = gp[6];  // (any off-centre / jitter terms kept)
    if (tan) {  // a headset's eye: its four tangents (ndc x = p0 X - p2 for X = x / -z: p0 tR - p2 = 1, p0 tL - p2 = -1)
        p[0] = 2 / (tan[1] - tan[0]), p[2] = (tan[1] + tan[0]) / (tan[1] - tan[0]);
        p[5] = 2 / (tan[2] - tan[3]), p[6] = (tan[2] + tan[3]) / (tan[2] - tan[3]);
    }
    p[10] = zero_one ? -f / (f - n) : -(f + n) / (f - n);
    p[11] = zero_one ? -f * n / (f - n) : -2 * f * n / (f - n);
    p[14] = -1;
    return true;
}

// The game camera's values that its blocks carry copies of (P_*), and ours for each.
// Measured 20/10 in the 1024-byte view block: the inverse view (3x4), the inverse projection and the inverse
// view-projection (pixel positions rebuilt from depth: lights, fog) and near / far / vertical fov / aspect.
enum { P_VP, P_V, P_V12, P_P, P_ROT, P_IV12, P_IP, P_IVP, P_EYE, P_BACK, P_DEPTH, P_LENS, P_COUNT };
static const int k_len[P_COUNT] = {16, 16, 12, 16, 16, 12, 16, 16, 3, 3, 4, 4};
static const char *const k_name[P_COUNT] = {"view-projection", "view", "view 3x4", "projection", "inverse rotation",
                                            "inverse view 3x4", "inverse projection", "inverse view-projection",
                                            "position", "back", "depth range", "lens"};
static bool small_kind(int k) { return k == P_EYE || k == P_BACK || k == P_DEPTH || k == P_LENS; }
typedef struct {
    float game[48];       // the game's view, projection, view-projection this was made from
    float eye[3], yaw, pitch, fov;  // our view it was made for
    float flight;                   // (fp_flight when made: the camera on its way)
    float jitter[2];                // SGSR2 prototype, UV units in the unflipped game image
    float ovs;                      // the overscan it was made with (fp_warp_overscan_set; 1: none)
    bool ok;
    uint32_t gen;         // bumped at each rebuild
    uint32_t xr;          // the headset view it was made for (xr_now's serial; 0: none)
    float from[P_COUNT][16], to[P_COUNT][16];
} Cam;
// The game's recent cameras, most recent first, learnt from the blocks that define one (camera_of). A draw's block can
// be a copy made a frame before or after the game camera moved: matched against the last camera only, those draws kept
// the game's view, and the view flickered to the one from above whenever Link walked or a dialogue framed him
// (Guillaume, 20/10).
#define NCAM 4
static Cam g_cams[NCAM];
static int g_ncam;
static uint32_t g_epoch;  // bumped whenever a camera's replacements change (the memo below then starts over)
#define g_c g_cams[0]

static bool perspective(const float *u) {
    return u[12] == 0 && u[13] == 0 && u[14] == 0 && u[15] == 1 && u[30] == -1 && u[31] == 0;
}

// near, far, near x far, far - near of a projection (how the scene block keeps its depth range)
static void depth_range(const float *p, float *d) {
    bool zero_one = nvn_depth_mode() != 0;
    float a = p[10], b = p[11], n = zero_one ? b / a : b / (a - 1), f = b / (a + 1);
    d[0] = n, d[1] = f, d[2] = n * f, d[3] = f - n;
}

// The camera on its flight (fp_flight: e = 0 the game's, 1 Link's eye): an orbit around Link, coming down. Around him
// it turns from where the game's camera stands to behind his head - the short way round - while it closes in and comes
// down to his eyes' height; then into his head through the back of it, looking where he looks: a spiral, never right
// above him (a curve over his head spun the look round at once - Guillaume, 20/10: "plus smooth"). It looks at his
// head all along (the game's camera looks at him already) and takes the view's own look as it comes in. Flying up is the
// same way back. v, p, eye: ours in, the flight's out. g_fl_dist: how far from his eyes the camera is (fp_draw shows his
// body until it is inside his head).
static float g_fl_dist = 1e9f;
static float sstep(float a, float b, float x) { float t = fminf(1, fmaxf(0, (x - a) / (b - a))); return t * t * (3 - 2 * t); }
static void norm3(float *a) { float l = sqrtf(a[0] * a[0] + a[1] * a[1] + a[2] * a[2]); if (l > 1e-6f) a[0] /= l, a[1] /= l, a[2] /= l; }
static void flight_camera(float e, const float *gv, const float *gp, const View *w, float *v, float *p, float *eye) {
    float giv[16], iv[16];
    if (!inv4(gv, giv) || !inv4(v, iv)) return;
    float g0[3] = {giv[3], giv[7], giv[11]}, e1[3] = {iv[3], iv[7], iv[11]};
    float fg[3] = {-giv[2], -giv[6], -giv[10]}, ff[3] = {-iv[2], -iv[6], -iv[10]}, uf[3] = {iv[1], iv[5], iv[9]};
    const float *L = w->link;
    float head[3] = {L[0], e1[1] - 0.15f, L[2]};  // (his head: the eye's height, at his middle)
    // where the game's camera stands around him, and behind his head along the view
    float gx = g0[0] - L[0], gz = g0[2] - L[2];
    float r0 = sqrtf(gx * gx + gz * gz), a0 = atan2f(gx, gz), h0 = g0[1];
    float a1 = atan2f(-ff[0], -ff[2]);  // (behind: against the look)
    float da = a1 - a0;
    while (da > (float)M_PI) da -= 2 * (float)M_PI;
    while (da < -(float)M_PI) da += 2 * (float)M_PI;
    float ea = sstep(0.05f, 0.8f, e), er = sstep(0.0f, 0.92f, e), eh = sstep(0.0f, 0.85f, e);
    float a = a0 + da * ea, r = r0 * (1 - er) + 0.6f * sinf((float)M_PI * er) * (r0 > 1 ? 1 : 0), h = h0 + (e1[1] - h0) * eh;
    float pos[3] = {L[0] + sinf(a) * r, h, L[2] + cosf(a) * r};
    float s3 = sstep(0.9f, 1.0f, e);  // (the last of it: into his eyes, a little ahead of his head's middle)
    for (int k = 0; k < 3; k++) pos[k] += (e1[k] - pos[k]) * s3;
    float look[3] = {head[0] - pos[0], head[1] - pos[1], head[2] - pos[2]};
    float dl = sqrtf(look[0] * look[0] + look[1] * look[1] + look[2] * look[2]);
    norm3(look);
    float s1 = sstep(0.0f, 0.12f, e), s2 = sstep(0.78f, 1.0f, e) + (dl < 0.5f ? 1 : 0), f[3], up[3], rr[3], u[3];
    if (s2 > 1) s2 = 1;
    for (int k = 0; k < 3; k++) {
        f[k] = (fg[k] * (1 - s1) + look[k] * s1) * (1 - s2) + ff[k] * s2;
        up[k] = (k == 1 ? 1.0f : 0.0f) * (1 - s2) + uf[k] * s2;
    }
    norm3(f), norm3(up);
    rr[0] = f[1] * up[2] - f[2] * up[1], rr[1] = f[2] * up[0] - f[0] * up[2], rr[2] = f[0] * up[1] - f[1] * up[0];
    norm3(rr);
    u[0] = rr[1] * f[2] - rr[2] * f[1], u[1] = rr[2] * f[0] - rr[0] * f[2], u[2] = rr[0] * f[1] - rr[1] * f[0];
    float m[16] = {rr[0], rr[1], rr[2], -(rr[0] * pos[0] + rr[1] * pos[1] + rr[2] * pos[2]),
                   u[0], u[1], u[2], -(u[0] * pos[0] + u[1] * pos[1] + u[2] * pos[2]),
                   -f[0], -f[1], -f[2], f[0] * pos[0] + f[1] * pos[1] + f[2] * pos[2],
                   0, 0, 0, 1};
    memcpy(v, m, sizeof m);
    float el = sstep(0.0f, 1.0f, e);
    for (int i = 0; i < 16; i++) p[i] = gp[i] + (p[i] - gp[i]) * el;  // (the lens: the game's field to ours)
    memcpy(eye, pos, 12);
    g_fl_dist = sqrtf((pos[0] - e1[0]) * (pos[0] - e1[0]) + (pos[1] - e1[1]) * (pos[1] - e1[1]) + (pos[2] - e1[2]) * (pos[2] - e1[2]));
}

// The view from behind (g_settings.fp_behind, Guillaume 21/10: "une troisieme vue ou on voit Link juste de derriere un
// peu au dessus"): the first-person view's camera moved back along its look and up - same turning, same controls - and
// Link's body drawn (fp_draw: as on the R3 flight, which then ends there). Around his middle at the eyes' height (the
// eyes stand ahead of his head: from them he was off to one side whenever he faced away from the look), 2.4 m back and
// 0.7 m up outside, 1.8 m and 0.56 m in a house or a dungeon, whose walls stand close (LARECOMP_FP_BEHIND=<back>,<up>).
bool fp_behind_on(void) { return g_settings.fp_behind; }
// The switch is a move: out of his eyes back to behind him, and back in - over 0.4 and 0.25 s (behind_probe's way to
// his eyes). It was a jump (« les autres versions ont une animation où on sort ou rentre dans la tête ou se décale
// vers l'arrière, qui fait plus pro », Guillaume, 27/10).
static bool g_behind_enter, g_behind_leave;
void fp_behind_toggle(void) {
    bool to = g_behind_leave ? true : !g_settings.fp_behind;
    if (to) g_settings.fp_behind = true, g_behind_enter = !g_behind_leave, g_behind_leave = false;
    else g_behind_leave = true;  // (off once at his eyes: behind_probe, which saves it then)
    extern void settings_save(void);
    if (to) settings_save();  // (saved on the way out, it kept "behind": the next start was behind him)
    extern void menu_notice(const char *text, double seconds);
    menu_notice(to ? "View from behind Link" : "View from Link's eyes", 2.0);
    LOG("[fp] view from %s\n", to ? "behind" : "the eyes");
}
static void behind_saved(void) { extern void settings_save(void); settings_save(); }  // (back at his eyes)
// How far back it may stand, the walls behind him allowing (behind_probe, the game thread; 1e9: all the way)
static _Atomic float g_behind_reach = 1e9f;
static _Atomic float g_behind_k = 1;  // (1: behind him; 0: at his eyes - a wall right behind him; between: on the way)
// where the camera stands, f the look (also the eye frustum's apex for the culling: fp_frustum_ready), at most reach
// from his middle, k of the way from his eyes
static float behind_at(const float *f, const float *eye, const float *link, float *pos, float reach, float k_eyes) {
    // how far back and how high: the menu's (g_settings.fp_behind_dist / fp_behind_up, ours by default: 2.4 and 0.7 -
    // Guillaume 13/11: « chacun fait sa sauce »), LARECOMP_FP_BEHIND=<back>,<up> over them
    float back = g_settings.fp_behind_dist > 0 ? g_settings.fp_behind_dist / 100.0f : 2.4f, up = g_settings.fp_behind_up / 100.0f;
    { const char *e = getenv("LARECOMP_FP_BEHIND"); if (e) sscanf(e, "%f,%f", &back, &up); }
    bool field = !strcmp(game_level_name(), "Field");
    float d = field ? back : back * 0.75f, h = field ? up : up * 0.8f;  // (indoors at half, his cap filled the view)
    float c[3] = {link[0], eye[1] - 0.15f, link[2]};  // (his middle, at the eyes' height)
    float full = sqrtf(d * d + h * h), k = reach < full ? fmaxf(reach, 0) / full : 1;
    d *= k, h *= k;
    pos[0] = c[0] - f[0] * d, pos[1] = c[1] - f[1] * d + h, pos[2] = c[2] - f[2] * d;
    // a wall right behind him: the camera at his eyes (behind_probe) - his head filled the view, then the inside of his
    // face (Tail Cave, 26/10); how far from them it is (fp_draw: his body seen beyond 0.45)
    float e = sstep(0.0f, 1.0f, k_eyes);
    for (int i = 0; i < 3; i++) pos[i] = eye[i] + (pos[i] - eye[i]) * e;
    float ex = pos[0] - eye[0], ey = pos[1] - eye[1], ez = pos[2] - eye[2];
    return sqrtf(ex * ex + ey * ey + ez * ez);
}
static float behind_pos(const float *f, const float *eye, const float *link, float *pos) {
    return behind_at(f, eye, link, pos, atomic_load(&g_behind_reach), atomic_load(&g_behind_k));
}

// The game's ray (GA(RAY_CAST), as the hookshot casts it: its result and filter laid out as it lays them out, under the
// game thread's stack): from a to b, true and where if it met something solid. The game thread only (its registers
// copied, left as they were).
static bool game_ray(const float *a, const float *b, float *hit, uint32_t *attr) {
    Cpu *cur = kernel_current_cpu();
    if (!cur || !GA(RAY_CAST)) return false;  // (a version whose ray isn't found yet: 1.0.1 / 1.1.0, nothing met)
    static Cpu c;
    c = *cur;
    uint64_t s = (c.sp - 0x800) & ~15ull, F = s + 0x40, R = s + 0x60, A = s + 0x140, B = s + 0x150;
    memset((void *)(uintptr_t)s, 0, 0x180);
    ST64(F, GA(RAY_FILTER_VT)), ST64(F + 0x10, F + 0x18);
    float rad = 0.22f;  // (the hookshot's)
    memcpy((void *)(uintptr_t)(F + 0x1c), &rad, 4);
    ST64(R, GA(RAY_RESULT_VT)), ST64(R + 0x48, R + 0x58), ST64(R + 0x50, GA(RAY_RESULT_HIT_VT)), ST64(R + 0xa8, R + 0x50);
    ST32(R + 0x68, 0xffffffffu), ST32(R + 0x88, 0x7f7fffffu);  // (no attribute yet; the nearest so far: FLT_MAX)
    memcpy((void *)(uintptr_t)A, a, 12), memcpy((void *)(uintptr_t)B, b, 12);
    c.sp = s;
    c.x[0] = R, c.x[1] = F, c.x[2] = A, c.x[3] = B, c.x[4] = GA(RAY_HOOKSHOT_MASK);
    guest_call(&c, GA(RAY_CAST));
    uint64_t rec = LD64(R + 0x48);
    float t;
    memcpy(&t, (const void *)(uintptr_t)(rec + 0x30), 4);
    bool met = (LD8(R + 0x40) & 1) && rec && t > 0;
    if (met) memcpy(hit, (const void *)(uintptr_t)(rec + 0x18), 12), *attr = LD32(rec + 0x10);
    uint64_t ctrl = LD64(R + 0x38);  // (what it met, held: let go, as the hookshot does)
    if (ctrl) c.x[0] = ctrl, guest_call(&c, GA(SP_RELEASE));
    return met;
}

// The view from behind kept on his side of the walls (« la caméra passe derrière les murs dans la vue au-dessus de
// Link quand on pivote vers un mur », Guillaume, 26/10): the game's ray from his middle to where the camera would
// stand, LARECOMP_FP_BEHIND_GAP (0.3) further; the camera then stands that much short of what it met - closer at once,
// back out over about half a second. LARECOMP_FP_NO_BEHIND_RAY=1: off; LARECOMP_LOG_BEHIND_RAY=1: what it meets.
static void behind_probe(void) {
    static float gap = -1;
    static int off = -1, logs;
    if (off < 0) off = ENV("LARECOMP_FP_NO_BEHIND_RAY") ? 1 : 0, gap = envf("LARECOMP_FP_BEHIND_GAP", 0.3f), logs = ENV("LARECOMP_LOG_BEHIND_RAY") ? 0 : -1;
    static bool eyes;
    static float k = 1;
    static double out_since, kt;
    if (off || !g_settings.fp_behind || !fp_view_on()) {
        eyes = false, k = 1, kt = 0;
        if (g_behind_leave) g_settings.fp_behind = false, g_behind_leave = false, behind_saved();
        atomic_store(&g_behind_reach, 1e9f), atomic_store(&g_behind_k, 1.0f);
        return;
    }
    if (g_behind_enter) k = 0, eyes = false, g_behind_enter = false;  // (from his eyes, back)
    View w = view_now();
    float f[3], pos[3];
    look_dir(&w, f);  // (the camera's own: behind_offset takes it from the view matrix)
    behind_at(f, w.eye, w.link, pos, 1e9f, 1);
    float c[3] = {w.link[0], w.eye[1] - 0.15f, w.link[2]}, b[3], hit[3];
    float full = sqrtf((pos[0] - c[0]) * (pos[0] - c[0]) + (pos[1] - c[1]) * (pos[1] - c[1]) + (pos[2] - c[2]) * (pos[2] - c[2]));
    if (!(full > 0.01f)) return;
    for (int i = 0; i < 3; i++) b[i] = c[i] + (pos[i] - c[i]) * (full + gap) / full;
    uint32_t attr = 0;
    float want = full;
    if (game_ray(c, b, hit, &attr)) {
        float dx = hit[0] - c[0], dy = hit[1] - c[1], dz = hit[2] - c[2];
        want = fminf(full, fmaxf(0.0f, sqrtf(dx * dx + dy * dy + dz * dz) - gap));
        if (logs >= 0 && logs++ % 20 == 0)
            LOG("[fp] behind: met %.2f of %.2f (attr %#x) at (%.2f, %.2f, %.2f) from (%.2f, %.2f, %.2f)\n",
                want, full, attr, hit[0], hit[1], hit[2], c[0], c[1], c[2]);
    }
    static float cur = 1e9f;
    cur = want < cur ? want : fminf(want, cur + (full / 30.0f));  // (out: the whole way in half a second at 60 fps)
    atomic_store(&g_behind_reach, cur);
    // At his eyes below 0.8 of room, back behind him once there is 1.1 for 0.6 s, the way there taken over a quarter of
    // a second (0.4 back): the room the ray finds trembles with him (0.54 to 0.59 against Tail Cave's wall), and a
    // camera put by it astride the distance his body shows at came and went in front of him - flashes (Guillaume, 27/10)
    double now = fl_now();
    if (!eyes && want < 0.8f) eyes = true, out_since = 0;
    else if (eyes) {
        if (want <= 1.1f) out_since = 0;
        else if (!out_since) out_since = now;
        else if (now - out_since > 0.6) eyes = false;
    }
    if (g_behind_leave) eyes = true;  // (switched off: into his eyes first)
    static bool was_eyes;
    if (logs >= 0 && eyes != was_eyes)
        LOG("[fp] behind: %s (room %.2f of %.2f, pitch %.0f degrees)\n", eyes ? "to his eyes" : "back behind him", want, full,
            w.pitch * 180 / M_PI);
    was_eyes = eyes;
    float dt = kt > 0 ? fminf((float)(now - kt), 0.1f) : 0;
    kt = now;
    k = eyes ? fmaxf(0, k - dt / 0.25f) : fminf(1, k + dt / 0.4f);
    atomic_store(&g_behind_k, k);
    if (g_behind_leave && k <= 0) {  // (there: the view from his eyes)
        g_settings.fp_behind = false, g_behind_leave = false, eyes = false, k = 1, behind_saved();
        atomic_store(&g_behind_reach, 1e9f), atomic_store(&g_behind_k, 1.0f);
    }
}
static float behind_offset(float *v, float *eye, const float *link) {  // v: the view (rows), eye: moved back
    float iv[16];
    if (!inv4(v, iv)) return 0;
    float f[3] = {-iv[2], -iv[6], -iv[10]}, pos[3];
    float dist = behind_pos(f, eye, link, pos);
    v[3] = -(v[0] * pos[0] + v[1] * pos[1] + v[2] * pos[2]);
    v[7] = -(v[4] * pos[0] + v[5] * pos[1] + v[6] * pos[2]);
    v[11] = -(v[8] * pos[0] + v[9] * pos[1] + v[10] * pos[2]);
    memcpy(eye, pos, 12);
    return dist;
}

// At his eyes, the camera moved back along the look by the menu's « Eye distance » (g_settings.fp_eye_back, hundredths;
// 0 by default: his eyes themselves)
static void eye_back_offset(float *v, float *eye, float d) {
    float iv[16];
    if (!(d > 0) || !inv4(v, iv)) return;
    float f[3] = {-iv[2], -iv[6], -iv[10]};
    for (int i = 0; i < 3; i++) eye[i] -= f[i] * d;
    v[3] = -(v[0] * eye[0] + v[1] * eye[1] + v[2] * eye[2]);
    v[7] = -(v[4] * eye[0] + v[5] * eye[1] + v[6] * eye[2]);
    v[11] = -(v[8] * eye[0] + v[9] * eye[1] + v[10] * eye[2]);
}

static void rebuild(Cam *c, const float *u, const View *w) {
    memmove(c->game, u, sizeof c->game);
    memcpy(c->eye, w->eye, sizeof c->eye);
    c->yaw = w->yaw, c->pitch = w->pitch, c->fov = (float)g_settings.fp_fov;
    c->ok = false;
    c->gen++;
    g_epoch++;
    uint32_t xs;
    const XrView *x = xr_now(&xs);
    c->xr = xs;
    float eye[3];
    memcpy(eye, w->eye, sizeof eye);
    const float *gv = u, *gp = u + 16, *gvp = u + 32;
    float t[16], giv[16], gip[16], givp[16], v[16], p[16], vp[16], iv[16], ip[16], ivp[16];
    bool pv;
    mul4(gp, gv, t);
    if (near_eq(gvp, t, 16)) pv = true;
    else { mul4(gv, gp, t); if (!near_eq(gvp, t, 16)) return; pv = false; }
    c->flight = fp_flight();
    // overscan: the view drawn wider than the screen, by what the presenter turns into at most (device.c) - the edge
    // the view turns toward is the game's, not a stretch of its last pixels (a dead tree's branch against the sky,
    // smeared to the screen's edge and gone at the next game image: « les branches flashent », Guillaume, 22/10).
    // Not in a headset, nor behind him, nor during the flight (the presenter shows those as they are).
    c->ovs = g_overscan > 1 && !x && !g_settings.fp_behind && c->flight >= 0.9999f ? g_overscan : 1;
    if (!proj_matrix(gp, p, x ? x->tan : NULL, c->ovs) || !inv4(gv, giv)) return;
    c->jitter[0] = c->jitter[1] = 0;
    const char *sgsr = getenv("LARECOMP_SGSR2"), *jitter = getenv("LARECOMP_SGSR2_JITTER");
    if (x && sgsr && *sgsr == '1' && !(jitter && *jitter == '0')) {
        extern void renderer_get_scale(float *, float *);
        float sx, sy; renderer_get_scale(&sx, &sy);
        unsigned phase = (unsigned)(renderer_present_count() % 8) + 1;
        for (int axis = 0; axis < 2; axis++) {
            unsigned n = phase, base = axis ? 3 : 2;
            float h = 0, f = 1;
            while (n) { f /= base; h += f * (n % base); n /= base; }
            c->jitter[axis] = (h - 0.5f) / fmaxf(axis ? 1080 * sy : 1920 * sx, 1);
        }
        // Vulkan backend uses a negative-height geometry viewport: clip +Y
        // moves towards smaller image V. Both texture jitter offsets must +UV.
        p[2] -= 2 * c->jitter[0]; p[6] += 2 * c->jitter[1];
    }
    if (x) view_matrix_xr(w, x, v, eye);
    else view_matrix(w, v);
    float behind = g_settings.fp_behind ? behind_offset(v, eye, w->link) : 0;  // (before the flight: it ends there)
    if (!g_settings.fp_behind && g_settings.fp_eye_back > 0) eye_back_offset(v, eye, g_settings.fp_eye_back / 100.0f);
    if (c->flight < 0.9999f) flight_camera(c->flight, gv, gp, w, v, p, eye);
    else g_fl_dist = behind;
    if (pv) mul4(p, v, vp); else mul4(v, p, vp);
    if (!inv4(v, iv) || !inv4(p, ip) || !inv4(vp, ivp) || !inv4(gp, gip) || !inv4(gvp, givp)) return;
    memcpy(c->from[P_VP], gvp, 64), memcpy(c->to[P_VP], vp, 64);
    memcpy(c->from[P_V], gv, 64), memcpy(c->to[P_V], v, 64);
    memcpy(c->from[P_V12], gv, 48), memcpy(c->to[P_V12], v, 48);
    memcpy(c->from[P_P], gp, 64), memcpy(c->to[P_P], p, 64);
    memcpy(c->from[P_ROT], giv, 64), memcpy(c->to[P_ROT], iv, 64);
    c->from[P_ROT][3] = c->from[P_ROT][7] = c->from[P_ROT][11] = 0;  // (rotation only: no translation)
    c->to[P_ROT][3] = c->to[P_ROT][7] = c->to[P_ROT][11] = 0;
    memcpy(c->from[P_IV12], giv, 48), memcpy(c->to[P_IV12], iv, 48);
    memcpy(c->from[P_IP], gip, 64), memcpy(c->to[P_IP], ip, 64);
    memcpy(c->from[P_IVP], givp, 64), memcpy(c->to[P_IVP], ivp, 64);
    float ge[3] = {giv[3], giv[7], giv[11]}, gb[3] = {giv[2], giv[6], giv[10]}, ob[3] = {iv[2], iv[6], iv[10]};
    memcpy(c->from[P_EYE], ge, 12), memcpy(c->to[P_EYE], eye, 12);
    memcpy(c->from[P_BACK], gb, 12), memcpy(c->to[P_BACK], ob, 12);
    depth_range(gp, c->from[P_DEPTH]), depth_range(p, c->to[P_DEPTH]);
    float *gl = c->from[P_LENS], *ol = c->to[P_LENS];  // near, far, vertical fov (radians), aspect
    gl[0] = c->from[P_DEPTH][0], gl[1] = c->from[P_DEPTH][1], gl[2] = 2 * atanf(1 / gp[5]), gl[3] = gp[5] / gp[0];
    ol[0] = c->to[P_DEPTH][0], ol[1] = c->to[P_DEPTH][1], ol[2] = 2 * atanf(1 / p[5]), ol[3] = p[5] / p[0];
    c->ok = true;
}

// Where a block carries the game camera, looked for in every block each draw binds: one block can hold different
// cameras within a frame (the 1024-byte view block in slot 1 holds the shadow light's during the shadow pass, then the
// game camera's, at the same address). Position, back direction and depth range (3-4 floats: could be anything) count
// only in a block that also holds one of the matrices. About 2,500 blocks a frame of a few hundred bytes: cheap.
static int scan(const Cam *c, const float *d, int nf, uint16_t *off, uint8_t *kind) {
    int n = 0;
    bool matrix = false;
    for (int pass = 0; pass < 2; pass++)  // matrices first, then the small values in blocks that hold one
        for (int o = 0; o + 3 <= nf && n < 16; o += 4)
            for (int k = 0; k < P_COUNT && n < 16; k++) {
                bool small = small_kind(k);
                if (small != (pass == 1) || (small && !matrix) || o + k_len[k] > nf) continue;
                if (!near_eq(d + o, c->from[k], 1)) continue;  // (the first value rules nearly every place out)
                bool taken = false;
                for (int i = 0; i < n; i++) taken |= o < off[i] + k_len[kind[i]] && off[i] < o + k_len[k];
                if (taken || !near_eq(d + o, c->from[k], k_len[k])) continue;
                if (k == P_V12 && o + 16 <= nf && near_eq(d + o, c->from[P_V], 16)) continue;  // (the whole view)
                off[n] = (uint16_t)o, kind[n] = (uint8_t)k, n++;
                matrix |= !small;
            }
    return n;
}

// LARECOMP_LOG_FP: what was found where, once per (stage, slot, size) and layout.
static void say(int stage, int idx, uint64_t range, int n, const uint16_t *off, const uint8_t *kind) {
    static uint64_t keys[256], sigs[256][4];  // (a block can alternate between layouts: each one said once -
    static int nk, ns[256];                   //  it alternated every draw at the beach, 20 000 lines a minute)
    uint64_t key = (uint64_t)(stage & 7) << 40 | (uint64_t)(idx & 0xff) << 32 | (range & 0xffffffff), sig = 1;
    for (int i = 0; i < n; i++) sig = sig * 131 + off[i] * 16u + kind[i];
    int i = 0;
    while (i < nk && keys[i] != key) i++;
    if (i == nk) { if (nk == 256) return; keys[nk++] = key; }
    for (int j = 0; j < ns[i]; j++) if (sigs[i][j] == sig) return;
    if (ns[i] == 4) return;
    sigs[i][ns[i]++] = sig;
    LOG("[fp] %c%d (%llu bytes):", stage == 5 ? 'c' : stage ? 'f' : 'v', idx, (unsigned long long)range);
    for (int j = 0; j < n; j++) LOG(" %s at %d%s", k_name[kind[j]], off[j] * 4, j + 1 < n ? "," : "");
    LOG("\n");
}

// A block that defines the game camera: the scene block (slot 5: view, projection, view-projection), or the view block
// (slot 1: view-projection at 0, view 3x4 at 36, projection at 92) when its projection is the scene block's - the view
// block also holds the cameras of lights casting shadows, and a spotlight's is a perspective one too: taken for the
// game camera, it came first in the list (no sky) and its shadow pass got our view.
static float g_lens[16];  // the scene block's projection, last seen
static bool camera_of(int idx, uint64_t range, const float *s, float *cam) {
    if (idx == 5 && range == 320 && perspective(s)) { memcpy(cam, s, 192); memcpy(g_lens, s + 16, 64); return true; }
    const float *p = s + 92;
    if (range != 1024 || idx != 1 || p[12] != 0 || p[13] != 0 || p[14] != -1 || p[15] != 0) return false;
    // (or the lens of a camera already learnt: in Dampé's shack an event drew a scene block with a lens of its own, the
    //  last lens was that one, and the room's own view block - the same values - was no longer the game camera: the
    //  frames of the talk drawn from above, 27/10)
    // A talk's camera zooms: the same lens but for its field (Dampé, 27/10: 2.246 / 2.672 -> 2.132 / 2.536, near and far
    // and the aspect the same) - and no scene block came to say so. A light's lens has near / far and an aspect of its
    // own. So: the last lens, a learnt camera's, or the last lens zoomed.
    if (!near_eq(p, g_lens, 16)) {
        bool known = false;
        for (int i = 0; i < g_ncam && !known; i++) known = near_eq(p, g_cams[i].game + 16, 16);
        bool zoomed = g_lens[0] != 0 && p[0] != 0 && fabsf(p[10] - g_lens[10]) < 1e-4f * fabsf(g_lens[10]) + 1e-6f &&
                      fabsf(p[11] - g_lens[11]) < 1e-4f * fabsf(g_lens[11]) + 1e-6f &&
                      fabsf(p[5] / p[0] - g_lens[5] / g_lens[0]) < 0.01f * fabsf(g_lens[5] / g_lens[0]) && p[2] == 0 && p[6] == 0;
        if (ENV("LARECOMP_FP_NO_ZOOM_LENS")) zoomed = false;  // (a test)
        if (!known && !zoomed) return false;
    }
    memcpy(cam, s + 36, 48);
    cam[12] = cam[13] = cam[14] = 0, cam[15] = 1;
    memcpy(cam + 16, p, 64);
    memcpy(cam + 32, s, 64);
    return true;
}

static bool same(const float *a, const float *b, int n) {  // (== : -0 and 0 are the same camera)
    for (int i = 0; i < n; i++) if (a[i] != b[i]) return false;
    return true;
}

static void learn(const float *cam, const View *w) {
    for (int i = 0; i < g_ncam; i++)
        if (same(g_cams[i].game, cam, 48)) {
            if (i) { Cam t = g_cams[i]; memmove(&g_cams[1], &g_cams[0], (size_t)i * sizeof(Cam)); g_cams[0] = t; }
            return;
        }
    if (g_ncam < NCAM) g_ncam++;
    memmove(&g_cams[1], &g_cams[0], (size_t)(g_ncam - 1) * sizeof(Cam));
    rebuild(&g_cams[0], cam, w);
}

// Which kinds of block (stage, slot, size) carry the game camera, learnt: a kind that never held it in its first 40
// binds is searched once in 64 after that. Searching every block of every draw (bone palettes of 2256 bytes, light
// lists of 4608) took the game from 60 to 30 frames a second, the GPU idle (measured 20/10, Mabe Village).
typedef struct { uint64_t key; uint32_t seen, hits; } Kind;
static Kind g_kinds[1024];  // open addressing on the key
static Kind *kind_of(uint64_t key) {
    uint32_t h = (uint32_t)((key * 0x9e3779b97f4a7c15ull) >> 54);  // 10 bits
    for (int i = 0; i < 1024; i++) {
        Kind *k = &g_kinds[(h + i) & 1023];
        if (k->key == key) return k;
        if (!k->key) { k->key = key; return k; }
    }
    return NULL;
}

static unsigned g_count[5];  // per presented frame (LARECOMP_LOG_FP): blocks rewritten, values rewritten, blocks seen,
                             // camera blocks left as the game's (= a frame showing the view from above)

// What Link holds - his arms, shield, sword, items - is shown in front of the view like a first-person game's: the draw
// being bound is one of his parts that stays (renderer.m keeps them, leaving his head, hair, body and boots out), and
// his bone palette in it is moved from his body to the camera. The body's frame is his first bone (his feet, turned
// the way he faces); the camera's is ours. LARECOMP_FP_VM "x,y,z": where his feet go in camera space (right, up, back).
// His shadow pass keeps the real bones (it isn't a camera pass).
static int g_vm;  // (0: not a held part; 1: arms, shield; 2: the sword - placed higher, its swing low in his reach)
static void note_cell(float x, float z);
static bool on_link(const float *f, const float *link);
void fp_viewmodel_draw(int on) { g_vm = on; }

// What he holds drawn once per eye (PC VR, exec.c draw_): -1 from the head's centre, 0 / 1 from that eye - the eyes'
// offsets from the head's centre along its right, metres (android/xr_app.c). Made from one image and its depth, the
// sword - thin, near - came out doubled, a ghost edge, in the headset only (Guillaume, 26-27/10): each eye its own now.
static int g_vm_eye = -1;
static float g_eye_off[2];
void fp_vm_eye(int e) { g_vm_eye = e; }
void fp_xr_eye_offsets(float left, float right) { g_eye_off[0] = left, g_eye_off[1] = right; }
static void viewmodel(float *d, int nf, const float *src) {
    if (ENV("LARECOMP_FP_LOG_BONES")) {  // (finding his head: every bone's place relative to the first, once a second)
        static uint64_t said;
        if (renderer_present_count() / 60 != said / 60) {
            said = renderer_present_count();
            LOG("[fp] Link's bones:");
            for (int i = 0; (i + 1) * 12 <= nf && i < 64; i++)
                LOG(" #%d(%.2f,%.2f,%.2f)", i, src[i * 12 + 3] - src[3], src[i * 12 + 7] - src[7], src[i * 12 + 11] - src[11]);
            LOG("\n");
        }
    }
    {   // (LARECOMP_FP_LOG_VM=1: how many of his parts' palettes are moved a frame, and how big - research)
        static uint64_t fr, calls, bones;
        uint64_t now = renderer_present_count();
        if (now != fr) {
            if (ENV("LARECOMP_FP_LOG_VM") && now % 60 == 0)
                LOG("[fp] frame %llu: %llu palettes moved (%llu bones)\n", (unsigned long long)fr, (unsigned long long)calls, (unsigned long long)bones);
            fr = now, calls = bones = 0;
        }
        calls++, bones += (uint64_t)nf / 12;
    }
    const Cam *c = NULL;
    for (int i = 0; i < g_ncam && !c; i++) if (g_cams[i].ok) c = &g_cams[i];
    if (!c) return;
    static float o[3] = {1e9f};
    if (o[0] > 1e8f) {
        o[0] = 0.12f, o[1] = -1.12f, o[2] = -0.62f;  // (tuned on screenshots 20/10: the raised shield low right)
        const char *e = getenv("LARECOMP_FP_VM");
        if (e) sscanf(e, "%f,%f,%f", &o[0], &o[1], &o[2]);
    }
    float B[16] = {src[0], src[1], src[2], src[3], src[4], src[5], src[6], src[7], src[8], src[9], src[10], src[11],
                   0, 0, 0, 1}, Bi[16], C[16], T[16], X[16], t[16];
    if (!inv4(B, Bi)) return;
    memcpy(C, c->to[P_IV12], 48);
    C[12] = C[13] = C[14] = 0, C[15] = 1;
    // his feet at o in camera space, his front (+z in his frame) along the camera's forward (-z): half a turn about y
    static float sy = -1e9f;  // (LARECOMP_FP_VM_SWORD: how much higher the sword's frame sits)
    if (sy < -1e8f) sy = envf("LARECOMP_FP_VM_SWORD", 0.42f);
    float lift = g_vm == 2 ? sy : 0;
    // (PC VR, drawn once per eye - exec.c: from that eye, the head's centre moved along its right by the eye's offset)
    const float unit = fp_xr_unit();
    float ex = g_vm_eye >= 0 && g_vm_eye < 2 ? -g_eye_off[g_vm_eye] / unit : 0;
    float R[16] = {-1, 0, 0, o[0] + ex, 0, 1, 0, o[1] + lift, 0, 0, -1, o[2], 0, 0, 0, 1};
    mul4(C, R, T);
    mul4(T, Bi, X);
    // (the bones read from src, the guest's block - d is the draw's copy in the transient ring, which a desktop GPU
    //  keeps in its own uncached memory: read back by the CPU, 9,000 reads a frame took 38 % of the Windows submission
    //  thread, 35-45 fps at Mabe, 26/10 - as fp_patch_ubo's did on Linux, 21/10; d is only written)
    for (int b = 0; (b + 1) * 12 <= nf; b++) {
        const float *m = src + b * 12;
        float M[16] = {m[0], m[1], m[2], m[3], m[4], m[5], m[6], m[7], m[8], m[9], m[10], m[11], 0, 0, 0, 1};
        mul4(X, M, t);
        memcpy(d + b * 12, t, 48);
    }
}

// runtime/nvn/renderer.m, every uniform block a draw binds: src is the guest's block (range bytes), dst the draw's
// snapshot of its first n bytes, rewritten where it holds the game camera while the view is on.
// The same block is bound again and again within a frame - the view block by every draw of a pass, and searched against
// all four cameras whenever it holds a light's: its answer is kept, keyed by its whole content, until a camera's
// replacements change (g_epoch). Measured 20/10 at 3024x1964: fp_patch_ubo took 10 % of the game thread, whose frame is
// serial (the game waits for the GPU's last frame before it submits the next one, and the submission is our encoding).
typedef struct { uint32_t epoch, nf, nrep; int found; uint16_t off[16]; uint8_t kind[16]; float rep[256], data[256]; } Memo;
static Memo g_memo[8];
static int g_memo_next;
static Memo *memo_find(const float *d, int nf) {
    if (nf > 256) return NULL;
    for (int i = 0; i < 8; i++) {
        Memo *m = &g_memo[i];
        if (m->epoch == g_epoch && m->nf == (uint32_t)nf && !memcmp(m->data, d, (size_t)nf * 4)) return m;
    }
    return NULL;
}
static Memo *memo_keep(const float *d, int nf) {
    if (nf > 256 || ENV("LARECOMP_FP_NO_MEMO")) return NULL;
    Memo *m = &g_memo[g_memo_next++ & 7];
    m->epoch = g_epoch, m->nf = (uint32_t)nf, m->found = 0, m->nrep = 0;
    memcpy(m->data, d, (size_t)nf * 4);
    return m;
}

void fp_patch_ubo(int stage, int idx, float *dst, uint64_t n, const float *src, uint64_t range) {
    if (!fp_view_on()) return;
    uint64_t frame = renderer_present_count();
    {
        static uint64_t last;
        if (frame != last) {
            // (LARECOMP_LOG_FP_EVERY=<from>:<to>: every frame in that span, with the cameras learnt and the flight - the
            //  dialogue's frames drawn from above, 27/10)
            static long ev_a = -2, ev_b;
            if (ev_a == -2) { const char *e = getenv("LARECOMP_LOG_FP_EVERY"); ev_a = -1; if (e) sscanf(e, "%ld:%ld", &ev_a, &ev_b); }
            bool every = ev_a >= 0 && (long)last >= ev_a && (long)last <= ev_b;
            if ((ENV("LARECOMP_LOG_FP") && (frame % 120 == 0 || g_count[3])) || every)
                LOG("[fp] frame %llu: %u of %u blocks rewritten (%u values), %u camera blocks missed, %u answered by the memo%s\n",
                    (unsigned long long)last, g_count[0], g_count[2], g_count[1], g_count[3], g_count[4], every ? "" : "");
            if (every) {
                LOG("[fp]   cameras %d, flight %.2f (to %d), view on %d:", g_ncam, fp_flight(), (int)g_fl_to, (int)fp_view_on());
                for (int ci = 0; ci < g_ncam; ci++)
                    LOG(" #%d ok %d lens %.3f %.3f eye %.1f %.1f %.1f", ci, g_cams[ci].ok, g_cams[ci].game[16], g_cams[ci].game[21],
                        g_cams[ci].game[12], g_cams[ci].game[13], g_cams[ci].game[14]);
                LOG("\n");
            }
            memset(g_count, 0, sizeof g_count);
            last = frame;
        }
    }
    g_count[2]++;
    View w = view_now();
    uint32_t xs;
    xr_now(&xs);
    for (int i = 0; i < g_ncam; i++)  // our view moved (looking up moves nothing else): every camera's replacements follow
        if (memcmp(g_cams[i].eye, w.eye, sizeof w.eye) || g_cams[i].yaw != w.yaw || g_cams[i].pitch != w.pitch ||
            g_cams[i].fov != (float)g_settings.fp_fov || g_cams[i].xr != xs || g_cams[i].flight != fp_flight())
            rebuild(&g_cams[i], g_cams[i].game, &w);
    if (g_vm && range >= 8 * 48 && on_link(src, w.link)) { viewmodel(dst, (int)(n / 4), src); return; }
    float cam[48];
    bool defines = camera_of(idx, range, src, cam);
    {   // (LARECOMP_LOG_FP_EVERY: each frame's slot 1 / 5 vertex blocks as seen - which test the dialogue's camera fails)
        static long ev_a = -2, ev_b;
        if (ev_a == -2) { const char *e = getenv("LARECOMP_LOG_FP_EVERY"); ev_a = -1; if (e) sscanf(e, "%ld:%ld", &ev_a, &ev_b); }
        static uint64_t said_f;
        static int said_n;
        if (said_f != frame) said_f = frame, said_n = 0;
        if (ev_a >= 0 && (long)frame >= ev_a && (long)frame <= ev_b && stage == 0 && (idx == 5 || idx == 1) && said_n++ < 6 && range >= 128)
            LOG("[fp]   f%llu v%d %llu bytes: defines %d, lens %g %g %g %g, last %g %g %g %g, cam0 %g %g %g %g\n",
                (unsigned long long)frame, idx, (unsigned long long)range, defines, range >= 512 ? src[92] : src[16],
                range >= 512 ? src[97] : src[21], range >= 512 ? src[102] : src[26], range >= 512 ? src[103] : src[27],
                g_lens[0], g_lens[5], g_lens[10], g_lens[11], g_ncam ? g_cams[0].game[16] : 0, g_ncam ? g_cams[0].game[21] : 0,
                g_ncam ? g_cams[0].game[26] : 0, g_ncam ? g_cams[0].game[27] : 0);
    }
    if (defines) learn(cam, &w);
    Kind *kd = kind_of((uint64_t)(stage & 7) << 40 | (uint64_t)(idx & 0xff) << 32 | (range & 0xffffffff) | 1ull << 48);
    if (kd && !defines && !kd->hits && kd->seen >= 40 && kd->seen++ % 64) return;
    if (kd && kd->seen < 40) kd->seen++;
    int nf = (int)(n / 4);
    // (every read from the guest's block, src - the same values as the copy: dst is in the transient ring, which on a
    //  desktop GPU is its own memory mapped write-combined - read back by the CPU it made fp_patch_ubo 76 % of the
    //  submission thread on Linux with an RX 7800 XT, 19 fps at the beach, 21/10)
    Memo *m = memo_find(src, nf);
    if (m) {  // (the same block, bound again: its answer)
        g_count[4]++;
        if (!m->found) { g_count[3] += defines && nf >= 48; return; }
        if (kd) kd->hits++;
        for (int i = 0, at = 0; i < m->found; at += k_len[m->kind[i]], i++)
            memcpy(dst + m->off[i], m->rep + at, (size_t)k_len[m->kind[i]] * 4);
        g_count[0]++, g_count[1] += m->nrep;
        return;
    }
    uint16_t off[16];
    uint8_t kind[16];
    int found = 0;
    const Cam *c = NULL;
    for (int i = 0; i < g_ncam && !found; i++)
        if (g_cams[i].ok) c = &g_cams[i], found = scan(c, src, nf, off, kind);
    m = memo_keep(src, nf);
    if (m) m->found = found;
    if (!found) { g_count[3] += defines && nf >= 48; return; }
    if (kd) kd->hits++;
    if (ENV("LARECOMP_LOG_FP")) say(stage, idx, range, found, off, kind);
    unsigned done = 0;
    for (int i = 0; i < found; i++) {
        if (m) m->off[i] = off[i], m->kind[i] = kind[i], memcpy(m->rep + done, c->to[kind[i]], (size_t)k_len[kind[i]] * 4);
        memcpy(dst + off[i], c->to[kind[i]], (size_t)k_len[kind[i]] * 4);
        done += (unsigned)k_len[kind[i]];
    }
    if (m) m->nrep = done;
    g_count[0]++, g_count[1] += done;
}

// ------------------------------------------------------------------ the view at the display's rate (LARECOMP_FP_WARP)
// runtime/nvn/metal.m's presenter shows, at every vsync of a 120 Hz screen, the last game image turned to the view of
// that moment: the frame's own view and when it was published (fp_warp_frame_view, the render thread at its present),
// and the view at the frame's publication plus the time since the image was due (fp_warp_view_at, the presenter's
// thread) - between two published looks, interpolated; past the newest, it plus the mouse motion the game thread
// hasn't taken yet and the right stick's turn since. The camera turns at 120 images a second, in even steps, while the
// game runs at 60, and keeps turning through a late game frame. (Extrapolating from the newest look at the vsync's
// own time stepped back by half a frame at each publication: the game turns by a fixed step a frame, not by the time.)
static _Atomic int32_t g_look_rx, g_look_ry;
void fp_warp_note_look(int32_t rx, int32_t ry) { atomic_store(&g_look_rx, rx), atomic_store(&g_look_ry, ry); }
bool fp_warp_frame_view(float *yaw, float *pitch, float *tx, float *ty, uint64_t *ns) {
    extern bool world_on_screen(void);
    uint32_t xs;
    if (!fp_view_on() || fp_flight() < 0.9999f || g_settings.fp_behind || xr_now(&xs) || !world_on_screen()) return false;
    View w = view_now();
    if (!w.on) return false;
    for (int i = 0; i < g_ncam; i++) {
        const float *p = g_cams[i].to[P_P];
        if (!g_cams[i].ok || p[0] == 0 || p[5] == 0) continue;
        *yaw = w.yaw, *pitch = w.pitch, *tx = 1 / p[0], *ty = 1 / p[5], *ns = w.ns;
        return true;
    }
    return false;
}
void fp_warp_view_at(uint64_t t, float *yaw, float *pitch) {
    uint32_t n = atomic_load(&g_nlooks);
    Look last = n ? g_looks[(n - 1) % NLOOK] : (Look){0, g_yaw, g_pitch};
    if (n && t < last.ns) {
        for (uint32_t k = n - 1; k >= 1 && n - k < NLOOK - 2; k--) {
            Look a = g_looks[(k - 1) % NLOOK], b = g_looks[k % NLOOK];
            if (a.ns > t) continue;
            float f = b.ns > a.ns ? (float)(t - a.ns) / (float)(b.ns - a.ns) : 1;
            *yaw = wrap_pi(a.yaw + wrap_pi(b.yaw - a.yaw) * f), *pitch = a.pitch + (b.pitch - a.pitch) * f;
            return;
        }
        *yaw = g_looks[(n - NLOOK + 3) % NLOOK].yaw, *pitch = g_looks[(n - NLOOK + 3) % NLOOK].pitch;  // (older than kept)
        return;
    }
    float y = last.yaw, p = last.pitch;
    if (g_on) {
        static float sens = -1;
        if (sens < 0) sens = envf("LARECOMP_FP_MOUSE", 0.0025f);
        float sp = (g_settings.fp_sens > 0 ? g_settings.fp_sens : 5) / 5.0f;
        int dx = atomic_load(&g_mdx), dy = atomic_load(&g_mdy);
        y += dx / 16.0f * sp * sens;
        p += (g_settings.fp_invert_y ? dy : -dy) / 16.0f * sp * sens;
        float dt = n && t > last.ns ? (float)(t - last.ns) / 1e9f : 0;
        if (dt > 0.05f) dt = 0.05f;  // (a game frame that late: no further than 3 of them)
        bool inv = ENV("LARECOMP_FP_INVERT_Y") != NULL || g_settings.fp_invert_y;
        y += stick_axis(atomic_load(&g_look_rx)) * 3.0f * sp * dt;
        p += stick_axis(atomic_load(&g_look_ry)) * (inv ? -2.0f : 2.0f) * sp * dt;
    }
    *yaw = y, *pitch = fmaxf(-1.4f, fminf(1.4f, p));
}
// Where the eye is at time t, on the same timeline (runtime/nvn/vulkan/device.c's presenter moves the image by its depth
// as well as turning it: turning while walking - round something, the view turned at the display's rate and the walk
// at the game's, unevenly at 165 Hz - the image swam: « ça se trouble quand je pivote en avançant », Guillaume, 22/10).
// Between two published looks, interpolated; past the newest, carried on at the last two's speed for 50 ms at most.
bool fp_warp_eye_at(uint64_t t, float *eye) {
    uint32_t n = atomic_load(&g_nlooks);
    if (n < 2) return false;
    Look last = g_looks[(n - 1) % NLOOK];
    if (!last.eye_ok) return false;
    if (t < last.ns) {
        for (uint32_t k = n - 1; k >= 1 && n - k < NLOOK - 2; k--) {
            Look a = g_looks[(k - 1) % NLOOK], b = g_looks[k % NLOOK];
            if (a.ns > t) continue;
            if (!a.eye_ok || !b.eye_ok) return false;
            float f = b.ns > a.ns ? (float)(t - a.ns) / (float)(b.ns - a.ns) : 1;
            for (int i = 0; i < 3; i++) eye[i] = a.eye[i] + (b.eye[i] - a.eye[i]) * f;
            return true;
        }
        return false;
    }
    Look prev = g_looks[(n - 2) % NLOOK];
    float dt = (float)(t - last.ns) / 1e9f, span = last.ns > prev.ns ? (float)(last.ns - prev.ns) / 1e9f : 0;
    if (dt > 0.05f) dt = 0.05f;
    for (int i = 0; i < 3; i++) eye[i] = last.eye[i] + (prev.eye_ok && span > 0 ? (last.eye[i] - prev.eye[i]) / span * dt : 0);
    return true;
}
// The view's state, logged when it changes (always: a state he meets in play - Bottle Grotto's grab, 26/10 - is then in
// his log): on, the game camera found, his skeleton seen, upright, a unit above the floor; LARECOMP_LOG_FP_STATE=1 also
// every 20 frames (fp_input)
static void fp_state_log(uint64_t pc, uint64_t frame, bool on) {
        extern const char *game_level_name(void);
        int ok = 0;
        for (int i = 0; i < g_ncam; i++) ok += g_cams[i].ok;
        const float *lp = pc ? (const float *)(uintptr_t)(pc + 0x80) : NULL;
        static int last = -1;
        static char lv0[48];
        const char *lv = game_level_name();
        bool high = lp && g_floor_y > -1e8f && lp[1] - g_floor_y > 1.0f;
        int sig = (int)on | (ok > 0) << 1 | (frame - g_head_frame <= 10) << 2 | (int)g_upright << 3 | (int)high << 4;
        bool changed = sig != last || strncmp(lv, lv0, sizeof lv0 - 1);
        if (!changed && !(ENV("LARECOMP_LOG_FP_STATE") && frame % 20 == 0)) return;
        last = sig, snprintf(lv0, sizeof lv0, "%s", lv);
        LOG("[fp state] f%llu %s on %d setting %d side %d lifted %d feet %.2f floor %.2f upright %d skeleton %lld frames ago, "
            "cameras %d/%d flight %.2f\n", (unsigned long long)frame, game_level_name(), on, g_settings.first_person,
            pc ? widescreen_side_view_zone() : -1, pc ? lifted_out(pc, frame) : -1, lp ? lp[1] : 0.0f, g_floor_y, g_upright,
            (long long)(frame - g_head_frame), ok, g_ncam, fp_flight());
}
// The frame's lens, whatever the view: its tangents and the overscan its camera was made with (the presenter shows the
// screen's part of a view drawn wider even when it doesn't turn it)
bool fp_warp_frame_lens(float *tx, float *ty, float *ovs) {
    // Only a frame our cameras drew is wider than the screen: out of first person the stale cameras' overscan cropped the
    // view from above and the menu's bar; and the world must be on screen, as for the turn (fp_warp_frame_view) - the
    // game's pause menu, its map, drawn flat over a still image, came out cut on every edge (Guillaume 13/11)
    extern bool world_on_screen(void);
    if (!fp_view_on() || !world_on_screen()) return false;
    for (int i = 0; i < g_ncam; i++) {
        const float *p = g_cams[i].to[P_P];
        if (!g_cams[i].ok || p[0] == 0 || p[5] == 0) continue;
        *tx = 1 / p[0], *ty = 1 / p[5], *ovs = g_cams[i].ovs > 1 ? g_cams[i].ovs : 1;
        return true;
    }
    return false;
}
// The eye of the view being drawn (with fp_warp_frame_view, the same view)
bool fp_warp_frame_eye(float *eye) {
    View w = view_now();
    if (!w.on) return false;
    memcpy(eye, w.eye, sizeof w.eye);
    return true;
}

// The terrain cells (15 x 12 units, 16 x 16 on the Field) drawn in the camera's passes, this frame and the last: their
// model matrix (a 256-byte vertex block) has no rotation and stands at the cell's centre (LARECOMP_FP_LOG_CELLS lists
// them; the haze no longer follows them - fp_sky_params). Measured 20/10: the game has a 5 x 7 block of cells around Mabe Village loaded (3
// rows north of Link, 1 south, 4 columns east) - more than its view from above shows; the 3 x 3 guess fogged away
// trees it was drawing.
static uint32_t g_cells[2][8];
static uint64_t g_cells_frame;
static void note_cell(float x, float z) {
    float c = (x - 7.5f) / 15, r = (z - 6) / 12;
    if (c < 0 || r < 0 || c > 15 || r > 15 || fabsf(c - roundf(c)) > 1e-3f || fabsf(r - roundf(r)) > 1e-3f) return;
    uint64_t f = renderer_present_count();
    if (f != g_cells_frame) { memcpy(g_cells[1], g_cells[0], sizeof g_cells[0]); memset(g_cells[0], 0, sizeof g_cells[0]); g_cells_frame = f; }
    int i = (int)roundf(r) * 16 + (int)roundf(c);
    g_cells[0][i >> 5] |= 1u << (i & 31);
}

static bool rotation_like(const float *m) {  // 3x4 rows: each row of the 3x3 part about unit length (a bone, scaled or not)
    for (int r = 0; r < 3; r++) {
        float l = m[r * 4] * m[r * 4] + m[r * 4 + 1] * m[r * 4 + 1] + m[r * 4 + 2] * m[r * 4 + 2];
        if (!(l > 0.25f && l < 4.0f)) return false;
    }
    return true;
}

// A bone palette on Link: of its first 8 matrices (3x4 rows), at least 4 are bones standing around his feet, and not all
// turned the same way (a skeleton, not copies of one object's matrix).
// Its root bone (the first) on him, too: a person a step away has his bones within 1 unit of Link's feet, was taken for
// him, and his palette moved before the view as Link's arms are - the person stuck to the view beside him (Guillaume,
// 27/10; found by the consultant, Gemini).
static bool on_link(const float *f, const float *link) {
    float rx = f[3] - link[0], rz = f[11] - link[2], hx = f[3] - g_head[0], hz = f[11] - g_head[2];
    if (rx * rx + rz * rz > 0.4f * 0.4f && hx * hx + hz * hz > 0.4f * 0.4f) return false;
    int near = 0;
    bool differ = false;
    for (int b = 0; b < 8; b++) {
        const float *m = f + b * 12;
        float dx = m[3] - link[0], dy = m[7] - link[1], dz = m[11] - link[2];
        if (!(dx * dx + dz * dz < 1.0f && dy > -0.5f && dy < 2.5f && rotation_like(m))) continue;
        near++;
        differ |= fabsf(m[0] - f[0]) + fabsf(m[1] - f[1]) + fabsf(m[5] - f[5]) + fabsf(m[10] - f[10]) > 0.01f;
    }
    return near >= 4 && differ;
}

// Link's own model, from inside his head: the hat brim and a hand filled the view whenever he turned. His draws in the
// camera's passes are left out, the shadow pass keeps his shadow: a draw whose vertex blocks hold the game camera (the
// view block: view-projection first; the scene block: view first) and his bone palette - measured 20/10, 2256 bytes in
// slot 3, bones (3x4 rows) around his feet. A palette, not one matrix: in Tail Cave the room's own model matrix stands
// exactly where Link enters, and taking it for him hid the whole room (15,000 of 19,000 draws).
// runtime/nvn/renderer.m, each draw: bit 0 = leave it out, bit 1 = drawn with the game camera (its depth target is the
// scene's, for the sky).
int fp_draw_class(const uint64_t *addr, const uint64_t *size, int n) {
    {   // (a new frame: the last one's need for the push, the next one's from nothing)
        uint64_t fr = renderer_present_count();
        if (fr != g_push_frame) {
            int n = atomic_exchange(&g_ncap_acc, 0);
            if (n > NCAP) n = NCAP;
            memcpy(g_cap_last, g_cap_acc, sizeof g_cap_last), atomic_store(&g_ncap_last, n), g_push_frame = fr;
        }
    }
    if (!fp_view_on() || !g_ncam) return 0;
    View w = view_now();
    bool camera = false, link = false;
    for (int i = 0; i < n; i++) {
        if (!addr[i] || size[i] < 48) continue;
        const float *f = (const float *)(uintptr_t)addr[i];
        for (int c = 0; c < g_ncam && !camera && size[i] >= 64; c++)
            camera = g_cams[c].ok && (near_eq(f, g_cams[c].from[P_VP], 16) || near_eq(f, g_cams[c].from[P_V], 16));
        if (ENV("LARECOMP_FP_LOG_CLASS") && size[i] == 1024 && i == 1 && !camera && renderer_present_count() % 120 == 0) {
            static uint64_t said;
            if (said != renderer_present_count()) {
                said = renderer_present_count();
                LOG("[fp] class: view block not the camera at frame %llu: [%g %g %g %g] p92 [%g %g %g %g]; %d cameras:",
                    (unsigned long long)said, f[0], f[1], f[2], f[3], f[92], f[97], f[102], f[103], g_ncam);
                for (int c = 0; c < g_ncam; c++)
                    LOG(" #%d ok %d vp [%g %g %g %g] v [%g %g %g %g]", c, g_cams[c].ok, g_cams[c].from[P_VP][0],
                        g_cams[c].from[P_VP][1], g_cams[c].from[P_VP][2], g_cams[c].from[P_VP][3], g_cams[c].from[P_V][0],
                        g_cams[c].from[P_V][1], g_cams[c].from[P_V][2], g_cams[c].from[P_V][3]);
                LOG("\n");
            }
        }
        if (camera && size[i] == 256 && f[0] == 1 && f[5] == 1 && f[10] == 1 && f[7] == 0) note_cell(f[3], f[11]);
        if (size[i] == 2256 && (addr[i] == g_link_pal[0] || addr[i] == g_link_pal[1])) link = true;
        if (size[i] >= 8 * 48 && size[i] % 48 == 0 && size[i] != 4608 && !on_link(f, w.link)) {
            // (a person's bones, drawn with the game camera: his capsule - their middle across, how far they reach from
            //  it, their lowest and highest - for publish to keep the eye out of; near the eye only)
            int nb = (int)(size[i] / 48);
            bool bones = true;
            for (int bb = 0; bb < nb && bb < 4 && bones; bb++) {  // (rotations, a scale near 1: not a light list)
                const float *m = f + bb * 12;
                float l0 = m[0] * m[0] + m[1] * m[1] + m[2] * m[2], l1 = m[4] * m[4] + m[5] * m[5] + m[6] * m[6];
                bones = l0 > 0.25f && l0 < 4 && l1 > 0.25f && l1 < 4;
            }
            float cx = 0, cz = 0, y0 = 1e9f, y1 = -1e9f;
            int k = 0;
            for (int bb = 0; bones && bb < nb && bb < 256; bb++, k++) {
                const float *m = f + bb * 12;
                cx += m[3], cz += m[11], y0 = fminf(y0, m[7]), y1 = fmaxf(y1, m[7]);
            }
            if (bones && k) {
                cx /= k, cz /= k;
                float ex = g_cull_eye[0] - cx, ez = g_cull_eye[2] - cz;
                if (ex * ex + ez * ez < 9) {
                    float spread = 0;
                    for (int bb = 0; bb < nb && bb < 256; bb++) {
                        const float *m = f + bb * 12;
                        float dx = m[3] - cx, dz = m[11] - cz;
                        spread = fmaxf(spread, dx * dx + dz * dz);
                    }
                    int at = atomic_fetch_add(&g_ncap_acc, 1);
                    if (at < NCAP) g_cap_acc[at] = (Cap){cx, cz, sqrtf(spread), y0, y1};
                }
            }
        }
        if (size[i] >= 8 * 48 && on_link(f, w.link)) {
            link = true;
            static int nlog;
            if (ENV("LARECOMP_LOG_FP") && nlog < 40 && renderer_present_count() % 120 == 0) {
                nlog++;
                LOG("[fp] Link's bones? v%d (%llu bytes) first bone at (%.2f, %.2f, %.2f), Link at (%.2f, %.2f, %.2f)\n", i,
                    (unsigned long long)size[i], f[3], f[7], f[11], w.link[0], w.link[1], w.link[2]);
            }
        }
    }
    if (camera && !link && ENV("LARECOMP_FP_LOG_NEAR") && renderer_present_count() % 60 == 0) {
        // (finding Link's other parts: blocks with any bone near him in draws that are kept)
        for (int i = 0; i < n; i++) {
            if (!addr[i] || size[i] < 48) continue;
            const float *f = (const float *)(uintptr_t)addr[i];
            int near = 0, first = -1, nb = (int)(size[i] / 48);
            for (int b = 0; b < nb && b < 256; b++) {
                const float *m = f + b * 12;
                float dx = m[3] - w.link[0], dy = m[7] - w.link[1], dz = m[11] - w.link[2];
                if (dx * dx + dz * dz < 2.0f && dy > -0.5f && dy < 3.0f && rotation_like(m)) { near++; if (first < 0) first = b; }
            }
            if (near) LOG("[fp] kept draw: v%d (%llu bytes) %d bones near Link, first #%d at (%.2f, %.2f, %.2f)\n", i,
                          (unsigned long long)size[i], near, first, f[first * 12 + 3], f[first * 12 + 7], f[first * 12 + 11]);
        }
    }
    return (camera && link && !ENV("LARECOMP_FP_SHOW_LINK")) | camera << 1;
}

// The sky and the haze (runtime/nvn/renderer.m fp_draw_sky), 24 floats: our camera's inverse rotation rows with the
// eye in their w; the projection's x and y scales, whether the depth image is upside down (LARECOMP_FP_SKY_FLIP) and
// indoors (dungeons, interiors: a dark vault, no haze); the projection's depth terms and the haze margin; then the part
// of the island the game has loaded. Measured 20/10 in Mabe Village: the game draws the 3x3 cells (15 x 12 units) around
// Link's - past them nothing, about 20 m away. The haze thickens over the last LARECOMP_FP_HAZE units (6) before that
// edge, into the sky's colour behind each point, so the island fades out instead of stopping dead.
// A fall into a hole or a pit: Link's feet drop below where they last stood still and the eye goes under the island
// (the grass from inside, then the world from below - Guillaume's screenshots, 20/10). The view fades to black from
// 1.8 to 2.8 below the floor he walks on (a ledge he jumps down from is nearer, and he stands again after it).
static float g_floor_y = -1e9f;  // the floor the ceiling stands on: where he last stood still, kept while it moves < 0.5
static float fall_fade(const float *link) {
    static float hist[24], ground = -1e9f;
    static int nh;
    static char lv[32];
    const char *l = game_level_name();
    if (strncmp(lv, l, sizeof lv - 1)) { snprintf(lv, sizeof lv, "%s", l); nh = 0; ground = -1e9f; g_floor_y = -1e9f; }
    hist[nh++ % 24] = link[1];
    if (nh >= 24) {
        float lo = 1e9f, hi = -1e9f;
        for (int i = 0; i < 24; i++) lo = fminf(lo, hist[i]), hi = fmaxf(hi, hist[i]);
        if (hi - lo < 0.05f) ground = link[1];
    }
    // Walking down a slope he never stands still: the floor followed only his stops, and going down a hill the view
    // went dark - the sky black a second after a turn to go down (Guillaume, 27/10). The floor follows him down as long
    // as he goes down slowly (a walk, not a drop), stays where it was while he drops fast.
    static float last_y = -1e9f;
    if (ground > -1e8f && last_y > -1e8f && last_y - link[1] < 0.06f && link[1] < ground) ground = link[1];
    last_y = link[1];
    // (the ceiling slid a little as he walked: it followed his feet each frame - Guillaume, 20/10)
    if (ground > -1e8f && fabsf(ground - g_floor_y) > 0.5f) g_floor_y = ground;
    if (ground < -1e8f || ENV("LARECOMP_FP_NO_FALL_DARK")) return 0;
    // (from 1.8 below: a ledge jumped down from is less - its drop darkened the view a moment; a hole goes on)
    float d = ground - link[1];
    return d <= 1.8f ? 0.0f : d >= 2.8f ? 1.0f : d - 1.8f;
}

// (android/xr_app.c's depth stereo) this frame's projection: a pixel's distance = p11 / (depth + p10), its depth in
// 0..1 (zero_one) or -1..1 (then 2 depth - 1)
// The game's own camera's projection (the view off in a headset: the view from above on its screen, in relief by its
// depth - android/xr_app.c): its scene block (slot 5, 320 bytes), noted by exec.c cbuf_info while the view is off.
static float g_game_lens[16];
static uint64_t g_game_lens_frame = UINT64_MAX;
void fp_note_game_block(int idx, const float *src, uint64_t range) {
    if (idx != 5 || range != 320 || !src || !perspective(src)) return;
    memcpy(g_game_lens, src + 16, 64), g_game_lens_frame = renderer_present_count();
}
bool fp_xr_depth(float *p10, float *p11, int *zero_one) {
    if (!fp_view_on()) {
        uint64_t f = renderer_present_count();
        if (g_game_lens_frame == UINT64_MAX || f - g_game_lens_frame > 2 || g_game_lens[11] == 0) return false;
        *p10 = g_game_lens[10], *p11 = g_game_lens[11], *zero_one = nvn_depth_mode() != 0;
        return true;
    }
    const Cam *c = NULL;
    for (int i = 0; i < g_ncam && !c; i++) if (g_cams[i].ok) c = &g_cams[i];
    if (!c || c->to[P_P][11] == 0) return false;
    *p10 = c->to[P_P][10], *p11 = c->to[P_P][11], *zero_one = nvn_depth_mode() != 0;
    return true;
}

// SGSR2 works in raw game-image coordinates, before the final vertical flip.
// Returned matrix is column-major unjittered previous VP * inverse(current VP).
bool fp_temporal_camera(float *matrix, float *jitter_uv, float *fov, bool *reset) {
    if (!fp_view_on()) return false;
    const Cam *c = NULL;
    for (int i = 0; i < g_ncam && !c; i++) if (g_cams[i].ok) c = &g_cams[i];
    if (!c || !g_xr_serial || nvn_depth_mode() == 0) return false;
    float p[16], cur[16], inv[16], delta[16];
    memcpy(p, c->to[P_P], sizeof p);
    p[2] += 2 * c->jitter[0]; p[6] -= 2 * c->jitter[1];
    mul4(p, c->to[P_V], cur);
    if (!inv4(cur, inv)) return false;
    static float prev[16]; static uint64_t frame = ~0ull; static char level[32];
    uint64_t now = renderer_present_count();
    const char *name = game_level_name();
    *reset = frame + 1 != now || strcmp(level, name) != 0;
    float difference = 0;
    for (int i = 0; i < 16; i++) difference += fabsf(prev[i] - cur[i]);
    if (difference > 10) *reset = true;
    mul4(*reset ? cur : prev, inv, delta);
    for (int r = 0; r < 4; r++) for (int col = 0; col < 4; col++) matrix[col * 4 + r] = delta[r * 4 + col];
    memcpy(prev, cur, sizeof prev); frame = now;
    snprintf(level, sizeof level, "%s", name);
    memcpy(jitter_uv, c->jitter, 2 * sizeof(float));
    *fov = fabsf(1 / p[0]);
    return true;
}

bool fp_sky_params(float *k) {
    const Cam *c = NULL;
    for (int i = 0; i < g_ncam && !c; i++) if (g_cams[i].ok) c = &g_cams[i];
    if (!c) return false;
    View w = view_now();
    for (int r = 0; r < 3; r++) {
        memcpy(k + r * 4, c->to[P_ROT] + r * 4, 12);
        k[r * 4 + 3] = c->to[P_EYE][r];  // (a headset's eye stands off the head's centre)
    }
    // o: the lens's off-centre terms (a headset's asymmetric eye), and LARECOMP_FP_SHOW_MV=1: the motion vectors the
    // Quest's space warp (AppSW) wants, drawn as colours - each pixel's world point (from the depth) seen with this frame's
    // view-projection and the last one's (pv: rows; P V, column vectors)
    k[32] = c->to[P_P][2], k[33] = c->to[P_P][6], k[34] = ENV("LARECOMP_FP_SHOW_MV") ? 1.0f : 0.0f;
    k[35] = ENV("LARECOMP_FP_SKY_Z") ? 1.0f : 0.0f;  // (test: the scene depth the sky pass reads, drawn: red = depth)
    {
        static float cur[16], prev[16];
        static uint64_t fr;
        float vp[16];
        mul4(c->to[P_P], c->to[P_V], vp);
        if (renderer_present_count() != fr) { memcpy(prev, cur, 64); fr = renderer_present_count(); }
        memcpy(cur, vp, 64);
        memcpy(k + 36, prev, 64);
    }
    extern const char *game_level_name(void);  // (runtime/hle/widescreen.c; "-" out of play)
    bool field = !strcmp(game_level_name(), "Field");
    k[12] = c->to[P_P][0], k[13] = c->to[P_P][5], k[14] = ENV("LARECOMP_FP_SKY_FLIP") ? 1.0f : 0.0f;
    k[15] = field ? 0.0f : 1.0f;
    if (ENV("LARECOMP_FP_DOOR_RED")) k[14] += 2;  // (test: the door in red)
    static float margin = -1;
    if (margin < 0) margin = envf("LARECOMP_FP_HAZE", 5);
    k[16] = c->to[P_P][10], k[17] = c->to[P_P][11], k[18] = margin, k[19] = nvn_depth_mode() != 0;
    if (!field) {  // indoors: the door, x, z, half width, height (a half width of 0: none)
        k[20] = g_door[0], k[21] = g_door[1];  // (the house is smaller than its cell: the mirrored wall stands just past
                                               // the mat, about 0.5 south of where he comes in - measured in Marin's)
        k[22] = g_door_known && !ENV("LARECOMP_FP_NO_DOOR") ? 0.8f : 0, k[23] = 1.9f;
        static uint64_t said;
        if (ENV("LARECOMP_LOG_FP") && renderer_present_count() / 120 != said / 120) {
            said = renderer_present_count();
            LOG("[fp] door x %g, wall z %g, half width %g, eye (%g, %g, %g)\n", k[20], k[21], k[22], w.eye[0], w.eye[1], w.eye[2]);
        }
    } else {
        float col = floorf(w.link[0] / 15), row = floorf(w.link[2] / 12);
        k[20] = (col - 1) * 15, k[21] = (col + 2) * 15, k[22] = (row - 1) * 12;
        k[23] = g_settings.fp_horizon && !ENV("LARECOMP_FP_NO_HORIZON") ? 1.0f : 0.0f;  // (outdoors: the painted range)
    }
    if (ENV("LARECOMP_FP_LOG_SKYK")) {
        static uint64_t said;
        if (renderer_present_count() / 120 != said / 120) {
            said = renderer_present_count();
            LOG("[fp] sky k: rows (%g %g %g | %g) (%g %g %g | %g) (%g %g %g | %g) f %g %g %g %g g %g %g %g %g\n", k[0], k[1], k[2],
                k[3], k[4], k[5], k[6], k[7], k[8], k[9], k[10], k[11], k[12], k[13], k[14], k[15], k[16], k[17], k[18], k[19]);
        }
    }
    if (ENV("LARECOMP_FP_LOG_CELLS")) {  // (research: the cells drawn, and where Link stands)
        static uint64_t said;
        if (renderer_present_count() / 120 != said / 120) {
            said = renderer_present_count();
            LOG("[fp] cells drawn:");
            for (int i = 0; i < 256; i++) if ((g_cells[0][i >> 5] | g_cells[1][i >> 5]) >> (i & 31) & 1) LOG(" %02d%c", i / 16 + 1, 'A' + i % 16);
            LOG(" | Link (%.2f, %.2f, %.2f) level %s\n", w.link[0], w.link[1], w.link[2], game_level_name());
        }
    }
    {   // cl, pal: indoors, a ceiling over the room and its walls carried up to it (the view from above never closed
        // them: Guillaume, 20/10, « il faut une solution pour les toits, c'est trop bizarre »). cl = floor (his feet),
        // height above it (LARECOMP_FP_CEIL), on, material (1 stone: dungeons, caves; 2 a house's plaster and boards);
        // pal = the walls' colour, a dungeon's own
        const char *lv = game_level_name();
        bool stone;
        float h = ceil_height(lv, &stone);
        static const float dungeon[11][3] = {{0.13f, 0.11f, 0.09f},  {0.20f, 0.09f, 0.06f}, {0.12f, 0.13f, 0.09f},
                                             {0.10f, 0.11f, 0.14f},  {0.07f, 0.11f, 0.15f}, {0.06f, 0.12f, 0.12f},
                                             {0.14f, 0.12f, 0.10f},  {0.13f, 0.12f, 0.11f}, {0.17f, 0.08f, 0.05f},
                                             {0.15f, 0.13f, 0.14f},  {0.12f, 0.08f, 0.14f}};  // (0: caves; 1-10: Lv01-Lv10)
        int d = !strncmp(lv, "Lv", 2) ? atoi(lv + 2) : 0;
        if (d < 0 || d > 10) d = 0;
        k[59] = fall_fade(w.link);  // (pal.w: the dark of a fall, see fall_fade; it also keeps the floor below)
        k[52] = g_floor_y > -1e8f ? g_floor_y : w.link[1], k[53] = h, k[54] = !field && !ENV("LARECOMP_FP_NO_CEIL") ? 1.0f : 0.0f, k[55] = stone ? 1.0f : 2.0f;
        if (stone) k[56] = dungeon[d][0], k[57] = dungeon[d][1], k[58] = dungeon[d][2];
        else k[56] = 0.30f, k[57] = 0.21f, k[58] = 0.12f;
    }
    {   // the haze's cells (256 bits): the block of whole cells the game is asked to keep loaded (rt_far_query), or its
        // own 7 x 7 around Link's cell - the haze is the edge of what is loaded, the same whichever way he looks. (It was
        // the cells whose ground had been drawn this frame or the last: a cell whose ground left the eye's field while
        // its cliffs stayed in it had them veiled in the sky's colour, and a step brought them back - « des textures en
        // blanc sur les falaises ... si je bouge un peu ça se réaffiche », Guillaume 27/10)
        uint32_t m[8] = {0};
        bool block = g_region > 3 && g_rc >= 0;
        int ring = block ? g_region : 3;
        int cc = block ? g_rc : (int)floorf(w.link[0] / CELL_W), cr = block ? g_rr : (int)floorf(w.link[2] / CELL_D);
        for (int r = cr - ring; r <= cr + ring; r++)
            for (int c = cc - ring; c <= cc + ring; c++)
                if (r >= 0 && c >= 0 && r < FIELD_CELLS && c < FIELD_CELLS) m[(r * 16 + c) >> 5] |= 1u << ((r * 16 + c) & 31);
        memcpy(k + 24, m, sizeof m);
    }
    return true;
}

// Effects the game hangs around its own camera, sized for the view from above, by fragment shader hash: in the
// first-person view they float in the air as hard-edged boxes. runtime/nvn/renderer.m leaves them out.
bool fp_skip_fs(uint64_t hash) {
    static const uint64_t k[] = {
        0xc42e882c08c5e5ebull,  // dungeon darkness around the camera (runtime/hle/widescreen.c)
        0xbee6b72c10d55f43ull,  // the dark covers the game lays over a dungeon's neighbouring rooms, at the walls' tops: seen edge on
                                // from the eye, a dark horizontal band at the horizon (« une barre à l'horizon », Guillaume 12/11;
                                // found 13/11 on his F9 dump in Key Cavern: pass 17, 7 draws of this shader, absent from pass 16)
    };
    if (!hash || !fp_view_on()) return false;
    for (size_t i = 0; i < sizeof k / sizeof k[0]; i++)
        if (k[i] == hash) return true;
    return false;
}

// Houses were built to be seen from above: the wall you come in by, always the south one, was never modelled, and in
// the first-person view it was a black void. The room's shell (walls and floor: the draws whose model matrix, a 256-byte
// vertex block, has no rotation and stands at the centre of the room's cell - measured 20/10 in Marin and Tarin's house,
// (7.5, 0, 6)) is drawn a second time mirrored across the room's middle (z): its north wall becomes the south one.
// runtime/nvn/renderer.m does the second draw (winding flipped, pushed back in depth so the mirrored floor never
// covers the real one). Returns the vertex slot of that model block, -1 for any other draw; *zc: the mirror plane.
int fp_mirror_draw(const uint64_t *addr, const uint64_t *size, int n, float *zc) {
    // (research only now, LARECOMP_FP_MIRROR=1: mirroring each room's north wall stood its furniture by the door and,
    //  in the shop, its counter flickering through the wall - one south wall for every house instead, fp_sky.h)
    if (!fp_view_on() || !ENV("LARECOMP_FP_MIRROR")) return -1;
    extern const char *game_level_name(void);
    const char *lv = game_level_name();
    if (!strcmp(lv, "Field") || !strncmp(lv, "Lv", 2) || !strcmp(lv, "-")) return -1;  // (houses only)
    View w = view_now();
    float cx = (floorf(w.link[0] / 15) + 0.5f) * 15, cz = (floorf(w.link[2] / 12) + 0.5f) * 12;
    for (int i = 0; i < n; i++) {
        if (!addr[i] || size[i] != 256) continue;
        const float *m = (const float *)(uintptr_t)addr[i];
        if (ENV("LARECOMP_FP_LOG_MIRROR") && renderer_present_count() % 60 == 0)
            LOG("[fp] mirror? v%d: %g %g %g %g | %g %g %g %g | %g %g %g %g (room centre %g %g, level %s)\n", i, m[0], m[1],
                m[2], m[3], m[4], m[5], m[6], m[7], m[8], m[9], m[10], m[11], cx, cz, lv);
        if (m[0] != 1 || m[1] != 0 || m[2] != 0 || m[4] != 0 || m[5] != 1 || m[6] != 0 || m[8] != 0 || m[9] != 0 || m[10] != 1)
            continue;
        if (fabsf(m[3] - cx) > 0.01f || fabsf(m[11] - cz) > 0.01f) continue;
        // (the plane half a unit south of the middle: across the middle the mirrored north wall stood at 9.8 in Marin's
        // house, just before the mat where he comes in (10) - he arrived behind it and saw its back)
        static float shift = -1e9f;
        if (shift < -1e8f) shift = envf("LARECOMP_FP_MIRROR_SHIFT", 0.5f);
        *zc = m[11] + shift;
        return i;
    }
    return -1;
}

// The mirrored shell's clip plane (runtime/nvn/renderer.m, the mirrored pass; the vertex shaders' clip distance is
// dot(plane, their clip position)): what lies past the wall - the north wall's mirror, about 0.5 south of the mat where
// Link came in - and LARECOMP_FP_MIRROR_CLIP (0.1: the frames hung on it) before it; the furniture standing against the
// north wall, which mirrored stood again by the door (Marin and Tarin's house: a shelf, a bowl, a plant), is cut off.
// The plane z >= zw in the world, carried into clip space by our view-projection: (VP^-1)^T (0, 0, 1, -zw). Unknown door
// (waking up in bed): no plane. LARECOMP_FP_MIRROR_CLIP_T=1 transposes (research).
bool fp_mirror_clip(float *out) {
    if (!g_door_known || !g_ncam || !g_cams[0].ok || ENV("LARECOMP_FP_NO_MIRROR_CLIP")) return false;
    static float eps = -1e9f;
    if (eps < -1e8f) eps = envf("LARECOMP_FP_MIRROR_CLIP", -0.5f);  // (measured in Marin's house: the wall stands 1.0 to 1.2 past the mat)
    float zw = g_door[1] + 0.5f - eps, pw[4] = {0, 0, 1, -zw}, inv[16], vp[16];
    memcpy(vp, g_cams[0].to[P_VP], sizeof vp);
    if (ENV("LARECOMP_FP_MIRROR_CLIP_T"))
        for (int i = 0; i < 4; i++) for (int j = 0; j < 4; j++) vp[i * 4 + j] = g_cams[0].to[P_VP][j * 4 + i];
    if (!inv4(vp, inv)) return false;
    for (int j = 0; j < 4; j++) out[j] = pw[0] * inv[j] + pw[1] * inv[4 + j] + pw[2] * inv[8 + j] + pw[3] * inv[12 + j];
    return true;
}

// The main camera's update, before it builds the frame's view from its eye (+0x320) and target (+0x3f0)
// (runtime/hle/widescreen.c rt_cam_begin, tools/patches.py CAM_BEGIN_SITE).
// LARECOMP_FP_CAMLOOK=d (test): the game camera put at Link's eye, aimed d units ahead where the first-person view
// looks - is what the game draws (the island's cells) chosen by this camera's view? Taken back the next frame when the
// game didn't rewrite them (no drift). (A first try wrote them at the fovy load, after the view is built: no effect,
// inconclusive. The camera's fields seen: fovy 0x564 in radians, near 0x568 = 0.1, far 0x56c = 100, aspect 0x570, a
// 45-degree rotation at 0x580.)
void fp_cam_begin(uint64_t cam) {
    behind_probe();
    static float d = -1, saved[6], written[6];
    if (d < 0) d = envf("LARECOMP_FP_CAMLOOK", 0);
    if (d <= 0) return;
    float *eye = (float *)(uintptr_t)(cam + 0x320), *tgt = (float *)(uintptr_t)(cam + 0x3f0);
    bool kept = true;
    for (int i = 0; i < 3; i++) kept = kept && eye[i] == written[i] && tgt[i] == written[3 + i];
    if (kept) for (int i = 0; i < 3; i++) eye[i] = saved[i], tgt[i] = saved[3 + i];
    if (!fp_view_on()) return;
    View w = view_now();
    for (int i = 0; i < 3; i++) saved[i] = eye[i], saved[3 + i] = tgt[i];
    float cp = cosf(w.pitch), fw[3] = {sinf(w.yaw) * cp, sinf(w.pitch), -cosf(w.yaw) * cp};
    for (int i = 0; i < 3; i++) eye[i] = w.eye[i], tgt[i] = w.eye[i] + fw[i] * d;
    for (int i = 0; i < 3; i++) written[i] = eye[i], written[3 + i] = tgt[i];
}

// The render camera object at [game camera + 0x2a8] (dumped 20/10): view matrix +0x20 (4 columns of float4), its
// inverse +0x60 (the eye at +0x90), projection +0xa0; its frustum (f_700112f650 on +0xe0): a world box, min +0xe0 and
// max +0xf0, then six planes from +0x100, 0x20 apart - outward normal at +0, distance at +0x10, a point inside when
// n.p + d <= 0 - their count at +0x1c0; near / far / aspect / fovy at +0x1d4 / +0x1d8 / +0x1dc / +0x1e0 (far: 100).
// fp_frustum_ready runs right after the game computes a camera's frustum (runtime/hle/widescreen.c rt_frustum_ready).
// The eye's culling (on with the view; measured 20/10: the cells the game draws are chosen with this frustum, read
// after this point - a frustum taking everything draws all 49 cells it has loaded): the game culls with what Link's eye
// sees, LARECOMP_FP_PLANES_FAR units deep (400) - the island ahead, as far as it is loaded, and nothing behind.
// LARECOMP_FP_PLANES: 0 = the game's own frustum, 1 = one taking everything (research), 2 = the eye's (default).
// Outdoors only: in a dungeon the eye sees through the walls into the other rooms, all loaded (Tail Cave 08D: 775 draws
// a frame instead of 546, 30 fps instead of 58, measured 20/10), where the game's camera from above keeps to the room.
static bool eye_culling_here(void) {
    extern const char *game_level_name(void);
    const char *l = game_level_name();
    return l && !strcmp(l, "Field");
}
// Indoors (dungeons, caves, houses) the eye's frustum too, at a short reach: LARECOMP_FP_INDOOR_DEPTH units (22, a room
// and a half; 0: the game's own culling). The game's frustum, widened to 150 degrees so that the next rooms are drawn,
// also drew what is behind him and to his sides - 35 to 40 fps in Tail Cave, the GPU at 17 ms a frame at the lowest
// resolution (Guillaume's game, 20/10). With the eye's at 400 it drew every room ahead through the walls (30 fps).
static float indoor_depth(void) {
    static float d = -1;
    if (d < 0) d = envf("LARECOMP_FP_INDOOR_DEPTH", 22);
    return d;
}
static bool eye_frustum_here(void) { return eye_culling_here() || indoor_depth() > 0; }
static uint64_t g_game_cam;
static uint64_t g_cull_frustum;  // the main camera's frustum, once replaced by the view's this frame (+0xe0 of it)
static float g_cull_eye[3], g_cull_link[3], g_cull_head[3];
static bool g_cull_behind;
static float behind_pos(const float *f, const float *eye, const float *link, float *pos);

// The frame controller. The far scenery costs draws - a cell brings 40 to 60, and above ~1,100 draws a frame the M1
// Pro's serial frame no longer holds 60 (measured 20/10) - so the far block follows the frame time as the dynamic
// resolution does: after two late windows (an average frame period over LARECOMP_FP_DETAIL_LATE_MS, 17.8; the headset
// sets 16.9) a ring of cells comes off it, down to the game's own 7 x 7; after `need` good windows one goes back
// (doubled when one given back is late within 6 s; the 3 s after a ring comes back are not judged: its cells load and
// their shaders build). Whole cells at the block's edge: the island ends nearer or further, never with holes.
// Only when the block is down to the game's own and frames are still late (the Mac at Martha's Bay: about 3,000 draws a
// frame with the 49 cells, 20/10) does the detail coarsen: beyond LARECOMP_FP_DETAIL_NEAR units (24) a model whose
// sphere looks smaller than the detail (its radius over its distance, at most LARECOMP_FP_DETAIL_MAX, 0.05) is left out
// (fp_cull_sphere). A far wall of small blocks then loses them one by one (« les monts au loin s'affichent par
// morceaux », Guillaume 20/10), so it is the last resort and the first thing given back.
// LARECOMP_FP_DETAIL=<ratio> fixes the detail (0: off), LARECOMP_FP_DETAIL_START is the detail at each new level (0);
// the eye's frustum is LARECOMP_FP_PLANES_FAR units deep (400: past any block). LARECOMP_LOG_FP logs the changes.
// The frame's COST leads it too (12/11/2026, Beta 6, his route down the hill by Mabe, Raccoon): the render thread's CPU
// time per frame climbed from 7 to 15.5 ms as the view opened (about 2,000 draws), and at 15.5 the game tipped into its
// 30 fps mode - held there by the pacer (runtime/nvn/pace.c), the frames 33 ms apart for seconds: « ça ralentit à
// fond ». The period stays 16.7 ms until that moment, so the late windows above came after it, every time. So each
// window also takes what a frame cost - the render thread's CPU time and the game thread's (half of it in the game's
// 30 fps mode: its second world step) per presented frame -, a ring comes off over LARECOMP_FP_COST_MS (12) on
// average, and one goes back only when the cost it would have - the part over LARECOMP_FP_COST_BASE (4 ms: the scene
// without far cells) grown as the block's cells - stays under LARECOMP_FP_COST_ADD_MS (11).
static float g_detail = -1;  // the detail ratio
static void detail_tick(void) {
    static float fixed = -2, kmax, late_ms, start;
    if (fixed < -1) {
        fixed = envf("LARECOMP_FP_DETAIL", -1), kmax = envf("LARECOMP_FP_DETAIL_MAX", 0.05f);
        late_ms = envf("LARECOMP_FP_DETAIL_LATE_MS", 17.8f), start = envf("LARECOMP_FP_DETAIL_START", 0);
    }
    if (g_region < 0) g_region = region_start();
    if (g_detail < 0) g_detail = fixed >= 0 ? fixed : start;
    static double last, sum, finer_at = -100, settle_until;
    static int n, good, slow, need = 6;
    static char level[32];
    static float cost_ms = -2, cost_add_ms, cost_base;
    if (cost_ms < -1)
        cost_ms = envf("LARECOMP_FP_COST_MS", 12), cost_add_ms = envf("LARECOMP_FP_COST_ADD_MS", 11), cost_base = envf("LARECOMP_FP_COST_BASE", 4);
    extern uint64_t rq_render_cpu_ns(void);         // nvn/rqueue.c: the render thread's CPU time so far
    extern uint64_t plat_thread_cpu_ns(void);       // (this thread's: the game's)
    extern uint64_t renderer_present_count(void);
    static uint64_t w_rc, w_gc, w_frames;            // (the window's start)
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    double now = ts.tv_sec + ts.tv_nsec * 1e-9;
    const char *lv = game_level_name();
    if (lv && strncmp(level, lv, sizeof level - 1)) {  // (a new level, a warp: start afresh, its loading hitches ignored)
        snprintf(level, sizeof level, "%s", lv);
        g_detail = fixed >= 0 ? fixed : start, good = slow = n = 0, sum = 0, need = 6;
        settle_until = now + 3;
    }
    if (now < settle_until) { last = now; w_frames = 0; return; }
    if (last > 0 && now - last >= 0.25) {  // (a pause, a menu, a loading gap: no verdict on a split window)
        sum = 0, n = good = slow = 0, last = now, settle_until = now + 2;
        w_frames = 0;
        return;
    }
    if (!w_frames) w_rc = rq_render_cpu_ns(), w_gc = plat_thread_cpu_ns(), w_frames = renderer_present_count() + 1;
    if (last > 0) sum += now - last, n++;
    last = now;
    if (n < 30) return;
    double avg = sum / n * 1000;
    sum = 0, n = 0;
    double cost = 0;  // (ms a frame: the busier of the two threads)
    {
        uint64_t rc = rq_render_cpu_ns(), gc = plat_thread_cpu_ns(), fr = renderer_present_count() + 1;
        uint64_t frames = fr > w_frames ? fr - w_frames : 1;
        double r = rc > w_rc ? (rc - w_rc) / 1e6 / frames : 0, g = gc > w_gc ? (gc - w_gc) / 1e6 / frames : 0;
        if (avg > 25) g /= 2;  // (the game's 30 fps mode: two world steps a frame, one at 60)
        cost = r > g ? r : g;
        w_rc = rc, w_gc = gc, w_frames = fr;
    }
    {   // (a window in which the game read its files - cells loading, a ring given back, a warp - is no verdict unless
        //  really slow, over 25 ms a frame: after a warp the block went down to the game's own (27/10), but never judged
        //  while walking - cells load all the time then - it stayed at 30-35 fps for ten seconds, Guillaume's headset)
        extern uint64_t g_romfs_bytes;
        static uint64_t seen;
        uint64_t read = g_romfs_bytes - seen;
        seen = g_romfs_bytes;
        if (read > 512 * 1024 && avg < 25 && cost <= cost_ms) {  // (too costly is judged all the same: walking, cells load all the time)
            static double said;
            if (ENV("LARECOMP_LOG_FP") && now - said > 5)
                said = now, LOG("[fp] far block: a window with %llu KB read (%.1f ms a frame), not judged\n", (unsigned long long)(read >> 10), avg);
            good = slow = 0;
            return;
        }
    }
    float pk = g_detail;
    int pr = g_region;
    slow = avg > late_ms ? slow + 1 : 0;
    bool field = lv && !strcmp(lv, "Field");  // (the block is the island's; in a dungeon, a house: the detail only)
    // (a window whose frames cost too much: a ring off at once, before the game tips to 30 - the rings only: the detail,
    //  which takes models away one by one, still waits for frames that are late)
    bool heavy = cost > cost_ms && field && g_region > 3;
    if (heavy) slow = slow < 2 ? 2 : slow;
    // what a ring more would cost: the part over the scene's own grown as the block's cells ((2r+3)^2 / (2r+1)^2)
    float grow = (float)((2 * g_region + 3) * (2 * g_region + 3)) / (float)((2 * g_region + 1) * (2 * g_region + 1));
    double cost_more = cost_base + (cost > cost_base ? cost - cost_base : 0) * grow;
    if (slow >= 2) {  // a ring off first; the detail only once the block is the game's own
        if (field && g_region > 3) g_region--;
        else if (fixed < 0 && g_detail < kmax) g_detail = fminf(kmax, g_detail <= 0 ? 0.012f : g_detail * (avg > 22 ? 1.6f : 1.3f));
        good = 0, slow = 0;
        if (now - finer_at < 6) need = need < 64 ? need * 2 : 64;
    } else if (!slow && ++good >= need) {  // the detail back first, then a ring - if what it would cost leaves room
        if (fixed < 0 && g_detail > start) g_detail = g_detail / 1.2f < 0.01f ? start : g_detail / 1.2f;
        else if (field && g_region < region_max() && cost_more < cost_add_ms) g_region++, settle_until = now + 3;
        good = 0, finer_at = now;
    }
    if (g_detail != pk || g_region != pr)  // (always: a handful a minute at most, and what a run says about its lags)
        LOG("[fp] far block %d -> %d rings, detail %.3f -> %.3f (frame %.1f ms apart, cost %.1f ms)\n", pr, g_region, pk, g_detail,
            avg, cost);
}

// The game's sphere-against-frustum test (f_7001130090, tools/patches.py SPHERE_TEST_SITE), with the eye's frustum:
// true leaves the model out (the detail above).
bool fp_cull_sphere(uint64_t frustum, uint64_t sphere) {
    static float near = -1;
    if (near < 0) near = envf("LARECOMP_FP_DETAIL_NEAR", 24);
    float k = g_detail;
    if (frustum != g_cull_frustum || !g_cull_frustum || sphere < 0x7000000000ull || sphere >= 0x7400000000ull) return false;
    const float *c = (const float *)(uintptr_t)sphere;
    float dx = c[0] - g_cull_eye[0], dy = c[1] - g_cull_eye[1], dz = c[2] - g_cull_eye[2], rad = c[4];
    float d2 = dx * dx + dy * dy + dz * dz;
    // the eye inside a small model - a person or a cucco against him: not drawn while it is (its inside showed, his
    // head in theirs looking down, Guillaume 20/10). Not his own: their centres stand on him (arms, shield, sword)
    // ... nor right under it: a person's head just below his eyes looking down, as if he were in them (20/10). Not his own
    // models (centred on him), nor what he holds up (above the eye)
    float lx = c[0] - g_cull_link[0], lz = c[2] - g_cull_link[2], hd2 = dx * dx + dz * dz;
    bool his = lx * lx + lz * lz < 0.4f * 0.4f;  // (his shield and sword in hand stand ~0.3 from his centre)
    static int nomerge = -1;  // (LARECOMP_FP_NO_MERGE=1: drawn anyway - a test)
    if (nomerge < 0) nomerge = ENV("LARECOMP_FP_NO_MERGE") != NULL;
    // (persons up to 2.5 units across: Dampé's is larger than 1.6, the limit first set, and his inside showed, 27/10)
    // (the people are kept out of the eye instead - their bones, fp_draw_class - and stay: small models
    //  only, a cucco, a pot)
    // (on a flat screen too since 27/10: the eye is pushed out of them there as well, and Dampé vanished as Link came
    //  up to him - « tous les correctifs du casque devront être appliqués sur le plat », Guillaume)
    float merge_max = !ENV("LARECOMP_FP_NO_PUSH") ? 0.6f : 2.5f;
    if (!nomerge && rad < merge_max && !his && (d2 < rad * rad * 0.8f || (hd2 < 0.45f * 0.45f && dy < 0.2f))) return true;
    if (g_cull_behind && !his && rad < 4.0f) {  // (the view from behind: a model between the camera and his head)
        float s[3] = {g_cull_head[0] - g_cull_eye[0], g_cull_head[1] - g_cull_eye[1], g_cull_head[2] - g_cull_eye[2]};
        float ss = s[0] * s[0] + s[1] * s[1] + s[2] * s[2];
        float tt = ss > 0 ? (dx * s[0] + dy * s[1] + dz * s[2]) / ss : 0;
        if (tt > 0 && tt < 1) {
            float px = dx - s[0] * tt, py = dy - s[1] * tt, pz = dz - s[2] * tt;
            if (px * px + py * py + pz * pz < rad * rad * 0.7f) return true;
        }
    }
    return k > 0 && d2 > near * near && rad * rad < d2 * k * k;
}

void fp_frustum_ready(uint64_t rc) {
    static int mode = -1;
    static float far;
    if (mode < 0) mode = (int)envf("LARECOMP_FP_PLANES", 2), far = envf("LARECOMP_FP_PLANES_FAR", 400);
    if (!g_game_cam || rc != *(const uint64_t *)(uintptr_t)(g_game_cam + 0x2a8)) return;
    g_cull_frustum = 0;
    if (!mode || !fp_view_on() || !eye_frustum_here() || fp_flight() < 0.7f) return;  // (on the flight: the game's)
    const float reach = eye_culling_here() ? far : indoor_depth();
    float *box = (float *)(uintptr_t)(rc + 0xe0), *pl = (float *)(uintptr_t)(rc + 0x100);
    if (mode == 1) {
        for (int i = 0; i < 6; i++) pl[i * 8] = pl[i * 8 + 1] = pl[i * 8 + 2] = 0, pl[i * 8 + 4] = -1e9f;
        for (int k = 0; k < 3; k++) box[k] = -1e6f, box[4 + k] = 1e6f;
        return;
    }
    // The view's own field (a headset's four tangents, or the vertical fov and the game's aspect), 15 % wider: what
    // the eye sees at the edge must not pop
    View w = view_now();
    const float *rp = (const float *)(uintptr_t)(rc + 0xa0);
    float t[4];
    uint32_t xs;
    const XrView *x = xr_now(&xs);
    if (x) memcpy(t, x->tan, sizeof t);
    else {
        float fov = envf("LARECOMP_FP_FOV", g_settings.fp_fov > 0 ? (float)g_settings.fp_fov : 60) * (float)M_PI / 180.0f;
        float ty = tanf(fov / 2), aspect = rp[0] > 0 ? rp[5] / rp[0] : 16.0f / 9.0f;
        t[0] = -ty * aspect, t[1] = ty * aspect, t[2] = ty, t[3] = -ty;
    }
    for (int k = 0; k < 4; k++) t[k] *= 1.15f;
    float pitch = w.pitch;  // (with a headset: the body's tilt from the stick, the head on top - view_matrix_xr)
    float cp = cosf(pitch), sp = sinf(pitch), cy = cosf(w.yaw), sy = sinf(w.yaw);
    float f[3] = {sy * cp, sp, -cy * cp}, r[3] = {cy, 0, sy}, u[3] = {-sy * sp, cp, cy * sp};
    if (x) {  // (the head's orientation on top of the body's yaw: the eye's own axes)
        float m[9], b[3][3] = {{r[0], r[1], r[2]}, {u[0], u[1], u[2]}, {-f[0], -f[1], -f[2]}};
        quat_basis(x->q, m);
        float ax[3][3];
        for (int c = 0; c < 3; c++)
            for (int k = 0; k < 3; k++) ax[c][k] = m[0 * 3 + c] * b[0][k] + m[1 * 3 + c] * b[1][k] + m[2 * 3 + c] * b[2][k];
        for (int k = 0; k < 3; k++) r[k] = ax[0][k], u[k] = ax[1][k], f[k] = -ax[2][k];
    }
    float n[6][3];
    for (int k = 0; k < 3; k++) {
        n[0][k] = -f[k], n[1][k] = f[k];                                   // near (at the eye), far
        n[2][k] = -r[k] + t[0] * f[k], n[3][k] = r[k] - t[1] * f[k];       // left, right
        n[4][k] = u[k] - t[2] * f[k], n[5][k] = -u[k] + t[3] * f[k];       // top, bottom
    }
    float apex[3];  // (the view from behind: the camera's place, back along the look)
    memcpy(apex, w.eye, sizeof apex);
    if (x) {  // (a headset: the eye where the head has moved it, as view_matrix_xr puts it - leaning in, the eye was
        //  inside a person while his eyes, tested, were not: « je rentrais dans son corps », Dampé, 27/10)
        const float unit = fp_xr_unit();
        float B[9];
        body_basis(&w, B);
        for (int k = 0; k < 3; k++) apex[k] += (B[k * 3] * x->pos[0] + B[k * 3 + 1] * x->pos[1] + B[k * 3 + 2] * x->pos[2]) / unit;
    }
    g_cull_behind = g_settings.fp_behind && fp_flight() >= 0.9999f;
    if (g_cull_behind) {
        behind_pos(f, w.eye, w.link, apex);
        g_cull_head[0] = w.link[0], g_cull_head[1] = w.eye[1] - 0.15f, g_cull_head[2] = w.link[2];
    }
    for (int i = 0; i < 6; i++) {
        float l = sqrtf(n[i][0] * n[i][0] + n[i][1] * n[i][1] + n[i][2] * n[i][2]);
        for (int k = 0; k < 3; k++) n[i][k] /= l;
        float q[3];
        for (int k = 0; k < 3; k++) q[k] = apex[k] + (i == 1 ? f[k] * reach : 0);
        pl[i * 8] = n[i][0], pl[i * 8 + 1] = n[i][1], pl[i * 8 + 2] = n[i][2];
        pl[i * 8 + 4] = -(n[i][0] * q[0] + n[i][1] * q[1] + n[i][2] * q[2]);
    }
    memcpy(g_cull_eye, apex, sizeof g_cull_eye);
    memcpy(g_cull_link, w.link, sizeof g_cull_link);
    g_cull_frustum = rc + 0xe0;
    detail_tick();
    if (eye_culling_here()) {  // (the cells shown: rt_cells_frustum - the box below written first)
        if (!g_eye_frustum) g_eye_frustum = (uint64_t)(uintptr_t)hle_alloc(0x100);
        g_eye_frustum_frame = UINT64_MAX;  // (until the copy is whole)
    }
    for (int k = 0; k < 3; k++) box[k] = box[4 + k] = w.eye[k];  // the box: the eye and the far corners
    for (int c = 0; c < 4; c++)
        for (int k = 0; k < 3; k++) {
            float v = w.eye[k] + reach * (f[k] + (c & 1 ? t[1] : t[0]) * r[k] + (c & 2 ? t[2] : t[3]) * u[k]);
            if (v < box[k]) box[k] = v;
            if (v > box[4 + k]) box[4 + k] = v;
        }
    if (g_eye_frustum && eye_culling_here()) {
        memcpy((void *)(uintptr_t)g_eye_frustum, box, 0xe8);
        g_eye_frustum_frame = renderer_present_count();
    }
}

// The game camera object (tools/patches.py main camera fovy load, runtime/hle/widescreen.c rt_fovy_cam): fovy at
// +0x564, aspect at +0x570. LARECOMP_FP_DUMP_CAM: its first 0x600 bytes as floats every 120 frames (finding its eye and
// target, to load the island where the first-person view looks).
void fp_game_camera(uint64_t cam) {
    g_game_cam = cam;
    {   // LARECOMP_FP_CAMSHIFT=d (test): the game camera's eye (+0x320) and target (+0x3f0) moved d units along where the
        // first-person view looks - does the island load around the game camera? Last frame's move is taken back first
        // when the game didn't rewrite them (no drift).
        static float d = -1, applied[3], written[3];
        if (d < 0) d = envf("LARECOMP_FP_CAMSHIFT", 0);
        float *eye = (float *)(uintptr_t)(cam + 0x320), *tgt = (float *)(uintptr_t)(cam + 0x3f0);
        if (d > 0) {
            if (eye[0] == written[0] && eye[1] == written[1] && eye[2] == written[2])
                for (int i = 0; i < 3; i++) eye[i] -= applied[i], tgt[i] -= applied[i];
            View w = view_now();
            float sh[3] = {0, 0, 0};
            if (fp_view_on()) sh[0] = sinf(w.yaw) * d, sh[2] = -cosf(w.yaw) * d;
            for (int i = 0; i < 3; i++) eye[i] += sh[i], tgt[i] += sh[i], applied[i] = sh[i], written[i] = eye[i];
        }
    }
    static uint64_t last;
    uint64_t f = renderer_present_count();
    if (!ENV("LARECOMP_FP_DUMP_CAM") || f / 120 == last / 120) return;
    last = f;
    const float *v = (const float *)(uintptr_t)cam;
    LOG("[fp] game camera %llx at frame %llu:", (unsigned long long)cam, (unsigned long long)f);
    for (int i = 0; i < 0x600 / 4; i++)
        if (v[i] == v[i] && fabsf(v[i]) > 1e-3f && fabsf(v[i]) < 1e5f) LOG(" %x=%g", i * 4, v[i]);
    LOG("\n");
    uint64_t rc = *(const uint64_t *)(uintptr_t)(cam + 0x2a8);  // (the object the camera builds its view into)
    if (rc < 0x7000000000ull || rc >= 0x7400000000ull) return;
    v = (const float *)(uintptr_t)rc;
    LOG("[fp] its +0x2a8 object %llx:", (unsigned long long)rc);
    for (int i = 0; i < 0x400 / 4; i++)
        if (v[i] == v[i] && fabsf(v[i]) > 1e-3f && fabsf(v[i]) < 1e5f) LOG(" %x=%g", i * 4, v[i]);
    LOG("\n");
}

// The right stick's click (R3) switches the first-person view on and off in play, any time: back to the view from above
// for a puzzle, the map, a cutscene, and back in (Guillaume, 20/10: going from one view to the other "was a given").
// runtime/hle/input.c, each pad read; the click doesn't reach the game while the view is on or was just switched.
uint64_t fp_buttons(uint64_t b) {
    static bool held;
    static uint64_t switched;
    bool now = (b >> 5) & 1;  // (BTN_RSTICK)
    uint64_t f = renderer_present_count();
    // a click while the stick leans past 60 % is the thumb pressing as it looks, not a toggle: his view went off in the
    // middle of a look, the right stick then turned nothing (his recorded session, 20/10: R3 down with the stick at -32250)
    if (now && !held && g_rs_raw > 20000) { held = true; return b & ~(1ull << 5); }
    if (now && !held && game_in_play() && !getenv("LARECOMP_FP")) {
        g_settings.first_person = !g_settings.first_person;
        extern void settings_save(void);
        settings_save();
        switched = f;
        if (!ENV("LARECOMP_FP_NO_FLIGHT")) g_fl_to = g_settings.first_person, g_fl_t0 = fl_now(), g_fl_wait = g_settings.first_person ? f : 0;
        // (in: the view looks the way Link faces - through his eyes, whichever way that is; the flight swings round to
        //  come in behind his head. His root bone tells; the game's camera's way if his skeleton wasn't seen lately)
        float fx = g_face[0], fz = g_face[1];
        if (g_settings.first_person && f - g_head_frame <= 10 && fx * fx + fz * fz > 1e-4f) {
            g_yaw = atan2f(fx, -fz), g_pitch = 0;
            if (ENV("LARECOMP_LOG_FP")) LOG("[fp] in by R3: Link faces %.0f degrees\n", g_yaw * 180 / M_PI);
        } else if (g_settings.first_person && g_game_cam) {
            const float *ce = (const float *)(uintptr_t)(g_game_cam + 0x320), *ct = (const float *)(uintptr_t)(g_game_cam + 0x3f0);
            float dx = ct[0] - ce[0], dz = ct[2] - ce[2];
            if (dx * dx + dz * dz > 1e-6f) g_yaw = atan2f(dx, -dz), g_pitch = 0;
        }
        LOG("[fp] view %s (R3)\n", g_settings.first_person ? "on" : "off");
    }
    held = now;
    if (g_settings.first_person || f - switched < 30) b &= ~(1ull << 5);
    {   // the left stick's click (L3), the view on: the camera at his eyes or behind him (F5 on the keyboard)
        static bool lheld;
        bool l3 = (b >> 4) & 1;
        if (l3 && !lheld && g_settings.first_person && game_in_play()) fp_behind_toggle();
        lheld = l3;
        if (g_settings.first_person) b &= ~(1ull << 4);
    }
    if (g_on) b |= atomic_load(&g_mbtn);  // (mouse buttons, while the mouse is captured)
    const uint64_t act = 0xf;  // A, B, X, Y
    static uint64_t prev;
    if (g_on && !ENV("LARECOMP_FP_NO_TURN")) {
        uint64_t down = b & act & ~prev;
        if (down && f >= g_turn_until && f >= g_emit_until) g_turn_until = f + 3, g_emit_mask = 0;
        prev = b & act;
        if (f < g_turn_until) { g_emit_mask |= b & act; b &= ~act; }  // (held back while he turns)
        else if (f < g_turn_until + 2 && g_emit_until <= g_turn_until) g_emit_until = g_turn_until + 2;
        if (f >= g_turn_until && f < g_emit_until) b |= g_emit_mask;  // (a tap: sent after the turn)
    } else prev = b & act;
    return b;
}

// The action prompt ("Lift", "Read", "Talk" - Cm_HUD's L_Interact_00) is placed by the game over its target as its own
// camera sees it: in the first-person view that's anywhere. It goes to the middle, a little below the centre, where the
// eye already is (runtime/hle/mirror.c rt_prompt_x / rt_prompt_y, tools/patches.py PROMPT_POS_SITE).
// LARECOMP_FP_PROMPT_Y: its height in the layout's units (LARECOMP_LOG_FP logs the game's own x, y).
bool fp_prompt(float *x, float *y) {
    if (!fp_view_on()) return false;
    static float py = -1e9f;
    if (py < -1e8f) py = envf("LARECOMP_FP_PROMPT_Y", -120);
    static uint64_t said;
    if (ENV("LARECOMP_LOG_FP") && renderer_present_count() / 60 != said / 60) {
        said = renderer_present_count();
        LOG("[fp] action prompt at (%g, %g) from the game\n", *x, *y);
    }
    *x = 0, *y = py;
    return true;
}

bool fp_show_hands(void) { return g_settings.fp_hands; }  // (runtime/nvn/renderer.m)

// ------------------------------------------------------------------ each draw, both renderers
// (runtime/nvn/renderer.m draw, runtime/nvn/vulkan/exec.c draw_) a, s: the blocks the vertex shader reads (n slots),
// count: the draw's index count. Returns false: leave the draw out - Link from inside his head: his head, hair, body,
// ears and boots; his arms and what he holds (shield, sword, items) stay and are moved in front of the view
// (viewmodel). His parts by index count, measured 20/10 one at a time from behind him: 4419 hat and body, 3831 / 1320 /
// 918 hair, 744 ears, 960 / 1464 boots, 5004 arms, 5574 shield, 3078 / 396 the sword's blade and hilt.
// LARECOMP_FP_HIDE_COUNTS="n,n" replaces the list; LARECOMP_FP_LOG_LINK logs them. *mirror: the slot of a room shell's
// model block to draw again mirrored across z = *mirror_z (-1: none; fp_mirror_draw). *camera: drawn with the game
// camera (its depth target is a candidate for the scene's: fp_depth_vote).
// LARECOMP_FP_CELL_GRID=<frame>,<frame>... (ascending): at those frames, the frame's draws counted by the island's cell
// they stand in (the first model matrix among their blocks: its translation), logged as the 16 x 16 grid of the Field -
// what is drawn where, the far scenery included, every pass (shadows too) - with the draws placed outside it and those
// without a model matrix. LARECOMP_FP_CELL_GRID_ROWS=<first>-<last> (1-based): each draw of those rows listed too
// (place, index count, whether the camera's pass)
static void cells_log(const uint64_t *a, const uint64_t *s, int n, uint64_t count, bool camera, uint64_t fs) {
    static long want[64];
    static int nw = -1, wi;
    static uint64_t at = UINT64_MAX;
    static int grid[FIELD_CELLS][FIELD_CELLS], outside, none;
    if (nw < 0) {
        nw = 0;
        for (const char *p = ENV("LARECOMP_FP_CELL_GRID"); p && *p && nw < 64;) {
            want[nw++] = strtol(p, (char **)&p, 10);
            while (*p == ',') p++;
        }
    }
    if (!nw) return;
    uint64_t f = renderer_present_count();
    if (at != UINT64_MAX && f != at) {  // (the frame is over: its grid)
        LOG("[fp] cells drawn at frame %llu (%d outside the island, %d without a model matrix):\n     A   B   C   D   E   F   G   H   I   J   K   L   M   N   O   P\n",
            (unsigned long long)at, outside, none);
        for (int r = 0; r < FIELD_CELLS; r++) {
            char line[128];
            int k = snprintf(line, sizeof line, "%02d ", r + 1);
            for (int c = 0; c < FIELD_CELLS; c++)
                k += grid[r][c] ? snprintf(line + k, sizeof line - k, "%4d", grid[r][c]) : snprintf(line + k, sizeof line - k, "   .");
            LOG("%s\n", line);
        }
        memset(grid, 0, sizeof grid), outside = none = 0, at = UINT64_MAX;
    }
    if (at == UINT64_MAX) {
        while (wi < nw && (uint64_t)want[wi] < f) wi++;
        if (wi >= nw || (uint64_t)want[wi] != f) return;
        at = f, wi++;
    }
    for (int i = 0; i < n; i++) {
        if (!a[i] || s[i] < 48 || s[i] > 512) continue;
        const float *m = (const float *)(uintptr_t)a[i];
        float sc = 0;  // (the model's scale: rotation_like takes 0.5 to 2 only - a big model, the egg, is scaled further)
        bool rows = true;
        for (int k = 0; k < 3 && rows; k++) {
            float l = sqrtf(m[k * 4] * m[k * 4] + m[k * 4 + 1] * m[k * 4 + 1] + m[k * 4 + 2] * m[k * 4 + 2]);
            rows = l > 0.05f && l < 200.0f, sc = fmaxf(sc, l);
        }
        if (!rows || !(fabsf(m[3]) < 1e5f && fabsf(m[7]) < 1e5f && fabsf(m[11]) < 1e5f)) continue;
        int c = (int)floorf(m[3] / CELL_W), r = (int)floorf(m[11] / CELL_D);
        if (c < 0 || r < 0 || c >= FIELD_CELLS || r >= FIELD_CELLS) outside++;
        else grid[r][c]++;
        static int r0 = -2, r1;
        if (r0 == -2) { r0 = -1; const char *e = ENV("LARECOMP_FP_CELL_GRID_ROWS"); if (e) sscanf(e, "%d-%d", &r0, &r1), r0--, r1--; }
        if (r0 >= 0 && r >= r0 && r <= r1)
            LOG("[fp]   %02d%c (%.1f %.1f %.1f) %llu indices, scale %.2f, fs %016llx%s\n", r + 1, 'A' + c, m[3], m[7], m[11],
                (unsigned long long)count, sc, (unsigned long long)fs, camera ? ", camera" : "");
        return;
    }
    none++;
}
// A SkySea piece's painted cards (fp_skysea_box): its rocks stay in first person - Mt Tamaranch's summit, the cliffs of
// the north edge - but its flat cards of sea and sky, made for the camera from above (24 and 48 indices, measured
// 27/10), stood behind the island as a wall of saturated blue; the first-person sky (fp_sky.h) is there instead.
// A piece's draws: their model matrix unrotated at the north edge, x 30 + 60 k, y 0, z 0. LARECOMP_FP_SKYSEA_SKIP=
// <index count>,... replaces the list ("0": none left out).
static bool skysea_skip(const uint64_t *a, const uint64_t *s, int n, uint64_t count) {
    static uint64_t skip[16];
    static int ns = -1;
    if (ns < 0) {
        ns = 0;
        const char *e = ENV("LARECOMP_FP_SKYSEA_SKIP");
        for (const char *p = e ? e : "24,48"; *p && ns < 16;) {
            skip[ns++] = strtoull(p, (char **)&p, 10);
            while (*p == ',') p++;
        }
    }
    if (!ns) return false;
    int k = 0;
    while (k < ns && skip[k] != count) k++;
    if (k == ns) return false;
    for (int i = 0; i < n; i++) {
        if (!a[i] || s[i] < 48 || s[i] > 512) continue;
        const float *m = (const float *)(uintptr_t)a[i];
        if (m[0] != 1 || m[5] != 1 || m[10] != 1 || m[1] != 0 || m[2] != 0 || m[4] != 0 || m[6] != 0 || m[8] != 0 || m[9] != 0) return false;
        float q = (m[3] - 30) / 60;
        return m[7] == 0 && m[11] == 0 && q >= 0 && q <= 3 && q == floorf(q);
    }
    return false;
}
static bool g_body_draw;  // (this draw is his body seen from behind or in the flight: exec.c, a headset's layer)
bool fp_body_draw_current(void) { return g_body_draw; }
bool fp_draw(const uint64_t *a, const uint64_t *s, int n, uint64_t count, uint64_t fs, int *mirror, float *mirror_z,
             bool *camera) {
    *mirror = -1, *camera = false;
    g_body_draw = false;
    if (count == 4419) fp_link_body(a, s, n);
    int cls = fp_draw_class(a, s, n);
    *camera = (cls & 2) != 0;
    cells_log(a, s, n, count, *camera, fs);
    if (fp_view_on() && skysea_skip(a, s, n, count)) return false;
    if (cls & 2) *mirror = fp_mirror_draw(a, s, n, mirror_z);
    if (*mirror >= 0) {  // LARECOMP_FP_MIRROR_SKIP="n,n": draws of these index counts not mirrored (research: which draws
                         //  of the shell are furniture); LARECOMP_FP_LOG_MIRROR logs the counts mirrored
        static int nskip = -1;
        static uint64_t skip[16];
        if (nskip < 0) {
            nskip = 0;
            const char *e = getenv("LARECOMP_FP_MIRROR_SKIP");
            for (const char *p = e; p && *p && nskip < 16;) { skip[nskip++] = strtoull(p, (char **)&p, 10); while (*p == ',') p++; }
        }
        for (int i = 0; i < nskip; i++) if (count == skip[i]) *mirror = -1;
        // (research) LARECOMP_FP_MIRROR_LARGEST=1: only draws of 60 % of the largest of the shell (14 draws in Marin and
        // Tarin's house, 120 to 8,550 indices) mirrored - WRONG as a rule (20/10): the north wall isn't the largest, it went
        // with the furniture. Leaving the furniture out needs the mirrored pass cut at the wall plane (an oblique near
        // plane), A-FAIRE/lasrecomp-portage-3d-feuille-de-route.md.
        static char lv_seen[48];
        static uint64_t top;
        extern const char *game_level_name(void);
        const char *lv = game_level_name();
        if (lv && strncmp(lv_seen, lv, sizeof lv_seen - 1)) snprintf(lv_seen, sizeof lv_seen, "%s", lv), top = 0;
        if (count > top) top = count;
        if (ENV("LARECOMP_FP_MIRROR_LARGEST") && count * 10 < top * 6) *mirror = -1;
        static uint64_t said;
        if (ENV("LARECOMP_FP_LOG_MIRROR") && renderer_present_count() / 120 != said / 120 + 0 && renderer_present_count() % 120 < 2)
            LOG("[fp] mirrored draw: count %llu%s\n", (unsigned long long)count, *mirror < 0 ? " (skipped)" : ""), said = 0;
    }
    int vm = 0;
    if ((cls & 1) && g_fl_dist > 0.45f && (fp_flight() < 0.9999f || g_settings.fp_behind)) {  // (flying, or the view from
        fp_viewmodel_draw(0);                                                                   //  behind: Link seen)
        g_body_draw = true;
        return true;
    }
    if (cls & 1) {
        static int nhide = -1;
        static uint64_t hide[16];
        if (nhide < 0) {
            nhide = 0;
            const char *e = getenv("LARECOMP_FP_HIDE_COUNTS");
            for (const char *p = e ? e : "4419,3831,1320,918,744,960,1464"; *p && nhide < 16;) {
                hide[nhide++] = strtoull(p, (char **)&p, 10);
                while (*p == ',') p++;
            }
        }
        // (lying in bed, his arms aren't held up in front of the view)
        static int no_vm = -1, log_link;
        if (no_vm < 0) no_vm = getenv("LARECOMP_FP_NO_VIEWMODEL") != NULL, log_link = getenv("LARECOMP_FP_LOG_LINK") != NULL;
        bool hidden = no_vm || !fp_link_upright() || !fp_show_hands();
        for (int i = 0; i < nhide; i++) hidden |= count == hide[i];
        // (a headset's view reaches 55 degrees below the eye: the arms, which carry the top of the torso, then float
        // under the look - seen 20/10 on the Quest. Only what Link holds stays until the hands follow the controllers)
        uint32_t xs;
        if (count == 5004 && xr_now(&xs)) hidden = true;
        if (log_link && renderer_present_count() % 120 == 0)
            LOG("[fp] Link draw f%llu: count %llu fs %llx%s\n", (unsigned long long)renderer_present_count(),
                (unsigned long long)count, (unsigned long long)fs, hidden ? "" : " (in view)");
        if (hidden) { fp_viewmodel_draw(0); return false; }
        vm = count == 3078 || count == 396 ? 2 : 1;
    }
    fp_viewmodel_draw(vm);
    return true;
}
int fp_viewmodel_current(void) { return g_vm; }  // (runtime/nvn/vulkan/exec.c: a held part's palette gets its own copy)

// The scene's depth: the target most of the frame's camera draws go to (in Bottle Grotto a later pass has a full-size
// depth target of its own, nearly empty: taken for the scene's, the vault covered everything - measured 20/10). Both
// renderers vote with each camera draw's depth target; fp_scene_depth gives this frame's leader so far (the composite
// may come before the frame count moves), else the last frame's, and the frame it was chosen in.
static const void *g_dt[8], *g_depth;
static unsigned g_dn[8];
static int g_dk;
static uint64_t g_df, g_depth_frame;
void fp_depth_vote(const void *t) {
    if (!t) return;
    uint64_t f = renderer_present_count();
    if (f != g_df) {
        unsigned bn = 0;
        const void *bt = NULL;
        for (int i = 0; i < g_dk; i++) if (g_dn[i] > bn) bn = g_dn[i], bt = g_dt[i];
        if (bt) g_depth = bt, g_depth_frame = g_df;
        if (ENV("LARECOMP_LOG_FP_STATE") && g_df % 20 == 0) {  // (which depth targets the camera draws went to)
            LOG("[fp state] f%llu depth targets:", (unsigned long long)g_df);
            for (int i = 0; i < g_dk; i++) LOG(" %p x%u%s", g_dt[i], g_dn[i], g_dt[i] == bt ? " (scene)" : "");
            LOG("\n");
        }
        g_dk = 0, g_df = f;
    }
    int i = 0;
    while (i < g_dk && g_dt[i] != t) i++;
    if (i == g_dk && g_dk < 8) g_dt[g_dk] = t, g_dn[g_dk++] = 0;
    if (i < 8) g_dn[i]++;
    unsigned bn = 0;
    for (int j = 0; j < g_dk; j++) if (g_dn[j] > bn) bn = g_dn[j], g_depth = g_dt[j], g_depth_frame = f;
}
const void *fp_scene_depth(uint64_t *frame) { if (frame) *frame = g_depth_frame; return g_depth; }
void fp_scene_depth_forget(void) { g_depth = NULL; }  // (the sky shader failed: no more tries)
