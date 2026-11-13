// Start screen, shown before the game boots (user 2026-10-08, styled after Dusklight's launcher): PLAY (the vanilla
// game), RANDOMIZER (‹ New Seed / a seed ›: user 2026-11-09), SETTINGS, QUIT, game-files status at the lower left.
// Nothing of the game ships with LAS Recomp: on first launch the user picks the folder with their own game files (1.0.0, 1.0.1 or 1.1.0), we check
// the main module's build id (runtime/game_addresses/versions.h), copy the files and build the game's data image; PLAY stays disabled until that worked. The background is a frame of the game's own title movie (from the user's files) once extracted.
//
// Skipped with --play, LARECOMP_NO_LAUNCHER=1, or any scripted test run (LARECOMP_INPUT): tests boot straight in
// (with LARECOMP_NO_LAUNCHER, set by tools/game_slot.sh, the app exits rather than show it: runtime/main.c).
// The in-game menu's "Title screen" comes back here without leaving the process (runtime/main.c reboots the game).
#if defined(__APPLE__) && !defined(LARECOMP_VULKAN)  // (the Mac draws with Metal, unless built with VK=1)
#define UI_METAL 1
#endif
#ifdef UI_METAL
#import <Metal/Metal.h>
#import <QuartzCore/CAMetalLayer.h>
#endif
#include <SDL3/SDL.h>
#include <ctype.h>
#include <dirent.h>
#include <limits.h>
#include <pthread.h>
#include <set>
#include <string>
#include <unordered_map>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>
#include <algorithm>
#include <atomic>

#include "imgui.h"
#include "imgui_internal.h"  // (SetKeyOwner)
#include "i18n.h"
#include "../itd_intro.h"
#include "option_rows.h"
#include "osk.h"
#include "pad_nav.h"
#include "plastic.h"
#include "ios_export_ui.h"
#include "live_preview.h"
#include "menu.h"
#include "mobile/touch_ui.h"
static TouchUi g_touch;  // (touch screens: the start screen's pages)
#ifdef UI_METAL
#include "imgui_impl_metal.h"
#else
#include "imgui_impl_vulkan.h"
#endif
#include "imgui_impl_sdl3.h"

extern "C" {
bool itd_headset_output(const char *name);  // (runtime/services/audio.c: PC VR's sound on the PC)
SDL_AudioDeviceID itd_pc_output(void);
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libswscale/swscale.h>
#include "platform.h"
#include "platform/ios_sdl.h"  // (iOS: SDL's file dialogs and SDL_OpenURL as the Files app's)
#if PLAT_TOUCH
#include "mobile/touch_safe.h"
#endif
#include "paths.h"
#include "gamebuild.h"
#include "build_cache.h"
#include "game_addresses.h"
#include "gamelib.h"
#include "iosexport.h"
#include "romfs_build.h"
#ifdef __ANDROID__
#include "platform/android_storage.h"
#endif
#include "settings.h"
#include "input_bind.h"
#include "menu_sfx.h"
#include "nvn/shader_prep.h"
bool controls_ui_took_back(void);  // ui/controls_ui.mm: B went back a step on Configure inputs
bool rando_tab_took_back(void);   // ui/rando_tab.mm: B went back a step in the right column
bool rando_tab_close_pressed(void);  // ui/rando_tab.mm (touch screens: the X is in its own pane)
float menu_page_corner(int tab, float scale);  // ui/menu.mm (touch screens: a page's icon for its title)
void ui_window_init(void);
SDL_Window *ui_window(void);
#ifdef UI_METAL
void *ui_metal_layer(void);
void ui_metal_hud_apply(void);
void *ui_metal_device(void);
#else
// runtime/nvn/vulkan: window frames and ImGui plumbing
bool vk_frame_begin(const float clear[4]);
void vk_frame_end(const char *shot_path);
void *vk_frame_cmd(void);
bool vk_imgui_backend_init(void);
void vk_imgui_backend_shutdown(void);
void vk_imgui_fonts_rebuild(void);
uint64_t vk_imgui_texture_rgba(const void *rgba, int w, int h);
uint64_t vk_imgui_texture_rgba_ex(const void *rgba, int w, int h, uint64_t *image_out);
void vk_imgui_texture_write(uint64_t image, const void *rgba, int w, int h);
#endif
}
#ifdef UI_METAL
#import <ImageIO/ImageIO.h>
#import <UniformTypeIdentifiers/UniformTypeIdentifiers.h>

// (test) BGRA texture -> PNG (the game renderer's saver isn't up yet before the game boots)
static void save_png(id<MTLTexture> tex, const char *path) {
    size_t w = tex.width, h = tex.height;
    std::vector<uint8_t> px(w * h * 4);
    [tex getBytes:px.data() bytesPerRow:w * 4 fromRegion:MTLRegionMake2D(0, 0, w, h) mipmapLevel:0];
    CGColorSpaceRef cs = CGColorSpaceCreateDeviceRGB();
    CGContextRef ctx = CGBitmapContextCreate(px.data(), w, h, 8, w * 4, cs,
                                             kCGImageAlphaNoneSkipFirst | kCGBitmapByteOrder32Little);
    CGImageRef img = CGBitmapContextCreateImage(ctx);
    CFURLRef url = CFURLCreateFromFileSystemRepresentation(NULL, (const UInt8 *)path, strlen(path), false);
    CGImageDestinationRef dest = CGImageDestinationCreateWithURL(url, (__bridge CFStringRef)UTTypePNG.identifier, 1, NULL);
    if (dest) {  // (NULL when the file can't be created, e.g. its folder isn't there: CFRelease(NULL) traps)
        CGImageDestinationAddImage(dest, img, NULL);
        CGImageDestinationFinalize(dest);
        CFRelease(dest);
    } else fprintf(stderr, "[launcher] can't write %s\n", path);
    CFRelease(url), CGImageRelease(img), CGContextRelease(ctx), CGColorSpaceRelease(cs);
}
#endif

// The release tag this build is (also names the release files; Makefile / tools/bundle_macos.sh / make_dmg.sh read
// it). Under the hood every release is a semantic version (0.<n>.<m> during the beta: .0 releases bring features, the
// rest are fixes). Tags: beta_<n> and beta_<n>.<m> ("Beta <n>.<m>", = 0.<n>.<m>) up to Beta 5; from Beta 6
// v0.<n>.<m> ("Beta <n>", "Beta <n>.<m>"); after the beta v<major>.<minor>.<patch> ("1.0.0"). A
// test build (a pre-release for a few players to try a fix) is test-<name>: the start screen says "Test build <name>",
// and it previews LARECOMP_TEST_OF - the update check offers that release (or a later one) as soon as it's out, so its
// players move to it.
#define LARECOMP_VERSION "beta_6"
#define LARECOMP_TEST_OF "beta_6"
// Experimental builds (.github/workflows/experimental.yml sets these): the build's time (YYYYMMDDHHMM, UTC; 0: not an
// experimental build). They only look for a newer experimental build of their own platform, never a normal release.
#define LARECOMP_EXPERIMENTAL_STAMP 0LL
// An experimental platform that joins the normal releases: from this release on, its experimental builds offer it
// (and any later one) ahead of a newer experimental build. Android ships in Beta 6.
#ifdef __ANDROID__
#define LARECOMP_EXPERIMENTAL_UNTIL "beta_6"
#else
#define LARECOMP_EXPERIMENTAL_UNTIL ""
#endif
#if defined(_WIN32) && defined(_M_ARM64)
#define LARECOMP_PLATFORM "windows-arm64"
#elif defined(_WIN32)
#define LARECOMP_PLATFORM "windows"
#elif defined(__APPLE__)
#define LARECOMP_PLATFORM "macOS"
#elif defined(__ANDROID__)
#define LARECOMP_PLATFORM "android"
#elif defined(__aarch64__)
#define LARECOMP_PLATFORM "linux-aarch64"
#else
#define LARECOMP_PLATFORM "linux-x86_64"
#endif
extern "C" const char *larecomp_version(void) { return LARECOMP_VERSION; }  // (the shader caches' tag)

// ------------------------------------------------------------------ update check
// The start screen asks GitHub for the project's releases once per launch (a background curl, which macOS and
// Windows 10+ ship; on Android the activity fetches it) and offers the best one if it outranks this build (runtime/ui/update_tags.h): any v1.0.0+ above
// every beta, betas by number, a beta's fixes releases (beta_<n>.<m>, v0.<n>.<m>) between it and the next beta, and
// v0.<n>.<m> the same as beta_<n>.<m>; other tags (the alpha_* ones) are ignored. (Beta 5 and earlier only know
// beta_<n>: they never offer a hotfix; builds before Beta 5.3 don't know v0.<n>.<m> either.) Offline / no curl /
// anything unexpected: nothing is shown.
#define RELEASES_URL "https://api.github.com/repos/peiikai/LASRecomp/releases?per_page=100"
#define RELEASE_PAGE "https://github.com/peiikai/LASRecomp/releases/tag/"

#include "update_tags.h"  // tag_rank, tag_label

static std::string g_update_tag;  // a newer release's tag (g_update_lock)
static pthread_mutex_t g_update_lock = PTHREAD_MUTEX_INITIALIZER;

static void *update_thread(void *) {
    std::string body;
    if (const char *t = getenv("LARECOMP_UPDATE_TAGS")) {  // (test) "beta_2,v1.0.0": these releases, no network
        for (const char *p = t; *p;) {
            const char *c = strchr(p, ',');
            std::string t(p, c ? c - p : strlen(p));  // (LAS.Recomp-*: an asset name)
            body += (t.compare(0, 11, "LAS.Recomp-") ? "\"tag_name\": \"" : "\"name\": \"") + t + "\"\n";
            p = c ? c + 1 : p + strlen(p);
        }
    } else {
#if PLAT_IOS
        // (no curl to run on iOS)
        @autoreleasepool {
            NSMutableURLRequest *rq = [NSMutableURLRequest requestWithURL:[NSURL URLWithString:@RELEASES_URL]
                                                              cachePolicy:NSURLRequestReloadIgnoringLocalCacheData
                                                          timeoutInterval:15];
            [rq setValue:@"application/vnd.github+json" forHTTPHeaderField:@"Accept"];
            [rq setValue:@"LASRecomp/" LARECOMP_VERSION forHTTPHeaderField:@"User-Agent"];
            dispatch_semaphore_t done = dispatch_semaphore_create(0);
            __block NSData *got = nil;
            [[[NSURLSession sharedSession] dataTaskWithRequest:rq completionHandler:^(NSData *d, NSURLResponse *r, NSError *err) {
                if (!err && [(NSHTTPURLResponse *)r statusCode] == 200) got = d;
                dispatch_semaphore_signal(done);
            }] resume];
            dispatch_semaphore_wait(done, DISPATCH_TIME_FOREVER);
            if (!got) return NULL;
            body.assign((const char *)got.bytes, std::min<size_t>(got.length, 8u << 20));
        }
#else
#ifdef _WIN32
        const char *sr = getenv("SystemRoot");
        std::string curl = std::string(sr ? sr : "C:\\Windows") + "\\System32\\curl.exe";
#else
        std::string curl = "/usr/bin/curl";
#endif
        std::string out = std::string(rt_data_root()) + "/cache/releases.json";
        mkdir((std::string(rt_data_root()) + "/cache").c_str(), 0755);
#ifdef __ANDROID__
        (void)curl;  // (Android has no curl: the activity fetches it, runtime/platform/android_storage.c)
        if (!android_fetch(RELEASES_URL, out.c_str(), "LASRecomp/" LARECOMP_VERSION)) { remove(out.c_str()); return NULL; }
#else
        const char *av[] = {curl.c_str(), "-fsSL", "--max-time", "15", "-H", "Accept: application/vnd.github+json",
                            "-A", "LASRecomp/" LARECOMP_VERSION, RELEASES_URL, NULL};
        PlatProc p = plat_spawn(av, out.c_str(), false);
        if (!p || plat_proc_wait(p, true) != 0) { remove(out.c_str()); return NULL; }
#endif
        if (FILE *f = fopen(out.c_str(), "rb")) {
            char buf[8192];
            size_t n;
            while ((n = fread(buf, 1, sizeof buf, f)) > 0 && body.size() < (8u << 20)) body.append(buf, n);
            fclose(f);
        }
        remove(out.c_str());
#endif
    }
    std::string best;
    // the newest release above this rank (a test build ranks just below the release it previews; other test builds
    // rank -1: never offered). An experimental build only takes the release its platform joins (or a later one).
    long long best_rank = LARECOMP_EXPERIMENTAL_STAMP ? (LARECOMP_EXPERIMENTAL_UNTIL[0] ? tag_rank(LARECOMP_EXPERIMENTAL_UNTIL) - 1 : LLONG_MAX)
                        : strncmp(LARECOMP_VERSION, "test-", 5) ? tag_rank(LARECOMP_VERSION) : tag_rank(LARECOMP_TEST_OF) - 1;
    for (size_t at = 0; best_rank < LLONG_MAX && (at = body.find("\"tag_name\"", at)) != std::string::npos;) {
        size_t q1 = body.find('"', body.find(':', at) + 1), q2 = q1 == std::string::npos ? q1 : body.find('"', q1 + 1);
        if (q2 == std::string::npos) break;
        std::string tag = body.substr(q1 + 1, q2 - q1 - 1);
        if (tag_rank(tag) > best_rank) best_rank = tag_rank(tag), best = tag;
        at = q2;
    }
    if (LARECOMP_EXPERIMENTAL_STAMP && best.empty() && newest_experimental(body, LARECOMP_PLATFORM) > LARECOMP_EXPERIMENTAL_STAMP)
        best = "experimental";
    if (!best.empty()) fprintf(stderr, "[launcher] update available: %s (this build %s)\n", best.c_str(), LARECOMP_VERSION);
    pthread_mutex_lock(&g_update_lock);
    g_update_tag = best;
    pthread_mutex_unlock(&g_update_lock);
    return NULL;
}

// ------------------------------------------------------------------ game files
static std::string g_root;  // extracted game: <root>/exefs, <root>/romfs

static bool file_exists(const std::string &p) {
    struct stat st;
    return stat(p.c_str(), &st) == 0;
}

// The game's RomFS (the same in every supported version) has exactly ROMFS_FILES files: a copy with fewer is incomplete (a copy or unzip that
// stopped partway) and the game crashes on the first one it misses (issues #24, #35). Only the number ships.
// Their sizes add up to game_romfs_bytes() (per version): every file there with another total is a damaged or
// changed file (issue #391). The game still plays then; the start screen says so. Files that aren't the game's are
// counted but their bytes left out (issue #553: an extra file beside the game's isn't a changed one).
// With check (base: where the path inside romfs starts), the files that aren't the game's are named in the log, the
// first ROMFS_ODD_LOGGED of them (check->odd counts them all), and copies of the game code at the romfs root
// (game_romfs_exefs_copy, game: the game folder) are left out of the count and the bytes (check->copies).
enum { ROMFS_FILES = 3836, ROMFS_ODD_LOGGED = 20 };
struct RomfsCheckWalk { GameRomfsCheck *check; size_t base; int odd; const char *game; int copies; };
static int count_files(const std::string &dir, int depth = 0, uint64_t *bytes = NULL, RomfsCheckWalk *check = NULL) {
    DIR *d = depth < 12 ? opendir(dir.c_str()) : NULL;
    if (!d) return 0;
    int n = 0;
    while (struct dirent *e = readdir(d)) {
        if (e->d_name[0] == '.') continue;  // (".", "..", .DS_Store)
        std::string p = dir + "/" + e->d_name;
        struct stat st;
        if (stat(p.c_str(), &st) != 0) continue;
        if (S_ISDIR(st.st_mode)) n += count_files(p, depth + 1, bytes, check);
        else if (S_ISREG(st.st_mode)) {
            if (check && depth == 0 && game_romfs_exefs_copy(check->game, e->d_name, (uint64_t)st.st_size)) {
                check->copies++;
                continue;
            }
            n++;
            uint64_t size = (uint64_t)st.st_size;
            if (!check) {
                if (bytes) *bytes += size;
            } else if (!game_romfs_check_file(check->check, p.c_str() + check->base, size, bytes) &&
                       check->odd++ < ROMFS_ODD_LOGGED)
                fprintf(stderr, "[launcher] romfs: not one of the game's files: %s (%llu bytes)\n", p.c_str() + check->base,
                        (unsigned long long)st.st_size);
        }
    }
    closedir(d);
    return n;
}
// The copied romfs' file count and bytes, counted once (the checks below run every frame): -1 = count again (game
// files set up, deleted)
static int g_romfs_count = -1;
static uint64_t g_romfs_bytes, g_romfs_want;  // (want: this version's total, 0 unknown)
static int g_romfs_missing;  // the game's files not there by name and size (a renamed or moved file: same count)
static std::atomic<bool> g_build_cache_changed{true};
static std::atomic<int> g_files_changes{0};  // (the version label on the start screen is read again)
static void game_files_changed(void) { g_romfs_count = -1; g_build_cache_changed = true; ++g_files_changes; }
// The setup page redraws every frame; read the build cache's disk state only after the files/build state changes.
static bool g_resumable, g_has_build_cache;
static void read_build_cache(void) {
    if (!g_build_cache_changed.exchange(false)) return;
    g_resumable = build_cache_resumable(g_root.c_str());
    g_has_build_cache = build_cache_present(g_root.c_str());
}
static bool resumable_build(void) { read_build_cache(); return g_resumable; }
static bool has_build_cache(void) { read_build_cache(); return g_has_build_cache; }

// (kept English, as the lines below: shown() puts them in the menus' language)
static const char *const INCOMPLETE_FMT = LN("Game files incomplete, %d of %d files");
static const char *const MISSING_FMT = LN("The romfs folder only has %d of the game's %d files, usually from a copy or "
                                          "unzip that stopped partway. Copy them again and choose the folder again.");
static std::string incomplete(int have) {
    char b[96];
    snprintf(b, sizeof b, INCOMPLETE_FMT, have, (int)ROMFS_FILES);
    return b;
}
// A status / setup line kept in English (LN) -> the menus' language, the numbers of the two above put in again
static std::string shown(const std::string &en) {
    int a, b;
    char buf[400];
    for (const char *f : {INCOMPLETE_FMT, MISSING_FMT})
        if (sscanf(en.c_str(), f, &a, &b) == 2) { snprintf(buf, sizeof buf, L(f), a, b); return buf; }
    return L_line(en);
}

// Game files of no supported version: what their exefs/main is, in the log once per folder and build ID (the start
// screen checks every frame, setup on every try). #696's log said only "an unsupported version", not which build.
// Returns the build ID's first 16 hex digits for the message ("": no program there).
static std::string refused_main(const std::string &exefs) {
    static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
    static std::set<std::string> logged;
    char hex[41];
    GameMainKind k = game_main_build_id((exefs + "/main").c_str(), hex);
    pthread_mutex_lock(&lock);
    bool first = logged.insert(exefs + "|" + std::to_string((int)k) + hex).second;
    pthread_mutex_unlock(&lock);
    if (first && k == GAME_MAIN_MISSING) fprintf(stderr, "[launcher] game files: no exefs/main, or it can't be read\n");
    if (first && k == GAME_MAIN_NOT_NSO)
        fprintf(stderr, "[launcher] game files: exefs/main isn't the game's program (no NSO0 header)\n");
    if (first && k == GAME_MAIN_NSO)
        fprintf(stderr, "[launcher] game files: an unsupported version (main build ID %s; supported: 1.0.0, 1.0.1, "
                "1.1.0)\n", hex);
    return k == GAME_MAIN_NSO ? std::string(hex, 16) : std::string();
}
// (with the build ID's first 16 hex digits; without one, the line alone)
static const char *const VERSION_HINT = LN("LAS Recomp needs version 1.0.0, 1.0.1 or 1.1.0 of the game.");
static const char *const VERSION_ID_HINT = LN("LAS Recomp needs version 1.0.0, 1.0.1 or 1.1.0 of the game. These game "
                                              "files have build ID %s.");
// (English, as the setup lines: shown() puts it in the menus' language)
static std::string version_hint(const std::string &exefs) {
    std::string id = refused_main(exefs);
    if (id.empty()) return VERSION_HINT;
    char b[320];
    snprintf(b, sizeof b, VERSION_ID_HINT, id.c_str());
    return b;
}

// The copied game files alone ("" = there), without the game code built from them
static std::string check_copied_files(const std::string &root) {
    if (!file_exists(root + "/exefs/main")) return LN("No game files yet");
    if (!game_version_of(root.c_str())) {
        refused_main(root + "/exefs");  // (the log line)
        return LN("Wrong version of the game");
    }
    if (!file_exists(root + "/romfs/region_common")) return incomplete(0);
    if (g_romfs_count < 0) {
        g_romfs_bytes = 0;
        const GameVersion *v = game_version_of(root.c_str());
        RomfsCheckWalk check = {game_romfs_check_begin(v), root.size() + strlen("/romfs/"), 0, root.c_str(), 0};
        g_romfs_count = count_files(root + "/romfs", 0, &g_romfs_bytes, &check);
        int missing = game_romfs_check_end(check.check);
        g_romfs_missing = missing;
        if (check.copies)
            fprintf(stderr, "[launcher] romfs: %d %s of the game code at the root, ignored\n", check.copies,
                    check.copies == 1 ? "copy" : "copies");
        if (check.odd > ROMFS_ODD_LOGGED)
            fprintf(stderr, "[launcher] romfs: %d more files that aren't the game's\n", check.odd - ROMFS_ODD_LOGGED);
        if (missing) fprintf(stderr, "[launcher] romfs: %d of the game's files missing or changed\n", missing);
        g_romfs_want = game_romfs_bytes(v);
        if (g_romfs_count >= ROMFS_FILES && g_romfs_want && g_romfs_bytes != g_romfs_want)
            fprintf(stderr, "[launcher] romfs: %llu bytes in %d files, this version's are %llu: damaged or changed "
                    "game files\n", (unsigned long long)g_romfs_bytes, g_romfs_count, (unsigned long long)g_romfs_want);
    }
    if (g_romfs_count < ROMFS_FILES) return incomplete(g_romfs_count);
    return "";
}

// Every file there, but not all of them the game's (name and size) or not the game's sizes: a damaged, changed,
// renamed or moved file (issue #391). Playable, so not a check_game_files() problem: the start screen shows it beside
// "ready".
static bool files_changed(const std::string &root) {
    return check_copied_files(root).empty() && (g_romfs_missing > 0 || (g_romfs_want && g_romfs_bytes != g_romfs_want));
}
static const char *const CHANGED_HEAD = LN("Game files damaged or changed");
static const char *const CHANGED_HINT = LN("Copy your game files again, then choose that folder and set them up again.");

#if PLAT_IOS
// iOS: the game code is the app's own, built for one version of the game (the desktops build it from the files set
// up). Game files of another version: false, and both versions' labels ("1.0.1 / 1.1.0", "1.0.0").
static const char *const OTHER_VERSION = LN("This app is for another version of the game");
static const char *const OTHER_VERSION_HINT = LN("This app was exported for version %s of the game, these game files are "
                                                 "version %s. Export the app again from these game files, or set up "
                                                 "files of version %s.");
static bool app_fits_files(const std::string &exefs_parent, std::string *app_v, std::string *files_v) {
    char built[64], label[32];
    const GameVersion *a = gamelib_built_for(built, sizeof built) ? game_version_by_build_prefix(built) : NULL;
    const GameVersion *f = game_version_of(exefs_parent.c_str());
    if (!a || !f || a == f) return true;
    const char *fl = game_release_label((exefs_parent + "/exefs").c_str(), label, sizeof label);
    if (app_v) *app_v = a->label;
    if (files_v) *files_v = fl ? fl : f->label;
    return false;
}
static std::string other_version_hint(const std::string &app_v, const std::string &files_v) {  // (menus' language)
    char b[600];
    snprintf(b, sizeof b, L(OTHER_VERSION_HINT), app_v.c_str(), files_v.c_str(), app_v.c_str());
    return b;
}
#endif

// "" = ready to play, else what is wrong
static std::string check_game_files(const std::string &root) {
    std::string why = check_copied_files(root);
#if PLAT_IOS
    if (why.empty() && !gamelib_present(root.c_str()) && !app_fits_files(root, NULL, NULL)) return OTHER_VERSION;
#endif
    if (why.empty() && !gamelib_present(root.c_str())) return LN("Game not built yet");
    return why;
}

// What to do about a check_game_files() problem (the line under it on the start screen), in the menus' language
static const char *status_hint(const std::string &why) {
    if (why.rfind("Game files incomplete", 0) == 0)
        return L("Some of the game's files are missing, usually from a copy or unzip that stopped partway. Copy your game "
                 "files again, then choose that folder and set them up again.");
    if (why == "Wrong version of the game") {
        static std::string h;
        h = shown(version_hint(g_root + "/exefs"));
        return h.c_str();
    }
    if (why == "Game not built yet")
        return L("Set up the game files to build it. It takes a few minutes and you only do it once.");
#if PLAT_IOS
    if (why == OTHER_VERSION) {
        static std::string h;
        std::string a, f;
        app_fits_files(g_root, &a, &f);
        h = other_version_hint(a, f);
        return h.c_str();
    }
#endif
    return L("Set up the game files to play.");
}

// ------------------------------------------------------------------ setup (background thread)
// The player brings their own game files: a folder holding exefs/ and romfs/. They're copied into the data root (APFS
// clones / hard links where the file system allows); the game reads romfs/ in place (runtime/services/fs.c).
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static std::string g_src;           // the picked folder
static std::string g_job;           // current setup step / result for the status line (English: shown())
static std::string g_job_hint;      // a failure: what to do about it (shown under it)
static bool g_busy, g_job_failed;
static bool g_confirm_delete;       // game-files page: "Delete the game files?" shown
static int g_fail_kind;             // the last failure (FAIL_*, below; 0: none) - which step the setup page marks

static void set_job(const std::string &s, bool failed = false, const std::string &hint = "") {
    pthread_mutex_lock(&g_lock);
    g_job = s;
    g_job_failed = failed;
    g_job_hint = hint;
    if (!failed) g_fail_kind = 0;
    pthread_mutex_unlock(&g_lock);
}

// (the not-found failure's hint, and the caption over the game files page's example folder)
static const char *const PICK_FOLDER_HINT = LN("Choose the folder that has the game's exefs and romfs folders in it.");

// Setup failures: a short headline and what to do next (also LARECOMP_LAUNCHER_SHOT=<f>:fail<1-6>, screenshots)
enum { FAIL_NOT_FOUND = 1, FAIL_UNUSABLE, FAIL_COPY, FAIL_VERSION, FAIL_MISSING, FAIL_BUILD };
static void set_failure(int k, int have = -1, const char *reason = NULL) {
    static const char *const head[] = {"", LN("No game files found in that folder"),
                                       LN("Those are game packages, not game files"), LN("Setup didn't finish"),
                                       LN("Wrong version of the game"), LN("Your game files are incomplete"),
                                       LN("The game code didn't build")};
    static const char *const hint[] = {
        "",
        PICK_FOLDER_HINT,
        LN("LAS Recomp needs the extracted exefs and romfs folders, not .xci, .nsp or .nca files."),
        LN("Make sure there's about 12 GB free and try again."),
        VERSION_HINT,
        LN("Some of the game's files are missing, usually from a copy or unzip that stopped partway. Copy them again "
           "and choose the folder again."),
        LN("Make sure there's about 2 GB free and try again. If it keeps happening, report it on GitHub.")};
    if (k < 1 || k > 6) return;
    if (k == FAIL_MISSING && have >= 0) {
        char b[320];
        snprintf(b, sizeof b, MISSING_FMT, have, (int)ROMFS_FILES);
        set_job(head[k], true, b);
        g_fail_kind = k;
        return;
    }
    set_job(head[k], true, reason ? std::string(reason) + " " + hint[k] : hint[k]);
    g_fail_kind = k;
}

// exefs: the folder with main and main.npdm; romfs: the folder with region_common (runtime/romfs_build.h: the
// picked folder or a few levels below it)
static void find_game(const std::string &dir, std::string &exefs, std::string &romfs, bool &unusable) {
    char e[2200], r[2200];
    romfs_find_game(dir.c_str(), e, sizeof e, r, sizeof r, &unusable);
    exefs = e, romfs = r;
}

// A supported game version (runtime/game_addresses/versions.h)? Logs which one.
static bool build_id_ok(const std::string &exefs) {
    char label[32];
    const char *v = game_release_label(exefs.c_str(), label, sizeof label);
    if (v) fprintf(stderr, "[launcher] game files: version %s\n", v);
    else refused_main(exefs);
    return v != NULL;
}
// Wrong version: the failure, its hint with the build ID
static void set_version_failure(const std::string &exefs) {
    set_job(LN("Wrong version of the game"), true, version_hint(exefs));
}

// progress (the overlay's bar): the romfs copy (bytes)
static std::atomic<unsigned long long> g_done, g_total;
// The bar: copying is the first tenth, building the game code (runtime/gamebuild.c) the rest
static unsigned long long g_phase_base;
static std::atomic<float> g_build_frac{-1};  // -1: not building
static std::atomic<bool> g_cancel_build{false};
static bool build_cancelled(void) { return g_cancel_build.load(); }
static bool g_copy_skipped;                  // (only the game code to build: the bar is all of that)
static bool g_auto_update;                   // started by itself: game files there, game code from an older version
static void setup_progress(uint64_t done, uint64_t) { g_done = g_phase_base + done; }
#ifdef __ANDROID__
static void import_progress(uint64_t done, uint64_t total) { g_total = total, g_done = done; }  // (the copy is the first phase)
#endif
static void build_progress(double f, const char *step) {
    g_build_frac = (float)f;
    static const char *last;
    if (step != last) last = step, set_job(step);
}
// Shader preparation, after the game files are ready (runtime/nvn/shader_prep.h): its own bar, and Skip
static std::atomic<float> g_prep_frac{-1};  // -1: not preparing
static volatile int g_prep_skip;
static bool g_prep_skip_ask;  // Skip pressed: "are you sure?" in front of the bar (LARECOMP_LAUNCHER_SHOT=<f>:skipask)
// The variants prepared and their total (done << 32 | total), one value so the bar and the count always agree
static std::atomic<uint64_t> g_prep_count{0};
static void prep_progress(int done, int total) {
    g_prep_count = (uint64_t)(uint32_t)done << 32 | (uint32_t)total;
    g_prep_frac = total ? (float)done / (float)total : 0.0f;
}
static void prepare_shaders(bool background = false) {
    g_prep_skip = 0, g_prep_count = 0, g_prep_frac = 0;
    if (!background) set_job(LN("Preparing shaders"));
    shader_prep_run((g_root + "/romfs").c_str(), prep_progress, &g_prep_skip);
    g_prep_frac = -1;
}

// Every later start: the same preparation in the background as soon as the start screen is up (quick: the system's
// shader caches already hold the compiled code). Only a PLAY before it is done shows its bar (g_prep_wait).
static std::atomic<bool> g_bg_prep{false};
static bool g_prep_wait;
static void start_background_prep(void) {
    static bool started;  // (once per process: the prepared objects stay for later boots)
    if (started) return;
    started = true;
    g_bg_prep = true;
    pthread_t th;
    pthread_create(&th, NULL, [](void *) -> void * { prepare_shaders(true); g_bg_prep = false; return NULL; }, NULL);
    pthread_detach(th);
}
// Before the game files change (setup, delete): the background preparation stops reading them
static void stop_background_prep(void) {
    if (!g_bg_prep) return;
    g_prep_skip = 1;
    while (g_bg_prep) SDL_Delay(5);
}

static float extract_progress(void) {
    if (float p = g_prep_frac; p >= 0) return p > 1 ? 1 : p;
    unsigned long long t = g_total, d = g_done;
    const float copy_share = PLAT_IOS ? 1.0f : 0.1f;  // (iOS: no build - the copy is the whole bar)
    float b = g_build_frac, f = b >= 0 ? (g_copy_skipped ? b : copy_share + (1 - copy_share) * b) : t ? copy_share * (float)((double)d / (double)t) : 0;
    return f < 0 ? 0 : f > 0.99f ? 0.99f : f;
}

// Builds the game code from the copied files (a few minutes); false = failed (the job line says so)
static bool build_game_code(void) {
    if (PLAT_IOS) return true;  // (the game code ships in the app: Frameworks/LASGame.framework)
    int cores = SDL_GetNumLogicalCPUCores(), ram_gb = SDL_GetSystemRAM() / 1024;
    GameBuildOpts o = {build_progress, cores < ram_gb - 2 ? cores : ram_gb - 2 > 1 ? ram_gb - 2 : 1, build_cancelled};
    char why[256] = "";
    g_build_frac = 0;
    bool ok = gamebuild_run(g_root.c_str(), &o, why, sizeof why);
    if (!ok) {
        fprintf(stderr, "[launcher] game code: %s\n", why);
        if (g_cancel_build) set_job(LN("Build cancelled. The finished parts are kept, so you can resume later."));
        else set_failure(FAIL_BUILD, -1, gamebuild_killed() ? why : NULL);
    }
    return ok;
}

static void *extract_thread(void *) {
    stop_background_prep();
    std::string src;
    pthread_mutex_lock(&g_lock);
    src = g_src;
    pthread_mutex_unlock(&g_lock);
    g_cancel_build = false;
    g_done = 0, g_total = 0, g_phase_base = 0, g_build_frac = -1, g_copy_skipped = false;
    game_files_changed();
    std::string exefs, romfs;
    bool unusable = false;  // (game packages rather than its files)
    if (src.empty() && check_copied_files(g_root).empty()) {  // copied already (an update needs new game code)
        g_copy_skipped = true;
        if (!build_game_code()) goto done;
        goto check;
    }
#ifdef __ANDROID__
    if (!src.compare(0, 10, "content://")) {  // (Android's folder picker: copied through the storage framework)
        set_job(LN("Copying the game files..."));
        mkdir(g_root.c_str(), 0755);
        rt_remove_game_files(g_root.c_str());
        game_files_changed();
        mkdir(g_root.c_str(), 0755);
        int r = android_import_game(src.c_str(), g_root.c_str(), import_progress, build_cancelled);
        if (r) { set_failure(r == 1 ? FAIL_NOT_FOUND : r == 2 ? FAIL_UNUSABLE : FAIL_COPY); goto done; }
        if (!build_id_ok(g_root + "/exefs")) { set_version_failure(g_root + "/exefs"); goto done; }
        if (int have = count_files(g_root + "/romfs"); have < ROMFS_FILES) { set_failure(FAIL_MISSING, have); goto done; }
        g_phase_base = g_total, g_done = (unsigned long long)g_total;
        pthread_mutex_lock(&g_lock);
        g_src.clear();
        pthread_mutex_unlock(&g_lock);
        if (!build_game_code()) goto done;
        goto check;
    }
#endif
    set_job(LN("Looking for the game files..."));
    find_game(src, exefs, romfs, unusable);
    if (exefs.empty() || romfs.empty()) { set_failure(unusable ? FAIL_UNUSABLE : FAIL_NOT_FOUND); goto done; }
    if (!build_id_ok(exefs)) { set_version_failure(exefs); goto done; }
#if PLAT_IOS
    {  // (refused before the earlier copy is removed: the app's game code wouldn't run them)
        std::string app_v, files_v, parent = exefs.substr(0, exefs.find_last_of("/\\"));
        if (!app_fits_files(parent, &app_v, &files_v)) {
            fprintf(stderr, "[launcher] game files: version %s, the app's game code: %s\n", files_v.c_str(), app_v.c_str());
            set_job(OTHER_VERSION, true, other_version_hint(app_v, files_v));
            goto done;
        }
    }
#endif
    if (int have = count_files(romfs); have < ROMFS_FILES) {  // (an incomplete copy: refused before copying it)
        fprintf(stderr, "[launcher] %s: %d of %d romfs files\n", romfs.c_str(), have, (int)ROMFS_FILES);
        set_failure(FAIL_MISSING, have);
        goto done;
    }
    {
        // (the picked folder can't be LAS Recomp's own copy: setting up again deletes that first)
        char a[4096], b[4096];
        mkdir(g_root.c_str(), 0755);
        if (realpath(exefs.c_str(), a) && realpath(g_root.c_str(), b) && !strncmp(a, b, strlen(b)) &&
            (a[strlen(b)] == '/' || a[strlen(b)] == '\\' || !a[strlen(b)])) {
            set_failure(FAIL_NOT_FOUND);
            goto done;
        }
        rt_remove_game_files(g_root.c_str());  // (an earlier or interrupted setup's; removes the folder too)
        game_files_changed();
        mkdir(g_root.c_str(), 0755);
        unsigned long long rbytes = romfs_dir_bytes(romfs.c_str());
        g_total = rbytes;
        set_job(LN("Copying the game files..."));
        if (!romfs_copy_tree(exefs.c_str(), (g_root + "/exefs").c_str(), NULL) ||
            !romfs_copy_tree(romfs.c_str(), (g_root + "/romfs").c_str(), setup_progress)) {
            set_failure(FAIL_COPY);
            goto done;
        }
        g_phase_base = rbytes, g_done = rbytes;
        // Retrying a cancelled compilation must use the copied files and their cache.
        pthread_mutex_lock(&g_lock);
        g_src.clear();
        pthread_mutex_unlock(&g_lock);
        if (!build_game_code()) goto done;
    }
check:
    {
        game_files_changed();
        std::string why = check_game_files(g_root);
        if (why.empty()) prepare_shaders();  // (the GPU device is up: the start screen draws with it)
        if (why.empty() && files_changed(g_root)) set_job(CHANGED_HEAD, true, CHANGED_HINT);  // (playable all the same)
        else if (why.empty()) set_job(LN("Game files ready."));
#if PLAT_IOS
        else if (why == OTHER_VERSION) {
            std::string a, f;
            app_fits_files(g_root, &a, &f);
            set_job(OTHER_VERSION, true, other_version_hint(a, f));
        }
#endif
        else if (why.find("version") != std::string::npos) set_version_failure(g_root + "/exefs");
        else set_failure(FAIL_MISSING, g_romfs_count);
    }
done:
    pthread_mutex_lock(&g_lock);
    g_busy = false;
    pthread_mutex_unlock(&g_lock);
    return NULL;
}

// ------------------------------------------------------------------ folder dialog (SDL calls back on any thread)
// files NULL: no window could be shown (an empty list is a cancel). Linux shows it through the desktop's file chooser
// portal or zenity, and a desktop with neither (a bare window manager) has none: say so instead of Choose doing nothing.
static const char *const PICKER_FAILED = LN("Couldn't open a folder window");
static std::string picker_hint(const char *what) {
    std::string err = SDL_GetError();
    fprintf(stderr, "[launcher] %s window: %s\n", what, err.c_str());
#if defined(__linux__) && !defined(__ANDROID__)
    char b[256];  // (what: "folder" / "file")
    snprintf(b, sizeof b, LN("LAS Recomp needs xdg-desktop-portal or zenity to open the %s window. Install one and try "
                             "again."), what);
    return b;
#else
    return err;
#endif
}

static void SDLCALL picked(void *, const char *const *files, int) {
    if (!files) { set_job(PICKER_FAILED, true, picker_hint(LN("folder"))); return; }
    if (!files[0]) return;
    pthread_mutex_lock(&g_lock);
    g_src = files[0];
    if (g_job == PICKER_FAILED) g_job.clear(), g_job_hint.clear(), g_job_failed = false;
    pthread_mutex_unlock(&g_lock);
}

// The folder picker: SDL's dialog (on iOS the Files app's through runtime/platform/ios_sdl.h's macros); on Android
// its own (the folder is a content:// URI, copied by the setup through the storage framework: extract_thread)
#ifdef __ANDROID__
static SDL_DialogFileCallback g_android_cb;
static void *g_android_ud;
static void picked_android(const char *uri) {
    fprintf(stderr, "[launcher] folder picked: %s\n", uri ? uri : "(none picked)");
    const char *list[2] = {uri, NULL};
    g_android_cb(g_android_ud, list, -1);  // (NULL uri: cancelled - an empty list)
}
#endif
static void show_folder_dialog(SDL_DialogFileCallback cb, void *ud, SDL_Window *win) {
#ifdef __ANDROID__
    (void)win;
    g_android_cb = cb, g_android_ud = ud;
    if (!android_pick_folder(picked_android)) cb(ud, NULL, -1);
#else
    SDL_ShowOpenFolderDialog(cb, ud, win, NULL, false);
#endif
}

// ------------------------------------------------------------------ a cloud drive (OneDrive on Windows, iCloud Drive on iOS)
// A data root inside a synced folder uploads every game file to the player's cloud drive: a warning on the start
// screen with roughly how much, and Choose another folder (the Data folder's Change...: move the data there) or Keep
// here (settings.onedrive_kept). Checked once per launch on the current data root (plat_cloud_synced); the size in
// the background: the data root's own once set up, before that the picked game files (or a typical copy) plus what
// the setup adds. LARECOMP_TEST_ONEDRIVE=1 (test): shown on any system, as OneDrive.
#define GAME_FILES_GB 6.3   // exefs + romfs, as players bring them
#define SETUP_ADDS_GB 0.4   // the built game code and its kept pieces (0.23, a beta_5 setup), shader caches
static const char *g_onedrive_name;        // the service's name (the English text names it); NULL: not synced
// (iOS: the system offloads a cloud folder's files it thinks unused; the game then waits for each one it reads)
static const char *const KEEP_DOWNLOADED = LN("In the Files app, touch and hold the data folder and choose Keep "
                                              "Downloaded, so its files stay on this iPhone.");
static int g_onedrive = -1;                // -1: not checked yet
static std::atomic<double> g_od_gb{-1};    // the estimate (-1: not known yet)
static std::atomic<int> g_od_job;
static std::string g_od_for = "\x02";      // what the estimate is for: "" set up, "\x01" nothing picked, else the folder

static void *onedrive_size_thread(void *arg) {
    std::string *src = (std::string *)arg;
    int job = g_od_job;
    double gb;
    if (src->empty()) gb = rt_tree_size(rt_data_root()) / 1e9;
    else {
        std::string exefs, romfs;
        bool unusable = false;
        if (*src != "\x01") find_game(*src, exefs, romfs, unusable);
        gb = (exefs.empty() || romfs.empty() ? GAME_FILES_GB : (rt_tree_size(exefs.c_str()) + rt_tree_size(romfs.c_str())) / 1e9)
             + SETUP_ADDS_GB;
    }
    if (job == g_od_job) g_od_gb = gb;
    delete src;
    return NULL;
}

// The warning, w wide at the cursor (the start screen's upper right; before the setup, under the System rows).
// 1: Keep here pressed, 2: Choose another folder (offered when the data folder can be changed and nothing is running)
static int onedrive_notice(float s, float w, bool ready) {
    std::string src;
    pthread_mutex_lock(&g_lock);
    src = ready ? "" : g_src.empty() ? "\x01" : g_src;
    pthread_mutex_unlock(&g_lock);
    if (src != g_od_for) {  // (set up, or another folder picked: estimate again)
        g_od_for = src, g_od_job++;
        pthread_t th;
        if (pthread_create(&th, NULL, onedrive_size_thread, new std::string(src)) == 0) pthread_detach(th);
    }
    double gb = g_od_gb;
    if (gb < 0) return 0;
    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.03f, 0.04f, 0.05f, 0.86f));
    ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 14 * s);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(24 * s, 18 * s));
    ImGui::BeginChild("##onedrive", ImVec2(w, 0), ImGuiChildFlags_AlwaysUseWindowPadding | ImGuiChildFlags_AutoResizeY,
                      ImGuiWindowFlags_NavFlattened);
    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor();
    ImGui::PushTextWrapPos(0);
    ImGui::TextColored(ImVec4(1, 0.72f, 0.4f, 1), L("!  This folder is synced by %s"), g_onedrive_name);
    ImGui::TextColored(ImVec4(0.86f, 0.85f, 0.80f, 1), L("About %.0f GB of game files will upload to your %s."),
                       fmax(1.0, gb), g_onedrive_name);
    if (PLAT_IOS) ImGui::TextColored(ImVec4(0.86f, 0.85f, 0.80f, 1), "%s", L(KEEP_DOWNLOADED));
    ImGui::PopTextWrapPos();
    ImGui::Dummy(ImVec2(0, 4 * s));
    int r = 0;
    pthread_mutex_lock(&g_lock);
    bool busy = g_busy;
    pthread_mutex_unlock(&g_lock);
    if (!rt_data_root_fixed()) {
        ImGui::BeginDisabled(busy);
        if (ImGui::Button(L("Choose another folder"))) r = 2;
        ImGui::EndDisabled();
        ImGui::SameLine();
    }
    if (ImGui::Button(L("Keep it here"))) r = 1;
    ImGui::EndChild();
    return r;
}
static void onedrive_keep(void) {
    g_settings.onedrive_kept = true;
    settings_save();
    fprintf(stderr, "[launcher] %s warning: kept here\n", g_onedrive_name);
}

#if PLAT_IOS
// iOS: signed without the increased-memory-limit entitlement (a signer that drops it): the game may start, but iOS
// ends it at a much lower footprint. Shown on the start screen until OK, every launch (runtime/platform/ios.c).
static bool g_memory_ok_seen;
static void memory_notice(float s, float w) {
    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.03f, 0.04f, 0.05f, 0.86f));
    ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 14 * s);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(24 * s, 18 * s));
    ImGui::BeginChild("##memory", ImVec2(w, 0), ImGuiChildFlags_AlwaysUseWindowPadding | ImGuiChildFlags_AutoResizeY,
                      ImGuiWindowFlags_NavFlattened);
    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor();
    ImGui::PushTextWrapPos(0);
    ImGui::TextColored(ImVec4(1, 0.72f, 0.4f, 1), "%s", L("!  Installed without the increased memory limit"));
    ImGui::TextColored(ImVec4(0.86f, 0.85f, 0.80f, 1), "%s",
                       L("iOS may close the game when it needs more memory. Sign the app again with the Increased "
                         "Memory Limit entitlement turned on."));
    ImGui::PopTextWrapPos();
    ImGui::Dummy(ImVec2(0, 4 * s));
    if (ImGui::Button(L("OK"))) g_memory_ok_seen = true;
    ImGui::EndChild();
}
#endif


// ------------------------------------------------------------------ the user's own Nintendo fonts (not shipped)
// runtime/hle/pl.c loads fonts/ first, else a close substitute of the requested language. Import copies recognized
// files only: the console's own names for the shared fonts (Standard, Chinese Simplified / its Ext, Chinese
// Traditional, Korean and NintendoExt).
static const char *FONT_STD[] = {"nintendo_udsg-r_std_003.bfttf", "nintendo_udsg-r_std_003.ttf",
                                 "nintendo_udsgr_std_003.ttf", "nintendo_udsgr_std_003.bfttf"};
static const char *FONT_LANG[] = {"nintendo_udsg-r_org_zh-cn_003.bfttf", "nintendo_udsg-r_org_zh-cn_003.ttf",
                                  "nintendo_udsg-r_ext_zh-cn_003.bfttf", "nintendo_udsg-r_ext_zh-cn_003.ttf",
                                  "nintendo_udjxh-db_zh-tw_003.bfttf", "nintendo_udjxh-db_zh-tw_003.ttf",
                                  "nintendo_udsg-r_ko_003.bfttf", "nintendo_udsg-r_ko_003.ttf"};
static const char *FONT_EXT[] = {"nintendo_ext_003.bfttf", "nintendo_ext_003.ttf"};
static std::vector<std::string> g_font_picks;
static std::string g_font_msg, g_font_err;  // (g_font_msg in the menus' language; g_font_err: why the file window
                                            //  couldn't open, English, from the dialog's thread)

static bool have_font(const char *const *names, int n) {
    for (int i = 0; i < n; i++)
        if (file_exists(std::string("fonts/") + names[i])) return true;
    return false;
}

static void SDLCALL picked_fonts(void *, const char *const *files, int) {
    std::string err = files ? "" : picker_hint(LN("file"));
    pthread_mutex_lock(&g_lock);
    for (int i = 0; files && files[i]; i++) g_font_picks.push_back(files[i]);
    if (!files) g_font_err = err;
    pthread_mutex_unlock(&g_lock);
}

static void import_fonts() {
    pthread_mutex_lock(&g_lock);
    std::vector<std::string> picks;
    picks.swap(g_font_picks);
    if (!g_font_err.empty()) g_font_msg = std::string(L("Couldn't open a file window.")) + " " + L_line(g_font_err), g_font_err.clear();
    pthread_mutex_unlock(&g_lock);
    if (picks.empty()) return;
    mkdir("fonts", 0755);
    int ok = 0, skipped = 0, failed = 0;
    for (auto &src : picks) {
        std::string base = src.substr(src.find_last_of("/\\") + 1), low = base;  // (Windows paths use '\\' too)
        for (auto &c : low) c = (char)tolower(c);
        bool known = false;
        for (auto n : FONT_STD) known |= low == n;
        for (auto n : FONT_LANG) known |= low == n;
        for (auto n : FONT_EXT) known |= low == n;
        if (!known) { skipped++; continue; }
        // Read all of it before fonts/ is written: the pick can be the file in fonts/ itself, and an unreadable pick
        // must not empty the copy already there.
        std::string data;
        bool read_ok = false;
        if (FILE *in = fopen(src.c_str(), "rb")) {
            char buf[65536];
            size_t n;
            while ((n = fread(buf, 1, sizeof buf, in)) > 0) data.append(buf, n);
            read_ok = !ferror(in);
            fclose(in);
        }
        FILE *out = read_ok ? fopen(("fonts/" + low).c_str(), "wb") : NULL;
        bool copied = out && fwrite(data.data(), 1, data.size(), out) == data.size();
        if (out && fclose(out) != 0) copied = false;
        if (copied) ok++;
        else failed++;
    }
    char b[160];
    snprintf(b, sizeof b, L("Font files imported: %d."), ok);
    g_font_msg = b;
    if (failed) snprintf(b, sizeof b, L("Couldn't be copied: %d."), failed), g_font_msg += std::string(" ") + b;
    if (skipped) snprintf(b, sizeof b, L("Not recognized: %d."), skipped), g_font_msg += std::string(" ") + b;
    g_font_msg += std::string(" ") + L("They load the next time you start the game.");
}

static void remove_fonts() {
    for (auto n : FONT_STD) unlink((std::string("fonts/") + n).c_str());
    for (auto n : FONT_LANG) unlink((std::string("fonts/") + n).c_str());
    for (auto n : FONT_EXT) unlink((std::string("fonts/") + n).c_str());
    g_font_msg = L("Imported fonts removed. You're back on the substitute fonts.");
}

// ------------------------------------------------------------------ background: a title movie frame
#ifdef UI_METAL
static id<MTLTexture> g_bg;
#define BG_TEXID ((ImTextureID)(__bridge void *)g_bg)
typedef id<MTLDevice> GpuDevice;
#else
static uint64_t g_bg;  // ImGui texture id (Vulkan descriptor set)
#define BG_TEXID ((ImTextureID)g_bg)
typedef void *GpuDevice;
#endif
static int g_bg_w, g_bg_h;

static void load_background(GpuDevice dev) {
    std::string path = g_root + "/romfs/region_common/movie/title.webm";
    AVFormatContext *fmt = NULL;
    if (avformat_open_input(&fmt, path.c_str(), NULL, NULL) < 0) return;
    int si = av_find_best_stream(fmt, AVMEDIA_TYPE_VIDEO, -1, -1, NULL, 0);
    if (si >= 0 && avformat_find_stream_info(fmt, NULL) >= 0) {
        AVStream *st = fmt->streams[si];
        const AVCodec *codec = avcodec_find_decoder(st->codecpar->codec_id);
        AVCodecContext *cc = avcodec_alloc_context3(codec);
        avcodec_parameters_to_context(cc, st->codecpar);
        if (codec && avcodec_open2(cc, codec, NULL) == 0) {
            AVPacket *pkt = av_packet_alloc();
            AVFrame *fr = av_frame_alloc();
            int64_t want = (int64_t)(6.0 / av_q2d(st->time_base));  // a few seconds in: the island, not black
            av_seek_frame(fmt, si, want, AVSEEK_FLAG_BACKWARD);
            bool got = false;
            while (!got && av_read_frame(fmt, pkt) >= 0) {
                if (pkt->stream_index == si && avcodec_send_packet(cc, pkt) == 0)
                    while (avcodec_receive_frame(cc, fr) == 0)
                        if (fr->best_effort_timestamp >= want) { got = true; break; }
                av_packet_unref(pkt);
            }
            if (got) {
                int w = fr->width, h = fr->height;
                std::vector<uint8_t> rgba((size_t)w * h * 4);
                SwsContext *sws = sws_getContext(w, h, (AVPixelFormat)fr->format, w, h, AV_PIX_FMT_RGBA, SWS_BILINEAR, NULL, NULL, NULL);
                uint8_t *dst[1] = {rgba.data()};
                int dst_ls[1] = {w * 4};
                sws_scale(sws, fr->data, fr->linesize, 0, h, dst, dst_ls);
                sws_freeContext(sws);
#ifdef UI_METAL
                MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm
                                                                                              width:w height:h mipmapped:NO];
                g_bg = [dev newTextureWithDescriptor:td];
                [g_bg replaceRegion:MTLRegionMake2D(0, 0, w, h) mipmapLevel:0 withBytes:rgba.data() bytesPerRow:w * 4];
#else
                (void)dev;
                g_bg = vk_imgui_texture_rgba(rgba.data(), w, h);
#endif
                g_bg_w = w, g_bg_h = h;
            }
            av_frame_free(&fr);
            av_packet_free(&pkt);
        }
        avcodec_free_context(&cc);
    }
    avformat_close_input(&fmt);
}

// ------------------------------------------------------------------ seeds
static std::vector<std::string> list_seeds() {
    std::vector<std::string> out;
    if (DIR *d = opendir("rando")) {
        while (struct dirent *e = readdir(d)) {
            if (e->d_name[0] == '.') continue;
            std::string dir = std::string("rando/") + e->d_name;
            if (file_exists(dir + "/settings.json") && file_exists(dir + "/romfs")) out.push_back(e->d_name);
        }
        closedir(d);
    }
    std::sort(out.begin(), out.end());
    return out;
}

// ------------------------------------------------------------------ screen
static ImFont *g_font_title, *g_font_menu, *g_font_small;
// SETTINGS: the in-game menu's tabs (runtime/ui/menu.mm); RANDO: the Randomizer panel (New Seed: rando_tab.mm);
// IOS: the export page (from Settings > Prelaunch)
enum Page { MAIN, SETTINGS, SETUP, RANDO, IOS };
static int g_refocus = MAIN;  // back on the main page: select the item of the page we came from (controllers)

// A big menu line: gold + a marker when focused / hovered (keyboard, gamepad or mouse), grey when disabled
static bool g_menu_small;  // sub pages: buttons in the small font
void menu_tabs_draw(float scale, ImFont *font, ImFont *font_big, bool in_game, void (*prelaunch)(float),
                    int only = -1000);  // menu.mm
bool menu_reset_corner(float bx, float scale, ImFont *font, ImFont *font_big, bool seed);  // menu.mm
void menu_style_begin(float scale);  // (menu.mm: the menu's look, until menu_style_end)
bool menu_preview_frame(float scale, ImFont *font, ImFont *font_big);  // (menu.mm: true while a live preview is up)
void menu_style_end(void);
void menu_tabs_focus(bool first_tab);
extern int g_tab_request_ext;
extern "C" void menu_apply_window_requests(void *sdl_window);
extern "C" void input_cursor_event(const SDL_Event *e);  // runtime/hle/input.c
extern "C" bool input_cursor_hidden(void);
extern "C" int g_start_screen;  // runtime/hle/warp.c: the start screen is up (no game loaded: don't read its memory)
// The Randomizer panel's Play / Play Now on the start screen (rando_tab.mm): start the game with that seed
static std::string g_play_seed;
static bool g_play_seed_req, g_play_pending;
extern "C" void launcher_play_seed(const char *seed) {
    g_play_seed = seed ? seed : "";
    g_play_seed_req = true;
}
static float g_menu_gap = 0.35f;  // space under each item, in line heights

// The first PLAY ever (settings.menu_tip_seen): a note on opening the LAS Recomp menu in game (it isn't obvious),
// OK starts the game. Returns true once OK is pressed.
static bool g_tip_test;  // (LARECOMP_LAUNCHER_SHOT=<frame>:tip)
// Something the player should know before playing (iPhone: the data folder offloaded to iCloud), in the menus' look
// with an OK - it was a system alert (user 2026-11-11). Main thread.
static std::string g_notice;
static void notice_popup(float s) {
    float sc = s * 720.0f / 1080.0f;
    if (!g_notice.empty() && !ImGui::IsPopupOpen("##notice")) ImGui::OpenPopup("##notice");
    ImVec2 ds = ImGui::GetIO().DisplaySize;
    ImGui::SetNextWindowPos(ImVec2(ds.x * 0.5f, ds.y * 0.5f), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    if (!ImGui::BeginPopupModal("##notice", NULL, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoTitleBar))
        return;
    ImGui::PushFont(g_font_small);
    ImGui::PushTextWrapPos(900 * sc);
    ImGui::TextUnformatted(g_notice.c_str());
    ImGui::PopTextWrapPos();
    ImGui::Spacing();
    ImGui::SetItemDefaultFocus();
    if (ImGui::Button(L("OK"), ImVec2(240 * sc, 0)) || ImGui::IsKeyPressed(ImGuiKey_GamepadFaceRight, false) ||
        ImGui::IsKeyPressed(ImGuiKey_Escape, false))
        g_notice.clear(), ImGui::CloseCurrentPopup();
    ImGui::PopFont();
    ImGui::EndPopup();
}
// First PLAY ever, before the menu note: authentic 16:9 or the unlocked (fill the window) aspect ratio.
static bool aspect_prompt(float s) {
    bool done = false;
    ImVec2 ds = ImGui::GetIO().DisplaySize;
    ImGui::SetNextWindowPos(ImVec2(ds.x * 0.5f, ds.y * 0.5f), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(fminf(ds.x * 0.8f, 760 * s), 0));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(36 * s, 28 * s));
    bool open = ImGui::BeginPopupModal("##aspect", NULL, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                                       ImGuiWindowFlags_NoMove | ImGuiWindowFlags_AlwaysAutoResize);
    ImGui::PopStyleVar();
    if (open) {  // (in the menus' look, as the data folder's question: the caller turns the pieces on)
        ImGui::PushFont(g_font_small);
        ImGui::PushFont(g_font_menu);
        ImGui::PushTextWrapPos(0);
        ImGui::TextColored(plastic_col(plastic_theme().title), "%s", L("How would you like to play?"));
        ImGui::PopFont();
        ImGui::Spacing();
        ImGui::TextColored(plastic_col(plastic_theme().text_dim), "%s", L("You can change this any time in Settings."));
        ImGui::PopTextWrapPos();
        ImGui::Dummy(ImVec2(0, 14 * s));
        float bw = 300 * s, gap = 20 * s;
        ImGui::SetCursorPosX((ImGui::GetWindowWidth() - 2 * bw - gap) * 0.5f);
        int pick = -1;
        ImGui::SetItemDefaultFocus();
        if (ImGui::Button(L("Original 16:9"), ImVec2(bw, 44 * s))) pick = ASPECT_ORIGINAL;
        ImGui::SameLine(0, gap);
        if (ImGui::Button(L("Unlocked, fill the screen"), ImVec2(bw, 44 * s))) pick = ASPECT_UNLOCKED;
        if (pick >= 0) {
            g_settings.aspect = pick;
            g_settings.aspect_asked = true;
            settings_save();
            ImGui::CloseCurrentPopup();
            done = true;
        }
        ImGui::PopFont();
        ImGui::EndPopup();
    }
    return done;
}

static bool menu_tip(float s) {
    bool go = false;
    ImVec2 ds = ImGui::GetIO().DisplaySize;
    ImGui::SetNextWindowPos(ImVec2(ds.x * 0.5f, ds.y * 0.5f), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(fminf(ds.x * 0.8f, 760 * s), 0));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(36 * s, 28 * s));
    bool open = ImGui::BeginPopupModal("##menutip", NULL, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                                       ImGuiWindowFlags_NoMove | ImGuiWindowFlags_AlwaysAutoResize);
    ImGui::PopStyleVar();
    if (open) {  // (in the menus' look: the caller turns the pieces on)
        ImGui::PushFont(g_font_small);
        // the controller's home button first (what players reach for: the PS button on a DualSense), the combo second
        static const char *const home[] = {LN("Home button"), LN("Xbox button"), LN("PlayStation button")};  // (bind_label_family)
        static const char *const pad[] = {LN("Minus + Plus"), LN("View + Menu"), LN("Create + Options")};
        ImGui::PushTextWrapPos(0);
        ImGui::TextUnformatted(L("LAS Recomp has its own menu for settings, the randomizer, cosmetics and more."));
        ImGui::Spacing();
        int fam = bind_label_family() % 3;
        ImGui::Text(L("Open it in game with F1 or Esc, or press the %s or %s together on a controller."), L(home[fam]),
                    L(pad[fam]));
        ImGui::PopTextWrapPos();
        ImGui::Dummy(ImVec2(0, 14 * s));
        ImGui::SetCursorPosX((ImGui::GetWindowWidth() - 180 * s) * 0.5f);
        ImGui::SetItemDefaultFocus();
        if (ImGui::Button(L("OK"), ImVec2(180 * s, 44 * s)) || ImGui::IsKeyPressed(ImGuiKey_Enter) ||
            ImGui::IsKeyPressed(ImGuiKey_GamepadFaceDown)) {
            g_settings.menu_tip_seen = true;
            settings_save();
            ImGui::CloseCurrentPopup();
            go = true;
        }
        ImGui::PopFont();
        ImGui::EndPopup();
    }
    return go;
}

#ifdef __ANDROID__
// The game files page's example on Android: the system's folder picker, as it looks with the folder to choose opened
// ("Game files", exefs and romfs in it) and its "Use this folder" button at the bottom, which a gold arrow points at.
// The picker chooses the folder it's in, not one picked from a list. Drawn as the desktop's (shapes and the page's font,
// sized from the font), up to w wide, at the cursor; it takes its space with a Dummy.
static float folder_example_height(void) {
    float fs = ImGui::GetFontSize();
    return fs * (1.6f + 1.2f + 2 * 1.25f + 0.35f + 1.15f + 2.0f + 0.45f);
}
static void draw_folder_example(float w) {
    ImDrawList *dl = ImGui::GetWindowDrawList();
    ImFont *font = ImGui::GetFont();
    float fs = ImGui::GetFontSize(), ts = fs * 0.82f;
    float bar = fs * 1.6f, crumb = fs * 1.2f, rh = fs * 1.25f, r = fs * 0.45f;
    w = fminf(w, fs * 15);
    float h = folder_example_height();
    ImVec2 a = ImGui::GetCursorScreenPos(), b(a.x + w, a.y + h);
    const ImU32 body = IM_COL32(24, 28, 27, 255), title = IM_COL32(40, 45, 43, 255), edge = IM_COL32(255, 255, 255, 38),
                text = IM_COL32(220, 216, 205, 255), dim = IM_COL32(150, 152, 144, 255),
                gold = IM_COL32(236, 194, 84, 255), gold_tab = IM_COL32(200, 160, 62, 255),
                accent = IM_COL32(120, 170, 255, 255), button = IM_COL32(32, 84, 176, 255),
                text_sel = IM_COL32(250, 250, 255, 255);
    dl->AddRectFilled(ImVec2(a.x, a.y + fs * 0.12f), ImVec2(b.x, b.y + fs * 0.22f), IM_COL32(0, 0, 0, 70), r);  // shadow
    dl->AddRectFilled(a, b, body, r);
    dl->AddRectFilled(a, ImVec2(b.x, a.y + bar), title, r, ImDrawFlags_RoundCornersTop);
    dl->AddRect(a, b, edge, r, 0, 1.0f);
    // the top bar: a back arrow, then the opened folder's name
    float cy = a.y + bar * 0.5f, ax = a.x + fs * 0.6f, u = fs * 0.28f, th = fs * 0.09f;
    dl->AddLine(ImVec2(ax, cy), ImVec2(ax + u * 2.2f, cy), text, th);
    dl->AddLine(ImVec2(ax + th * 0.3f, cy), ImVec2(ax + u, cy - u), text, th);
    dl->AddLine(ImVec2(ax + th * 0.3f, cy), ImVec2(ax + u, cy + u), text, th);
    const char *gf = L("Game files");
    float tsz = fs * 0.95f;
    dl->AddText(font, tsz, ImVec2(ax + fs * 1.4f, cy - font->CalcTextSizeA(tsz, FLT_MAX, 0, gf).y * 0.5f), text, gf);
    // the path to it: Documents > Game files
    float y = a.y + bar, my = y + crumb * 0.5f, x = a.x + fs * 0.6f;
    const char *doc = L("Documents");
    ImVec2 dsz = font->CalcTextSizeA(ts, FLT_MAX, 0, doc);
    dl->AddText(font, ts, ImVec2(x, my - dsz.y * 0.5f), dim, doc);
    x += dsz.x + fs * 0.35f;
    float c = fs * 0.16f;
    dl->AddLine(ImVec2(x, my - c), ImVec2(x + c, my), dim, th * 0.8f);
    dl->AddLine(ImVec2(x + c, my), ImVec2(x, my + c), dim, th * 0.8f);
    x += c + fs * 0.35f;
    dl->AddText(font, ts, ImVec2(x, my - font->CalcTextSizeA(ts, FLT_MAX, 0, gf).y * 0.5f), accent, gf);
    // what's in it
    float g = fs * 0.78f;
    static const char *const rows[] = {"exefs", "romfs"};
    for (int i = 0; i < 2; i++) {
        float ry = y + crumb + i * rh, rmy = ry + rh * 0.5f, gx = a.x + fs * 0.6f, gy = rmy - g * 0.5f;
        if (i & 1) dl->AddRectFilled(ImVec2(a.x + 1, ry), ImVec2(b.x - 1, ry + rh), IM_COL32(255, 255, 255, 9));
        dl->AddRectFilled(ImVec2(gx, gy + g * 0.12f), ImVec2(gx + g * 0.46f, gy + g * 0.42f), gold_tab, g * 0.1f);
        dl->AddRectFilled(ImVec2(gx, gy + g * 0.26f), ImVec2(gx + g, gy + g * 0.9f), gold, g * 0.1f);
        ImVec2 nsz = font->CalcTextSizeA(ts, FLT_MAX, 0, rows[i]);
        dl->AddText(font, ts, ImVec2(gx + g + fs * 0.45f, rmy - nsz.y * 0.5f), text, rows[i]);
    }
    // the button, bottom right, and over it what to do with a gold arrow down to it
    const char *use = L("USE THIS FOLDER");
    ImVec2 usz = font->CalcTextSizeA(ts, FLT_MAX, 0, use);
    float bh = fs * 1.5f, bw = usz.x + fs * 1.3f, bx1 = b.x - fs * 0.5f, bx0 = bx1 - bw, by1 = b.y - fs * 0.45f,
          by0 = by1 - bh;
    dl->AddRectFilled(ImVec2(bx0, by0), ImVec2(bx1, by1), button, bh * 0.5f);
    dl->AddText(font, ts, ImVec2(bx0 + (bw - usz.x) * 0.5f, by0 + (bh - usz.y) * 0.5f), text_sel, use);
    const char *hint = L("Open the folder, then tap this");
    float hs = ts, room = w - fs * 1.0f, hw = font->CalcTextSizeA(hs, FLT_MAX, 0, hint).x;
    if (hw > room) hs *= fmaxf(0.7f, room / hw), hw = font->CalcTextSizeA(hs, FLT_MAX, 0, hint).x;
    float hy = by0 - fs * 0.5f - fs * 1.15f * 0.5f, acx = (bx0 + bx1) * 0.5f, hx = fminf(acx - hw * 0.5f, bx1 - hw);
    hx = fmaxf(hx, a.x + fs * 0.5f);
    dl->AddText(font, hs, ImVec2(hx, hy - font->CalcTextSizeA(hs, FLT_MAX, 0, hint).y * 0.5f), gold, hint);
    float t0 = hy + fs * 0.42f, t1 = by0 - fs * 0.08f, hd = fs * 0.22f;  // (the arrow, from under the hint to the button)
    dl->AddLine(ImVec2(acx, t0), ImVec2(acx, t1 - hd), gold, th);
    dl->AddTriangleFilled(ImVec2(acx - hd, t1 - hd), ImVec2(acx + hd, t1 - hd), ImVec2(acx, t1), gold);
    ImGui::Dummy(ImVec2(w, h));
}
#else
// The game files page's example: a Finder-style window (the same on every platform) with the folder to choose -
// "Game files", opened: exefs in it opened (its files), romfs closed. All drawn here (shapes and the page's font, sized from the font: sharp at
// any scale), up to w wide, at the cursor; it takes its space with a Dummy.
enum { FOLDER_EXAMPLE_ROWS = 9 };
static float folder_example_height(void) {  // (its window, without the Dummy's gap under it)
    float fs = ImGui::GetFontSize();
    return fs * 1.45f + fs * 0.35f * 2 + FOLDER_EXAMPLE_ROWS * fs * 1.12f;
}
static void draw_folder_example(float w) {
    ImDrawList *dl = ImGui::GetWindowDrawList();
    ImFont *font = ImGui::GetFont();
    float fs = ImGui::GetFontSize(), ts = fs * 0.82f;  // (the rows' text a little under the page's)
    float rh = fs * 1.12f, bar = fs * 1.45f, pad = fs * 0.35f, r = fs * 0.45f;
    struct Item { const char *name; int depth; int kind; };  // kind: 0 file, 1 folder opened, 2 folder closed
    static const Item items[] = {{LN("Game files"), 0, 1}, {"exefs", 1, 1}, {"main", 2, 0}, {"main.npdm", 2, 0},
                                 {"rtld", 2, 0}, {"sdk", 2, 0}, {"subsdk0", 2, 0}, {"subsdk1", 2, 0}, {"romfs", 1, 2}};
    const int n = FOLDER_EXAMPLE_ROWS;
    static_assert(sizeof items / sizeof items[0] == FOLDER_EXAMPLE_ROWS, "one row per item");
    w = fminf(w, fs * 15);
    float h = folder_example_height();
    ImVec2 a = ImGui::GetCursorScreenPos(), b(a.x + w, a.y + h);
    const ImU32 body = IM_COL32(24, 28, 27, 255), title = IM_COL32(40, 45, 43, 255), edge = IM_COL32(255, 255, 255, 38),
                text = IM_COL32(220, 216, 205, 255), dim = IM_COL32(150, 152, 144, 255),
                gold = IM_COL32(236, 194, 84, 255), gold_tab = IM_COL32(200, 160, 62, 255),
                paper = IM_COL32(226, 224, 216, 255), fold = IM_COL32(160, 158, 150, 255),
                select = IM_COL32(32, 84, 176, 255), text_sel = IM_COL32(250, 250, 255, 255);
    dl->AddRectFilled(ImVec2(a.x, a.y + fs * 0.12f), ImVec2(b.x, b.y + fs * 0.22f), IM_COL32(0, 0, 0, 70), r);  // shadow
    dl->AddRectFilled(a, b, body, r);
    dl->AddRectFilled(a, ImVec2(b.x, a.y + bar), title, r, ImDrawFlags_RoundCornersTop);
    dl->AddLine(ImVec2(a.x, a.y + bar), ImVec2(b.x, a.y + bar), IM_COL32(0, 0, 0, 90), 1.0f);
    dl->AddRect(a, b, edge, r, 0, 1.0f);
    // the title bar: three dots on the left, the folder it's in (a typical one) in the middle
    static const ImU32 dots[] = {IM_COL32(255, 95, 87, 255), IM_COL32(254, 188, 46, 255), IM_COL32(40, 200, 64, 255)};
    float dr = fs * 0.21f, cy = a.y + bar * 0.5f;
    for (int i = 0; i < 3; i++) dl->AddCircleFilled(ImVec2(a.x + fs * 0.65f + i * fs * 0.62f, cy), dr, dots[i]);
    const char *t = L("Documents");
    ImVec2 tsz = font->CalcTextSizeA(ts, FLT_MAX, 0, t);
    dl->AddText(font, ts, ImVec2(a.x + (w - tsz.x) * 0.5f, cy - tsz.y * 0.5f), text, t);
    // the list: a disclosure triangle on folders, the folder / file glyph, the name
    float g = fs * 0.78f;  // (glyph size)
    for (int i = 0; i < n; i++) {
        const Item &it = items[i];
        float y = a.y + bar + pad + i * rh, my = y + rh * 0.5f;
        if (i == 0)  // the folder to choose: selected, as Finder shows it
            dl->AddRectFilled(ImVec2(a.x + fs * 0.25f, y), ImVec2(b.x - fs * 0.25f, y + rh), select, fs * 0.25f);
        else if (i & 1) dl->AddRectFilled(ImVec2(a.x + 1, y), ImVec2(b.x - 1, y + rh), IM_COL32(255, 255, 255, 9));
        float x = a.x + fs * 0.5f + it.depth * fs * 1.25f;
        if (it.kind) {  // ▼ opened, ▶ closed
            float u = fs * 0.2f, cx = x + u;
            ImU32 tc = i == 0 ? text_sel : dim;
            if (it.kind == 1)
                dl->AddTriangleFilled(ImVec2(cx - u, my - u * 0.6f), ImVec2(cx + u, my - u * 0.6f),
                                      ImVec2(cx, my + u * 0.75f), tc);
            else
                dl->AddTriangleFilled(ImVec2(cx - u * 0.6f, my - u), ImVec2(cx - u * 0.6f, my + u),
                                      ImVec2(cx + u * 0.75f, my), tc);
        }
        x += fs * 0.6f;
        float gx = x, gy = my - g * 0.5f;
        if (it.kind) {  // folder: a tab, then the body over it
            dl->AddRectFilled(ImVec2(gx, gy + g * 0.12f), ImVec2(gx + g * 0.46f, gy + g * 0.42f), gold_tab, g * 0.1f);
            dl->AddRectFilled(ImVec2(gx, gy + g * 0.26f), ImVec2(gx + g, gy + g * 0.9f), gold, g * 0.1f);
        } else {  // file: a page with its top right corner folded
            float l = gx + g * 0.16f, rr = gx + g * 0.84f, tp = gy + g * 0.02f, bt = gy + g * 0.98f, c = g * 0.24f;
            ImVec2 pts[] = {ImVec2(l, tp), ImVec2(rr - c, tp), ImVec2(rr, tp + c), ImVec2(rr, bt), ImVec2(l, bt)};
            dl->AddConvexPolyFilled(pts, 5, paper);
            dl->AddTriangleFilled(ImVec2(rr - c, tp), ImVec2(rr - c, tp + c), ImVec2(rr, tp + c), fold);
        }
        const char *name = i == 0 ? L(it.name) : it.name;  // (the rest are the game's own names)
        ImVec2 nsz = font->CalcTextSizeA(ts, FLT_MAX, 0, name);
        float nx = x + g + fs * 0.4f;
        dl->AddText(font, ts, ImVec2(nx, my - nsz.y * 0.5f), i == 0 ? text_sel : text, name);
        if (i == 0) {  // an arrow pointing back at the name, then what to do (smaller when the row is narrow)
            const char *hint = L("Choose this one");
            float ax = nx + nsz.x + fs * 0.5f, aw = fs * 1.1f, gap = fs * 0.3f, room = b.x - fs * 0.6f - ax;
            float hs = ts, hw = font->CalcTextSizeA(hs, FLT_MAX, 0, hint).x;
            if (aw + gap + hw > room) hs *= fmaxf(0.7f, (room - aw - gap) / hw);
            float hx = ax + aw + gap, th = fs * 0.09f, hd = fs * 0.26f;
            dl->AddLine(ImVec2(ax + th, my), ImVec2(ax + aw, my), gold, th);
            dl->AddTriangleFilled(ImVec2(ax - th, my), ImVec2(ax + hd, my - hd), ImVec2(ax + hd, my + hd), gold);
            dl->AddText(font, hs, ImVec2(hx, my - font->CalcTextSizeA(hs, FLT_MAX, 0, hint).y * 0.5f), gold, hint);
        }
    }
    ImGui::Dummy(ImVec2(w, h));
}
#endif

// Start-screen text: a soft drop shadow, then the text with a top-to-bottom colour gradient (top == bottom: flat)
static void text_fx(ImDrawList *dl, ImFont *font, float size, ImVec2 p, ImU32 top, ImU32 bottom, const char *t,
                    float shadow) {
    if (shadow > 0)
        for (float d : {shadow, shadow * 0.5f}) dl->AddText(font, size, ImVec2(p.x + d, p.y + d), IM_COL32(4, 8, 30, 110), t);
    int v0 = dl->VtxBuffer.Size;
    dl->AddText(font, size, p, top, t);
    if (top == bottom) return;
    float y0 = p.y + size * 0.2f, y1 = p.y + size * 0.85f;
    for (int i = v0; i < dl->VtxBuffer.Size; i++) {
        float f = (dl->VtxBuffer[i].pos.y - y0) / (y1 - y0);
        f = f < 0 ? 0 : f > 1 ? 1 : f;
        ImVec4 a = ImGui::ColorConvertU32ToFloat4(top), b = ImGui::ColorConvertU32ToFloat4(bottom);
        dl->VtxBuffer[i].col = ImGui::ColorConvertFloat4ToU32(ImVec4(a.x + (b.x - a.x) * f, a.y + (b.y - a.y) * f,
                                                                     a.z + (b.z - a.z) * f, a.w + (b.w - a.w) * f));
    }
}

// A four-point sparkle (PLAY's twinkle): k = 0..1 grows and fades it
static void sparkle(ImDrawList *dl, ImVec2 c, float r, float k, float spin) {
    if (k <= 0) return;
    ImVec2 pts[8];
    for (int i = 0; i < 8; i++) {
        float a = spin + i * 3.14159265f / 4, rr = (i & 1 ? r * 0.22f : r) * k;
        pts[i] = ImVec2(c.x + cosf(a) * rr, c.y + sinf(a) * rr);
    }
    dl->AddCircleFilled(c, r * 0.55f * k, IM_COL32(255, 230, 150, (int)(60 * k)));  // soft glow
    dl->AddConvexPolyFilled(pts, 8, IM_COL32(255, 248, 215, (int)(235 * k)));
}

// split .. split_end: that part of the label in its own colour (RANDOMIZER's choice: New Seed in vanilla cream)
// A selected main-page item's look: a shadow band behind it (like the version corner's), feathered, with a warm glow
// (at, sz: the item's text)
static void menu_band(ImDrawList *dl, ImVec2 at, ImVec2 sz) {
    float x0 = ImGui::GetWindowPos().x, x1 = at.x + sz.x * 0.75f, x2 = at.x + sz.x + sz.y * 3.5f;
    float f = sz.y * 0.22f, y0 = at.y - f * 0.4f, y1 = y0 + f, y3 = at.y + sz.y + f * 0.2f, y2 = y3 - f;
    if (PLAT_TOUCH) {  // (taller than RANDOMIZER's ‹ choice › boxes: the band frames them)
        // (centred on the capitals' middle, as the boxes are - the line's middle is lower: room for descenders)
        ImFont *fn = ImGui::GetFont();
        const ImFontGlyph *gH = fn->FindGlyph('H');
        float c = at.y + (gH ? (gH->Y0 + gH->Y1) * 0.5f * ImGui::GetFontSize() / fn->FontSize : sz.y * 0.5f);
        float half = sz.y * 0.62f;
        y1 = c - half, y2 = c + half, y0 = y1 - f, y3 = y2 + f;
    }
    ImU32 c = IM_COL32(6, 10, 34, 150), g = IM_COL32(255, 190, 80, 34), n = IM_COL32(6, 10, 34, 0), ng = IM_COL32(255, 190, 80, 0);
    for (int pass = 0; pass < 2; pass++) {  // dark band, then the glow over it
        ImU32 a = pass ? g : c, z = pass ? ng : n;
        dl->AddRectFilledMultiColor(ImVec2(x0, y1), ImVec2(x1, y2), a, a, a, a);
        dl->AddRectFilledMultiColor(ImVec2(x1, y1), ImVec2(x2, y2), a, z, z, a);
        dl->AddRectFilledMultiColor(ImVec2(x0, y0), ImVec2(x1, y1), z, z, a, a);  // (feathered top / bottom)
        dl->AddRectFilledMultiColor(ImVec2(x1, y0), ImVec2(x2, y1), z, z, z, a);
        dl->AddRectFilledMultiColor(ImVec2(x0, y2), ImVec2(x1, y3), a, a, z, z);
        dl->AddRectFilledMultiColor(ImVec2(x1, y2), ImVec2(x2, y3), a, z, z, z);
    }
}

static bool menu_item(const char *label, bool enabled, bool *hovered = NULL, const char *split = NULL, ImU32 col2 = 0,
                      const char *split_end = NULL) {
    ImGui::PushFont(g_menu_small ? g_font_small : g_font_menu);
    ImGui::BeginDisabled(!enabled);
    ImVec2 at = ImGui::GetCursorScreenPos();
    // (the ID from the label's fixed part: RANDOMIZER's changes with the seed shown, and a new ID loses the selection)
    ImGui::PushID(label, split ? split : label + strlen(label));
    ImVec2 sz = ImGui::CalcTextSize(label);
    // (EnableNav: otherwise an InvisibleButton can't be reached with the d-pad / arrow keys - ImGui 1.91)
    ImGui::PushStyleColor(ImGuiCol_NavCursor, ImVec4(0, 0, 0, 0));  // (the hover band shows the selection, no box)
    bool clicked = ImGui::InvisibleButton("##b", ImVec2(sz.x + 40, sz.y + 6), ImGuiButtonFlags_EnableNav);
    ImGui::PopStyleColor();
    bool hot = ImGui::IsItemFocused() || ImGui::IsItemHovered();
    ImGui::PopID();
    if (hovered) *hovered = hot;
    ImU32 col = !enabled ? IM_COL32(170, 175, 200, 140) : hot ? IM_COL32(255, 214, 64, 255) : IM_COL32(250, 250, 255, 255);
    ImDrawList *dl = ImGui::GetWindowDrawList();
    if (hot && enabled) menu_band(dl, at, sz);
    ImU32 col_b = hot && enabled ? IM_COL32(255, 160, 50, 255) : col;
    if (split && enabled) {
        std::string head(label, split - label);
        text_fx(dl, ImGui::GetFont(), ImGui::GetFontSize(), at, col, col_b, head.c_str(), sz.y * 0.05f);
        std::string mid = split_end ? std::string(split, split_end - split) : std::string(split);
        ImVec2 p2(at.x + ImGui::CalcTextSize(label, split).x, at.y);
        text_fx(dl, ImGui::GetFont(), ImGui::GetFontSize(), p2, col2, col2, mid.c_str(), sz.y * 0.05f);
        if (split_end)
            text_fx(dl, ImGui::GetFont(), ImGui::GetFontSize(), ImVec2(at.x + ImGui::CalcTextSize(label, split_end).x, at.y),
                    col, col, split_end, sz.y * 0.05f);
    } else
        text_fx(dl, ImGui::GetFont(), ImGui::GetFontSize(), at, col, col_b, label, enabled ? sz.y * 0.05f : 0);
    if (g_menu_gap >= 0) ImGui::Dummy(ImVec2(0, sz.y * g_menu_gap));  // line spacing
    else ImGui::SetCursorPosY(ImGui::GetCursorPosY() + sz.y * g_menu_gap);  // (tighter than the font's line height)
    ImGui::EndDisabled();
    ImGui::PopFont();
    return clicked;
}

// Phones: the main page's items larger, RANDOMIZER's row as boxes a finger can hit - the title, then ‹ [the choice] ›;
// a tap on an arrow or a sideways swipe on the row steps the choice (g_pick_step, read by the caller), stopping at
// either end
static const float kPhoneMenu = PLAT_TOUCH && !plat_touch_tablet() ? 1.25f : 1.0f;  // (a tablet: a monitor's size)
static ImRect g_pick_rect;  // the ‹ choice › boxes as last drawn (the swipe's target)
static int g_pick_step;     // -1 back, +1 on

static void pick_swipe_event(const SDL_Event &e, ImVec2 ds) {
    static float sx, sy;
    static bool on;
    float x = e.tfinger.x * ds.x, y = e.tfinger.y * ds.y;
    if (e.type == SDL_EVENT_FINGER_DOWN) on = g_pick_rect.Contains(ImVec2(x, y)), sx = x, sy = y;
    else if (e.type == SDL_EVENT_FINGER_UP && on) {  // (right to left: the next one, as a carousel)
        float dx = x - sx, dy = y - sy;
        if (fabsf(dx) > ds.y * 0.05f && fabsf(dx) > 2 * fabsf(dy)) g_pick_step = dx < 0 ? 1 : -1;
        on = false;
    } else if (e.type == SDL_EVENT_FINGER_CANCELED) on = false;
}

static bool phone_pick_row(const char *title, const char *pick, int choice, int n, bool on, bool live, float win_w,
                           bool *hov) {
    ImGui::SetWindowFontScale(kPhoneMenu);
    ImGui::PushFont(g_font_menu);
    ImDrawList *dl = ImGui::GetWindowDrawList();
    ImFont *f = ImGui::GetFont();
    ImVec2 at = ImGui::GetCursorScreenPos();
    float h = ImGui::GetFontSize(), pad = h * 0.4f, gap = h * 0.18f;
    // (a capital's middle, down from the top of a line of that size: what the boxes and the name centre on)
    const ImFontGlyph *gH = f->FindGlyph('H');
    auto mid = [&](float size) { return gH ? (gH->Y0 + gH->Y1) * 0.5f * size / f->FontSize : size * 0.5f; };
    // (the boxes no taller than a line, so the row keeps the other items' spacing and SETTINGS sits as far below it as
    // it sits below PLAY)
    float bh = h * 0.84f, cy = at.y + mid(h), y0 = cy - bh * 0.5f, y1 = cy + bh * 0.5f;
    float pw = ImGui::CalcTextSize(title).x + pad;  // (the title bare, at the items' left edge; its target reaches the ‹)
    float nroom = win_w * 0.9f - at.x - pw - 3 * gap - 2 * bh - 2 * pad;  // (a long name: smaller past 90% of the window)
    float ntw = ImGui::CalcTextSize(pick).x, nsc = ntw > nroom ? fmaxf(nroom / ntw, 0.55f) : 1.0f;
    ImVec2 pa(at.x, y0), pb(at.x + pw, y1), la(pb.x + gap, y0), lb(la.x + bh, y1);
    ImVec2 na(lb.x + gap, y0), nb(na.x + ntw * 0.9f * nsc + 2 * pad, y1), ra(nb.x + gap, y0), rb(ra.x + bh, y1);
    g_pick_rect = ImRect(la, rb);
    bool can_back = choice > 0, can_on = choice < n - 1;  // (a step each way left)

    ImGui::SetCursorScreenPos(pa);
    ImGui::BeginDisabled(!on);
    ImGui::PushStyleColor(ImGuiCol_NavCursor, ImVec4(0, 0, 0, 0));  // (the box's gold edge shows it)
    bool pressed = ImGui::InvisibleButton("##play", ImVec2(pw, bh), ImGuiButtonFlags_EnableNav);
    ImGui::PopStyleColor();
    *hov = ImGui::IsItemFocused() || ImGui::IsItemHovered();
    bool play_down = ImGui::IsItemActive();  // (the title pressed)
    ImGui::EndDisabled();

    // the arrows' targets: their boxes and a margin (half the gap across, a third of a line up and down)
    ImVec2 m = ImGui::GetIO().MousePos;
    float ex = gap * 0.5f, ey = h * 0.33f;
    auto in = [&](ImVec2 a, ImVec2 b) { return m.x >= a.x - ex && m.x <= b.x + ex && m.y >= a.y - ey && m.y <= b.y + ey; };
    int under = live ? (in(la, lb) && can_back ? -1 : in(ra, rb) && can_on ? 1 : 0) : 0;
    if (under && ImGui::IsMouseReleased(0)) g_pick_step = under;
    if (!live || (g_pick_step < 0 && !can_back) || (g_pick_step > 0 && !can_on)) g_pick_step = 0;
    bool held = under && ImGui::IsMouseDown(0);

    auto box = [&](ImVec2 a, ImVec2 b, bool lit, bool down) {
        float r = bh * 0.28f;
        dl->AddRectFilled(a, b, down ? IM_COL32(56, 104, 214, 215) : lit ? IM_COL32(34, 70, 170, 190) : IM_COL32(14, 30, 92, 165), r);
        dl->AddRect(a, b, lit ? IM_COL32(255, 214, 64, 235) : IM_COL32(110, 170, 255, 160), r, 0, fmaxf(1.5f, h * 0.045f));
    };
    auto chevron = [&](ImVec2 a, ImVec2 b, int dir, bool lit) {
        float c = (a.x + b.x) * 0.5f, u = bh * 0.19f;
        ImVec2 pts[3] = {ImVec2(c - dir * u * 0.45f, cy - u), ImVec2(c + dir * u * 0.55f, cy), ImVec2(c - dir * u * 0.45f, cy + u)};
        bool can = dir < 0 ? can_back : can_on;  // (at an end: that arrow greyed out)
        ImU32 col = !on || !can ? IM_COL32(170, 175, 200, 120) : lit ? IM_COL32(255, 236, 140, 255) : IM_COL32(255, 196, 64, 255);
        dl->AddPolyline(pts, 3, col, 0, bh * 0.1f);
    };
    if (on && (*hov || play_down)) menu_band(dl, ImVec2(pa.x, at.y), ImVec2(ImGui::CalcTextSize(title).x, h));  // (behind the boxes)
    box(la, lb, held && under < 0, held && under < 0);
    box(na, nb, false, false);
    box(ra, rb, held && under > 0, held && under > 0);
    chevron(la, lb, -1, held && under < 0);
    chevron(ra, rb, 1, held && under > 0);
    // (as menu_item draws SETTINGS: white, or gold to orange on its band when selected)
    bool hot = on && (*hov || play_down);
    ImU32 top = !on ? IM_COL32(170, 175, 200, 140) : hot ? IM_COL32(255, 214, 64, 255) : IM_COL32(250, 250, 255, 255);
    ImU32 bottom = hot ? IM_COL32(255, 160, 50, 255) : top;
    text_fx(dl, f, h, ImVec2(pa.x, at.y), top, bottom, title, on ? h * 0.05f : 0);
    ImU32 nc = choice ? IM_COL32(250, 250, 255, 255) : IM_COL32(246, 232, 176, 255);  // ("New Seed" in vanilla cream)
    float nh = h * 0.9f * nsc;  // (a little under the line, inside the shorter box)
    text_fx(dl, f, nh, ImVec2(na.x + pad, cy - mid(nh)), nc, nc, pick, nh * 0.05f);

    ImGui::SetCursorScreenPos(at);  // (then down as menu_item steps: its button, then the line spacing)
    ImGui::Dummy(ImVec2(rb.x - at.x, h + 6));
    if (g_menu_gap >= 0) ImGui::Dummy(ImVec2(0, h * g_menu_gap));
    else ImGui::SetCursorPosY(ImGui::GetCursorPosY() + h * g_menu_gap);
    ImGui::PopFont();
    ImGui::SetWindowFontScale(1.0f);
    return pressed;
}

// Controllers, as in the in-game menu (runtime/ui/menu.mm): read straight from SDL, confirm / back as the Confirm
// button setting says (pad_nav.h: the game's layout, or the controller's convention); d-pad / left stick move.
// (ImGui's own SDL gamepad handling is switched off: per the user, it didn't work on the start screen.)

static void launcher_pad_open_all(void) {
    int n = 0;
    SDL_JoystickID *ids = SDL_GetGamepads(&n);
    for (int i = 0; ids && i < n; i++) SDL_OpenGamepad(ids[i]);
    SDL_free(ids);
}

// true when the event was a controller event (handled here)
static bool launcher_pad_event(const SDL_Event &e) {
    ImGuiIO &io = ImGui::GetIO();
    switch (e.type) {
    case SDL_EVENT_GAMEPAD_ADDED:
        SDL_OpenGamepad(e.gdevice.which);
        io.BackendFlags |= ImGuiBackendFlags_HasGamepad;
        return true;
    case SDL_EVENT_GAMEPAD_BUTTON_DOWN: case SDL_EVENT_GAMEPAD_BUTTON_UP: {
        ImGuiKey k = ui_pad_key(e.gbutton.which, e.gbutton.button, e.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN);
        if (k != ImGuiKey_None) io.AddKeyEvent(k, e.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN);
        return true;
    }
    case SDL_EVENT_GAMEPAD_AXIS_MOTION: {
        float v = e.gaxis.value / 32767.0f;
        // the left stick also works as the d-pad (ImGui moves between items with the d-pad only; the stick scrolls):
        // pressed past half way, released under 0.3
        if (e.gaxis.axis == SDL_GAMEPAD_AXIS_LEFTX || e.gaxis.axis == SDL_GAMEPAD_AXIS_LEFTY) {
            static bool held[4];  // left, right, up, down
            bool x = e.gaxis.axis == SDL_GAMEPAD_AXIS_LEFTX;
            int neg = x ? 0 : 2, pos = neg + 1;
            const ImGuiKey keys[4] = {ImGuiKey_GamepadDpadLeft, ImGuiKey_GamepadDpadRight, ImGuiKey_GamepadDpadUp,
                                      ImGuiKey_GamepadDpadDown};
            for (int i : {neg, pos}) {
                float m = i == neg ? -v : v;
                bool now = held[i] ? m > 0.3f : m > 0.5f;
                if (now != held[i]) { held[i] = now; io.AddKeyEvent(keys[i], now); }
            }
        }
        if (e.gaxis.axis == SDL_GAMEPAD_AXIS_LEFTX) {
            io.AddKeyAnalogEvent(ImGuiKey_GamepadLStickLeft, v < -0.3f, v < -0.3f ? -v : 0);
            io.AddKeyAnalogEvent(ImGuiKey_GamepadLStickRight, v > 0.3f, v > 0.3f ? v : 0);
        } else if (e.gaxis.axis == SDL_GAMEPAD_AXIS_LEFTY) {
            io.AddKeyAnalogEvent(ImGuiKey_GamepadLStickUp, v < -0.3f, v < -0.3f ? -v : 0);
            io.AddKeyAnalogEvent(ImGuiKey_GamepadLStickDown, v > 0.3f, v > 0.3f ? v : 0);
        } else if (e.gaxis.axis == SDL_GAMEPAD_AXIS_RIGHTY) {  // (scrolls a right column of plain text: option_rows.mm)
            io.AddKeyAnalogEvent(ImGuiKey_GamepadRStickUp, v < -0.1f, v < 0 ? -v : 0);
            io.AddKeyAnalogEvent(ImGuiKey_GamepadRStickDown, v > 0.1f, v > 0 ? v : 0);
        }
        return true;
    }
    default: return false;
    }
}

// Returns when PLAY is chosen (seed stored in g_settings.rando_seed); QUIT exits.
// Settings' first tab, on the start screen only: the extracted game files (size, delete) and the user's own Switch
// fonts (import / back to the substitute)
// Open a folder in Finder / Explorer / the Files app (relative paths are in the data root, the working directory)
#ifndef __ANDROID__
static void reveal_folder(const std::string &dir) {
    std::string path = dir;
    bool abs = !path.empty() && (path[0] == '/' || path[0] == '\\' || (path.size() > 1 && path[1] == ':'));
    if (!abs) path = std::string(rt_data_root()) + "/" + path;
    std::string url = "file://";
    if (path[0] != '/') url += "/";  // (Windows: file:///C:/...)
    for (char ch : path) {
        if (ch == '\\') ch = '/';
        if (ch == ' ') url += "%20";
        else if (ch == '%') url += "%25";
        else if (ch == '#') url += "%23";
        else url += ch;
    }
    SDL_OpenURL(url.c_str());
}
#endif

static SDL_Window *g_launcher_win;

// The window's safe area insets (the notch / Dynamic Island, the home indicator), in the start screen's points; 0 on the desktops
static void safe_insets(float *l, float *t, float *r, float *b) {
    *l = *t = *r = *b = 0;
#if PLAT_TOUCH
    // Held while the system keyboard is up: SDL slides the view up to keep the text field (ImGui's IME rect) above
    // the keyboard, which takes the view's bottom off the home indicator - a new safe area, which would move the
    // field, and SDL would slide again: the screen shook back and forth between the two.
    static float kl, kt, kr, kb;
    bool kbd = SDL_TextInputActive(g_launcher_win) || SDL_ScreenKeyboardShown(g_launcher_win);
    SDL_Rect sa;
    int ww, wh;
    if (!kbd && SDL_GetWindowSafeArea(g_launcher_win, &sa) && SDL_GetWindowSize(g_launcher_win, &ww, &wh) && ww > 0)
        kl = sa.x, kt = sa.y, kr = ww - sa.x - sa.w, kb = wh - sa.y - sa.h;
    *l = kl, *t = kt, *r = kr, *b = kb;
#endif
}

// The paper behind the Settings / Game files window (pa, w x h): phones and tablets fill the screen with it
static void page_panel(ImVec2 wp, ImVec2 ds, ImVec2 pa, float w, float h, float sc) {
    if (PLAT_TOUCH) plastic_panel_full(ImGui::GetWindowDrawList(), wp, ImVec2(wp.x + ds.x, wp.y + ds.y), sc);
    else plastic_panel(ImGui::GetWindowDrawList(), pa, ImVec2(pa.x + w, pa.y + h), sc);  // (framed)
}

// Touch screens: the window reaches the right edge for its X, what's on the page keeps that side's inset
static void page_content_inset(void) {
#if PLAT_TOUCH
    float sl, st, sr, sb;
    safe_insets(&sl, &st, &sr, &sb);
    touch_cushion(&sl, &sr, g_launcher_win);
    ImGui::GetCurrentWindow()->WorkRect.Max.x -= sr;
    ImGui::GetCurrentWindow()->ContentRegionRect.Max.x -= sr;
#endif
}

// The Settings / Game files window: its scale (1 at 1080 lines) and size, centred in the screen; phones: a third
// larger (as the in-game menu there: runtime/ui/menu.mm kUi) and the whole safe area (the notch, the home indicator).
// Once a frame: it scales FontGlobalScale, which the frame's start sets afresh.
static void panel_layout(ImVec2 ds, float *sc, float *w, float *h, ImVec2 *centre) {
    *sc = ds.y / 1080.0f, *w = fminf(ds.x * 0.9f, 1500 * *sc), *h = ds.y - 2 * 18 * *sc;  // (all the height: a slim margin)
    *centre = ImVec2(ds.x * 0.5f, ds.y * 0.5f);
#if PLAT_TOUCH
    // (the start screen's DisplaySize is in points, as SDL's safe area)
    float sl, st, sr, sb;
    safe_insets(&sl, &st, &sr, &sb);
    const float phone = plat_touch_tablet() ? 1.0f : 1.35f;  // (a tablet's screen is a monitor's size: no enlargement)
    *sc *= phone;
    ImGui::GetIO().FontGlobalScale *= phone;
    // (each side's own inset: a camera cutout is on one side only in landscape. Top to bottom the whole screen, and
    // past it by the window's padding, so the page runs to the edges - menu.mm's page window)
    g_plastic_corner_r = touch_corner_radius(sl, sr);  // (the corner buttons keep off the curve)
    touch_cushion(&sl, &sr, g_launcher_win);
    float pad = 24 * *sc, top = 14 * *sc - pad;
    *w = ds.x - sl, *h = ds.y + pad - 8 * *sc - top;  // (to the right edge, for the X: page_content_inset)
    *centre = ImVec2(sl + *w * 0.5f, top + *h * 0.5f);
#endif
}

// ------------------------------------------------------------------ iOS export (runtime/ui/ios_export_ui.mm)
// Its page shares the setup's busy state, job line and progress bar through these hooks.
#if LARECOMP_IOSEXPORT
static void ios_export_wire(void) {
    static bool wired;
    if (wired) return;
    wired = true;
    IosExportHost h = {
        [] { pthread_mutex_lock(&g_lock); },
        [] { pthread_mutex_unlock(&g_lock); },
        &g_busy,
        [](const std::string &line, bool failed, const std::string &hint) { set_job(line, failed, hint); },
        [] {  // (as the setup's thread starts: no copy phase, the bar is the build)
            stop_background_prep();
            g_cancel_build = false;
            g_done = 0, g_total = 0, g_phase_base = 0, g_build_frac = 0, g_copy_skipped = true;
            set_job(LN("Getting the game files ready..."));  // (not the last export's line, path and all)
        },
        build_progress,
        build_cancelled,
        [] { g_build_frac = -1; },
        [](const std::string &dir) { reveal_folder(dir); },
        &g_root,
    };
    ios_export_ui_init(h);
}
#endif

// A button of the progress overlay, drawn as the menus' buttons are (hot: lit, dark text) - the overlay is drawn on
// the foreground over everything, not as ImGui items
static void busy_button(ImDrawList *dl, ImVec2 a, ImVec2 b, const char *label, float fs, bool hot, float sc) {
    const PlasticTheme &t = plastic_theme();
    plastic_pill(dl, a, b, (b.y - a.y) * 0.5f, hot ? t.row_hot : t.row, (hot ? 6 : 4.5f) * sc, hot ? 1.0f : 0.7f, sc, hot);
    ImVec2 ls = g_font_small->CalcTextSizeA(fs, FLT_MAX, 0, label);
    dl->AddText(g_font_small, fs, ImVec2((a.x + b.x - ls.x) * 0.5f, (a.y + b.y - ls.y) * 0.5f),
                hot ? t.text_sel : t.text_button, label);
}

// The progress overlay in front of the start screen (input waits): the setup, an update's build, or the shader
// preparation (with Skip) - the setup's, or the background one when PLAY was pressed before it finished
static void draw_busy_overlay(ImVec2 ds, const std::string &job, bool test_busy, bool prep_wait = false);  // (Inside The Dream's, below)

// ------------------------------------------------------------------ the data folder (runtime/paths.h): another one
// picked (Settings > Prelaunch, the System window), the data moved there or a fresh start, then a restart there
enum { DF_NONE, DF_ASK, DF_MOVING, DF_FAILED, DF_RESTART };
static int g_df;                  // (main thread)
static std::string g_df_pick;     // the folder picked (g_lock: the dialog calls back on any thread)
static RtDataTarget g_df_t;
static const char *g_df_cloud;    // the folder picked is synced to a cloud drive too (its name; NULL: not)
static std::atomic<int> g_df_step{0}, g_df_result{0};  // result: 0 running, 1 moved, -1 failed
static std::atomic<float> g_df_frac{0};
static char g_df_err[600];
static bool g_df_auto_move, g_df_auto_fresh;  // (test: LARECOMP_LAUNCHER_DATA_PICK=<folder>:move / :fresh)
static bool g_df_cancelling;                  // Cancel pressed during the move: waiting for it to stop

// A folder as the player knows it: its path; on iOS as Files names it ("iCloud Drive › LASData"), the full path
// when the place isn't recognized
static std::string folder_shown(const char *path) {
#if PLAT_IOS
    char root[256], rest[1024];
    if (!plat_ios_folder_place(path, root, sizeof root, rest, sizeof rest)) return path;
    const char *top = root[0] ? root : plat_touch_tablet() ? L("On My iPad") : L("On My iPhone");
    return rest[0] ? std::string(top) + " \xe2\x80\xba " + rest : top;
#else
    return path;
#endif
}
static std::string data_root_shown(void) { return folder_shown(rt_data_root()); }

static void SDLCALL picked_data(void *, const char *const *files, int) {
    if (!files || !files[0]) return;
    pthread_mutex_lock(&g_lock);
    g_df_pick = files[0];
    pthread_mutex_unlock(&g_lock);
}
static void choose_data_folder(void) { SDL_ShowOpenFolderDialog(picked_data, NULL, g_launcher_win, NULL, false); }
static void show_font_dialog(void) {
#ifdef __ANDROID__
    android_pick_files(picked_fonts);  // (SDL's file window gives content:// links there, not files)
#else
    static SDL_DialogFileFilter font_f[] = {{"Switch fonts", "bfttf;ttf"}};
    font_f[0].name = L("Switch fonts");  // (the file window's; L's text stays)
    SDL_ShowOpenFileDialog(picked_fonts, NULL, g_launcher_win, font_f, 1, NULL, true);
#endif
}
static void df_progress(int step, double f) { g_df_step = step, g_df_frac = (float)f; }
static void *df_move_thread(void *) {
    g_df_result = rt_data_move(g_df_t.path, df_progress, g_df_err, sizeof g_df_err) ? 1 : -1;
    return NULL;
}
static void df_restart(void) {
    unsetenv("LARECOMP_LAUNCHER_DATA_PICK");
#if PLAT_IOS
    g_df = DF_RESTART;  // (no exec on iOS: the app ends on OK, the player opens it again)
#else
    rt_paths_restart();
    snprintf(g_df_err, sizeof g_df_err, "%s", LN("Couldn't restart LAS Recomp. Start it again to use the new folder."));
    g_df = DF_FAILED;
#endif
}

#ifdef __ANDROID__
// Android: the folder is on the SD card - the app's folder there (LARECOMP_ANDROID_SD_DIR) or one inside it (a
// "LAS Recomp" folder a move made there when the card's folder had other files)
static bool android_on_sd(const char *p) {
    const char *sd = getenv("LARECOMP_ANDROID_SD_DIR");
    char a[PATH_MAX], b[PATH_MAX];
    if (!sd || !*sd || !realpath(p, a) || !realpath(sd, b)) return false;
    size_t n = strlen(b);
    return !strncmp(a, b, n) && (a[n] == 0 || a[n] == '/');
}
#endif

// The question / the move's progress, in front of everything (called at the start screen's top level)
static void data_folder_popup(float s) {
    float sc = s * 720.0f / 1080.0f;
    pthread_mutex_lock(&g_lock);
    std::string pick;
    pick.swap(g_df_pick);
    bool busy = g_busy;
    pthread_mutex_unlock(&g_lock);
    if (!pick.empty() && g_df == DF_NONE && !busy) {
        rt_data_target(pick.c_str(), &g_df_t);
        g_df_cloud = g_df_t.refuse[0] ? NULL : plat_cloud_synced(g_df_t.path);
        g_df = DF_ASK;
        ImGui::OpenPopup("##datafolder");
    }
    ImVec2 ds = ImGui::GetIO().DisplaySize;
    ImGui::SetNextWindowPos(ImVec2(ds.x * 0.5f, ds.y * 0.5f), ImGuiCond_Always, ImVec2(0.5f, 0.5f));  // (centred as it shrinks)
    if (!ImGui::BeginPopupModal("##datafolder", NULL, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoTitleBar))
        return;
    const RtDataTarget &t = g_df_t;
    const ImVec4 red = plastic_col(plastic_theme().warn);
    ImVec2 bw(240 * sc, 0);
    ImGui::PushFont(g_font_small);
    ImGui::PushFont(g_font_menu);
    ImGui::TextColored(ImVec4(0.93f, 0.76f, 0.33f, 1), "%s", g_df == DF_MOVING ? L("Moving your data") : L("Data folder"));
    ImGui::PopFont();
    ImGui::PushTextWrapPos(900 * sc);
    bool close = false, esc = ImGui::IsKeyPressed(ImGuiKey_GamepadFaceRight, false) || ImGui::IsKeyPressed(ImGuiKey_Escape, false);
#ifdef __ANDROID__
    // Android: where the data goes as the Storage row names it (the paths say nothing to a player): "Internal storage
    // -> SD card" (the menu font's arrow when it has one)
    auto on_sd = android_on_sd;
    const char *arrow = ImGui::GetFont()->FindGlyphNoFallback(0x2192) ? " \xe2\x86\x92 " : " -> ";
    std::string dir = std::string(L(on_sd(rt_data_root()) ? "SD card" : "Internal storage")) + arrow +
                      L(on_sd(t.path) ? "SD card" : "Internal storage");
#endif
    auto use_it = [&] {
        stop_background_prep();
        settings_save();
        if (rt_data_use(t.path, g_df_err, sizeof g_df_err)) df_restart();
        else g_df = DF_FAILED;
    };
    if (g_df == DF_ASK) {
#ifdef __ANDROID__
        ImGui::TextDisabled("%s", dir.c_str());
#else
        ImGui::TextDisabled("%s", folder_shown(t.path).c_str());
#endif
        if (g_df_cloud) ImGui::TextColored(ImVec4(1, 0.72f, 0.4f, 1), L("This folder is synced by %s too."), g_df_cloud);
        ImGui::Spacing();
        if (t.refuse[0]) {
            ImGui::TextColored(red, "%s", t.refuse);
            ImGui::Spacing();
            close = ImGui::Button(L("OK"), bw) || esc;
        } else if (t.has_data) {
            ImGui::TextUnformatted(L("This folder already has LAS Recomp data. Use it?"));
            ImGui::Spacing();
            if (ImGui::Button(L("Use it"), bw) || g_df_auto_fresh) g_df_auto_fresh = false, use_it();
            ImGui::SameLine();
            close = ImGui::Button(L("Cancel"), bw) || esc;
        } else {
            bool room = t.free >= t.need;
            ImGui::Text(L("Move your saves, seeds, settings and game files there, %.1f GB in all, or start fresh?"),
                        t.bytes / 1e9);
            if (!room) ImGui::TextColored(red, L("Not enough space there. Only %.1f GB is free."), t.free / 1e9);
            ImGui::Spacing();
            ImGui::BeginDisabled(!room);
            if ((ImGui::Button(L("Move"), bw) || g_df_auto_move) && room) {
                g_df_auto_move = false;
                stop_background_prep();
                settings_save();
                g_df = DF_MOVING, g_df_step = 0, g_df_frac = 0, g_df_result = 0;
                pthread_t th;
                if (pthread_create(&th, NULL, df_move_thread, NULL) == 0) pthread_detach(th);
                else snprintf(g_df_err, sizeof g_df_err, "%s", LN("Couldn't start the move.")), g_df = DF_FAILED;
            }
            ImGui::EndDisabled();
            ImGui::SameLine();
            if (ImGui::Button(L("Start fresh"), bw) || g_df_auto_fresh) g_df_auto_fresh = false, use_it();
            ImGui::SameLine();
            ImGui::SetItemDefaultFocus();
            close = ImGui::Button(L("Cancel"), bw) || esc;
        }
    } else if (g_df == DF_MOVING) {
        static const char *const steps[] = {LN("Copying..."), LN("Checking the copy..."), LN("Removing the old copy...")};
        int r = g_df_result, k = g_df_step;
        ImGui::Text("%s", r == 1 ? L("Restarting...") : g_df_cancelling ? L("Cancelling...") : L(steps[k < 0 ? 0 : k > 2 ? 2 : k]));
        plastic_progress_bar(r == 1 ? 1.0f : g_df_frac.load(), ImVec2(900 * sc, 0), NULL, sc);
#ifdef __ANDROID__
        ImGui::TextDisabled("%s", dir.c_str());  // (under the bar: which way it goes)
#endif
        // Cancel: while it copies or checks (the copy is removed, the data stays where it was); not once the switch
        // is made and the old copy is being removed
        if (r == 0 && k < 2) {
            ImGui::Spacing();
            ImGui::BeginDisabled(g_df_cancelling);
            if (ImGui::Button(L("Cancel"), bw) || (esc && !g_df_cancelling)) g_df_cancelling = true, rt_data_move_cancel();
            ImGui::EndDisabled();
        }
        if (r == 1) df_restart();
        else if (r == -1 && g_df_cancelling) close = true;  // (cancelled: nothing to report)
        else if (r == -1) g_df = DF_FAILED;
        if (r != 0) g_df_cancelling = false;
    } else if (g_df == DF_RESTART) {
        ImGui::TextUnformatted(L("LAS Recomp needs to restart to use the new folder. It closes now; open it again."));
        ImGui::Spacing();
        if (ImGui::Button(L("OK"), bw)) { fflush(stderr); exit(0); }
    } else {
        ImGui::TextColored(red, "%s", L_line(g_df_err).c_str());  // (English: here or runtime/paths.c)
        ImGui::Text(L("Your data is still in %s"), data_root_shown().c_str());
        ImGui::Spacing();
        close = ImGui::Button(L("OK"), bw) || esc;
    }
    ImGui::PopTextWrapPos();
    ImGui::PopFont();
    if (close) g_df = DF_NONE, ImGui::CloseCurrentPopup();
    ImGui::EndPopup();
}

// The Storage section (Settings > Prelaunch, the System window). Android: the app's own folder in internal
// storage or on the SD card - the only places it can use by path (runtime/paths.c rt_data_root_fixed); another one
// picked goes through the same question as on the desktop (move the data there, or start fresh).
static void storage_rows(bool busy) {
    const char *fixed = rt_data_root_fixed() ? L(rt_data_root_fixed()) : NULL;
    std::string d = std::string(L("Where LAS Recomp keeps game files, saves, seeds and settings.")) + "\n\n" +
                    data_root_shown() + (fixed ? std::string("\n\n") + fixed : "");
    opt_section("Storage");
#ifdef __ANDROID__
    const char *sd = getenv("LARECOMP_ANDROID_SD_DIR"), *home = rt_data_home();
    bool have_sd = sd && *sd;
    int cur = have_sd && android_on_sd(rt_data_root()) ? 1 : 0, pick = cur;
    static const char *const where[] = {LN("Internal storage"), LN("SD card")};
    if (!have_sd && !cur) d += std::string("\n\n") + L("Put in an SD card to keep it there.");
    if (busy || fixed || (!have_sd && !cur)) opt_disable_next();
    if (opt_choice("Data folder", &pick, where, 2, 0, d.c_str(), false) && pick != cur) {
        pthread_mutex_lock(&g_lock);
        g_df_pick = pick ? sd : home;
        pthread_mutex_unlock(&g_lock);
    }
#else
    if (busy || fixed) opt_disable_next();
    if (opt_action("Data folder", "Change...", d.c_str(), true)) choose_data_folder();
#endif
}

#ifdef __ANDROID__
// The Vulkan driver package (Settings > System): the activity copies a picked one into the drivers folder, and the
// driver is loaded at the next start (runtime/nvn/vulkan/android_driver.c). From LASRecompAndroid by mamcjr (MIT).
static std::string g_driver_msg;
static bool g_driver_restart;
static void driver_picked(const char *r) {  // (another thread)
    pthread_mutex_lock(&g_lock);
    g_driver_msg = *r ? "" : L("That file isn't a driver package (.zip or .so).");
    g_driver_restart = *r != 0;  // (a package, or the phone's own again: driver_rows restarts on it)
    pthread_mutex_unlock(&g_lock);
}

extern "C" uint32_t vk_gpu_vendor(void);  // runtime/nvn/vulkan/device.c
static void driver_rows(void) {
    android_on_driver(driver_picked);
    auto base = [](const char *p) { const char *s = p ? strrchr(p, '/') : NULL; return std::string(s ? s + 1 : p ? p : ""); };
    std::string want = base(getenv("LARECOMP_VK_DRIVER")), used = base(getenv("LARECOMP_VK_DRIVER_IN_USE"));
    std::string d = L("The Vulkan driver the game runs on. On some Adreno GPUs a custom Turnip driver package may be "
                      "required to run or may provide better performance.");
    if (!want.empty() && used.empty()) {
        char b[400];
        snprintf(b, sizeof b, L("%s couldn't run here, so the phone's own driver is in use."), want.c_str());
        d += std::string("\n\n") + b;
    }
    pthread_mutex_lock(&g_lock);
    std::string msg = g_driver_msg;
    bool restart = g_driver_restart;
    g_driver_restart = false;
    pthread_mutex_unlock(&g_lock);
    // the driver is loaded at the start (runtime/nvn/vulkan/android_driver.c): a new one takes a restart, made at once
    if (restart) {
        stop_background_prep();
        settings_save();
        android_call("restartApp");
    }
    if (!msg.empty()) d += "\n\n" + msg;
    // Driver packages (Turnip) exist only for Adreno: the replacement driver has to talk to the phone's own kernel
    // driver, and only Mesa's Turnip does (Qualcomm's KGSL); Mesa's Mali, Xclipse and PowerVR drivers need kernel
    // drivers phones don't ship. Elsewhere the section is hidden - unless a package is set anyway (settings from
    // another phone), so it can be cleared. Vendor not known yet (0): shown.
    uint32_t vendor = vk_gpu_vendor();
    if (vendor && vendor != 0x5143 && want.empty()) return;
    opt_section("Graphics driver");
    if (opt_action("Vulkan driver", used.empty() ? LN("Phone's own") : used.c_str(), d.c_str(), true))
        android_call("pickDriver");
    if (!want.empty() && opt_action("Use the phone's own driver", "", "Goes back to the phone's own Vulkan driver. "
                                    "LAS Recomp restarts to use it.", true))
        android_call("useSystemDriver");
}
#endif

static bool g_files_deleted;  // set here: the start screen goes back to setting the game files up
// Settings > System, as the other tabs: option rows on the left, the selected row's description on the right
static void prelaunch_tab(float sc) {
    static double gb = -1;
    static bool confirm_delete;
    if (gb < 0) gb = rt_game_files_size(g_root.c_str()) / 1e9;
    char gbs[32];
    snprintf(gbs, sizeof gbs, "%.1f GB", gb);
    opt_begin("##gamefiles", sc);
    opt_section("Game files");
#ifdef __ANDROID__
    // (none: other apps can't open the app's own folder there)
#else
#if PLAT_IOS
    const char *show = LN("Open in Files");
#elif defined(_WIN32)
    const char *show = LN("Show in Explorer");
#elif defined(__linux__)
    const char *show = LN("Open folder");
#else
    const char *show = LN("Show in Finder");
#endif
    if (opt_action(show, "", "Opens the folder with LAS Recomp's copy of the game files.")) reveal_folder(g_root);
#endif
    static bool shot_del = getenv("LARECOMP_LAUNCHER_SHOT") && strstr(getenv("LARECOMP_LAUNCHER_SHOT"), ":askdelete");
    if (shot_del) confirm_delete = true, shot_del = false;  // (test: :askdelete)
    if (!confirm_delete) {
        if (opt_action("Delete game files", gbs, "Frees up the space LAS Recomp's copy of the game files uses. Your saves, "
                       "seeds and settings stay.", true))
            confirm_delete = true;
    } else {
        std::string d = L("Deletes the extracted game files now.");
        if (rt_dev_mode()) {  // (a source checkout: the files are the project's own extracted/)
            bool abs = !g_root.empty() && (g_root[0] == '/' || (g_root.size() > 1 && g_root[1] == ':'));
            std::string where = abs ? g_root : std::string(rt_data_root()) + "/" + g_root;
            char b[700];
            snprintf(b, sizeof b, L("Dev checkout: this is %s"), where.c_str());
            d += std::string("\n\n") + b;
        }
        opt_warn_next();
        if (opt_action("Yes, delete them", gbs, d.c_str(), true)) {
            stop_background_prep();
            bool ok = rt_remove_game_files(g_root.c_str());  // (runtime/paths.c: only what the extraction made)
            game_files_changed();
            set_job(ok ? LN("Game files deleted.") : LN("Couldn't delete the game files. They aren't in LAS Recomp's data folder."),
                    !ok);
            g_files_deleted = true, gb = -1, confirm_delete = false;
        }
        if (opt_action("Cancel", "", "Keeps the game files.", true)) confirm_delete = false;
    }
#if LARECOMP_IOSEXPORT
    {  // the iOS export's page opens from here (runtime/ui/ios_export_ui.mm)
        pthread_mutex_lock(&g_lock);
        bool busy = g_busy;
        pthread_mutex_unlock(&g_lock);
        opt_section("iOS");
        ios_export_prelaunch(busy);
    }
#endif
    storage_rows(false);  // where all of it is kept (as the setup window's Storage row)
#ifdef __ANDROID__
    driver_rows();
#endif
    opt_section("Game font");
    import_fonts();
    bool fs = have_font(FONT_STD, 4), fl = have_font(FONT_LANG, 8), fe = have_font(FONT_EXT, 2);
    const char *fonts_d = LN("Nintendo's fonts aren't included. For the original look, import your own "
                             "nintendo_udsg-r_std_003 for text, nintendo_udsg-r_org_zh-cn_003, nintendo_udsg-r_ext_zh-cn_003, "
                             "nintendo_udjxh-db_zh-tw_003 and nintendo_udsg-r_ko_003 for Chinese and Korean, and "
                             "nintendo_ext_003 for button icons. Both .bfttf and .ttf work.");
    // (what's in use: rows with a value only, nothing to change)
    opt_action("Text font", fs ? LN("Imported") : LN("Substitute"), fonts_d, true, fs);
    opt_action("Chinese and Korean font", fl ? LN("Imported") : LN("Substitute"), fonts_d, true, fl);
    opt_action("Button icons", fe ? LN("Imported") : LN("Substitute"), fonts_d, true, fe);
    std::string imp_d = L(fonts_d);
    if (!g_font_msg.empty()) imp_d += "\n\n" + g_font_msg;
    if (opt_action("Import fonts", "", imp_d.c_str(), true)) show_font_dialog();
    if ((fs || fl || fe) && opt_action("Remove imported fonts", "", "Deletes the fonts you imported. The game goes back to "
                                       "the substitutes.", true))
        remove_fonts();
    opt_end();
}

// The start screen's fonts, with the glyphs of the menus' language (i18n.h): again when it changes
static void itd_load_fonts(ImGuiIO &io, GpuDevice dev);  // (Inside The Dream, below)
static void load_fonts(void) {
    ImGuiIO &io = ImGui::GetIO();
    // fonts belong to this context: back here via "Title screen" (in-place reboot) the previous run's are gone with
    // its context, so load them again (a stale pointer here aborted in PushFont - user, 0.1 RC)
    g_font_small = g_font_menu = g_font_title = NULL;
    io.Fonts->Clear();
    i18n_fonts_stale(io.Fonts);
    // Baloo 2 SemiBold (OFL, shipped: third_party/fonts/Baloo2) - rounded, game-like; it runs small, hence the sizes
    char baloo[1024];
    std::string own = rt_resource("Baloo2-SemiBold.ttf", "third_party/fonts/Baloo2/Baloo2-SemiBold.ttf", baloo, sizeof baloo) ? baloo : "";
    const char *fonts[] = {getenv("LARECOMP_LAUNCHER_FONT") ? getenv("LARECOMP_LAUNCHER_FONT") : "",  // (test)
                           own.c_str(), "fonts/nintendo_udsgr_std_003.ttf", "/System/Library/Fonts/Avenir Next.ttc",
                           "/System/Library/Fonts/Helvetica.ttc", "C:/Windows/Fonts/segoeui.ttf"};
    // the menus' language's glyphs (i18n.h), and its script's font merged in; the title only says "LAS Recompiled"
    static const ImWchar title_ranges[] = {0x20, 0x7E, 0};
    for (const char *p : fonts) {
        if (g_font_menu || !*p || !file_exists(p)) continue;
        bool b2 = p == own.c_str() || getenv("LARECOMP_LAUNCHER_FONT");
        float rel = b2 ? 0.8f : 1;
        g_font_small = io.Fonts->AddFontFromFileTTF(p, 30 * (b2 ? 1.75f : 1), NULL, i18n_glyph_ranges());
        i18n_merge_fonts(io.Fonts, 30 * (b2 ? 1.75f : 1), rel);
        g_font_menu = io.Fonts->AddFontFromFileTTF(p, 52 * (b2 ? 1.85f : 1), NULL, i18n_glyph_ranges());
        i18n_merge_fonts(io.Fonts, 52 * (b2 ? 1.85f : 1), rel);
        g_font_title = io.Fonts->AddFontFromFileTTF(p, 92 * (b2 ? 2.3f : 1), NULL, title_ranges);
    }
    if (!g_font_menu) g_font_small = g_font_menu = g_font_title = io.Fonts->AddFontDefault();
    itd_load_fonts(io, NULL);  // (Inside The Dream: Anton, Barlow - in the same atlas, so again after a Language change)
}

// ------------------------------------------------------------------ Inside The Dream: the start screen in Port0r's look
// Our fork's own face (28/10/2026, Guillaume: « je veux que ce soit notre fork », « on marque clairement que c'est un first
// person view mod tiré de LASRecomp qui font un énorme travail »): port0r.com's look - Anton capitals with the yellow block
// behind the last words, Barlow, pill buttons, the two eyes, the wordmark - over the title movie's still from the player's
// own game files (no Nintendo artwork is shipped). The name has no Nintendo mark in it (Nintendo had ~400 Switch emulator
// repositories taken down in February 2026); the game is only named to say what this is. LAS Recomp is credited in the
// kicker and the footer, with its link. Shipped next to the executable (rt_resource): the Anton and Barlow fonts (OFL,
// third_party/fonts) and the light wordmark (docs/branding/port0r-wordmark-light.bmp, SDL reads BMP).
#define ITD_NAME "Inside The Dream"
#define ITD_VERSION "0.1.0"
#define ITD_SITE "https://port0r.com/"
#define ITD_DISCORD "https://discord.gg/XMk7GgapuN"
#define ITD_UPSTREAM "https://github.com/peiikai/LASRecomp"
static ImFont *g_itd_title, *g_itd_big, *g_itd_btn, *g_itd_label, *g_itd_body;
#ifdef UI_METAL
static id<MTLTexture> g_mark;
#define MARK_TEXID ((ImTextureID)(__bridge void *)g_mark)
#else
static uint64_t g_mark;
#define MARK_TEXID ((ImTextureID)g_mark)
#endif
static int g_mark_w, g_mark_h;
// port0r.com's tokens (port0r-site/site.css, the brand of the new logo, 30/10/2026): ink, paper (a warm white), the void
// for the cards; the title's block and PLAY in the ring's light blue (outside-1), its hover the deeper one; the kicker's
// two dots the ring's inside and outside (inside-2 orange, outside-2 blue) - the old logo's red and cyan eyes are gone
#define ITD_INK IM_COL32(12, 14, 18, 255)
#define ITD_NIGHT(a) IM_COL32(10, 17, 32, a)
#define ITD_ACCENT IM_COL32(108, 198, 238, 255)
#define ITD_ACCENT_HOT IM_COL32(166, 227, 240, 255)
#define ITD_RED_EYE IM_COL32(224, 96, 58, 255)
#define ITD_CYAN_EYE IM_COL32(62, 147, 220, 255)
#define ITD_WHITE(a) IM_COL32(244, 239, 229, a)

// The start screen in the menus' language (the Language setting; Auto: the system's), Beta 6's 11 and English
// (Guillaume, 12/11: « toutes nos langues pour le launcher, détection auto ou réglage »): French beside each English
// text here, the 10 others in runtime/ui/itd_lang.h (tools/i18n/itd_lang_data.py). A text not there stays in English.
#include "itd_lang.h"
static int itd_lang_col(void) {  // k_itd_lang's column for the menus' language: -1 English, -2 French
    const char *c = g_language_codes[i18n_lang()];  // (the menus' own choice - LARECOMP_MENU_LANG too, for a test)
    if (!strcmp(c, "fr")) return -2;
    for (int i = 0; i < ITD_LANGS; i++)
        if (!strcmp(c, k_itd_lang_codes[i])) return i;
    return -1;
}
static const char *itd_lang_find(const char *en, int col) {
    static std::unordered_map<std::string, int> at;
    if (at.empty())
        for (int i = 0; k_itd_lang[i][0]; i++) at.emplace(k_itd_lang[i][0], i);
    auto it = at.find(en);
    return it == at.end() ? NULL : k_itd_lang[it->second][1 + col];
}
static const char *itd_t(const char *en, const char *fr) {
    int col = itd_lang_col();
    if (col == -2) return fr ? fr : en;
    if (col < 0) return en;
    const char *t = itd_lang_find(en, col);
    return t ? t : en;
}
// The characters the start screen's texts have in the menus' language: merged into its Latin fonts from that
// script's own (i18n.mm) - Japanese, Korean, Chinese, Russian
static bool itd_needs_script(void) {
    int col = itd_lang_col();
    if (col < 0) return false;
    const char *c = k_itd_lang_codes[col];
    return !strcmp(c, "ja") || !strcmp(c, "ko") || !strcmp(c, "ru") || !strncmp(c, "zh", 2);
}
static const ImWchar *itd_text_ranges(void) {
    static std::vector<const char *> texts[ITD_LANGS];
    int col = itd_lang_col();
    if (col < 0) return NULL;
    if (texts[col].empty())
        for (int i = 0; k_itd_lang[i][0]; i++) texts[col].push_back(k_itd_lang[i][1 + col]);
    return i18n_text_ranges(texts[col].data(), (int)texts[col].size());
}

// The setup's messages in French (Inside The Dream, 28/10): looked up at display, the setup keeps writing English (its
// log lines, the tests); a message not listed is shown as it is.
static std::string itd_tr(const std::string &en) {
    int col = itd_lang_col();
    if (col == -1 || en.empty()) return en;
    if (col >= 0) {  // (the 10 others: itd_lang.h, the numbered ones by their format)
        if (const char *t = itd_lang_find(en.c_str(), col)) return t;
        int have = 0, all = 0;
        for (const char *fmt : {"Game files incomplete (%d of %d files)", "The romfs folder has %d of the game's %d files"})
            if (sscanf(en.c_str(), fmt, &have, &all) == 2) {
                const char *t = itd_lang_find(fmt, col);
                if (!t) break;
                char b[512];
                snprintf(b, sizeof b, t, have, all);
                return b;
            }
        return L(en.c_str());  // (Beta 6's own, in its 11 languages: its status lines, "Game not built yet")
    }
    static const char *const map[][2] = {
        {"No game files found in that folder", "Aucun fichier du jeu dans ce dossier"},
        {"Those are game packages, not game files", "Ce sont des paquets du jeu, pas ses fichiers"},
        {"Setup didn't finish", "La pr\xc3\xa9paration ne s'est pas termin\xc3\xa9" "e"},
        {"Wrong version of the game", "Ce n'est pas la bonne version du jeu"},
        {"Your game files are incomplete", "Vos fichiers du jeu sont incomplets"},
        {"The game code didn't build", "Le code du jeu n'a pas pu \xc3\xaa" "tre compil\xc3\xa9"},
        {"Choose the folder that has the game's exefs and romfs folders in it.", "Choisissez le dossier qui contient les dossiers exefs et romfs du jeu."},
        {"Inside The Dream needs the game's own files (the exefs and romfs folders), not .xci / .nsp / .nca files.",
         "Inside The Dream a besoin des fichiers du jeu eux-m\xc3\xaames (les dossiers exefs et romfs), pas de fichiers .xci / .nsp / .nca."},
        {"Make sure there's about 12 GB free and try again.", "V\xc3\xa9rifiez qu'il reste environ 12 Go libres, puis r\xc3\xa9" "essayez."},
        {"Inside The Dream needs version 1.0.0 of the game, without updates.", "Inside The Dream a besoin de la version 1.0.0 du jeu, sans mise \xc3\xa0 jour."},
        {"Some of the game's files are missing - usually a copy or unzip that stopped partway. Copy the game files again so none are missing, then choose the folder again.",
         "Il manque des fichiers du jeu, souvent \xc3\xa0 cause d'une copie ou d'une d\xc3\xa9" "compression interrompue. Recopiez les fichiers du jeu en entier, puis choisissez \xc3\xa0 nouveau le dossier."},
        {"Make sure there's about 2 GB free and try again. If it keeps happening, tell us on our Discord.",
         "V\xc3\xa9rifiez qu'il reste environ 2 Go libres, puis r\xc3\xa9" "essayez. Si cela recommence, dites-le-nous sur notre Discord."},
        {"Some of the game's files are missing, usually from a copy or unzip that stopped partway. Copy your game files again, then choose that folder and set them up again.",
         "Il manque des fichiers du jeu, souvent \xc3\xa0 cause d'une copie ou d'une d\xc3\xa9" "compression interrompue. Recopiez vos fichiers du jeu, puis choisissez ce dossier et relancez la pr\xc3\xa9paration."},
        {"Set up the game files to build it (a few minutes, only once).", "Pr\xc3\xa9parez le jeu pour le compiler (quelques minutes, une seule fois)."},
        {"Set up the game files to play.", "Pr\xc3\xa9parez le jeu pour jouer."},
        {"Game code not built yet", "Le code du jeu n'est pas encore compil\xc3\xa9"},
        {"Looking for the game files...", "Recherche des fichiers du jeu..."},
        {"Copying the game files...", "Copie des fichiers du jeu..."},
        {"Game files ready.", "Fichiers du jeu pr\xc3\xaats."},
        {"Preparing shaders", "Pr\xc3\xa9paration des shaders"},
        {"Compilation cancelled. Completed files are kept.", "Compilation annul\xc3\xa9" "e. Les fichiers d\xc3\xa9j\xc3\xa0 compil\xc3\xa9s sont gard\xc3\xa9s."},
        {"Checking completed build files...", "V\xc3\xa9rification des fichiers d\xc3\xa9j\xc3\xa0 compil\xc3\xa9s..."},
        {"Reading the game code...", "Lecture du code du jeu..."},
        {"Resuming compilation...", "Reprise de la compilation..."},
        {"Recompiling...", "Recompilation..."},
        {"Finishing...", "Finalisation..."},
    };
    for (auto &m : map) if (en == m[0]) return m[1];
    int have, all;
    if (sscanf(en.c_str(), "Game files incomplete (%d of %d files)", &have, &all) == 2) {
        char b[128];
        snprintf(b, sizeof b, "Fichiers du jeu incomplets (%d sur %d)", have, all);
        return b;
    }
    if (sscanf(en.c_str(), "The romfs folder has %d of the game's %d files", &have, &all) == 2) {
        char b[400];
        snprintf(b, sizeof b, "Le dossier romfs contient %d des %d fichiers du jeu : il en manque, souvent \xc3\xa0 cause d'une copie ou "
                 "d'une d\xc3\xa9" "compression interrompue. Recopiez les fichiers du jeu en entier, puis choisissez \xc3\xa0 nouveau le dossier.", have, all);
        return b;
    }
    return L(en.c_str());  // (Beta 6's own French: "Game not built yet" and its other status lines)
}

static void itd_load_fonts(ImGuiIO &io, GpuDevice dev) {
    static const ImWchar ranges[] = {0x20, 0xFF, 0x2014, 0x2014, 0x2019, 0x2019, 0x2039, 0x203A, 0x2192, 0x2192, 0};  // + — ’ ‹ › →
    // The menus' language's script (Japanese, Korean, Chinese, Russian), merged in from its own font (i18n.mm) - what each
    // font draws: the title only its two words of the setup page (SET UP / THE GAME: at 192 px a whole script would make
    // the atlas huge; INSIDE THE DREAM is the name, in every language); the headings our texts; the smaller ones our texts
    // and Beta 6's too (its setup messages, through L(): a katakana of them missing showed as "?", 12/11/2026)
    enum { TITLE, OURS, ALL };
    static ImVector<ImWchar> title_ranges[ITD_LANGS];
    auto font = [&](const char *name, const char *dev_rel, float px, int what) -> ImFont * {
        char p[1024];
        if (!rt_resource(name, dev_rel, p, sizeof p) || !file_exists(p)) return NULL;
        ImFont *f = io.Fonts->AddFontFromFileTTF(p, px, NULL, ranges);
        if (!f || !itd_needs_script()) return f;
        int col = itd_lang_col();
        if (what == TITLE) {
            if (title_ranges[col].empty()) {
                ImFontGlyphRangesBuilder b;
                b.AddText(itd_lang_find("SET UP", col));
                b.AddText(itd_lang_find("THE GAME", col));
                b.BuildRanges(&title_ranges[col]);
            }
            i18n_merge_fonts_ranges(io.Fonts, px, 1.0f, title_ranges[col].Data);
            return f;
        }
        i18n_merge_fonts_ranges(io.Fonts, px, 1.0f, itd_text_ranges());
        if (what == ALL) i18n_merge_fonts_ranges(io.Fonts, px, 1.0f, i18n_glyph_ranges());
        return f;
    };
    // (built at twice the 720-line layout's sizes, as the start screen's other fonts: drawn at size x the window's scale)
    g_itd_title = font("Anton-Regular.ttf", "third_party/fonts/Anton/Anton-Regular.ttf", 2 * 96, TITLE);
    g_itd_big = font("Anton-Regular.ttf", "third_party/fonts/Anton/Anton-Regular.ttf", 2 * 30, OURS);
    g_itd_btn = font("Barlow-Bold.ttf", "third_party/fonts/Barlow/Barlow-Bold.ttf", 2 * 15, ALL);
    g_itd_label = font("Barlow-SemiBold.ttf", "third_party/fonts/Barlow/Barlow-SemiBold.ttf", 2 * 13, ALL);
    g_itd_body = font("Barlow-Medium.ttf", "third_party/fonts/Barlow/Barlow-Medium.ttf", 2 * 19, ALL);
    ImFont *fb = g_font_menu ? g_font_menu : io.Fonts->AddFontDefault();  // (a missing file: the start screen's own font)
    for (ImFont **f : {&g_itd_title, &g_itd_big, &g_itd_btn, &g_itd_label, &g_itd_body}) if (!*f) *f = fb;
    (void)dev;
}
// the wordmark's texture (once the renderer's ImGui backend is up: its textures are its descriptor sets)
static void itd_load_mark(GpuDevice dev) {
    if (g_mark) return;
    char p[1024];
    if (rt_resource("port0r-wordmark-light.bmp", "docs/branding/port0r-wordmark-light.bmp", p, sizeof p)) {
        if (SDL_Surface *s = SDL_LoadBMP(p)) {
            if (SDL_Surface *c = SDL_ConvertSurface(s, SDL_PIXELFORMAT_RGBA32)) {
                std::vector<uint8_t> rgba((size_t)c->w * c->h * 4);
                for (int y = 0; y < c->h; y++) memcpy(&rgba[(size_t)y * c->w * 4], (uint8_t *)c->pixels + (size_t)y * c->pitch, (size_t)c->w * 4);
#ifdef UI_METAL
                MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm
                                                                                              width:c->w height:c->h mipmapped:NO];
                g_mark = [dev newTextureWithDescriptor:td];
                [g_mark replaceRegion:MTLRegionMake2D(0, 0, c->w, c->h) mipmapLevel:0 withBytes:rgba.data() bytesPerRow:c->w * 4];
#else
                (void)dev;
                g_mark = vk_imgui_texture_rgba(rgba.data(), c->w, c->h);
#endif
                g_mark_w = c->w, g_mark_h = c->h;
                SDL_DestroySurface(c);
            }
            SDL_DestroySurface(s);
        }
    }
}

// letter-spaced text (CSS letter-spacing, track in ems): its width, then drawn glyph by glyph
static float itd_text_w(ImFont *f, float size, const char *t, float track) {
    float w = 0;
    int n = 0;
    for (const char *c = t; *c;) {
        unsigned int cp;
        int len = ImTextCharFromUtf8(&cp, c, NULL);
        w += f->CalcTextSizeA(size, FLT_MAX, 0, c, c + len).x;
        c += len, n++;
    }
    return w + (n > 1 ? (n - 1) * track * size : 0);
}
static void itd_text(ImDrawList *dl, ImFont *f, float size, ImVec2 p, ImU32 col, const char *t, float track = 0) {
    if (track == 0) { dl->AddText(f, size, p, col, t); return; }
    for (const char *c = t; *c;) {
        unsigned int cp;
        int len = ImTextCharFromUtf8(&cp, c, NULL);
        dl->AddText(f, size, p, col, c, c + len);
        p.x += f->CalcTextSizeA(size, FLT_MAX, 0, c, c + len).x + track * size;
        c += len;
    }
}
// a soft drop shadow under a rounded shape (CSS box-shadow, layered)
static void itd_shadow(ImDrawList *dl, ImVec2 a, ImVec2 b, float r, float s, float strength) {
    for (int i = 0; i < 4; i++) {
        float grow = (4 + i * 5) * s, dy = (8 + i * 4) * s;
        dl->AddRectFilled(ImVec2(a.x - grow * 0.4f, a.y + dy - grow * 0.2f), ImVec2(b.x + grow * 0.4f, b.y + dy + grow * 0.5f),
                          IM_COL32(4, 6, 12, (int)(strength * (46 - i * 9))), r + grow);
    }
}

enum { PILL_ACCENT, PILL_LIGHT, PILL_SOFT, PILL_GHOST };
enum { ICON_NONE, ICON_PLAY, ICON_VR, ICON_FOLDER, ICON_SLIDERS, ICON_QUIT };
static void itd_icon(ImDrawList *dl, int icon, ImVec2 c, float u, ImU32 fg, ImU32 bg) {
    switch (icon) {
    case ICON_PLAY:
        dl->AddTriangleFilled(ImVec2(c.x - u * 0.32f, c.y - u * 0.42f), ImVec2(c.x - u * 0.32f, c.y + u * 0.42f), ImVec2(c.x + u * 0.42f, c.y), fg);
        break;
    case ICON_VR:  // the headset: a body, two lenses, the nose
        dl->AddRectFilled(ImVec2(c.x - u * 0.62f, c.y - u * 0.36f), ImVec2(c.x + u * 0.62f, c.y + u * 0.38f), fg, u * 0.16f);
        dl->AddCircleFilled(ImVec2(c.x - u * 0.30f, c.y), u * 0.16f, bg);
        dl->AddCircleFilled(ImVec2(c.x + u * 0.30f, c.y), u * 0.16f, bg);
        dl->AddTriangleFilled(ImVec2(c.x - u * 0.14f, c.y + u * 0.40f), ImVec2(c.x + u * 0.14f, c.y + u * 0.40f), ImVec2(c.x, c.y + u * 0.16f), bg);
        break;
    case ICON_FOLDER:
        dl->AddRectFilled(ImVec2(c.x - u * 0.5f, c.y - u * 0.36f), ImVec2(c.x - u * 0.06f, c.y - u * 0.18f), fg, u * 0.06f);
        dl->AddRectFilled(ImVec2(c.x - u * 0.5f, c.y - u * 0.26f), ImVec2(c.x + u * 0.5f, c.y + u * 0.38f), fg, u * 0.08f);
        break;
    case ICON_SLIDERS:
        for (int i = 0; i < 3; i++) {
            float y = c.y + (i - 1) * u * 0.32f, k = i == 1 ? 0.22f : i ? -0.18f : 0.12f;
            dl->AddLine(ImVec2(c.x - u * 0.48f, y), ImVec2(c.x + u * 0.48f, y), fg, u * 0.09f);
            dl->AddCircleFilled(ImVec2(c.x + k * u, y), u * 0.12f, fg);
        }
        break;
    case ICON_QUIT:
        dl->PathArcTo(c, u * 0.36f, -IM_PI * 0.5f + 0.75f, IM_PI * 1.5f - 0.75f, 20);
        dl->PathStroke(fg, 0, u * 0.1f);
        dl->AddLine(ImVec2(c.x, c.y - u * 0.5f), ImVec2(c.x, c.y - u * 0.05f), fg, u * 0.1f);
        break;
    }
}
// A pill button (port0r.com's .btn): focusable with the controller and the keyboard like the menu items; hot (hovered or
// selected) it rises and gets its shadow, and a selection by controller / keyboard shows a soft glow of the accent behind
// it (no outline). Returns pressed; *hot_out: hovered or selected.
static bool itd_pill(const char *id, ImVec2 pos, ImVec2 size, int kind, int icon, const char *label, ImFont *font, float fsize,
                     float track, bool enabled, float s, bool *hot_out = NULL, const char *sub = NULL) {
    ImGui::SetCursorScreenPos(pos);
    ImGui::PushID(id);
    ImGui::BeginDisabled(!enabled);
    ImGui::PushStyleColor(ImGuiCol_NavCursor, ImVec4(0, 0, 0, 0));
    bool pressed = ImGui::InvisibleButton("##p", size, ImGuiButtonFlags_EnableNav);
    ImGui::PopStyleColor();
    bool hovered = ImGui::IsItemHovered(), focused = ImGui::IsItemFocused() && ImGui::GetCurrentContext()->NavCursorVisible;
    bool hot = enabled && (hovered || focused), held = hot && ImGui::IsItemActive();
    ImGui::EndDisabled();
    ImGui::PopID();
    if (hot_out) *hot_out = enabled && (hovered || ImGui::IsItemFocused());
    ImDrawList *dl = ImGui::GetWindowDrawList();
    float lift = hot && !held ? -2 * s : 0, r = size.y * 0.5f;
    ImVec2 a(pos.x, pos.y + lift), b(pos.x + size.x, pos.y + size.y + lift);
    ImU32 bg, fg;
    switch (kind) {
    case PILL_ACCENT: bg = hot ? ITD_ACCENT_HOT : ITD_ACCENT, fg = ITD_INK; break;
    case PILL_LIGHT: bg = hot ? IM_COL32(255, 255, 255, 255) : IM_COL32(240, 243, 248, 255), fg = ITD_INK; break;
    case PILL_SOFT: bg = ITD_WHITE(hot ? 46 : 26), fg = ITD_WHITE(255); break;
    default: bg = ITD_WHITE(hot ? 30 : 0), fg = ITD_WHITE(hot ? 255 : 205); break;
    }
    if (!enabled) bg = kind == PILL_GHOST ? 0 : ITD_WHITE(18), fg = ITD_WHITE(90);
    if (focused) dl->AddRectFilled(ImVec2(a.x - 5 * s, a.y - 5 * s), ImVec2(b.x + 5 * s, b.y + 5 * s), IM_COL32(255, 201, 40, 70), r + 5 * s);
    if (hot && kind != PILL_GHOST && kind != PILL_SOFT) itd_shadow(dl, a, b, r, s, 1.0f);
    if (bg) dl->AddRectFilled(a, b, bg, r);
    float u = fsize * 1.05f, cw = itd_text_w(font, fsize, label, track) + (icon ? u * 1.45f : 0);
    float sw = sub ? itd_text_w(g_itd_label, 12.5f * s, sub, 0.1f) + 16 * s : 0;
    float x = a.x + (size.x - cw - sw) * 0.5f, cy = (a.y + b.y) * 0.5f;
    if (icon) itd_icon(dl, icon, ImVec2(x + u * 0.5f, cy), u, fg, bg ? bg : ITD_NIGHT(255)), x += u * 1.45f;
    float th = font->CalcTextSizeA(fsize, FLT_MAX, 0, "A").y;
    itd_text(dl, font, fsize, ImVec2(x, cy - th * 0.5f), fg, label, track);
    if (sub) {
        float lx = x + itd_text_w(font, fsize, label, track) + 16 * s, lh = g_itd_label->CalcTextSizeA(12.5f * s, FLT_MAX, 0, "A").y;
        itd_text(dl, g_itd_label, 12.5f * s, ImVec2(lx, cy - lh * 0.5f), (fg & 0x00ffffff) | (190u << 24), sub, 0.1f);
    }
    if (hovered && enabled) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
    return pressed;
}

static void draw_busy_overlay(ImVec2 ds, const std::string &job, bool test_busy, bool prep_wait) {
    // (Inside The Dream, 28/10: port0r.com's look - a card of the night, no outline, the yellow bar, pill buttons)
    float sc = ds.y / 1080.0f, s = ds.y / 720.0f;
    ImDrawList *fg = ImGui::GetForegroundDrawList();
    ImVec2 o = ImGui::GetMainViewport()->Pos;
    if (!g_auto_update) fg->AddRectFilled(o, ImVec2(o.x + ds.x, o.y + ds.y), IM_COL32(6, 8, 14, 175));  // (an update: plain)
    float f = test_busy && g_prep_frac < 0 ? 0.42f : prep_wait && g_prep_frac < 0 ? 0.99f : extract_progress();
    // (while the game code is built: what's happening, on whose files - runtime/gamebuild.c)
    bool preparing = g_prep_frac >= 0 || prep_wait, recompiling = g_build_frac >= 0 && !preparing;
    uint64_t pc = g_prep_count;  // (beta 6: the bar from the shaders' count, once the game files are indexed)
    unsigned prep_done = (unsigned)(pc >> 32), prep_total = (unsigned)pc;
    if (preparing && g_prep_frac >= 0 && prep_total) f = (float)prep_done / (float)prep_total;
    const char *title = preparing ? itd_t("PREPARING THE SHADERS", "PR\xc3\x89PARATION DES SHADERS")
                      : g_auto_update ? itd_t("UPDATING THE GAME", "MISE \xc3\x80 JOUR DU JEU")
                      : recompiling ? itd_t("RECOMPILING THE GAME", "RECOMPILATION DU JEU") : itd_t("SETTING UP THE GAME FILES", "PR\xc3\x89PARATION DES FICHIERS");
    const char *note = recompiling && !g_auto_update ? itd_t("Only once: it may take a few minutes.", "Une seule fois : cela peut prendre quelques minutes.") : NULL;
    float tfs = 30 * s, lfs = 14 * s, pw = fminf(ds.x - 32 * sc, 620 * s);
    float ph = 64 * s + tfs + 30 * s + 14 * s + 26 * s + lfs + (note ? 8 * s + lfs : 0) + 30 * s + (preparing || recompiling ? 46 * s + 24 * s : 0);
    ImVec2 a(o.x + (ds.x - pw) * 0.5f, o.y + (ds.y - ph) * 0.5f), b(a.x + pw, a.y + ph);
    itd_shadow(fg, a, b, 22 * s, s, 1.4f);
    fg->AddRectFilled(a, b, ITD_NIGHT(250), 22 * s);
    float x = a.x + 34 * s, y = a.y + 30 * s, w = pw - 68 * s;
    fg->AddCircleFilled(ImVec2(x + 5 * s, y + 7 * s), 5 * s, ITD_RED_EYE);
    fg->AddCircleFilled(ImVec2(x + 18 * s, y + 7 * s), 5 * s, ITD_CYAN_EYE);
    itd_text(fg, g_itd_btn, 12 * s, ImVec2(x + 32 * s, y), ITD_WHITE(200), "INSIDE THE DREAM", 0.12f);
    y += 30 * s;
    itd_text(fg, g_itd_big, tfs, ImVec2(x, y), ITD_WHITE(255), title, 0.01f);
    y += tfs + 30 * s;
    ImVec2 ba(x, y), bb(x + w, y + 14 * s);
    fg->AddRectFilled(ba, bb, ITD_WHITE(26), 7 * s);
    if (f > 0) fg->AddRectFilled(ba, ImVec2(ba.x + fmaxf((bb.x - ba.x) * f, 14 * s), bb.y), ITD_ACCENT, 7 * s);
    y += 14 * s + 26 * s;
    char pct[16], line[220];
    snprintf(pct, sizeof pct, "%d%%", (int)(f * 100));
    snprintf(line, sizeof line, "%s", preparing ? "" : test_busy ? itd_t("Setting up the game files...", "Pr\xc3\xa9paration des fichiers du jeu...") : itd_tr(job).c_str());
    fg->AddText(g_itd_label, lfs, ImVec2(x, y), ITD_WHITE(225), line);
    float pcw = g_itd_btn->CalcTextSizeA(lfs, FLT_MAX, 0, pct).x;
    fg->AddText(g_itd_btn, lfs, ImVec2(x + w - pcw, y), ITD_ACCENT, pct);
    y += lfs;
    if (note) y += 8 * s, fg->AddText(g_itd_label, lfs, ImVec2(x, y), ITD_WHITE(140), note), y += lfs;
    y += 30 * s;
    // Cancel (the game code's build) / Skip (the shaders: the game then compiles what it needs as it goes) - B / Esc too
    auto pill = [&](ImVec2 p, ImVec2 sz, const char *t, bool accent, bool live) -> bool {
        bool hover = live && ImGui::IsMouseHoveringRect(p, ImVec2(p.x + sz.x, p.y + sz.y), false);
        fg->AddRectFilled(p, ImVec2(p.x + sz.x, p.y + sz.y), accent ? (hover ? ITD_ACCENT_HOT : ITD_ACCENT) : ITD_WHITE(hover ? 46 : 26), sz.y * 0.5f);
        float tw = itd_text_w(g_itd_btn, 13 * s, t, 0.08f), th = g_itd_btn->CalcTextSizeA(13 * s, FLT_MAX, 0, "A").y;
        itd_text(fg, g_itd_btn, 13 * s, ImVec2(p.x + (sz.x - tw) * 0.5f, p.y + (sz.y - th) * 0.5f), accent ? ITD_INK : ITD_WHITE(live ? 255 : 150), t, 0.08f);
        if (hover) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
        return hover && ImGui::IsMouseClicked(ImGuiMouseButton_Left);
    };
    bool back = ImGui::IsKeyPressed(ImGuiKey_GamepadFaceRight, false) || ImGui::IsKeyPressed(ImGuiKey_Escape, false);
    if (recompiling) {
        const char *t = g_cancel_build ? itd_t("CANCELLING...", "ANNULATION...") : itd_t("CANCEL", "ANNULER");
        if ((pill(ImVec2(x, y), ImVec2(170 * s, 46 * s), t, false, !g_cancel_build) || back) && !g_cancel_build) g_cancel_build = true;
    }
    if (preparing) {
        const char *t = g_prep_skip ? itd_t("SKIPPING...", "PASSAGE...") : itd_t("SKIP", "PASSER");
        bool live = !g_prep_skip && !g_prep_skip_ask;
        if ((pill(ImVec2(x, y), ImVec2(170 * s, 46 * s), t, false, live) || (back && live)) && live) g_prep_skip_ask = true;
    }
    if (preparing && g_prep_skip_ask && !g_prep_skip) {  // what Skip means, before it happens
        const char *q = itd_t("The shaders will then compile as you play: the first minutes may stutter and some things pop in.",
                              "Les shaders se compileront alors en jouant : les premi\xc3\xa8res minutes peuvent saccader et des \xc3\xa9l\xc3\xa9ments appara\xc3\xae" "tre en retard.");
        float qw = fminf(ds.x - 32 * sc, 560 * s), qh = 30 * s + 28 * s + 44 * s + 26 * s + 46 * s + 30 * s;
        fg->AddRectFilled(a, b, IM_COL32(0, 0, 0, 120), 22 * s);  // (the bar behind it dimmed)
        ImVec2 qa(o.x + (ds.x - qw) * 0.5f, o.y + (ds.y - qh) * 0.5f), qb(qa.x + qw, qa.y + qh);
        itd_shadow(fg, qa, qb, 22 * s, s, 1.4f);
        fg->AddRectFilled(qa, qb, ITD_NIGHT(255), 22 * s);
        float qx = qa.x + 34 * s, qy = qa.y + 30 * s;
        itd_text(fg, g_itd_big, 24 * s, ImVec2(qx, qy), ITD_WHITE(255), itd_t("SKIP THE SHADERS?", "PASSER LES SHADERS ?"), 0.01f);
        qy += 24 * s + 16 * s;
        fg->AddText(g_itd_label, lfs, ImVec2(qx, qy), ITD_WHITE(215), q, NULL, qw - 68 * s);
        qy = qb.y - 30 * s - 46 * s;
        bool yes = pill(ImVec2(qx, qy), ImVec2(190 * s, 46 * s), itd_t("YES, SKIP", "OUI, PASSER"), false, true);
        bool no = pill(ImVec2(qx + 200 * s, qy), ImVec2(190 * s, 46 * s), itd_t("NO, WAIT", "NON, ATTENDRE"), true, true);
        // A / Enter: the highlighted default (wait); B / Esc: also wait; Yes only by clicking it or Y / Space
        if (yes || ImGui::IsKeyPressed(ImGuiKey_GamepadFaceUp, false) || ImGui::IsKeyPressed(ImGuiKey_Space, false))
            g_prep_skip = 1, g_prep_skip_ask = false;
        else if (no || ImGui::IsKeyPressed(ImGuiKey_GamepadFaceDown, false) || ImGui::IsKeyPressed(ImGuiKey_Enter, false) ||
                 ImGui::IsKeyPressed(ImGuiKey_GamepadFaceRight, false) || ImGui::IsKeyPressed(ImGuiKey_Escape, false))
            g_prep_skip_ask = false;
    }
}

// the bar (the wordmark, the links) and the footer (our name and version, LAS Recomp's credit with its link, not
// Nintendo's), shared by the start screen's pages
static void itd_bar(ImDrawList *dl, ImVec2 wp, ImVec2 ds, float s, float X, float Y) {
    if (g_mark) {
        float h = 28 * s, w = h * g_mark_w / g_mark_h;
        dl->AddImage(MARK_TEXID, ImVec2(X, Y + 44 * s), ImVec2(X + w, Y + 44 * s + h));
    }
    {
        struct { const char *label, *url; } links[] = {{"PORT0R.COM", ITD_SITE}, {"DISCORD", ITD_DISCORD}, {"LAS RECOMP", ITD_UPSTREAM}};
        float right = wp.x + ds.x - 60 * s;
        for (auto &l : links) {
            float w = itd_text_w(g_itd_btn, 12.5f * s, l.label, 0.08f) + 30 * s;
            right -= w;
            if (itd_pill(l.label, ImVec2(right, Y + 39 * s), ImVec2(w, 38 * s), PILL_GHOST, ICON_NONE, l.label, g_itd_btn,
                         12.5f * s, 0.08f, true, s))
                SDL_OpenURL(l.url);
            right -= 4 * s;
        }
    }
}
static void itd_footer(ImDrawList *dl, ImVec2 wp, ImVec2 ds, float s, float X) {
    {
        float fy = wp.y + ds.y - 40 * s, fx = X, fsz = 11.5f * s;
        char a[96];
        snprintf(a, sizeof a, "%s %s  \xc2\xb7  ", ITD_NAME, ITD_VERSION);
        for (char *c = a; *c; c++) *c = (char)toupper((unsigned char)*c);
        itd_text(dl, g_itd_btn, fsz, ImVec2(fx, fy), ITD_WHITE(170), a, 0.08f);
        fx += itd_text_w(g_itd_btn, fsz, a, 0.08f) + 0.08f * fsz;
        const char *b = itd_t("BUILT ON ", "CONSTRUIT SUR ");
        itd_text(dl, g_itd_btn, fsz, ImVec2(fx, fy), ITD_WHITE(170), b, 0.08f);
        fx += itd_text_w(g_itd_btn, fsz, b, 0.08f) + 0.08f * fsz;
        std::string up = "LAS RECOMP " + tag_label(LARECOMP_VERSION, false);
        for (char &c : up) c = (char)toupper((unsigned char)c);
        float uw = itd_text_w(g_itd_btn, fsz, up.c_str(), 0.08f);
        ImGui::SetCursorScreenPos(ImVec2(fx, fy - 3 * s));
        bool uclick = ImGui::InvisibleButton("##upstream", ImVec2(uw, fsz + 8 * s));
        bool uhov = ImGui::IsItemHovered();
        itd_text(dl, g_itd_btn, fsz, ImVec2(fx, fy), uhov ? ITD_ACCENT_HOT : ITD_ACCENT, up.c_str(), 0.08f);
        dl->AddRectFilled(ImVec2(fx, fy + fsz + 2 * s), ImVec2(fx + uw, fy + fsz + 4 * s), uhov ? ITD_ACCENT_HOT : ITD_ACCENT);
        if (uhov) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
        if (uclick) SDL_OpenURL(ITD_UPSTREAM);
        fx += uw + 0.08f * fsz;
        const char *c = itd_t(" BY PEIIKAI, WHO RECOMPILED THE GAME  \xc2\xb7  NOT AFFILIATED WITH NINTENDO",
                              " DE PEIIKAI, QUI A RECOMPIL\xc3\x89 LE JEU  \xc2\xb7  NON AFFILI\xc3\x89 \xc3\x80 NINTENDO");
        itd_text(dl, g_itd_btn, fsz, ImVec2(fx, fy), ITD_WHITE(170), c, 0.08f);
    }
}

#ifdef LARECOMP_DESKTOP_XR
// the OpenXR runtime a game started now would get, by name ("" = none found)
#ifdef _WIN32
extern "C" __declspec(dllimport) long __stdcall RegGetValueA(void *, const char *, const char *, unsigned long, unsigned long *, void *, unsigned long *);
#endif
static std::string itd_xr_runtime(void) {
    std::string path;
    if (const char *j = getenv("XR_RUNTIME_JSON"); j && *j) path = j;
#ifdef _WIN32
    if (path.empty()) {
        char buf[1024] = "";
        unsigned long size = sizeof buf;
        if (RegGetValueA((void *)(intptr_t)0x80000002 /* HKEY_LOCAL_MACHINE */, "SOFTWARE\\Khronos\\OpenXR\\1", "ActiveRuntime",
                         0x00000002 /* RRF_RT_REG_SZ */, NULL, buf, &size) == 0)
            path = buf;
    }
#else
    if (path.empty()) {  // (the user's choice, then the system's: the OpenXR loader's order)
        const char *xdg = getenv("XDG_CONFIG_HOME"), *home = getenv("HOME");
        std::string user = xdg && *xdg ? std::string(xdg) : home ? std::string(home) + "/.config" : "";
        for (std::string p : {user + "/openxr/1/active_runtime.json", std::string("/etc/xdg/openxr/1/active_runtime.json")}) {
            char real[4096];
            if (!user.empty() && realpath(p.c_str(), real)) { path = real; break; }
        }
    }
#endif
    if (path.empty() || !file_exists(path)) return "";
    std::string l = path;
    for (char &c : l) c = (char)tolower((unsigned char)c);
    if (l.find("steamvr") != std::string::npos || l.find("steamxr") != std::string::npos) return "SteamVR";
    if (l.find("oculus") != std::string::npos || l.find("meta") != std::string::npos) return "Meta Quest Link";
    if (l.find("virtualdesktop") != std::string::npos || l.find("vdxr") != std::string::npos) return "Virtual Desktop";
    if (l.find("wivrn") != std::string::npos) return "WiVRn";
    if (l.find("monado") != std::string::npos) return "Monado";
    if (l.find("mixedreality") != std::string::npos) return "Windows Mixed Reality";
    if (l.find("pico") != std::string::npos) return "Pico Connect";
    size_t slash = path.find_last_of("/\\"), dot = path.find_last_of('.');
    return path.substr(slash == std::string::npos ? 0 : slash + 1, dot == std::string::npos ? std::string::npos : dot - (slash + 1));
}
#endif

// Inside The Dream: Guillaume's Wind Fish (v14, made with FM-1 SWITCH, his program on the M-VAVE FM-1's firmware) in a loop
// under the start screen, quietly (12/11/2026: « ma musique en boucle pas fort dans le launcher »): itd-theme.wav (PCM),
// faded in, at the master volume's share; gone when the game starts (PLAY, PLAY IN VR, QUIT: the process goes on or ends).
// LARECOMP_LAUNCHER_NO_MUSIC=1: none.
static SDL_AudioStream *g_itd_music;
static Uint8 *g_itd_music_pcm;
static Uint32 g_itd_music_len, g_itd_music_pos;
static void SDLCALL itd_music_feed(void *, SDL_AudioStream *s, int need, int) {
    while (need > 0 && g_itd_music_len) {
        Uint32 n = g_itd_music_len - g_itd_music_pos;
        if ((Uint32)need < n) n = (Uint32)need;
        SDL_PutAudioStreamData(s, g_itd_music_pcm + g_itd_music_pos, (int)n);
        g_itd_music_pos = (g_itd_music_pos + n) % g_itd_music_len;
        need -= (int)n;
    }
}
static void itd_music_start(void) {
    if (g_itd_music || getenv("LARECOMP_NO_AUDIO") || getenv("LARECOMP_LAUNCHER_NO_MUSIC")) return;
    char p[1024];
    if (!rt_resource("itd-theme.wav", "docs/branding/itd-theme.wav", p, sizeof p) || !file_exists(p)) return;
    if (!SDL_InitSubSystem(SDL_INIT_AUDIO)) return;
    SDL_AudioSpec spec;
    if (!SDL_LoadWAV(p, &spec, &g_itd_music_pcm, &g_itd_music_len)) {
        fprintf(stderr, "[launcher] music: %s\n", SDL_GetError());
        return;
    }
    // (whole sample frames only: the loop point never splits one)
    Uint32 frame = (Uint32)(SDL_AUDIO_BYTESIZE(spec.format) * spec.channels);
    if (frame) g_itd_music_len -= g_itd_music_len % frame;
    g_itd_music_pos = 0;
    g_itd_music = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, itd_music_feed, NULL);
    if (!g_itd_music) {
        fprintf(stderr, "[launcher] music: no audio device (%s)\n", SDL_GetError());
        SDL_free(g_itd_music_pcm), g_itd_music_pcm = NULL, g_itd_music_len = 0;
        return;
    }
    SDL_SetAudioStreamGain(g_itd_music, 0.0f);
    SDL_ResumeAudioStreamDevice(g_itd_music);
}
static void itd_music_tick(float dt) {  // (on / off and the volume from the settings; a 3 s fade in, the master followed)
    if (!g_itd_music) return;
    static bool paused;
    if (!g_settings.itd_music) {
        if (!paused) SDL_PauseAudioStreamDevice(g_itd_music), SDL_SetAudioStreamGain(g_itd_music, 0.0f), paused = true;
        return;
    }
    if (paused) SDL_ResumeAudioStreamDevice(g_itd_music), paused = false;
    float master = g_settings.volume > 0 ? fminf(g_settings.volume, 1.0f) : 0.0f;
    float want = 0.6f * g_settings.itd_music_vol * master;
    float g = SDL_GetAudioStreamGain(g_itd_music);
    if (g < want) g = fminf(want, g + dt * fmaxf(want, 0.05f) / 3.0f);
    else g = want;
    SDL_SetAudioStreamGain(g_itd_music, g);
}
// The music's control, lower right on the footer's line: play / pause, its name, its volume (eight steps: a click, or the
// wheel over it). Kept in the settings (itd_music, itd_music_vol).
static void itd_music_control(ImDrawList *dl, ImVec2 wp, ImVec2 ds, float s) {
    if (!g_itd_music) return;
    const char *name = itd_t("BALLAD OF THE WIND FISH  \xc2\xb7  FM-1 SWITCH REMIX", "BALLAD OF THE WIND FISH  \xc2\xb7  REMIX FM-1 SWITCH");
    float fsz = 11 * s, h = 30 * s, bd = 22 * s, bars = 8, bw = 4 * s, bg = 3 * s;
    float tw = itd_text_w(g_itd_btn, fsz, name, 0.08f), vw = bars * bw + (bars - 1) * bg;
    float w = 4 * s + bd + 10 * s + tw + 14 * s + vw + 12 * s;
    ImVec2 b(wp.x + ds.x - 60 * s, wp.y + ds.y - 26 * s), a(b.x - w, b.y - h);
    dl->AddRectFilled(a, b, ITD_WHITE(22), h * 0.5f);
    bool on = g_settings.itd_music;
    // play / pause
    ImVec2 c(a.x + 4 * s + bd * 0.5f, a.y + h * 0.5f);
    ImGui::SetCursorScreenPos(ImVec2(c.x - bd * 0.5f, c.y - bd * 0.5f));
    bool click = ImGui::InvisibleButton("##music_toggle", ImVec2(bd, bd));
    bool hov = ImGui::IsItemHovered();
    dl->AddCircleFilled(c, bd * 0.5f, on ? (hov ? ITD_ACCENT_HOT : ITD_ACCENT) : ITD_WHITE(hov ? 60 : 36));
    ImU32 ic = on ? ITD_INK : ITD_WHITE(230);
    if (on) {  // (playing: pause)
        dl->AddRectFilled(ImVec2(c.x - 4 * s, c.y - 5 * s), ImVec2(c.x - 1.5f * s, c.y + 5 * s), ic, 1 * s);
        dl->AddRectFilled(ImVec2(c.x + 1.5f * s, c.y - 5 * s), ImVec2(c.x + 4 * s, c.y + 5 * s), ic, 1 * s);
    } else
        dl->AddTriangleFilled(ImVec2(c.x - 3 * s, c.y - 5.5f * s), ImVec2(c.x - 3 * s, c.y + 5.5f * s), ImVec2(c.x + 5.5f * s, c.y), ic);
    if (hov) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
    if (click) g_settings.itd_music = !on, settings_save();
    // its name
    float th = g_itd_btn->CalcTextSizeA(fsz, FLT_MAX, 0, "A").y;
    itd_text(dl, g_itd_btn, fsz, ImVec2(c.x + bd * 0.5f + 10 * s, a.y + (h - th) * 0.5f), ITD_WHITE(on ? 200 : 120), name, 0.08f);
    // the volume: eight bars rising, those up to it lit
    ImVec2 va(b.x - 12 * s - vw, a.y + 8 * s), vb(b.x - 12 * s, b.y - 8 * s);
    ImGui::SetCursorScreenPos(ImVec2(va.x - 3 * s, a.y));
    bool vclick = ImGui::InvisibleButton("##music_vol", ImVec2(vw + 6 * s, h));
    bool vhov = ImGui::IsItemHovered();
    int lit = (int)lroundf(g_settings.itd_music_vol * bars);
    for (int i = 0; i < (int)bars; i++) {
        float x0 = va.x + i * (bw + bg), bh = (vb.y - va.y) * (0.35f + 0.65f * (i + 1) / bars);
        dl->AddRectFilled(ImVec2(x0, vb.y - bh), ImVec2(x0 + bw, vb.y), i < lit && on ? ITD_ACCENT : ITD_WHITE(vhov ? 70 : 45), 1.5f * s);
    }
    if (vhov) {
        ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
        float wheel = ImGui::GetIO().MouseWheel;
        int n = lit;
        if (vclick) n = (int)ceilf((ImGui::GetIO().MousePos.x - va.x) / (bw + bg));
        if (wheel > 0) n++;
        if (wheel < 0) n--;
        n = n < 0 ? 0 : n > (int)bars ? (int)bars : n;
        if (n != lit || vclick) {
            g_settings.itd_music_vol = n / bars;
            if (n > 0) g_settings.itd_music = true;
            settings_save();
        }
    }
}
static void itd_music_stop(void) {
    if (!g_itd_music) return;
    SDL_DestroyAudioStream(g_itd_music), g_itd_music = NULL;
    SDL_free(g_itd_music_pcm), g_itd_music_pcm = NULL, g_itd_music_len = 0;
}

// Port0r's animated logo before the start screen (12/11/2026, Guillaume: « notre nouveau logo avec animation au lancement
// dans le launcher »): the site's port0r-logo-animation (3.6 s, its sound), full screen on black, then the start screen
// comes through it; a click, a key or a button skips it. Once a run (not again after "Title screen"); none in tests
// (LARECOMP_LAUNCHER_SHOT, LARECOMP_NO_AUDIO) or with LARECOMP_LAUNCHER_NO_INTRO=1. The start screen's music after it.
static ItdIntro *g_intro;
static ImTextureID g_intro_tex;
static uint64_t g_intro_img;  // (Vulkan: the image the pictures go into)
#ifdef UI_METAL
static id<MTLTexture> g_intro_mtl;
#endif
static SDL_AudioStream *g_intro_snd;
static uint64_t g_intro_t0;
static float g_intro_out = -1;  // (the fade to the start screen: its start, in seconds from the start; -1 not yet)
static const int ITD_INTRO_W = 1280, ITD_INTRO_H = 720;
static bool itd_intro_on(void) { return g_intro != NULL; }
static void itd_intro_begin(GpuDevice dev) {
    static bool shown;
    if (shown || getenv("LARECOMP_LAUNCHER_SHOT") || getenv("LARECOMP_NO_AUDIO") || getenv("LARECOMP_LAUNCHER_NO_INTRO")) return;
    shown = true;
    char p[1024];
    if (!rt_resource("port0r-logo-animation.mp4", "docs/branding/port0r-logo-animation.mp4", p, sizeof p) || !file_exists(p)) return;
    float *pcm = NULL;
    int frames = 0, ch = 0, rate = 0;
    g_intro = itd_intro_open(p, ITD_INTRO_W, ITD_INTRO_H, &pcm, &frames, &ch, &rate);
    if (!g_intro) { fprintf(stderr, "[launcher] logo animation: not readable\n"); return; }
    std::vector<uint8_t> black((size_t)ITD_INTRO_W * ITD_INTRO_H * 4, 0);
    for (size_t i = 3; i < black.size(); i += 4) black[i] = 255;
#ifdef UI_METAL
    MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm
                                                                                  width:ITD_INTRO_W height:ITD_INTRO_H mipmapped:NO];
    g_intro_mtl = [dev newTextureWithDescriptor:td];
    [g_intro_mtl replaceRegion:MTLRegionMake2D(0, 0, ITD_INTRO_W, ITD_INTRO_H) mipmapLevel:0 withBytes:black.data() bytesPerRow:ITD_INTRO_W * 4];
    g_intro_tex = (ImTextureID)(__bridge void *)g_intro_mtl;
#else
    (void)dev;
    g_intro_tex = (ImTextureID)vk_imgui_texture_rgba_ex(black.data(), ITD_INTRO_W, ITD_INTRO_H, &g_intro_img);
#endif
    if (pcm && frames && ch && rate && SDL_InitSubSystem(SDL_INIT_AUDIO)) {
        SDL_AudioSpec spec = {SDL_AUDIO_F32, ch, rate};
        g_intro_snd = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, NULL, NULL);
        if (g_intro_snd) {
            float master = g_settings.volume > 0 ? fminf(g_settings.volume, 1.0f) : 0.0f;
            SDL_SetAudioStreamGain(g_intro_snd, 0.8f * master);
            SDL_PutAudioStreamData(g_intro_snd, pcm, frames * ch * (int)sizeof(float));
            SDL_FlushAudioStream(g_intro_snd);
            SDL_ResumeAudioStreamDevice(g_intro_snd);
        }
    }
    free(pcm);
    g_intro_t0 = SDL_GetTicksNS();
    fprintf(stderr, "[launcher] logo animation: %.1f s\n", itd_intro_duration(g_intro));
}
static void itd_intro_end(void) {
    itd_intro_close(g_intro), g_intro = NULL;
    if (g_intro_snd) SDL_DestroyAudioStream(g_intro_snd), g_intro_snd = NULL;
    itd_music_start();  // (the start screen's music, now)
}
// Once a frame while it plays: its picture, over everything; the start screen fades in over its last half second.
static void itd_intro_frame(ImVec2 ds) {
    if (!g_intro) return;
    double t = (SDL_GetTicksNS() - g_intro_t0) / 1e9;
    if (const uint8_t *px = itd_intro_picture(g_intro, t)) {
#ifdef UI_METAL
        [g_intro_mtl replaceRegion:MTLRegionMake2D(0, 0, ITD_INTRO_W, ITD_INTRO_H) mipmapLevel:0 withBytes:px bytesPerRow:ITD_INTRO_W * 4];
#else
        vk_imgui_texture_write(g_intro_img, px, ITD_INTRO_W, ITD_INTRO_H);
#endif
    }
    ImGuiIO &io = ImGui::GetIO();
    bool skip = ImGui::IsMouseClicked(ImGuiMouseButton_Left) || ImGui::IsKeyPressed(ImGuiKey_Escape, false) ||
                ImGui::IsKeyPressed(ImGuiKey_Enter, false) || ImGui::IsKeyPressed(ImGuiKey_Space, false) ||
                ImGui::IsKeyPressed(ImGuiKey_GamepadFaceDown, false) || ImGui::IsKeyPressed(ImGuiKey_GamepadFaceRight, false) ||
                ImGui::IsKeyPressed(ImGuiKey_GamepadStart, false);
    double len = itd_intro_duration(g_intro);
    if (g_intro_out < 0 && (skip || itd_intro_ended(g_intro) || t >= len - 0.5)) g_intro_out = (float)t;
    float a = 1;
    if (g_intro_out >= 0) {
        a = 1 - (float)((t - g_intro_out) / 0.5);
        if (g_intro_snd && skip) SDL_SetAudioStreamGain(g_intro_snd, 0);
        if (a <= 0) { itd_intro_end(); return; }
    }
    ImDrawList *fg = ImGui::GetForegroundDrawList();
    ImVec2 o = ImGui::GetMainViewport()->Pos;
    fg->AddRectFilled(o, ImVec2(o.x + ds.x, o.y + ds.y), IM_COL32(0, 0, 0, (int)(255 * a)));
    float vw = ds.x, vh = ds.x * ITD_INTRO_H / ITD_INTRO_W;  // (16:9, fitted: the black around it is the picture's own)
    if (vh > ds.y) vh = ds.y, vw = ds.y * ITD_INTRO_W / ITD_INTRO_H;
    ImVec2 a0(o.x + (ds.x - vw) * 0.5f, o.y + (ds.y - vh) * 0.5f);
    fg->AddImage(g_intro_tex, a0, ImVec2(a0.x + vw, a0.y + vh), ImVec2(0, 0), ImVec2(1, 1), IM_COL32(255, 255, 255, (int)(255 * a)));
    (void)io;
}

extern "C" void launcher_run(const char *root) {
    g_root = root;
    g_start_screen = 1;
    menu_sfx_set_game_dir(root);
#if LARECOMP_IOSEXPORT
    ios_export_wire();
#endif
#if PLAT_TOUCH
    plat_touch_shortcuts_sync();  // (seeds added or deleted outside the app since the last run)
#endif
    static bool update_checked;  // (once per launch, not again after a "Title screen" reboot)
    // (Inside The Dream: not against LAS Recomp's releases - their update would replace this mod with their own build,
    //  without the first-person view or PC VR. LARECOMP_UPDATE_TAGS still tests the banner.)
    if (!update_checked && getenv("LARECOMP_UPDATE_TAGS")) {
        update_checked = true;
        pthread_t t;
        if (pthread_create(&t, NULL, update_thread, NULL) == 0) pthread_detach(t);
    }
    ui_window_init();
    SDL_Window *win = ui_window();
    g_launcher_win = win;
#ifdef UI_METAL
    CAMetalLayer *layer = (__bridge CAMetalLayer *)ui_metal_layer();
    id<MTLDevice> dev = (__bridge id<MTLDevice>)ui_metal_device();
    id<MTLCommandQueue> queue = [dev newCommandQueue];
#else
    GpuDevice dev = NULL;
#endif

    IMGUI_CHECKVERSION();
    ImGuiContext *prev_ctx = ImGui::GetCurrentContext();  // the in-game menu's, when back here after a reboot
    ImGuiContext *ctx = ImGui::CreateContext();
    ImGui::SetCurrentContext(ctx);
    ImGuiIO &io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard | ImGuiConfigFlags_NavEnableGamepad;
    io.IniFilename = NULL;
    load_fonts();
    ImGuiStyle &st = ImGui::GetStyle();
    st.Colors[ImGuiCol_Header] = ImVec4(0, 0, 0, 0);
    st.Colors[ImGuiCol_HeaderHovered] = ImVec4(1, 1, 1, 0.08f);
    st.Colors[ImGuiCol_HeaderActive] = ImVec4(1, 1, 1, 0.14f);
    st.Colors[ImGuiCol_NavCursor] = ImVec4(0.95f, 0.78f, 0.35f, 1);
    st.FrameRounding = st.WindowRounding = 10;
#ifdef UI_METAL
    ImGui_ImplSDL3_InitForMetal(win);
    ImGui_ImplMetal_Init(dev);
#else
    ImGui_ImplSDL3_InitForVulkan(win);
    vk_imgui_backend_init();
#endif
#if PLAT_TOUCH
    {   // the backend starts / stops the system keyboard; the field's frame goes to SDL from ui_text_field_rect, not
        // the cursor's every move (SDL slides the view to keep it above the keyboard, and a slide under a dragging
        // finger moved the finger relative to the view: more scroll, another slide - the page ran away under it)
        static void (*backend_ime)(ImGuiContext *, ImGuiViewport *, ImGuiPlatformImeData *) = ImGui::GetPlatformIO().Platform_SetImeDataFn;
        ImGui::GetPlatformIO().Platform_SetImeDataFn = [](ImGuiContext *ctx, ImGuiViewport *vp, ImGuiPlatformImeData *d) {
            static bool visible;
            if (d->WantVisible == visible) return;
            visible = d->WantVisible;
            ImGuiPlatformImeData no_rect = *d;
            no_rect.InputLineHeight = 0;  // (a 0-high rect: SDL doesn't slide for it)
            backend_ime(ctx, vp, &no_rect);
        };
    }
#endif
    ImGui_ImplSDL3_SetGamepadMode(ImGui_ImplSDL3_GamepadMode_Manual, NULL, 0);  // (controllers: launcher_pad_event)
    launcher_pad_open_all();
    io.BackendFlags |= ImGuiBackendFlags_HasGamepad;
    itd_load_mark(dev);  // (Inside The Dream's wordmark)
    itd_intro_begin(dev);  // (the logo first; the music after it - or now, without it)
    if (!itd_intro_on()) itd_music_start();

    // an older version's game data image: the game reads the romfs/ folder itself now - 5.8 GB back
    if (check_copied_files(g_root).empty() && file_exists(g_root + "/romfs.bin") &&
        rt_remove_game_file(g_root.c_str(), "romfs.bin"))
        fprintf(stderr, "[launcher] removed the old game data image (romfs.bin)\n");
    std::string status = check_game_files(g_root);
    if (check_copied_files(g_root).empty()) load_background(dev);  // (also behind an update of the game code)
    // the RANDOMIZER row's choices: 0 New Seed, then the seeds (starting on the last one played)
    std::vector<std::string> seeds = list_seeds();
    int choice = 0;
    auto choose = [&](const char *name) {
        choice = 0;
        for (size_t i = 0; i < seeds.size(); i++)
            if (seeds[i] == name) choice = (int)i + 1;
    };
    choose(g_settings.rando_last);
    // what the game starts with ("" vanilla): PLAY's, or the row's seed; until one is pressed, the seed of the last
    // start (the test hooks that press PLAY themselves keep booting what they did)
    std::string start_seed = g_settings.rando_seed;
    Page page = status.empty() ? MAIN : SETUP;
    bool play = false;
    // LARECOMP_LAUNCHER_SHOT=<frame>[:<page>] (test): save build/launcher.png at that frame and exit; page:
    // settings[:<tab>] / rando / askdelete / setup / extracting / preparing / skipask / recompiling / tip / aspect / notice
    long shot_at = getenv("LARECOMP_LAUNCHER_SHOT") ? atol(getenv("LARECOMP_LAUNCHER_SHOT")) : 0, frame = 0;
    if (shot_at) {
#ifdef UI_METAL
        layer.framebufferOnly = NO;
#endif
        const char *pg = strchr(getenv("LARECOMP_LAUNCHER_SHOT"), ':');
        if (pg && (!strncmp(pg + 1, "settings", 8) || !strcmp(pg + 1, "askdelete"))) page = SETTINGS, menu_tabs_focus(true);
        if (pg && !strcmp(pg + 1, "setup")) page = SETUP;
        if (pg && !strcmp(pg + 1, "rando")) page = RANDO, menu_tabs_focus(false);
        if (pg && !strcmp(pg + 1, "ios")) page = IOS;
        if (pg && !strcmp(pg + 1, "preparing")) page = SETUP, prep_progress(666, 1585), set_job(LN("Preparing shaders"));
        if (pg && !strcmp(pg + 1, "skipask")) page = SETUP, prep_progress(666, 1585), set_job(LN("Preparing shaders")), g_prep_skip_ask = true;
        if (pg && !strncmp(pg + 1, "fail", 4)) page = SETUP, set_failure(atoi(pg + 5));  // (the failure messages)
        if (pg && !strncmp(pg + 1, "settings", 8) && pg[9] == ':') g_tab_request_ext = atoi(pg + 10);  // settings:<tab>
        if (pg && !strcmp(pg + 1, "delete")) page = SETUP, g_confirm_delete = true;
        if (pg && !strcmp(pg + 1, "tip")) g_settings.menu_tip_seen = false, g_tip_test = true;
        if (pg && !strcmp(pg + 1, "recompiling")) page = SETUP, g_build_frac = 0.42f, set_job(LN("Recompiling the game"));
        if (pg && !strcmp(pg + 1, "notice"))  // (the iPhone's iCloud notice, anywhere)
            g_notice = std::string(L("Some of LAS Recomp's files aren't on this iPhone. They download as the game reads "
                                     "them, so it may be very slow or close.")) + "\n\n" + L(KEEP_DOWNLOADED);
        if (pg && !strcmp(pg + 1, "aspect")) g_settings.aspect_asked = g_settings.menu_tip_seen = false, g_tip_test = true;
    }

    static int runs;  // (test: =once - only the first time, so a "Title screen" reboot lands on the start screen)
    const char *ap = getenv("LARECOMP_LAUNCHER_AUTOPLAY");
    if (ap && status.empty() && !(runs && !strcmp(ap, "once"))) play = true;  // (test: PLAY at once)
    if (getenv("LARECOMP_LAUNCHER_PREPARE") && status.empty() && !runs) {  // (test) the setup's shader preparation
        g_busy = true;                                                      // alone, then PLAY with AUTOPLAY
        page = SETUP, play = false;
        pthread_t th;
        pthread_create(&th, NULL, [](void *) -> void * {
            prepare_shaders();
            set_job(LN("Game files ready."));
            pthread_mutex_lock(&g_lock);
            g_busy = false;
            pthread_mutex_unlock(&g_lock);
            return NULL;
        }, NULL);
        pthread_detach(th);
    }
    runs++;
    if (const char *t = getenv("LARECOMP_LAUNCHER_DATA_PICK")) {  // (test) <folder>[:move / :fresh]: picked as the data folder
        std::string p = t;
        size_t c = p.rfind(':');
        if (c != std::string::npos && (p.substr(c) == ":move" || p.substr(c) == ":fresh"))
            g_df_auto_move = p.substr(c) == ":move", g_df_auto_fresh = p.substr(c) == ":fresh", p.resize(c);
        g_df_pick = p;
    }
    if (const char *t = getenv("LARECOMP_LAUNCHER_EXTRACT")) {  // (test) <folder>: set the game files up, log, exit
        g_src = t;
        g_busy = true;
        extract_thread(NULL);
        fprintf(stderr, "[launcher] setup: %s -> %s\n", g_job.c_str(), check_game_files(g_root).empty() ? "READY" : "NOT READY");
        exit(0);
    }
    if (const char *t = getenv("LARECOMP_LAUNCHER_SETUP")) {  // (test) <folder>: Set up started on the start screen
        g_src = t;
        g_busy = true;
        page = SETUP;
        pthread_t th;
        pthread_create(&th, NULL, extract_thread, NULL);
        pthread_detach(th);
    }
    // An update: the game files are there and only the game code is out of date - rebuilt at once, by itself (only
    // the pieces that changed: runtime/gamebuild.c), with the progress bar; then the start screen as usual. A failure
    // stays on the setup page with what went wrong.
    if (!status.empty() && check_copied_files(g_root).empty() && gamebuild_available() &&
        !getenv("LARECOMP_LAUNCHER_DELETE")) {
        fprintf(stderr, "[launcher] game code out of date: updating it\n");
        g_src.clear();
        g_busy = true, g_auto_update = true;
        page = SETUP;
        pthread_t th;
        pthread_create(&th, NULL, extract_thread, NULL);
        pthread_detach(th);
    }
    if (getenv("LARECOMP_LAUNCHER_DELETE")) {  // (test) what DELETE GAME FILES does, logged; then the start screen
        double gb = rt_game_files_size(g_root.c_str()) / 1e9;
        stop_background_prep();
        bool ok = rt_remove_game_files(g_root.c_str());
        game_files_changed();
        status = check_game_files(g_root);
        fprintf(stderr, "[launcher] delete %.1f GB: %s -> %s\n", gb, ok ? "ok" : "refused", status.c_str());
        if (!status.empty()) { g_bg = 0; page = SETUP; }
    }
    if (g_onedrive < 0) {  // (once per launch: a "Title screen" reboot keeps it)
        if (PLAT_IOS) fprintf(stderr, "[launcher] data folder shown as: %s\n", data_root_shown().c_str());
        const char *t = getenv("LARECOMP_TEST_ONEDRIVE");
        g_onedrive_name = t ? (atoi(t) != 0 ? "OneDrive" : NULL) : plat_cloud_synced(rt_data_root());
        g_onedrive = g_onedrive_name != NULL;
        if (g_onedrive) fprintf(stderr, "[launcher] the data folder is synced by %s%s: %s\n", g_onedrive_name,
                                g_settings.onedrive_kept ? " (kept there)" : "", rt_data_root());
#if PLAT_IOS
        if (g_onedrive && plat_ios_cloud_offloaded(rt_data_root())) {
            char msg[700];
            snprintf(msg, sizeof msg, "%s\n\n%s", L("Some of LAS Recomp's files aren't on this iPhone. They download as the "
                                                  "game reads them, so it may be very slow or close."), L(KEEP_DOWNLOADED));
            g_notice = msg;  // (notice_popup: in the menus' look)
        }
#endif
    }
    // the shader preparation of every later start: in the background (first setups and updates do theirs above)
    // (test :preparing / :skipask: no background preparation - finishing, it took the test's bar away)
    const char *shot_pg = getenv("LARECOMP_LAUNCHER_SHOT");
    bool prep_shot = shot_pg && (strstr(shot_pg, ":preparing") || strstr(shot_pg, ":skipask") ||
                                 strstr(shot_pg, ":recompiling"));
    if (status.empty() && !g_busy && !getenv("LARECOMP_LAUNCHER_PREPARE") && !prep_shot) start_background_prep();
    // (test) LARECOMP_LAUNCHER_PLAY_AT=<s>: PLAY that many seconds in; LARECOMP_LAUNCHER_SKIP_AT=<s>: Skip then
    const char *play_at_env = getenv("LARECOMP_LAUNCHER_PLAY_AT"), *skip_at_env = getenv("LARECOMP_LAUNCHER_SKIP_AT");
    uint64_t loop_t0 = SDL_GetTicks(), last_frame = loop_t0, bg_gap = 0, bg_frames = 0;
    bool bg_logged = false;
    for (;;) {
        uint64_t now = SDL_GetTicks();
        if (g_bg_prep) bg_frames++, bg_gap = std::max(bg_gap, now - last_frame);
        else if (!bg_logged && bg_frames) {  // (how the start screen kept up meanwhile)
            bg_logged = true;
            fprintf(stderr, "[launcher] background shader preparation: %llu frames meanwhile, longest %llu ms\n",
                    (unsigned long long)bg_frames, (unsigned long long)bg_gap);
        }
        last_frame = now;
        if (play_at_env && status.empty() && now - loop_t0 >= (uint64_t)(atof(play_at_env) * 1000)) play = true, play_at_env = NULL;
        if (skip_at_env && g_prep_wait && now - loop_t0 >= (uint64_t)(atof(skip_at_env) * 1000)) g_prep_skip = 1, skip_at_env = NULL;
        // PLAY while that still runs: its bar (with Skip) until it's done
        if (play && g_bg_prep) {
            play = false, g_prep_wait = true;
            fprintf(stderr, "[launcher] PLAY: waiting for the shader preparation\n");
        }
        if (g_prep_wait && !g_bg_prep) {
            g_prep_wait = false, play = true;
            fprintf(stderr, "[launcher] shader preparation %s: playing\n", g_prep_skip ? "skipped" : "done");
        }
        if (play) break;
        menu_apply_window_requests(win);  // (the Video tab's Fullscreen)
        // no seed on the start screen: PLAY is the vanilla game (Settings > Gameplay greys out only what a seed forces)
        g_settings.rando_seed[0] = 0;
        if (g_files_deleted) {  // (Settings > Prelaunch deleted them)
            g_files_deleted = false;
            status = check_game_files(g_root);
            if (!status.empty()) { g_bg = 0; bind_capture_cancel(); settings_save(); page = SETUP; }
        }
#if PLAT_TOUCH
        if (char qs[128]; plat_touch_play_request(qs, sizeof qs)) launcher_play_seed(qs);
#endif
        // the Randomizer panel's Play or a launcher shortcut: that seed, then PLAY as if pressed (first-launch
        // questions); a seed deleted since leaves the start screen as it is
        if (g_play_seed_req) {
            g_play_seed_req = false;
            seeds = list_seeds();
            int want = g_play_seed.empty() ? 0 : -1;
            for (size_t i = 0; i < seeds.size(); i++)
                if (seeds[i] == g_play_seed) want = (int)i + 1;
            if (want < 0) {
                fprintf(stderr, "[launcher] play: seed \"%s\" is gone\n", g_play_seed.c_str());
#if PLAT_TOUCH
            } else if (!status.empty() || g_busy) {  // (a shortcut before the game files are set up)
                fprintf(stderr, "[launcher] play: the game files aren't ready\n");
#endif
            } else {
                if (want) choice = want;  // (the row on that seed, back from the game's Title screen)
                start_seed = g_play_seed;
                bind_capture_cancel();
                settings_save();
                page = MAIN;
                g_play_pending = true;
            }
        }
        // a rebind takes every release while it waits (menu.mm drain_events): nothing held when it starts or ends
        static bool capture_was;
        if (bind_capture_active(NULL, NULL, NULL) != capture_was) capture_was = !capture_was, ui_pad_release_all(io);
#ifdef UI_METAL
        ui_metal_hud_apply();
#endif
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            input_cursor_event(&e);  // (pad / key: the cursor hides; ImGui's backend leaves it so, below)
            if (page == SETTINGS && bind_capture_event(&e)) continue;  // (waiting for a new binding: controls_ui.mm)
#if PLAT_TOUCH
            // touch screens: the finger events go through touch_ui.h (taps click, drags scroll); the mouse events SDL
            // makes from the same touches (SDL_TOUCH_MOUSEID) would press on touch-down - dropped
            if (((e.type == SDL_EVENT_MOUSE_MOTION) && e.motion.which == SDL_TOUCH_MOUSEID) ||
                ((e.type == SDL_EVENT_MOUSE_BUTTON_DOWN || e.type == SDL_EVENT_MOUSE_BUTTON_UP) && e.button.which == SDL_TOUCH_MOUSEID) ||
                (e.type == SDL_EVENT_MOUSE_WHEEL && e.wheel.which == SDL_TOUCH_MOUSEID))
                continue;
            if (e.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN || (e.type == SDL_EVENT_GAMEPAD_AXIS_MOTION && abs(e.gaxis.value) > 12000))
                ui_typing_pad(true);  // (osk.mm: a controller types on its on-screen keys, a finger in the system's prompt)
            if (e.type == SDL_EVENT_FINGER_DOWN || e.type == SDL_EVENT_FINGER_UP || e.type == SDL_EVENT_FINGER_MOTION ||
                e.type == SDL_EVENT_FINGER_CANCELED) {
                ImGuiIO &tio = ImGui::GetIO();
                if (e.type == SDL_EVENT_FINGER_DOWN) ui_typing_pad(false);
                if (page == MAIN) pick_swipe_event(e, tio.DisplaySize);
                tio.AddMouseSourceEvent(ImGuiMouseSource_TouchScreen);
                g_touch.event(&e, tio.DisplaySize.x, tio.DisplaySize.y, [&](float x, float y) { tio.AddMousePosEvent(x, y); },
                              [&](bool down) { tio.AddMouseButtonEvent(0, down); });
                continue;
            }
#endif
            if (!launcher_pad_event(e)) ImGui_ImplSDL3_ProcessEvent(&e);
            if (e.type == SDL_EVENT_QUIT) exit(0);
        }
        if (ui_in_background()) { SDL_Delay(50); continue; }  // (iOS: a Metal drawable there is an error)
        // extraction finished: re-check, load the background
        pthread_mutex_lock(&g_lock);
        bool busy = g_busy;
        pthread_mutex_unlock(&g_lock);
        static bool was_busy;
        if (was_busy && !busy) {
            g_build_cache_changed = true;
            status = check_game_files(g_root);
            if (status.empty()) { if (!g_bg) load_background(dev); page = MAIN; }
            if (status.empty() && ap && getenv("LARECOMP_LAUNCHER_PREPARE")) play = true;
            g_auto_update = false;
        }
        was_busy = busy;
        if (i18n_fonts_stale(io.Fonts)) {  // (the Language setting changed: the glyphs of the new one)
            load_fonts();
#ifdef UI_METAL
            ImGui_ImplMetal_DestroyFontsTexture();
            ImGui_ImplMetal_CreateFontsTexture(dev);
#else
            vk_imgui_fonts_rebuild();
#endif
        }

#ifdef UI_METAL
        int w, h;
        SDL_GetWindowSizeInPixels(win, &w, &h);
        layer.drawableSize = CGSizeMake(w, h);
        @autoreleasepool {
            id<CAMetalDrawable> drawable = [layer nextDrawable];
            if (!drawable) { SDL_Delay(16); continue; }
            MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor renderPassDescriptor];
            rp.colorAttachments[0].texture = drawable.texture;
            rp.colorAttachments[0].loadAction = MTLLoadActionClear;
            rp.colorAttachments[0].clearColor = MTLClearColorMake(0.04, 0.05, 0.06, 1);
            rp.colorAttachments[0].storeAction = MTLStoreActionStore;
            id<MTLCommandBuffer> cb = [queue commandBuffer];
            id<MTLRenderCommandEncoder> enc = [cb renderCommandEncoderWithDescriptor:rp];
            ImGui_ImplMetal_NewFrame(rp);
#else
        {
            // one frame per refresh of the window's display: the swapchain presents without waiting (mailbox, for the
            // game's own pacing: device.c), so the start screen alone would draw as fast as the GPU goes
            static uint64_t next_frame;
            const SDL_DisplayMode *dm = SDL_GetCurrentDisplayMode(SDL_GetDisplayForWindow(win));
            uint64_t period = (uint64_t)(1e9 / (dm && dm->refresh_rate > 1 ? dm->refresh_rate : 60.0f)), now = plat_now_ns();
            bool on_time = next_frame > now && next_frame - now <= period;  // (else late, or the rate changed: start over)
            if (on_time) plat_sleep_until_ns(next_frame);
            next_frame = (on_time ? next_frame : now) + period;
            static const float clear_col[4] = {0.04f, 0.05f, 0.06f, 1};
            if (!vk_frame_begin(clear_col)) { SDL_Delay(16); continue; }
            ImGui_ImplVulkan_NewFrame();
#endif
            if (input_cursor_hidden()) io.ConfigFlags |= ImGuiConfigFlags_NoMouseCursorChange;
            else io.ConfigFlags &= ~ImGuiConfigFlags_NoMouseCursorChange;
            ImGui_ImplSDL3_NewFrame();
            // (test) LARECOMP_LAUNCHER_KEYS=<frame>:<key>,...: that key pressed at that frame, let go 2 frames later.
            // Keys: left right up down enter esc (keyboard), pl pr pu pd pa pb (controller d-pad, confirm, back)
            if (static const char *keys = getenv("LARECOMP_LAUNCHER_KEYS"); keys) {
                io.BackendFlags |= ImGuiBackendFlags_HasGamepad;  // (no controller needed for its presses)
                static long kf;
                kf++;
                for (const char *k = keys; k && *k; k = strchr(k, ',') ? strchr(k, ',') + 1 : NULL) {
                    long at = atol(k);
                    const char *name = strchr(k, ':');
                    if (!name || (kf != at && kf != at + 2)) continue;
                    static const struct { const char *n; ImGuiKey k; } map[] = {
                        {"left", ImGuiKey_LeftArrow}, {"right", ImGuiKey_RightArrow}, {"up", ImGuiKey_UpArrow},
                        {"down", ImGuiKey_DownArrow}, {"enter", ImGuiKey_Enter}, {"esc", ImGuiKey_Escape},
                        {"pl", ImGuiKey_GamepadDpadLeft}, {"pr", ImGuiKey_GamepadDpadRight}, {"pu", ImGuiKey_GamepadDpadUp},
                        {"pd", ImGuiKey_GamepadDpadDown}, {"pa", ImGuiKey_GamepadFaceDown}, {"pb", ImGuiKey_GamepadFaceRight}};
                    size_t len = strcspn(name + 1, ",");
                    for (auto &m : map)
                        if (strlen(m.n) == len && !strncmp(name + 1, m.n, len)) io.AddKeyEvent(m.k, kf == at);
                }
            }
            // the backend (manual mode, no gamepads of its own) clears HasGamepad every frame, and ImGui then drops the
            // controller presses launcher_pad_event fed in and turns gamepad navigation off: set it again while a
            // controller is connected
            if (SDL_HasGamepad()) io.BackendFlags |= ImGuiBackendFlags_HasGamepad;
            // a drag / flick (touch_ui.h): this frame's scroll, onto the page under the finger
            float touch_px = g_touch.frame(io.DeltaTime, [&](float x, float y) { io.AddMousePosEvent(x, y); },
                                           [&](bool down) { io.AddMouseButtonEvent(0, down); });
            ImGui::NewFrame();
            itd_music_tick(ImGui::GetIO().DeltaTime);
            const bool intro_now = itd_intro_on();  // (Port0r's logo over everything: the start screen's buttons wait)
            if (intro_now) itd_intro_frame(io.DisplaySize);
            g_touch.scroll_apply(touch_px);
            const Page frame_page = page;  // (menu_sfx_frame: B on the main page does nothing)

            ImVec2 ds = io.DisplaySize;
            float s = ds.y / 720.0f;  // layout scale (fonts are built for ~2x)
            bool pieces = false;  // (this frame has plastic pieces: Settings, the game files window, the keyboard)
            ImGui::GetIO().FontGlobalScale = s * 0.5f;
            ImDrawList *bg = ImGui::GetBackgroundDrawList();
            // an update by itself: only its bar, on the start screen's background (gray without it; a failure or
            // Cancel then shows the System page)
            const bool update_only = g_auto_update && busy;
            if (g_bg && (status.empty() || update_only)) {  // cover the window, keep the aspect
                float ia = (float)g_bg_w / g_bg_h, wa = ds.x / ds.y;
                ImVec2 uv0(0, 0), uv1(1, 1);
                if (wa > ia) { float v = ia / wa; uv0.y = (1 - v) / 2; uv1.y = 1 - uv0.y; }
                else { float u = wa / ia; uv0.x = (1 - u) / 2; uv1.x = 1 - uv0.x; }
                bg->AddImage(BG_TEXID, ImVec2(0, 0), ds, uv0, uv1);
            } else if (update_only) {
                bg->AddRectFilled(ImVec2(0, 0), ds, IM_COL32(40, 40, 40, 255));
            } else {
                bg->AddRectFilledMultiColor(ImVec2(0, 0), ds, IM_COL32(18, 34, 40, 255), IM_COL32(40, 70, 64, 255),
                                            IM_COL32(12, 20, 24, 255), IM_COL32(8, 12, 16, 255));
            }
            if (!update_only) {  // the night rising from the left behind the title and the buttons (port0r.com's media
                                 // overlay), and from the bottom behind the footer and the game files' card
                const float stops[][2] = {{0.0f, 240}, {0.32f, 228}, {0.46f, 186}, {0.58f, 108}, {0.68f, 40}, {0.78f, 0}};
                for (int i = 0; i + 1 < (int)(sizeof stops / sizeof stops[0]); i++) {
                    ImU32 a = IM_COL32(11, 15, 25, (int)stops[i][1]), b = IM_COL32(11, 15, 25, (int)stops[i + 1][1]);
                    bg->AddRectFilledMultiColor(ImVec2(ds.x * stops[i][0], 0), ImVec2(ds.x * stops[i + 1][0], ds.y), a, b, b, a);
                }
                ImU32 c0 = IM_COL32(11, 15, 25, 0), c1 = IM_COL32(11, 15, 25, 200);
                bg->AddRectFilledMultiColor(ImVec2(0, ds.y - 230 * s), ImVec2(ds.x, ds.y), c0, c0, c1, c1);
            }

            ImGui::SetNextWindowPos(ImVec2(0, 0));
            ImGui::SetNextWindowSize(ds);
            ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0, 0, 0, 0));
            ImGui::Begin("##launcher", NULL, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                                                 ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBringToFrontOnFocus);
            const bool waiting = g_prep_wait;  // (the menu waits under the bar)
            ImGui::BeginDisabled(waiting);
            ImGui::SetScrollX(0);  // (the start screen never scrolls; a long seed name or a stray nav move mustn't shift it)
            ImGui::SetScrollY(0);
            float safe_l, safe_t, safe_r, safe_b;
            safe_insets(&safe_l, &safe_t, &safe_r, &safe_b);
            float x0 = fmaxf(90 * s, safe_l + 30 * s);
            ImGui::SetCursorPos(ImVec2(x0, 285 * s));
            // typing in a text field / the on-screen keyboard (osk.h): B / Esc are for it, not "back" (from the frame
            // before too, so the Esc that ends the typing doesn't also leave the page)
            static bool typed_before;
            bool typing = typed_before || ui_text_busy();

            if (page == MAIN && intro_now) {  // (under the logo animation: not drawn, nothing to press by mistake)
            } else if (page == MAIN) {  // Inside The Dream's face (above): the bar, the title, the play buttons, the game files
                ImDrawList *dl = ImGui::GetWindowDrawList();
                ImVec2 wp = ImGui::GetWindowPos();
                const float X = wp.x + 72 * s, Y = wp.y;
                const bool ready = status.empty();
                itd_bar(dl, wp, ds, s, X, Y);
                // the kicker (the two eyes), the title with its yellow block, the lead
                float ky = Y + 158 * s;
                dl->AddCircleFilled(ImVec2(X + 5 * s, ky + 8 * s), 5 * s, ITD_RED_EYE);
                dl->AddCircleFilled(ImVec2(X + 18 * s, ky + 8 * s), 5 * s, ITD_CYAN_EYE);
                itd_text(dl, g_itd_btn, 14 * s, ImVec2(X + 33 * s, ky), ITD_WHITE(225),
                         itd_t("FIRST-PERSON VIEW MOD  \xc2\xb7  BUILT ON LAS RECOMP", "MOD DE VUE SUBJECTIVE  \xc2\xb7  CONSTRUIT SUR LAS RECOMP"), 0.12f);
                const float ts = 98 * s;
                float y1 = ky + 16 * s, y2 = y1 + ts * 0.86f;  // (Anton's capitals: from 0.22 to 0.79 of its line)
                itd_text(dl, g_itd_title, ts, ImVec2(X - 4 * s, y1), ITD_WHITE(255), "INSIDE", 0.004f);
                {
                    float hw = itd_text_w(g_itd_title, ts, "THE DREAM", 0.004f), pad = ts * 0.12f;
                    // (the block behind the whole capitals: the site's lets their tops show above it, ink on paper - on the
                    //  night they vanished)
                    dl->AddRectFilled(ImVec2(X - 4 * s, y2 + ts * 0.14f), ImVec2(X - 4 * s + hw + 2 * pad, y2 + ts * 0.88f), ITD_ACCENT);
                    itd_text(dl, g_itd_title, ts, ImVec2(X - 4 * s + pad, y2), ITD_INK, "THE DREAM", 0.004f);
                }
#ifdef LARECOMP_DESKTOP_XR
                const bool vr = true;
#else
                const bool vr = false;
#endif
                float ly = y2 + ts * 1.04f, lw = 560 * s, lsz = 19 * s;
                const char *lead = vr ? itd_t("The Legend of Zelda: Link's Awakening (Switch, 2019), seen through Link's eyes: in first "
                                              "person, on your screen or in a VR headset.",
                                              "The Legend of Zelda: Link's Awakening (Switch, 2019), \xc3\xa0 travers les yeux de Link : "
                                              "en vue subjective, sur l'\xc3\xa9" "cran ou dans un casque VR.")
                                      : itd_t("The Legend of Zelda: Link's Awakening (Switch, 2019), seen through Link's eyes: in first "
                                              "person, on your screen.",
                                              "The Legend of Zelda: Link's Awakening (Switch, 2019), \xc3\xa0 travers les yeux de Link : "
                                              "en vue subjective, sur votre \xc3\xa9" "cran.");
                ImVec2 lsz2 = g_itd_body->CalcTextSizeA(lsz, FLT_MAX, lw, lead);
                dl->AddText(g_itd_body, lsz, ImVec2(X, ly), ITD_WHITE(205), lead, NULL, lw);
                // the buttons: PLAY and PLAY IN VR (SET UP THE GAME first while the game files aren't ready)
                float by = ly + lsz2.y + 34 * s, bh = 68 * s, bw = 292 * s, gap = 14 * s;
                static bool focused_once;
                bool play_hot = false, pressed = false;
                if (!focused_once) { ImGui::SetKeyboardFocusHere(); focused_once = true; }
                if (ready) {
                    pressed = itd_pill("play", ImVec2(X, by), ImVec2(bw, bh), PILL_ACCENT, ICON_PLAY, itd_t("PLAY", "JOUER"), g_itd_big,
                                       30 * s, 0.02f, true, s, &play_hot);
                    if (pressed) start_seed.clear();  // (PLAY: the vanilla game; a seed from the randomizer pill)
                } else if (itd_pill("setup", ImVec2(X, by), ImVec2(bw, bh), PILL_ACCENT, ICON_FOLDER, itd_t("SET UP THE GAME", "PR\xc3\x89PARER LE JEU"),
                                    g_itd_big, 26 * s, 0.02f, !busy, s)) {
                    page = SETUP;
                }
                static std::string xr_name;
                static bool xr_looked;
                if (vr) {
                    if (!xr_looked) {
#ifdef LARECOMP_DESKTOP_XR
                        xr_name = itd_xr_runtime();
#endif
                        xr_looked = true;
                    }
                    if (itd_pill("vr", ImVec2(X + bw + gap, by), ImVec2(bw, bh), PILL_LIGHT, ICON_VR, itd_t("PLAY IN VR", "JOUER EN VR"), g_itd_big,
                                 30 * s, 0.02f, ready, s)) {
                        settings_save();
                        char exe[4096];
                        if (plat_exe_path(exe, sizeof exe)) {
                            char *argv[] = {exe, (char *)g_root.c_str(), (char *)"--play", (char *)"--vr", NULL};
                            fprintf(stderr, "[launcher] PLAY IN VR: %s %s --play --vr\n", exe, g_root.c_str());
                            // (Windows: plat_exec waits for the game to end; the start screen's window, left up and
                            //  no longer answering, was reported "not responding" - it goes, with the music)
                            itd_music_stop();
                            SDL_HideWindow(win);
                            plat_exec(argv);
                        }
                    }
                    char hint[160];
                    if (xr_name.empty()) snprintf(hint, sizeof hint, "%s", itd_t("No VR runtime found: install SteamVR", "Aucun runtime VR trouv\xc3\xa9 : installez SteamVR"));
                    else snprintf(hint, sizeof hint, itd_t("VR runtime: %s", "Runtime VR : %s"), xr_name.c_str());
                    float hwid = g_itd_label->CalcTextSizeA(13 * s, FLT_MAX, 0, hint).x;
                    dl->AddText(g_itd_label, 13 * s, ImVec2(X + bw + gap + (bw - hwid) * 0.5f, by + bh + 10 * s),
                                xr_name.empty() ? IM_COL32(255, 150, 140, 230) : ITD_WHITE(160), hint);
                }
                if (ready) {
                    char ph[96];
                    snprintf(ph, sizeof ph, "%s", itd_t("On the screen: F1 or Minus + Plus for the menu", "Sur l'\xc3\xa9" "cran : F1 ou Moins + Plus pour le menu"));
                    float pw = g_itd_label->CalcTextSizeA(13 * s, FLT_MAX, 0, ph).x;
                    dl->AddText(g_itd_label, 13 * s, ImVec2(X + (bw - pw) * 0.5f, by + bh + 10 * s), ITD_WHITE(160), ph);
                }
                // RANDOMIZER  ‹ New Seed / a seed ›: its own pill (LAS Recomp beta 6's row). New Seed opens the Randomizer page,
                // a seed starts the game with it; left / right on it step through them and stop at either end
                float ry = by + bh + 48 * s, rh = 46 * s, rx = X;
                {
                    const int n = (int)seeds.size() + 1;
                    std::string nm = choice ? seeds[choice - 1] : std::string(itd_t("New Seed", "Nouvelle graine"));
                    if (nm.size() > 22) nm = nm.substr(0, 21) + "\xe2\x80\xa6";
                    char mode[128];
                    snprintf(mode, sizeof mode, "%s  \xe2\x80\xb9  %s  \xe2\x80\xba", itd_t("RANDOMIZER", "RANDOMIZER"), nm.c_str());
                    float mw = itd_text_w(g_itd_btn, 14 * s, mode, 0.04f) + 40 * s;
                    bool mode_hot = false;
                    if (g_refocus == RANDO) ImGui::SetKeyboardFocusHere(), g_refocus = MAIN;
                    if (itd_pill("mode", ImVec2(rx, ry), ImVec2(mw, rh), PILL_SOFT, ICON_NONE, mode, g_itd_btn, 14 * s, 0.04f, ready, s, &mode_hot)) {
                        if (!choice) menu_tabs_focus(false), page = RANDO;
                        else start_seed = seeds[choice - 1], pressed = true;
                    }
                    if (mode_hot && ready) {
                        if (ImGui::IsKeyPressed(ImGuiKey_LeftArrow) || ImGui::IsKeyPressed(ImGuiKey_GamepadDpadLeft)) choice = std::max(choice - 1, 0);
                        if (ImGui::IsKeyPressed(ImGuiKey_RightArrow) || ImGui::IsKeyPressed(ImGuiKey_GamepadDpadRight)) choice = std::min(choice + 1, n - 1);
                        ImGuiID own = ImGui::GetID("##rando_lr");  // (ours while there: ImGui's navigation would move away)
                        for (ImGuiKey k : {ImGuiKey_LeftArrow, ImGuiKey_RightArrow, ImGuiKey_GamepadDpadLeft, ImGuiKey_GamepadDpadRight})
                            ImGui::SetKeyOwner(k, own);
                    }
                    static int logged = -1;
                    if (choice != logged)
                        logged = choice, fprintf(stderr, "[launcher] randomizer row: %s\n", choice ? seeds[choice - 1].c_str() : "New Seed");
                    rx += mw + 10 * s;
                }
                {
                    const char *st_l = itd_t("SETTINGS", "R\xc3\x89GLAGES");
                    float sw = itd_text_w(g_itd_btn, 14 * s, st_l, 0.08f) + 70 * s;
                    if (g_refocus == SETTINGS) ImGui::SetKeyboardFocusHere(), g_refocus = MAIN;  // (back from Settings: it stays selected)
                    if (itd_pill("settings", ImVec2(rx, ry), ImVec2(sw, rh), PILL_SOFT, ICON_SLIDERS, st_l, g_itd_btn, 14 * s, 0.08f, true, s)) {
                        static bool opened;
                        menu_tabs_focus(!opened);  // (first time this session: Prelaunch; later: the selected tab)
                        opened = true;
                        page = SETTINGS;
                    }
                    rx += sw + 10 * s;
                    const char *q_l = itd_t("QUIT", "QUITTER");
                    float qw = itd_text_w(g_itd_btn, 14 * s, q_l, 0.08f) + 70 * s;
#if !PLAT_TOUCH  // (a phone app has no quit: the home gesture)
                    if (itd_pill("quit", ImVec2(rx, ry), ImVec2(qw, rh), PILL_SOFT, ICON_QUIT, q_l, g_itd_btn, 14 * s, 0.08f, true, s)) exit(0);
#else
                    (void)qw;
#endif
                }
                if (vr) {  // PC VR's own choices, before it starts (Guillaume, 12/11: « utiliser la manette du PC, utiliser le son du
                    // PC, réglable avant de lancer »): kept in the settings, read by the game in the headset
                    float oy = ry + rh + 14 * s, ox = X, oh = 38 * s;
                    itd_text(dl, g_itd_btn, 11 * s, ImVec2(ox, oy + (oh - 11 * s) * 0.5f), ITD_WHITE(150), itd_t("IN VR", "EN VR"), 0.12f);
                    ox += itd_text_w(g_itd_btn, 11 * s, itd_t("IN VR", "EN VR"), 0.12f) + 14 * s;
                    // the output Windows plays on now, the music's, kept for the game in the headset (« tu enregistres la
                    // sortie qui est en ce moment active et tu prends ça », 12/11); a headset's own (SteamVR's streaming)
                    // is never taken for the PC's. Looked at once a second: changing it in Windows is followed.
                    static double out_at = -10;
                    static bool audio_up;
                    static char pc_name[48];
                    if (ImGui::GetTime() - out_at > 1.0) {
                        out_at = ImGui::GetTime();
                        if (!audio_up) audio_up = SDL_InitSubSystem(SDL_INIT_AUDIO);
                        const char *now = audio_up ? SDL_GetAudioDeviceName(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK) : NULL;
                        if (now && *now && !itd_headset_output(now) && strcmp(now, g_settings.itd_vr_audio_dev)) {
                            snprintf(g_settings.itd_vr_audio_dev, sizeof g_settings.itd_vr_audio_dev, "%s", now);
                            settings_save();
                        }
                        SDL_AudioDeviceID id = audio_up ? itd_pc_output() : 0;
                        const char *nm = id ? SDL_GetAudioDeviceName(id) : NULL;
                        size_t k = 0;
                        for (; nm && nm[k] && k < sizeof pc_name - 4; k++) pc_name[k] = nm[k];
                        if (nm && nm[k]) {  // (cut on a character's start, then "...")
                            while (k && ((unsigned char)pc_name[k] & 0xC0) == 0x80) k--;
                            memcpy(pc_name + k, "...", 3), k += 3;
                        }
                        pc_name[k] = 0;
                    }
                    static char snd_pc[96];
                    snprintf(snd_pc, sizeof snd_pc, "%s%s%s", itd_t("SOUND: ON THE PC", "SON : SUR LE PC"), pc_name[0] ? " · " : "", pc_name);
                    const char *snd = g_settings.itd_vr_audio == 1 ? snd_pc : itd_t("SOUND: IN THE HEADSET", "SON : DANS LE CASQUE");
                    float sw2 = itd_text_w(g_itd_btn, 12 * s, snd, 0.06f) + 36 * s;
                    if (itd_pill("vr_sound", ImVec2(ox, oy), ImVec2(sw2, oh), PILL_GHOST, ICON_NONE, snd, g_itd_btn, 12 * s, 0.06f, true, s))
                        g_settings.itd_vr_audio = g_settings.itd_vr_audio == 1 ? 0 : 1, settings_save();
                    ox += sw2 + 10 * s;
                    const char *pad = g_settings.itd_vr_pad ? itd_t("PC CONTROLLER: YES", "MANETTE DU PC : OUI")
                                                            : itd_t("PC CONTROLLER: NO", "MANETTE DU PC : NON");
                    float pw2 = itd_text_w(g_itd_btn, 12 * s, pad, 0.06f) + 36 * s;
                    if (itd_pill("vr_pad", ImVec2(ox, oy), ImVec2(pw2, oh), PILL_GHOST, ICON_NONE, pad, g_itd_btn, 12 * s, 0.06f, true, s))
                        g_settings.itd_vr_pad = !g_settings.itd_vr_pad, settings_save();
                }
                // first PLAY ever: the aspect ratio question, then how to reach our menu, then the game
                if (g_play_pending && ready) pressed = true, g_play_pending = false;
                if (pressed) {
                    if (!g_settings.aspect_asked) ImGui::OpenPopup("##aspect");
                    else if (!g_settings.menu_tip_seen) ImGui::OpenPopup("##menutip");
                    else play = true;
                }
                if (g_tip_test) ImGui::OpenPopup(g_settings.aspect_asked ? "##menutip" : "##aspect"), g_tip_test = false;
                menu_style_begin(ImGui::GetIO().DisplaySize.y / 1080.0f), plastic_widgets(true);
                if (aspect_prompt(s)) {
                    if (g_settings.menu_tip_seen) play = true;
                    else ImGui::OpenPopup("##menutip");
                }
                if (menu_tip(s)) play = true;
                plastic_widgets(false), menu_style_end();

                // the game files, in a card of the night (lower right)
                {
                    float cw = 400 * s, ch = ready ? 132 * s : 150 * s;
                    ImVec2 ca(wp.x + ds.x - 60 * s - cw, wp.y + ds.y - 64 * s - ch), cb(ca.x + cw, ca.y + ch);
                    dl->AddRectFilled(ca, cb, ITD_NIGHT(214), 18 * s);
                    float px = ca.x + 22 * s, py = ca.y + 18 * s;
                    itd_text(dl, g_itd_big, 22 * s, ImVec2(px, py), ITD_WHITE(255), itd_t("GAME FILES", "FICHIERS DU JEU"), 0.01f);
                    const char *chip = busy ? itd_t("WORKING", "EN COURS") : ready ? itd_t("READY", "PR\xc3\x8aT") : itd_t("TO SET UP", "\xc3\x80 PR\xc3\x89PARER");
                    ImU32 cbg = busy ? IM_COL32(255, 238, 186, 255) : ready ? IM_COL32(217, 247, 232, 255) : IM_COL32(253, 227, 225, 255);
                    ImU32 cfg = busy ? IM_COL32(122, 84, 0, 255) : ready ? IM_COL32(11, 107, 58, 255) : IM_COL32(163, 35, 27, 255);
                    float chw = itd_text_w(g_itd_btn, 11 * s, chip, 0.1f) + 20 * s;
                    ImVec2 c0(cb.x - 22 * s - chw, py + 1 * s);
                    dl->AddRectFilled(c0, ImVec2(c0.x + chw, c0.y + 24 * s), cbg, 5 * s);
                    itd_text(dl, g_itd_btn, 11 * s, ImVec2(c0.x + 10 * s, c0.y + 5.5f * s), cfg, chip, 0.1f);
                    auto row = [&](float y, bool ok, const char *t) {
                        ImVec2 c(px + 11 * s, y + 11 * s);
                        dl->AddCircleFilled(c, 11 * s, ok ? IM_COL32(34, 212, 123, 255) : ITD_WHITE(40));
                        if (ok) {
                            dl->AddLine(ImVec2(c.x - 5 * s, c.y), ImVec2(c.x - 1.5f * s, c.y + 3.5f * s), ITD_WHITE(255), 2.2f * s);
                            dl->AddLine(ImVec2(c.x - 1.5f * s, c.y + 3.5f * s), ImVec2(c.x + 5 * s, c.y - 4 * s), ITD_WHITE(255), 2.2f * s);
                        } else {
                            dl->AddLine(ImVec2(c.x - 4.5f * s, c.y), ImVec2(c.x + 4.5f * s, c.y), ITD_WHITE(170), 2.2f * s);
                        }
                        dl->AddText(g_itd_label, 15 * s, ImVec2(px + 32 * s, y + 2 * s), ITD_WHITE(232), t);
                    };
                    bool files_ok = check_copied_files(g_root).empty();
                    row(py + 38 * s, files_ok, itd_t("Your own copy of the game (Switch)", "Votre copie du jeu (Switch)"));
                    row(py + 66 * s, ready, itd_t("The game code, recompiled on this computer", "Le code du jeu, recompil\xc3\xa9 sur cet ordinateur"));
                    if (!ready) {
                        std::string why = busy ? itd_t("Setting up the game files...", "Pr\xc3\xa9paration des fichiers du jeu...") : itd_tr(status_hint(status));
                        dl->AddText(g_itd_label, 13 * s, ImVec2(px, py + 98 * s), IM_COL32(255, 170, 160, 240), why.c_str(), NULL, cw - 44 * s);
                    }
                }
                itd_footer(dl, wp, ds, s, X);
                itd_music_control(dl, wp, ds, s);
                g_menu_gap = 0.35f;
            } else if (page == SETTINGS && preview_active()) {
                // (a row's live preview, live_preview.h: the panel at the bottom instead of the window - at the
                //  window's scale, the phones' enlargement included, as the in-game menu draws it)
                float sc, w, h;
                ImVec2 centre;
                panel_layout(ds, &sc, &w, &h, &centre);
                float sl, st, sr, sb;
                safe_insets(&sl, &st, &sr, &sb);
                preview_safe_area(sl, st, sr, sb);
                menu_preview_frame(sc, g_font_small, g_font_menu);
                pieces = true;
            } else if (page == SETTINGS || page == RANDO) {
                // the in-game menu's tabs (runtime/ui/menu.mm), in a window like the in-game one; tabs that need a
                // loaded game say so. RANDO: the Randomizer page alone (its own panel, not one of Settings' tabs)
                float sc, w, h;
                ImVec2 centre;
                panel_layout(ds, &sc, &w, &h, &centre);
                ImGui::GetWindowDrawList()->AddRectFilled(ImGui::GetWindowPos(), ImVec2(ImGui::GetWindowPos().x + ds.x,
                                                          ImGui::GetWindowPos().y + ds.y),
                                                          IM_COL32(0, 0, 0, (int)(plastic_theme().dim * 255)));
                ImVec2 wp = ImGui::GetWindowPos();
                ImVec2 pa(wp.x + centre.x - w * 0.5f, wp.y + centre.y - h * 0.5f);
                pieces = true;
                page_panel(wp, ds, pa, w, h, sc);
                ImGui::SetCursorScreenPos(pa);
                ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0, 0, 0, 0));
                ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 14 * sc);
                ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(28 * sc, 24 * sc));
                ImGui::BeginChild("##settings", ImVec2(w, h), ImGuiChildFlags_AlwaysUseWindowPadding, ImGuiWindowFlags_NavFlattened);
                page_content_inset();
                ImGui::PopStyleVar(2);
                ImGui::PopStyleColor();
                plastic_widgets(true);  // (popups, buttons and fields inside as pieces too)
                ImGui::PushFont(g_font_small);
                ImGui::PushFont(g_font_menu);
                if (!PLAT_TOUCH)  // (touch: an icon for the title - Settings' gear in tabs_body, the Randomizer's below)
                    ImGui::TextColored(plastic_col(plastic_theme().title), "%s", page == RANDO ? L("Randomizer") : L("Settings"));
                ImGui::PopFont();
                bool back = false;
                {  // close: the round X in the corner (B / Esc too); touch, the Randomizer: in its own pane (rando_tab.mm)
                    float bx = 44 * sc;
                    if (!(PLAT_TOUCH && page == RANDO)) back = plastic_close_corner(bx, sc);
                    // (Settings: Reset to vanilla beside it, as in game; PLAY then starts the vanilla game)
                    // (the seed is dropped from the saved settings too, as the reset's other settings are)
                    if (page != RANDO && menu_reset_corner(bx, sc, g_font_small, g_font_menu, !start_seed.empty())) {
                        start_seed.clear();
                        g_settings.rando_seed[0] = 0;
                        settings_save();
                    }
                }
                bool popup = ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId);  // (before the pages run, like the menu)
                if (page == RANDO) {
                    float indent = PLAT_TOUCH ? menu_page_corner(3, sc) : 0;
                    menu_tabs_draw(sc, g_font_small, g_font_menu, false, NULL, 3);
                    if (indent > 0) ImGui::Unindent(indent);
                    if (PLAT_TOUCH && rando_tab_close_pressed()) back = true;
                } else menu_tabs_draw(sc, g_font_small, g_font_menu, false, prelaunch_tab);
                int cap_dev;
                bind_capture_active(&cap_dev, NULL, NULL);
                if (!popup && !typing && cap_dev < 0 && !controls_ui_took_back() && !rando_tab_took_back() && ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) &&
                    (ImGui::IsKeyPressed(ImGuiKey_GamepadFaceRight, false) || ImGui::IsKeyPressed(ImGuiKey_Escape, false)))
                    back = true;
                ImGui::PopFont();
                plastic_widgets(false);
                ImGui::EndChild();
#if LARECOMP_IOSEXPORT
                bool to_ios = ios_export_page_requested();  // (Prelaunch's button: leave Settings for the export page)
                if (to_ios) back = true;
#else
                bool to_ios = false;
#endif
                if (back) {
                    bind_capture_cancel();
                    settings_save();
                    g_refocus = page, page = to_ios ? IOS : MAIN;
                    std::string cur = choice ? seeds[choice - 1] : "";  // seeds made / renamed / deleted in the panel
                    seeds = list_seeds();
                    choose(cur.c_str());
                }
            } else if (update_only) {
                pthread_mutex_lock(&g_lock);
                std::string job = g_job;
                pthread_mutex_unlock(&g_lock);
                draw_busy_overlay(ds, job, false);
            } else if (!(status.empty() && !busy)) {
                // SETUP, the game files not ready yet (Inside The Dream: a page of its own in Port0r's look, as PLAY's):
                // 1 the folder with the player's own copy, 2 set the game up; what went wrong, and what to do, in a card
                ImDrawList *dl = ImGui::GetWindowDrawList();
                ImVec2 wp = ImGui::GetWindowPos();
                const float X = wp.x + 72 * s, Y = wp.y;
                pthread_mutex_lock(&g_lock);
                std::string src = g_src, job = g_job, job_hint = g_job_hint;
                bool failed = g_job_failed;
                pthread_mutex_unlock(&g_lock);
                itd_bar(dl, wp, ds, s, X, Y);
                float ky = Y + 118 * s;
                dl->AddCircleFilled(ImVec2(X + 5 * s, ky + 8 * s), 5 * s, ITD_RED_EYE);
                dl->AddCircleFilled(ImVec2(X + 18 * s, ky + 8 * s), 5 * s, ITD_CYAN_EYE);
                itd_text(dl, g_itd_btn, 14 * s, ImVec2(X + 33 * s, ky), ITD_WHITE(225),
                         itd_t("FIRST LAUNCH  \xc2\xb7  ONCE", "PREMIER LANCEMENT  \xc2\xb7  UNE SEULE FOIS"), 0.12f);
                const float ts = 72 * s;
                float y1 = ky + 16 * s, y2 = y1 + ts * 0.86f;
                itd_text(dl, g_itd_title, ts, ImVec2(X - 4 * s, y1), ITD_WHITE(255), itd_t("SET UP", "PR\xc3\x89PARER"), 0.004f);
                {
                    const char *hl = itd_t("THE GAME", "LE JEU");
                    float hw = itd_text_w(g_itd_title, ts, hl, 0.004f), pad = ts * 0.12f;
                    dl->AddRectFilled(ImVec2(X - 4 * s, y2 + ts * 0.14f), ImVec2(X - 4 * s + hw + 2 * pad, y2 + ts * 0.88f), ITD_ACCENT);
                    itd_text(dl, g_itd_title, ts, ImVec2(X - 4 * s + pad, y2), ITD_INK, hl, 0.004f);
                }
                float ly = y2 + ts * 1.04f, lw = 820 * s, lsz = 18 * s;
                const char *lead = itd_t("No game is included. Show Inside The Dream your own copy: the extracted files of The Legend of "
                                         "Zelda: Link's Awakening for Nintendo Switch, version 1.0.0 without updates (its exefs and "
                                         "romfs folders). It copies what it needs and recompiles the game on this computer, once. "
                                         "Your own files aren't changed.",
                                         "Aucun jeu n'est inclus. Montrez \xc3\xa0 Inside The Dream votre propre copie : les fichiers "
                                         "extraits de The Legend of Zelda: Link's Awakening pour Nintendo Switch, version 1.0.0 sans mise "
                                         "\xc3\xa0 jour (ses dossiers exefs et romfs). Il copie ce dont il a besoin et recompile le jeu sur "
                                         "cet ordinateur, une seule fois. Vos fichiers ne sont pas modifi\xc3\xa9s.");
                ImVec2 lsz2 = g_itd_body->CalcTextSizeA(lsz, FLT_MAX, lw, lead);
                dl->AddText(g_itd_body, lsz, ImVec2(X, ly), ITD_WHITE(205), lead, NULL, lw);
                // the two steps, side by side
                float cy = ly + lsz2.y + 28 * s, cw = 404 * s, ch = 178 * s, cg = 18 * s;
                bool have_src = !src.empty(), resume = !busy && src.empty() && resumable_build();
                bool can_set_up = !busy && (have_src || check_copied_files(g_root).empty() || resume);
                auto card = [&](float x, int n, const char *title, const char *chip, int tone) {
                    ImVec2 a(x, cy), b(x + cw, cy + ch);
                    dl->AddRectFilled(a, b, ITD_NIGHT(220), 18 * s);
                    char t[96];
                    snprintf(t, sizeof t, "%d  \xc2\xb7  %s", n, title);
                    itd_text(dl, g_itd_big, 22 * s, ImVec2(a.x + 22 * s, a.y + 18 * s), ITD_WHITE(255), t, 0.01f);
                    ImU32 cbg = tone == 1 ? IM_COL32(217, 247, 232, 255) : tone == 2 ? IM_COL32(253, 227, 225, 255) : ITD_WHITE(30);
                    ImU32 cfg = tone == 1 ? IM_COL32(11, 107, 58, 255) : tone == 2 ? IM_COL32(163, 35, 27, 255) : ITD_WHITE(215);
                    float chw = itd_text_w(g_itd_btn, 11 * s, chip, 0.1f) + 20 * s;
                    ImVec2 c0(b.x - 22 * s - chw, a.y + 19 * s);
                    dl->AddRectFilled(c0, ImVec2(c0.x + chw, c0.y + 24 * s), cbg, 5 * s);
                    itd_text(dl, g_itd_btn, 11 * s, ImVec2(c0.x + 10 * s, c0.y + 5.5f * s), cfg, chip, 0.1f);
                };
                // 1: the folder
                // (a failure about the folder itself marks step 1; one while copying or compiling, step 2)
                bool files_problem = failed && (g_fail_kind == FAIL_NOT_FOUND || g_fail_kind == FAIL_UNUSABLE ||
                                                g_fail_kind == FAIL_VERSION || g_fail_kind == FAIL_MISSING);
                card(X, 1, itd_t("YOUR GAME FILES", "VOS FICHIERS DU JEU"),
                     files_problem ? itd_t("TO CHECK", "\xc3\x80 V\xc3\x89RIFIER") : have_src ? itd_t("CHOSEN", "CHOISI") : itd_t("TO CHOOSE", "\xc3\x80 CHOISIR"),
                     files_problem ? 2 : have_src ? 1 : 0);
                {
                    std::string shown = have_src ? src : std::string(itd_t("No folder chosen yet.", "Aucun dossier choisi pour l'instant."));
                    float tw = cw - 44 * s;  // (a long path: its start cut, the end kept - the folder's own name shows)
                    while (have_src && shown.size() > 8 && g_itd_label->CalcTextSizeA(15 * s, FLT_MAX, 0, shown.c_str()).x > tw)
                        shown = "\xe2\x80\xa6" + shown.substr(shown[0] == '\xe2' ? 6 : 4);
                    dl->AddText(g_itd_label, 15 * s, ImVec2(X + 22 * s, cy + 62 * s), ITD_WHITE(have_src ? 235 : 150), shown.c_str());
                    dl->AddText(g_itd_label, 13 * s, ImVec2(X + 22 * s, cy + 88 * s), ITD_WHITE(140),
                                itd_t("The folder that holds exefs and romfs, or the two themselves.",
                                      "Le dossier qui contient exefs et romfs, ou ces deux dossiers."), NULL, cw - 44 * s);
                    static int focus_frames;  // (the controller's cursor on the first step's button when the page opens)
                    static int seen = -10;
                    if (seen != ImGui::GetFrameCount() - 1) focus_frames = 3;
                    seen = ImGui::GetFrameCount();
                    if (focus_frames > 0 && focus_frames--) ImGui::SetKeyboardFocusHere();
                    if (itd_pill("pick", ImVec2(X + 22 * s, cy + ch - 22 * s - 52 * s), ImVec2(cw - 44 * s, 52 * s), have_src ? PILL_SOFT : PILL_LIGHT,
                                 ICON_FOLDER, have_src ? itd_t("CHOOSE ANOTHER FOLDER", "CHOISIR UN AUTRE DOSSIER") : itd_t("CHOOSE THE FOLDER", "CHOISIR LE DOSSIER"),
                                 g_itd_btn, 14 * s, 0.08f, !busy, s))
                        SDL_ShowOpenFolderDialog(picked, NULL, win, NULL, false);
                }
                // 2: set up
                {
                    float x2 = X + cw + cg;
                    bool done_ok = !failed && !job.empty() && status.empty();
                    card(x2, 2, itd_t("SET UP", "PR\xc3\x89PARATION"),
                         busy ? itd_t("WORKING", "EN COURS") : failed && !files_problem ? itd_t("STOPPED", "ARR\xc3\x8aT\xc3\x89" "E")
                              : done_ok ? itd_t("DONE", "TERMIN\xc3\x89" "E") : can_set_up ? itd_t("READY", "PR\xc3\x8a" "TE") : itd_t("WAITING", "EN ATTENTE"),
                         failed && !files_problem ? 2 : done_ok || can_set_up ? 1 : 0);
                    dl->AddText(g_itd_label, 15 * s, ImVec2(x2 + 22 * s, cy + 62 * s), ITD_WHITE(235),
                                itd_t("Copies the game files, then recompiles the game code.", "Copie les fichiers du jeu, puis recompile le code du jeu."),
                                NULL, cw - 44 * s);
                    dl->AddText(g_itd_label, 13 * s, ImVec2(x2 + 22 * s, cy + 88 * s), ITD_WHITE(140),
                                itd_t("About 7 GB free, and a few minutes the first time.", "Environ 7 Go libres, et quelques minutes la premi\xc3\xa8re fois."),
                                NULL, cw - 44 * s);
                    if (itd_pill("setup_go", ImVec2(x2 + 22 * s, cy + ch - 22 * s - 52 * s), ImVec2(cw - 44 * s, 52 * s), PILL_ACCENT, ICON_PLAY,
                                 resume ? itd_t("RESUME THE GAME CODE", "REPRENDRE LE CODE DU JEU") : itd_t("SET UP THE GAME", "PR\xc3\x89PARER LE JEU"),
                                 g_itd_btn, 14 * s, 0.08f, can_set_up, s)) {
                        pthread_mutex_lock(&g_lock);
                        g_busy = true;
                        pthread_mutex_unlock(&g_lock);
                        pthread_t t;
                        pthread_create(&t, NULL, extract_thread, NULL);
                        pthread_detach(t);
                    }
                }
                // what went wrong (the step's own message and what to do), or why the files on disk can't be played
                {
                    std::string what, todo;
                    if (failed && !job.empty()) what = itd_tr(job), todo = itd_tr(job_hint);
                    else if (!busy && !status.empty() && status != "No game files yet" && job.empty()) what = itd_tr(status), todo = itd_tr(status_hint(status));
                    if (!what.empty()) {
                        float ey = cy + ch + 18 * s, ew = 2 * cw + cg;
                        float th = g_itd_label->CalcTextSizeA(14 * s, FLT_MAX, ew - 64 * s, todo.c_str()).y;
                        ImVec2 a(X, ey), b(X + ew, ey + 62 * s + th);
                        dl->AddRectFilled(a, b, IM_COL32(120, 22, 18, 200), 16 * s);
                        dl->AddCircleFilled(ImVec2(a.x + 30 * s, a.y + 28 * s), 11 * s, IM_COL32(255, 92, 80, 255));
                        dl->AddRectFilled(ImVec2(a.x + 28.6f * s, a.y + 21 * s), ImVec2(a.x + 31.4f * s, a.y + 30 * s), ITD_WHITE(255), 1 * s);
                        dl->AddCircleFilled(ImVec2(a.x + 30 * s, a.y + 34 * s), 1.7f * s, ITD_WHITE(255));
                        dl->AddText(g_itd_btn, 15 * s, ImVec2(a.x + 52 * s, a.y + 19 * s), ITD_WHITE(255), what.c_str(), NULL, ew - 80 * s);
                        if (!todo.empty())
                            dl->AddText(g_itd_label, 14 * s, ImVec2(a.x + 52 * s, a.y + 44 * s), IM_COL32(255, 214, 208, 255), todo.c_str(), NULL, ew - 80 * s);
                    }
                }
                // back to the start screen (B / Esc too); it says the game files are needed
                if (itd_pill("back", ImVec2(X + 2 * cw + 2 * cg, cy + ch - 22 * s - 52 * s), ImVec2(140 * s, 52 * s), PILL_SOFT, ICON_NONE,
                             itd_t("\xe2\x80\xb9  BACK", "\xe2\x80\xb9  RETOUR"), g_itd_btn, 14 * s, 0.08f, !busy, s) ||
                    (!busy && !typing && (ImGui::IsKeyPressed(ImGuiKey_GamepadFaceRight, false) || ImGui::IsKeyPressed(ImGuiKey_Escape, false)))) {
                    settings_save();
                    page = MAIN;
                }
                itd_footer(dl, wp, ds, s, X);
                itd_music_control(dl, wp, ds, s);
                static bool test_busy = getenv("LARECOMP_LAUNCHER_SHOT") && (strstr(getenv("LARECOMP_LAUNCHER_SHOT"), ":extracting") ||
                                                                         strstr(getenv("LARECOMP_LAUNCHER_SHOT"), ":preparing") ||
                                                                         strstr(getenv("LARECOMP_LAUNCHER_SHOT"), ":skipask"));
                if (busy || test_busy) draw_busy_overlay(ds, job, test_busy);  // (everything dimmed, the progress in front)
            } else {
                // SETUP: the game files, in a window like Settings (option rows: runtime/ui/option_rows.h) - choose the
                // folder with the game files, set up; once set up, delete. Each row's explanation on the right.
                // IOS: the same window with the export rows (ios_export_rows).
                bool ios_page = page == IOS;
                float sc, w, h;
                ImVec2 centre;
                panel_layout(ds, &sc, &w, &h, &centre);
                ImGui::GetWindowDrawList()->AddRectFilled(ImGui::GetWindowPos(), ImVec2(ImGui::GetWindowPos().x + ds.x,
                                                          ImGui::GetWindowPos().y + ds.y),
                                                          IM_COL32(0, 0, 0, (int)(plastic_theme().dim * 255)));
                ImVec2 wp = ImGui::GetWindowPos();
                ImVec2 pa(wp.x + centre.x - w * 0.5f, wp.y + centre.y - h * 0.5f);
                pieces = true;
                page_panel(wp, ds, pa, w, h, sc);
                ImGui::SetCursorScreenPos(pa);
                ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0, 0, 0, 0));
                ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 14 * sc);
                ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(28 * sc, 24 * sc));
                ImGui::BeginChild("##setup", ImVec2(w, h), ImGuiChildFlags_AlwaysUseWindowPadding, ImGuiWindowFlags_NavFlattened);
                page_content_inset();
                ImGui::PopStyleVar(2);
                ImGui::PopStyleColor();
                menu_style_begin(sc);  // (the menu's colours: the rows' descriptions, the text under them)
                plastic_widgets(true);
                ImGui::PushFont(g_font_small);
                ImGui::PushFont(g_font_menu);
                ImGui::TextColored(plastic_col(plastic_theme().title), "%s", ios_page ? L("Recomp for iOS") : L("System"));
                ImGui::PopFont();
                bool back = false;
                if (!busy) {  // close: the round X in the corner, as Settings (B / Esc too)
                    float bx = 44 * sc;
                    back = plastic_close_corner(bx, sc);
                }
                ImGui::Spacing();

                pthread_mutex_lock(&g_lock);
                std::string src = g_src, job = g_job, job_hint = g_job_hint;
                bool failed = g_job_failed;
                pthread_mutex_unlock(&g_lock);
                bool ready = status.empty() && !busy;  // (set up: the window offers to delete them)
                static double files_gb = -1;
                // chosen: "Selected" with a green check (long paths ran over the label); the path is in the
                // explanation, and choosing again opens the picker again
                std::string src_v = src.empty() ? LN("Choose...") : LN("Selected");
                auto check_left_of_value = [&](const std::string &v) {
                    float vx, cy;
                    opt_last_value_pos(&vx, &cy);
                    if (v != "Selected" || vx <= 0) return;
                    float fs = ImGui::GetFontSize(), u = fs * 0.62f, x = vx - fs * 0.35f - u;
                    ImU32 g = IM_COL32(120, 220, 120, 255);
                    ImDrawList *dl = ImGui::GetWindowDrawList();
                    dl->AddLine(ImVec2(x, cy), ImVec2(x + u * 0.35f, cy + u * 0.35f), g, fs * 0.11f);
                    dl->AddLine(ImVec2(x + u * 0.35f, cy + u * 0.35f), ImVec2(x + u, cy - u * 0.4f), g, fs * 0.11f);
                };
                // (the descriptions put together here in the menus' language: the row finds no English for them)
                std::string src_d = L("Pick the folder with your extracted exefs and romfs from The Legend of Zelda: Link's "
                                      "Awakening for Switch. LAS Recomp doesn't include the game.");
                if (!src.empty()) src_d += std::string("\n\n") + L("Chosen:") + " " + folder_shown(src.c_str());
                // the step that's running / its result, under the rows and in the Set up row's explanation
                const std::string &job_line = job;
                std::string ex_d = L("Copies the files LAS Recomp needs and recompiles the game from them. You only do this "
                                     "once, and it takes a while.");
                if (!job.empty()) ex_d += "\n\n" + shown(job) + (job_hint.empty() ? "" : ".\n" + shown(job_hint));

                // the controller's cursor starts on the first row (not the X), a few frames while the window lays out
                static int seen_frame = -10, focus_first;  // (not drawn last frame: the window has just opened)
                if (seen_frame != ImGui::GetFrameCount() - 1) focus_first = 4;
                seen_frame = ImGui::GetFrameCount();
                auto first_row = [&] {
                    if (focus_first > 0 && focus_first--) ImGui::SetFocusID(ImGui::GetItemID(), ImGui::GetCurrentWindow());
                };
                opt_begin(ios_page ? "##ios" : "##files", sc, !ios_page && !ready);  // (before the setup: the example folder on the right)
#if LARECOMP_IOSEXPORT
                if (ios_page) {
                    ios_export_rows(ready, busy, win);
                    first_row();
                } else
#endif
                if (!ready) {
#ifdef __ANDROID__
                    storage_rows(busy);
#endif
                    opt_section("Your copy of the game");
                    if (busy) opt_disable_next();
                    if (opt_action("Game files folder", src_v.c_str(), src_d.c_str(), true))
                        show_folder_dialog(picked, NULL, win);
                    check_left_of_value(src_v);
                    first_row();
                    opt_section("Set up");
                    if (busy || (src.empty() && !check_copied_files(g_root).empty())) opt_disable_next();
                    bool resume = !busy && src.empty() && resumable_build();
                    if (opt_action(resume ? LN("Resume setup") : LN("Set up game files"), busy ? LN("Working...") : "", ex_d.c_str(), true)) {
                        pthread_mutex_lock(&g_lock);
                        g_busy = true;
                        pthread_mutex_unlock(&g_lock);
                        pthread_t t;
                        pthread_create(&t, NULL, extract_thread, NULL);
                        pthread_detach(t);
                    }
                    files_gb = -1;  // (re-measured after a setup)
                } else {
                    // DELETE: frees LAS Recomp's copy (runtime/paths.c removes only what the setup made, only inside
                    // the data root). Saves, seeds, settings and the user's own files stay.
                    if (files_gb < 0) files_gb = rt_game_files_size(g_root.c_str()) / 1e9;
                    char gb[32];
                    snprintf(gb, sizeof gb, "%.1f GB", files_gb);
                    opt_section("Game files");
                    if (!g_confirm_delete) {
                        if (opt_action("Delete game files", gb, "Frees up the space LAS Recomp's copy of the game files "
                                       "uses. Your saves, seeds and settings stay.", true))
                            g_confirm_delete = true;
                        first_row();
                    } else {
                        if (opt_action("Yes, delete them", gb, "Deletes the extracted game files now.", true)) {
                            stop_background_prep();
                            bool ok = rt_remove_game_files(g_root.c_str());
                            game_files_changed();
                            status = check_game_files(g_root);
                            if (ok) set_job(LN("Game files deleted."));
                            else set_job(LN("Couldn't delete the game files. They aren't in LAS Recomp's data folder."), true);
                            if (!status.empty()) g_bg = 0;
                            g_confirm_delete = false, files_gb = -1;
                        }
                        if (opt_action("Cancel", "", "Keeps the game files.", true)) g_confirm_delete = false;
                    }
                }
#ifdef __ANDROID__
                if (ready)  // (before the setup it's the first row: chosen before the game files are copied)
#endif
                if (!ios_page) storage_rows(busy);  // where all of it is kept: another folder (a bigger drive)
                if (!ios_page && !PLAT_IOS && !busy && has_build_cache()) {
                    opt_section("Build cache");
                    if (opt_action("Clear build cache", "", "Deletes the built game code. It gets rebuilt the next time you "
                                   "set up.", true)) {  // (the iOS export's build too)
                        char why[512] = "";
                        g_build_cache_changed = true;
#if LARECOMP_IOSEXPORT
                        char ios_build[1100];
                        iosexport_build_dir(ios_build, sizeof ios_build);
                        plat_remove_tree(ios_build);
#endif
                        if (build_cache_clear(g_root.c_str(), why, sizeof why)) set_job(LN("Build cache cleared."));
                        else set_job(LN("Couldn't clear the build cache."), true, why);
                    }
                }
                if (!job_line.empty()) {  // the current step / result
                    ImGui::Dummy(ImVec2(0, 14 * sc));
                    ImGui::PushTextWrapPos(0);
                    const PlasticTheme &pt = plastic_theme();
                    ImGui::TextColored(failed ? plastic_col(pt.warn) : busy ? plastic_col(pt.title) : plastic_col(pt.section), "%s", shown(job_line).c_str());
                    if (failed && !job_hint.empty())  // (what to do next, in the rows' own text colour)
                        ImGui::TextColored(plastic_col(pt.text), "%s", shown(job_hint).c_str());
                    ImGui::PopTextWrapPos();
                } else if (!busy && !status.empty() && status != "No game files yet") {
                    // (no setup run yet this session: why the game files on disk can't be played - an incomplete copy
                    // opens this window instead of crashing on Play)
                    ImGui::Dummy(ImVec2(0, 14 * sc));
                    ImGui::PushTextWrapPos(0);
                    ImGui::TextColored(plastic_col(plastic_theme().warn), "%s", shown(status).c_str());
                    ImGui::TextColored(plastic_col(plastic_theme().text), "%s", status_hint(status));
                    ImGui::PopTextWrapPos();
                } else if (ready && files_changed(g_root)) {  // (playable, but a damaged or changed file: #391)
                    ImGui::Dummy(ImVec2(0, 14 * sc));
                    ImGui::PushTextWrapPos(0);
                    ImGui::TextColored(ImVec4(1, 0.72f, 0.4f, 1), "%s", L(CHANGED_HEAD));
                    ImGui::TextColored(ImVec4(0.86f, 0.85f, 0.80f, 1), "%s", L(CHANGED_HINT));
                    ImGui::PopTextWrapPos();
                }
                if (!ready && g_onedrive > 0 && !g_settings.onedrive_kept) {  // (before the copy is the time to move)
                    ImGui::Dummy(ImVec2(0, 14 * sc));
                    int r = onedrive_notice(1.5f * sc, ImGui::GetContentRegionAvail().x, false);
                    if (r == 1) onedrive_keep();
                    else if (r == 2) choose_data_folder();
                }
                if (ready) opt_end();
                else {  // before the setup: under the description, what the folder to choose looks like, and the hint
                    opt_split();
                    opt_desc_text();
                    float fs = ImGui::GetFontSize(), ew = fminf(ImGui::GetContentRegionAvail().x, fs * 15);
                    ImGui::Dummy(ImVec2(0, fs * 0.5f));  // (a paragraph's space)
                    draw_folder_example(ew);
                    ImGui::Dummy(ImVec2(0, fs * 0.3f));  // (its shadow)
                    ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + ew);
                    ImGui::TextColored(plastic_col(plastic_theme().text_dim), "%s", L(PICK_FOLDER_HINT));
                    ImGui::PopTextWrapPos();
                    opt_split_end();
                }
                if (g_confirm_delete && (ImGui::IsKeyPressed(ImGuiKey_GamepadFaceRight, false) ||
                                         ImGui::IsKeyPressed(ImGuiKey_Escape, false)))
                    g_confirm_delete = false;  // (B / Esc: back out of the question first)
                else if (!busy && !typing && g_df == DF_NONE && (ImGui::IsKeyPressed(ImGuiKey_GamepadFaceRight, false) ||
                                                                 ImGui::IsKeyPressed(ImGuiKey_Escape, false)))
                    back = true;
                ImGui::PopFont();
                plastic_widgets(false);
                menu_style_end();
                ImGui::EndChild();
                if (back && !busy) {
                    settings_save();
                    g_confirm_delete = false;
                    g_refocus = ios_page ? SETTINGS : SETUP;  // (the export page opens from Settings > Prelaunch)
                    page = MAIN;
                }
                static bool test_busy = getenv("LARECOMP_LAUNCHER_SHOT") && (strstr(getenv("LARECOMP_LAUNCHER_SHOT"), ":extracting") ||
                                                                         strstr(getenv("LARECOMP_LAUNCHER_SHOT"), ":preparing") ||
                                                                         strstr(getenv("LARECOMP_LAUNCHER_SHOT"), ":skipask") ||
                                                                         strstr(getenv("LARECOMP_LAUNCHER_SHOT"), ":recompiling"));
                if (busy || test_busy) draw_busy_overlay(ds, job, test_busy);  // (everything dimmed, a bar in front)

            }

            // (status and version: Inside The Dream draws its own, the game files card and itd_footer; no update link -
            //  LAS Recomp's releases would replace this mod)
            (void)safe_b;
            ImGui::EndDisabled();
            {   // (another data folder picked: move / start fresh, then restart; in the menus' look from every page -
                // Settings, the System window at the first setup)
                menu_style_begin(ds.y / 1080.0f), plastic_widgets(true), pieces = true;
                data_folder_popup(s);
                notice_popup(s);
                plastic_widgets(false), menu_style_end();
            }
            ImGui::End();
            ImGui::PopStyleColor();
            if (waiting) draw_busy_overlay(ds, "", false, true);
            bool osk_up = osk_is_open();  // (a text field activated with the controller: as the menu's keyboard)
            {   // (phones: the in-game menu's size for it - 1 at 1080 lines, the enlargement as panel_layout's)
                float osc = PLAT_TOUCH ? ds.y / 1080.0f * (plat_touch_tablet() ? 1.0f : 1.35f) : s * 0.7f;
                if (osk_up) menu_style_begin(osc), plastic_widgets(true), pieces = true;
                osk_draw(g_font_menu, osc);
                if (osk_up) plastic_widgets(false), menu_style_end();
            }
            typed_before = ui_text_busy();
            menu_sfx_frame(frame_page != MAIN);

            ImGui::Render();
#if PLAT_TOUCH
            {   // (after Render: its EndFrame starts the keyboard through the backend; the start screen draws in points)
                ImVec2 mn, mx;
                if (ui_text_field_rect(&mn, &mx)) {
                    SDL_Rect r = {(int)mn.x, (int)mn.y, (int)(mx.x - mn.x), (int)(mx.y - mn.y)};
                    SDL_SetTextInputArea(win, &r, 0);
                }
            }
#endif
#ifdef UI_METAL
            static const float no_uv[4] = {0, 0, 1, 1};
            if (pieces) plastic_frame_begin(dev, cb, drawable.texture, nil, no_uv, io.MousePos, ds.y / 1080.0f);
            plastic_set_encoder(pieces ? enc : nil);
            ImGui_ImplMetal_RenderDrawData(ImGui::GetDrawData(), cb, enc);
            plastic_set_encoder(nil);
            [enc endEncoding];
            id<MTLTexture> shot = nil;
            if (shot_at && ++frame == shot_at) {
                MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:drawable.texture.pixelFormat
                                             width:drawable.texture.width height:drawable.texture.height mipmapped:NO];
                td.storageMode = MTLStorageModeShared;
                shot = [dev newTextureWithDescriptor:td];
                id<MTLBlitCommandEncoder> bl = [cb blitCommandEncoder];
                [bl copyFromTexture:drawable.texture toTexture:shot];
                [bl endEncoding];
            }
            [cb presentDrawable:drawable];
            [cb commit];
            if (shot) {
                [cb waitUntilCompleted];
                save_png(shot, getenv("LARECOMP_SHOT_PATH") ? getenv("LARECOMP_SHOT_PATH") : "build/launcher.png");
                exit(0);
            }
#else
            static const float no_uv[4] = {0, 0, 1, 1};
            if (pieces) {  // (before the drain below)
                int pw, ph;
                SDL_GetWindowSizeInPixels(win, &pw, &ph);
                plastic_frame_begin_vk(NULL, no_uv, (float)pw, (float)ph, io.MousePos, ds.y / 1080.0f);
            }
            ImGui_ImplVulkan_RenderDrawData(ImGui::GetDrawData(), (VkCommandBuffer)vk_frame_cmd());
            bool shot = shot_at && ++frame == shot_at;
            vk_frame_end(shot ? getenv("LARECOMP_SHOT_PATH") ? getenv("LARECOMP_SHOT_PATH") : "build/launcher.png" : NULL);
            if (shot) exit(0);
#endif
        }
    }
    itd_music_stop();
    // play: remember the seed (the RANDOMIZER row starts on the last one played), hand the window to the game
    snprintf(g_settings.rando_seed, sizeof g_settings.rando_seed, "%s", start_seed.c_str());
    if (!start_seed.empty()) snprintf(g_settings.rando_last, sizeof g_settings.rando_last, "%s", start_seed.c_str());
    settings_save();
#ifdef UI_METAL
    ImGui_ImplMetal_Shutdown();
#else
    vk_imgui_backend_shutdown();
#endif
    ImGui_ImplSDL3_Shutdown();
    ImGui::DestroyContext(ctx);
    ImGui::SetCurrentContext(prev_ctx);
    g_bg = 0;
    g_mark = 0;  // (gone with the backend: made again on the next start screen)
    g_start_screen = 0;
}
