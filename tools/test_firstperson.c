// Bench for runtime/hle/firstperson.c without the game: a made-up camera from above in the game's blocks (the scene
// block, slot 5, 320 bytes; the view block, slot 1, 1024 bytes), then what the first-person view makes of them - the
// camera math, the memo of fp_patch_ubo, a headset's eye (fp_xr_view). Seconds, no game files.
// cc -O1 -Iruntime -I/opt/homebrew/opt/sdl3/include tools/test_firstperson.c -o build/test_firstperson -lm && build/test_firstperson
#include "../runtime/hle/firstperson.c"

#include <stdio.h>

Settings g_settings;
static uint64_t t_frame = 100;
static uint32_t t_depth;
static float t_link[64];  // a fake PlayerCollision: position at +0x80
uint64_t renderer_present_count(void) { return t_frame; }
int game_in_play(void) { return 1; }
uint64_t doorjump_player(uint64_t frame) { (void)frame; return (uint64_t)(uintptr_t)t_link; }
uint32_t nvn_depth_mode(void) { return t_depth; }
bool widescreen_side_view_zone(void) { return false; }
const char *game_level_name(void) { return "Field"; }
void settings_save(void) {}
double output_scale_dynamic_factor(void) { return 1.0; }
void *hle_alloc(uint64_t n) { return calloc(1, n); }

static int fails;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("FAIL %s:%d ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static void xform(const float *m, const float *p, float *o) {  // rows x column (x, y, z, 1)
    for (int i = 0; i < 4; i++) o[i] = m[i * 4] * p[0] + m[i * 4 + 1] * p[1] + m[i * 4 + 2] * p[2] + m[i * 4 + 3];
}

// The game's camera: eye (0, 20, 15) looking at the origin, 30 degrees, near 1, far 300; the scene block's layout.
static void game_cam(float *v, float *p, bool zero_one) {
    View w = {.eye = {0, 20, 15}, .yaw = 0, .pitch = -atanf(20.0f / 15.0f)};
    view_matrix(&w, v);
    float n = 1, f = 300, t = 1 / tanf(15 * (float)M_PI / 180);
    memset(p, 0, 64);
    p[0] = t / (16.0f / 9.0f), p[5] = t, p[14] = -1;
    p[10] = zero_one ? -f / (f - n) : -(f + n) / (f - n);
    p[11] = zero_one ? -f * n / (f - n) : -2 * f * n / (f - n);
}
static void scene_block(float *u, bool zero_one) {
    memset(u, 0, 320);
    game_cam(u, u + 16, zero_one);
    mul4(u + 16, u, u + 32);
    inv4(u, u + 48);
    u[48 + 3] = u[48 + 7] = u[48 + 11] = 0;  // (inverse rotation at 192)
    u[68] = 0, u[69] = 20, u[70] = 15;       // position at 272
    u[75] = 0.25f;                           // something else, to keep
}
static void view_block(float *u, bool zero_one) {  // VP at 0, position at 32, view 3x4 at 36, projection at 92
    float v[16], p[16];
    memset(u, 0, 1024);
    game_cam(v, p, zero_one);
    mul4(p, v, u);
    u[32] = 0, u[33] = 20, u[34] = 15;
    memcpy(u + 36, v, 48);
    memcpy(u + 92, p, 64);
    u[200] = 7.5f;  // (something else)
}

static void frame_on(float yaw, float pitch) {
    g_settings.first_person = true;
    g_yaw = yaw, g_pitch = pitch;
    int32_t lx = 0, ly = 0, rx = 0, ry = 0;
    t_frame++;
    fp_input(&lx, &ly, &rx, &ry);
}

static void run(bool zero_one) {
    t_depth = zero_one;
    t_link[32] = 2, t_link[33] = 0, t_link[34] = -3;  // Link at (2, 0, -3)
    fp_xr_off();
    frame_on((float)M_PI / 2, -10 * (float)M_PI / 180);  // turned east, looking 10 degrees down
    CHECK(fp_view_on(), "view on");
    float src[80], dst[80];
    scene_block(src, zero_one);
    memcpy(dst, src, sizeof dst);
    fp_patch_ubo(0, 5, dst, 320, src, 320);
    View w = view_now();
    float o[4], e[3] = {w.eye[0], w.eye[1], w.eye[2]};
    xform(dst, e, o);
    CHECK(fabsf(o[0]) < 1e-4 && fabsf(o[1]) < 1e-4 && fabsf(o[2]) < 1e-4, "eye -> view origin: %f %f %f", o[0], o[1], o[2]);
    float north[3] = {e[0], e[1], e[2] - 5};
    xform(dst, north, o);
    CHECK(o[0] < -4.9f, "north is on the left when facing east: x %f", o[0]);
    float pv[16];
    mul4(dst + 16, dst, pv);
    CHECK(near_eq(dst + 32, pv, 16), "view-projection = projection x view");
    float pn[3] = {0, 0, -0.05f}, pf[3] = {0, 0, -400}, c[4];
    xform(dst + 16, pn, c);
    CHECK(fabsf(c[2] / c[3] - (zero_one ? 0.0f : -1.0f)) < 1e-4, "near depth %f", c[2] / c[3]);
    xform(dst + 16, pf, c);
    CHECK(fabsf(c[2] / c[3] - 1) < 1e-4, "far depth %f", c[2] / c[3]);
    CHECK(fabsf(dst[16 + 5] / dst[16] - 16.0f / 9.0f) < 1e-4, "aspect kept");
    CHECK(fabsf(dst[68] - e[0]) < 1e-4 && fabsf(dst[69] - e[1]) < 1e-4 && fabsf(dst[70] - e[2]) < 1e-4, "position");
    CHECK(dst[75] == 0.25f, "unknown value kept");

    // the memo: the same block again gives the same bytes, without a search
    float again[80];
    memcpy(again, src, sizeof again);
    unsigned m0 = g_count[4];
    fp_patch_ubo(1, 5, again, 320, src, 320);
    CHECK(!memcmp(again, dst, sizeof dst) && g_count[4] == m0 + 1, "memo: same answer (%u -> %u)", m0, g_count[4]);

    // the view block (slot 1): the same camera found at its own offsets
    static float vsrc[256], vdst[256];
    view_block(vsrc, zero_one);
    memcpy(vdst, vsrc, sizeof vdst);
    fp_patch_ubo(0, 1, vdst, 1024, vsrc, 1024);
    CHECK(near_eq(vdst, dst + 32, 16), "view block: view-projection");
    CHECK(near_eq(vdst + 36, dst, 12), "view block: view 3x4");
    CHECK(near_eq(vdst + 92, dst + 16, 16), "view block: projection");
    CHECK(vdst[200] == 7.5f, "view block: unknown value kept");

    // a light's camera (its own place, orthographic): untouched
    float ortho[80], before[80];
    memset(ortho, 0, sizeof ortho);
    View lw = {.eye = {10, 30, 5}, .yaw = 0.3f, .pitch = -1.2f};
    view_matrix(&lw, ortho);
    for (int i = 0; i < 16; i++) ortho[16 + i] = i % 5 == 0 ? 0.01f : 0;
    ortho[31] = 1;
    mul4(ortho + 16, ortho, ortho + 32);
    memcpy(before, ortho, sizeof before);
    fp_patch_ubo(0, 5, ortho, 320, ortho, 320);
    CHECK(!memcmp(ortho, before, sizeof ortho), "orthographic block untouched");

    // a headset's eye: identity orientation, 32 mm to the right, an asymmetric field
    float q[4] = {0, 0, 0, 1}, pos[3] = {0.032f, 0, 0}, tan4[4] = {-1.0f, 0.6f, 0.5f, -0.4f};
    fp_xr_view(q, pos, tan4);
    frame_on(0, 0);  // facing north
    w = view_now();
    memcpy(dst, src, sizeof dst);
    fp_patch_ubo(0, 5, dst, 320, src, 320);
    const float *P = dst + 16;
    CHECK(fabsf(P[0] - 1.25f) < 1e-5 && fabsf(P[2] + 0.25f) < 1e-5 && fabsf(P[5] - 2 / 0.9f) < 1e-4 &&
          fabsf(P[6] - 0.1f / 0.9f) < 1e-4, "asymmetric lens: %f %f %f %f", P[0], P[2], P[5], P[6]);
    float eye_r[3] = {w.eye[0] + 0.032f / 2.5f, w.eye[1], w.eye[2]};
    xform(dst, eye_r, o);
    CHECK(fabsf(o[0]) < 1e-4 && fabsf(o[1]) < 1e-4 && fabsf(o[2]) < 1e-4, "eye 32 mm right of the head: %f %f %f", o[0], o[1], o[2]);
    float edge[3] = {eye_r[0] + 0.6f * 10, eye_r[1] + 0.5f * 10, eye_r[2] - 10};  // the field's top right corner
    xform(dst + 32, edge, c);
    CHECK(fabsf(c[0] / c[3] - 1) < 1e-4 && fabsf(c[1] / c[3] - 1) < 1e-4, "top right corner -> ndc (1, 1): %f %f",
          c[0] / c[3], c[1] / c[3]);
    fp_xr_off();

    // off: nothing changes
    g_settings.first_person = false;
    t_frame += 30;
    int32_t lx = 0, ly = 32767, rx = 1000, ry = 0;
    CHECK(!fp_input(&lx, &ly, &rx, &ry) && lx == 0 && ly == 32767 && rx == 1000, "off: input untouched");
    memcpy(dst, src, sizeof dst);
    fp_patch_ubo(0, 5, dst, 320, src, 320);
    CHECK(!memcmp(dst, src, sizeof dst), "off: block untouched");
    CHECK(fp_cull_fovy(30) == 30 && fp_cull_aspect_scale() == 1, "off: culling untouched");
}

int main(void) {
    run(false);
    run(true);
    printf(fails ? "%d FAILED\n" : "all first-person checks passed\n", fails);
    return fails != 0;
}
