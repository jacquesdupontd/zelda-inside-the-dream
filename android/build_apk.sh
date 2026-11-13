#!/bin/bash
# build_apk.sh: the Quest 3 app -> build/android/lasrecomp-quest.apk. Needs the Android build of the runtime and of the
# game code first (build/android/: build_rt_android.sh, build_game_android.sh, build_ffmpeg_android.sh, SDL3, shaderc),
# and the OpenXR headers + loader in build/android/xr (copied from an OpenXR SDK build).
# Install: adb install -r build/android/lasrecomp-quest.apk; the game files: android/README.md.
set -e
cd "$(git -C "$(dirname "$0")" rev-parse --show-toplevel)"  # (the tree this script is in: worktrees too)
source android/prep/env.sh
A=build/android
O=$A/obj
X=$A/xr
L=$A/apk/lib/arm64-v8a
GLUE=$NDK/sources/android/native_app_glue
AT=${ADRENOTOOLS:-build/libadrenotools}  # (shared local dependency, outside Git)
if [ ! -d "$AT" ]; then AT=$HOME/turnip-quest/libadrenotools; fi
INC="-Iruntime -Iruntime/nvn -I$A/prefix/include -I$VK_HEADERS -I$X/include -I$GLUE -I$AT/include"
rm -rf $A/apk && mkdir -p $L
$CC -std=gnu11 -O2 -g -fPIC -Wall -Wno-missing-braces $INC -c android/xr_app.c -o $A/xr_app.o
$CC -std=gnu11 -O2 -fPIC -I$GLUE -c $GLUE/android_native_app_glue.c -o $A/glue.o
$CC -std=gnu11 -O2 -fPIC -Wall $INC -c android/aaudio_sdl.c -o $A/aaudio_sdl.o
$CC -std=gnu11 -O2 -fPIC $INC -c android/prep/stubs.c -o $O/stubs.o
OBJS=$(ls $O/*.o | grep -v main_exe.o)
# the runtime + this shell (ANativeActivity_onCreate kept: the glue's entry point, called by NativeActivity; the
# static libraries' symbols stay inside, which their assembly needs - FFmpeg's NEON code isn't position-independent)
$CXX -shared -o $L/liblasrecomp.so -Wl,-soname,liblasrecomp.so -Wl,--exclude-libs,ALL -u ANativeActivity_onCreate $OBJS $A/xr_app.o $A/glue.o $A/aaudio_sdl.o \
    -L$A/prefix/lib -lSDL3 -L$A/shaderc-build/libshaderc -lshaderc_combined \
    -L$A/ffprefix/lib -lavformat -lavcodec -lswscale -lswresample -lavutil -lz \
    $AT/build/libadrenotools.a $AT/build/lib/linkernsbypass/liblinkernsbypass.a \
    -L$X/lib -lopenxr_loader -lvulkan -laaudio -llog -landroid -ldl -lm -static-libstdc++
# the game code, needing the runtime: an app's libraries aren't global, so its calls into the runtime resolve
# through this DT_NEEDED (the desktop's executable exports them instead)
$CC -shared -o $L/liblasgame.so -Wl,--unresolved-symbols=ignore-all -Wl,--no-as-needed ${GOBJ:-$A/gobj}/main/*.o ${GOBJ:-$A/gobj}/sdk/*.o \
    ${GOBJ:-$A/gobj}/gamelib_entry.o -L$L -llasrecomp
cp $A/prefix/lib/libSDL3.so $X/lib/libopenxr_loader.so $L/
# the game's fixed address range taken at the process's start (android/guestreserve.c, preloaded by wrap.sh: the
# manifest is debuggable for it), as on the tablet
$CC -std=gnu11 -O2 -fPIC -shared -o $L/libguestreserve.so android/guestreserve.c -llog
cp android/wrap.sh $L/wrap.sh && chmod 755 $L/wrap.sh
mkdir -p "$A/apk/assets/licenses"
cp third_party/sgsr2/NOTICE-BINARY.txt "$A/apk/assets/licenses/SGSR2.txt"
cp $AT/build/src/hook/libmain_hook.so $AT/build/src/hook/libhook_impl.so $AT/build/src/hook/libfile_redirect_hook.so \
   $AT/build/src/hook/libgsl_alloc_hook.so $L/  # (libadrenotools' hooks: Turnip, LARECOMP_XR_TURNIP)
rm -rf $A/sym && mkdir -p $A/sym && cp $L/liblasrecomp.so $L/liblasgame.so $A/sym/  # (unstripped: tools/samples.py)
for f in $L/*.so; do $TC/llvm-strip --strip-unneeded $f; done
$BT/aapt2 link -o $A/apk/base.apk --manifest android/AndroidManifest.xml -I $ANDROID_SDK_ROOT/platforms/android-34/android.jar \
    --min-sdk-version 32 --target-sdk-version 32
(cd $A/apk && zip -qr base.apk lib assets)
$BT/zipalign -p -f 4 $A/apk/base.apk $A/apk/aligned.apk
$BT/apksigner sign --ks "${ANDROID_KEYSTORE:-$HOME/.android/debug.keystore}" --ks-pass pass:android --key-pass pass:android \
    --out $A/lasrecomp-quest.apk $A/apk/aligned.apk
ls -la $A/lasrecomp-quest.apk
