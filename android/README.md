# LASRecomp on the Quest 3

A native Android app for the headset: the game runs exactly as on the desktop, through the Vulkan backend in its
offscreen mode, in first person (`LARECOMP_FP=1`). The view follows the head: each frame is rendered from the head's
pose over both eyes' fields (`fp_xr_view`) and shown as an OpenXR projection layer, which the compositor turns with
the head at the display's rate even while the game runs at 30. Both stick clicks together switch to a screen floating
2.2 m ahead (a quad layer) and back. The Touch controllers are the Switch pad, the sound goes out through AAudio.

One image serves both eyes: no depth yet. The goal is stereo (multiview), at 120 Hz with Application SpaceWarp (the
motion vectors already exist).

## What is where

| File | What |
|---|---|
| `xr_app.c` | the NativeActivity: OpenXR instance, session, quad layer, controllers; starts `lasrecomp_main` on its own thread |
| `aaudio_sdl.c` | the SDL audio stream calls the runtime makes, answered with AAudio (SDL's Android audio needs Java) |
| `AndroidManifest.xml` | NativeActivity, the OpenXR loader's permissions and queries, the 90 / 120 Hz features |
| `build_apk.sh` | links the runtime + shell into `liblasrecomp.so`, the game code into `liblasgame.so`, packs and signs the APK |
| `prep/` | the prerequisites, built once into `build/android/` |
| `../runtime/nvn/vulkan/vk_xr.h` | the hooks the shell plugs into the Vulkan backend (instance and device through OpenXR, each finished frame) |

## Building (Linux or macOS, Android SDK, NDK 27.2, build-tools 34)

The scripts in `prep/` build the Android dependencies once into the ignored `build/android/` directory; point them at
your own NDK (an environment file that selects the NDK compiler is not included). The Android build is not part of this
release.

With those prerequisites already present:

```sh
FORCE=1 bash android/prep/build_rt_android.sh
bash android/prep/movie_android.sh
ANDROID_KEYSTORE=/path/outside/git/debug.keystore bash android/build_apk.sh
```

Use the signing key matching the installed application to preserve its data with `adb install -r`.
`FORCE=1` is required after a header change: the historical runtime script only checks each source file's timestamp.
For an isolated headset benchmark, set `ANDROID_SERIAL` and run `android/bench_quest.sh label seconds`, followed by
optional `LARECOMP_NAME=value` arguments. It copies the save files and keeps raw runtime/compositor logs and the
effective options in `build/quest/label/`. Every completed label is preserved rather than overwritten.

Once, into `build/android/`:
1. SDL3 for Android: cmake from an SDL3 source tree with the NDK toolchain (`-DANDROID_ABI=arm64-v8a
   -DANDROID_PLATFORM=android-32`), installed to `build/android/prefix`.
2. `prep/build_shaderc_android.sh`, `prep/build_ffmpeg_android.sh` (FFmpeg 7.0 reduced to VP9 + Vorbis in WebM: the
   game's movies).
3. `prep/build_rt_android.sh` (the runtime objects, `main` renamed `lasrecomp_main`), `prep/movie_android.sh`,
   `prep/build_game_android.sh` (the generated game code).
4. The OpenXR headers and the Khronos loader (`libopenxr_loader.so`, arm64) in `build/android/xr/include` and
   `build/android/xr/lib`.

Then `bash android/build_apk.sh` -> `build/android/lasrecomp-quest.apk`.

`prep/link_rt_android.sh` builds the same runtime as a plain executable instead, run over adb with
`LARECOMP_OFFSCREEN=1280x720` (a test bed with no headset display: `LARECOMP_WINDOW_SHOT` + `LARECOMP_SHOT_PATH` save
a frame).

## Installing

```
adb install -r build/android/lasrecomp-quest.apk
```

The game files are your own dump, in the app's folder `/sdcard/Android/data/com.lasrecomp.quest/files/`:
`extracted/exefs/`, `extracted/romfs.bin`, `extracted/romfs/region_common/movie/*.webm`, a font (`Arial.ttf`), and a
`save/` folder if you bring a save. Files copied there by `adb shell cp` get mode 660, which the app can't read:
`chmod -R a+rwX` them (`adb push` keeps the source's mode).

The log is `lasrecomp.log` in that folder. `env.txt` there (`KEY=VALUE` lines) sets runtime options without
rebuilding: scripted input, a warp, a shot.

## Controls

| Quest | Switch |
|---|---|
| A, B, X, Y | A, B, X, Y |
| triggers | ZL, ZR |
| grips | L, R |
| left menu button | + |
| left stick click | - |
| right stick click | right stick click |
| sticks | sticks (the left walks, the right turns the body) |
| both stick clicks together | head view / floating screen |

## Current quality preset (25/10/2026)

Immersive presentation is the default, with eye scale 1.5 (2640x2752 per eye on
the tested Quest 3), without foveation. This preset prioritizes image quality;
it does not maintain 30 fps in the tested outdoor scene. SMAA and more precise
depth reconstruction are enabled. Conservative row bounds accelerate the eye
search while preserving the result of the selected reconstruction method.
Experimental shader
structuring and descriptor bind filtering remain opt-in. See
[the measurements and limitations](QUEST-RESULTS-2026-10-25.md).

For image diagnostics and later measurements, see
[Pixels and the 60 fps objective](PIXELS-AND-60FPS-2026-10-25.md).
For measured AppSW presets, separate source/eye resolutions, frame interpolation,
and the experimental EXT synthesis path, see
[AppSW and interpolation](APPSW-AND-INTERPOLATION-2026-10-25.md).
`ANDROID_SERIAL=<Quest> bash android/quest_profile.sh appsw` applies the measured
fluidity preset; `quality` restores the high-resolution preset. A 60 fps median
in the reference scene does not mean a locked 60 fps across the game.
`appsw-dynamic` adapts internal resolution to scene cost. `sgsr2` enables the
experimental Qualcomm temporal reconstruction with camera motion, eight-phase
jitter and a wider filter kernel. See [dynamic resolution and SGSR2](SGSR2-AND-DYNAMIC-2026-10-25.md)
for measurements, GPU controls and limitations involving moving objects and UI.
SMAA edges use RG8 where supported (same two 8-bit channels);
`LARECOMP_SMAA_RGBA=1` selects the RGBA8 control.
`python3 tools/quest_metrics.py <runtime.log>` reports scoped frame-time percentiles
and retains startup, resize and hitch events separately.
`LARECOMP_XR_DEPTH_BOUNDS=0` disables bounds, `=1` uses 64x64 tiles and `=2`
(default) keeps independent rows. `LARECOMP_XR_WARP_REFINE=0` selects the original
12-step search with two refinements; `=1` (default) selects 24 steps with adaptive
refinement. Neither option changes the colour render resolution.

`LARECOMP_XR_WARP_COMPARE_AT=1250,1500` compares full and bounded reconstruction
from the same colour/depth frame for both eyes; optional
`LARECOMP_XR_WARP_COMPARE_PATH=<prefix>` saves the independent images.
`LARECOMP_XR_WARP_COMPARE_NEGATIVE=1` deliberately disables stereo shift on the
reference to test that the comparison detects differences.
`LARECOMP_XR_WARP_COMPARE_ORACLE=1` compares with a finer 48-step reference.
These diagnostics stall rendering and must be separated from performance runs.

Run `python3 tools/test_xr_depth_bounds.py` for the conservative-search property.
`ANDROID_SERIAL=<Quest> bash android/prep/test_half_android.sh --run` builds and
executes the separate GPU arithmetic probe; LASRecomp must be stopped first.
This probe passing does not qualify a shader optimization for the real game.

## Historical measurements (20/10/2026, different rendering preset)

- Title screen: 60 fps. Outside (Field), first-person: 30 fps, the game's own 30 fps mode; original view: 30 fps.
  CPU-bound: the main thread runs the game code (58 % of samples) and the command translation; the GPU waits.
- No `VK_EXT_external_memory_host` on the Adreno 740: vertex data is copied every frame (about 11 MB per frame
  outside, against under 1 MB on the Mac, which imports the guest heap).
- 70 swapchain formats offered; `R8G8B8A8_SRGB` is used, the game's image copied bit for bit (an UNORM swapchain
  washes the colors out: the compositor encodes it again).

For the texture/ghosting feedback after the SGSR2 trial, use
`ANDROID_SERIAL=<Quest> bash android/quest_profile.sh fidelity`.
This keeps immersive AppSW, uses SMAA + spatial FSR1, a higher input-resolution
floor and optional 4x texture anisotropy. It avoids SGSR2 history; the temporal
path remains experimental. See [texture fidelity fixes](TEXTURE-FIDELITY-2026-10-25.md)
for current-frame depth checks, UI composite framing, geometry recovery,
rejected controls and remaining performance/visual limitations.

See [AppSW surface consistency and audited gameplay measurements](APPSW-SURFACE-CONSISTENCY-2026-10-26.md) for the shared colour/motion depth solver, conservative bounds reuse, and per-frame scene/health qualification that excludes death/menu FPS.

Experimental 45→90 preparation: [profiles, frame-budget changes and mandatory gameplay-speed checks](FIDELITY90-PREPARATION-2026-10-26.md). `fidelity90` and `fidelity90-hq` are unqualified candidates; changing refresh rate does not establish correct simulation speed.

Native 90-Hz investigation remains in progress after reports of black flashes, stutter and aliasing: [measured history mismatches, rejected timing prototype and pending frame-ID/AA validation](FIDELITY90-NATIVE-2026-10-26.md). No 90-Hz user profile is qualified yet.
