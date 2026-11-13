# Turnip under Meta's OpenXR runtime: the decisive test

**The question.** On a Snapdragon 8 Elite, this game runs at 60 images/s with Mesa's open driver (Turnip), and at 30
with Qualcomm's (mamcjr/LASRecompAndroid). The Quest's OpenXR runtime makes us use Qualcomm's. Turnip does run on a
Quest 3: github.com/Jbbrack03/Quest-3-Turnip-Driver, loaded by libadrenotools, with Eden and XEMU. But that has only
been shown in flat apps, never in a VR session.

`xr_turnip.c` is the smallest test that answers:
- Turnip is loaded by libadrenotools, then its `vkGetInstanceProcAddr` is handed to OpenXR (`XR_KHR_vulkan_enable2`);
- the app runs a session with one swapchain per eye, each eye cleared to its colour, for 20 s;
- the same test runs with the system driver as the baseline.

The open point is the swapchain images: the runtime creates them through OUR device, so with Turnip they must be
imported into a Turnip device.

## Run it

Run `bash android/turnip_test/qturnip.sh` on the Mac, with the headset on adb (`~/bin_q.sh`). It installs, pushes the
driver, runs the system driver then Turnip, and pulls logs, crash buffers and 5 s of the headset's view into
`build/quest/turnip_*`.

**Success** is all of these at once:
- `OpenXR: ... driver id 18 (Mesa Turnip)` ;
- frames with 0 failures ;
- the left eye red and the right eye blue in `turnip_turnip.mp4`.

## Build it

1. **libadrenotools** (once): `git clone https://github.com/bylaws/libadrenotools ~/turnip-quest/libadrenotools`, then
   `git submodule update --init`, then configure with the NDK's cmake:

   ```
   $HOME/android-sdk/cmake/3.22.1/bin/cmake -S . -B build -G Ninja \
     -DCMAKE_MAKE_PROGRAM=$HOME/android-sdk/cmake/3.22.1/bin/ninja \
     -DCMAKE_TOOLCHAIN_FILE=$NDK/build/cmake/android.toolchain.cmake \
     -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=android-32 -DCMAKE_BUILD_TYPE=Release
   ```

   then `ninja -C build`.
2. **The driver**: `Turnip_Quest3_XEMU_2026.07b.adpkg.zip` from that repository, unzipped in
   `~/turnip-quest/xemu07b/`. Checked on 21/10/2026:
   - package SHA-256 `a4797cfa70ff4d65b4b5e43b209ebc212195bef30ba40494cb1f10e7644a278b` ;
   - `vulkan.ad07xx.so` SHA-256 `096f6b3d191d385ee42f970c315189b59675b80e9d3727b6ba22008252e6aca0` (as published).

   It also brings `VK_EXT_external_memory_host` on KGSL's coherent memory: the host import the Qualcomm driver lacks,
   which our page cache and vertex copies work around.
3. **The app**: `bash android/turnip_test/build_turnip_test.sh` → `build/android/turnip-test.apk`.

## If it works

This is the consultant's order (`RAPPORT-CONSULTANT-QUEST-60FPS.md`, §3), then measure:
1. route every Vulkan call of the backend through that loader. Its calls are linked to the system's `libvulkan`
   today, and VMA's too;
2. import the guest heap with `VK_EXT_external_memory_host`, in steps up to the real size;
3. measure LASRecomp at the beach and indoors, against the Qualcomm build.
