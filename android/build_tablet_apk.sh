#!/bin/bash
# build_tablet_apk.sh: LAS Recomp for an Android tablet or phone, on its own screen (android/flat_app.c) ->
# build/android/lasrecomp-tablet.apk. The same runtime and game code as the Quest app (build_rt_android.sh,
# build_game_android.sh: build/android/obj and gobj), without OpenXR. Install: adb install -r; the game files into
# /sdcard/Android/data/com.lasrecomp.tablet/files/extracted (adb push), Arial.ttf beside them.
set -e
cd "$(git -C "$(dirname "$0")" rev-parse --show-toplevel)"
source android/prep/env.sh
SDK=$ANDROID_SDK_ROOT
VK=$VK_HEADERS
A=build/android
O=$A/obj
# (the movie module is built apart, against FFmpeg: without it the game's first movie crashed, 27/10)
[ $O/runtime_hle_movie.c.o -nt runtime/hle/movie.c ] || bash android/prep/movie_android.sh || exit 1
L=$A/apk-tablet/lib/arm64-v8a
GLUE=$NDK/sources/android/native_app_glue
AT=${ADRENOTOOLS:-build/libadrenotools}  # (libadrenotools: Turnip, android/flat_app.c load_vulkan)
if [ ! -d "$AT" ]; then AT=$HOME/turnip-quest/libadrenotools; fi
INC="-Iruntime -Iruntime/nvn -I$A/prefix/include -I$VK -I$GLUE -I$AT/include"
rm -rf $A/apk-tablet && mkdir -p $L
$CC -std=gnu11 -O2 -g -fPIC -Wall -Wno-missing-braces $INC -c android/flat_app.c -o $A/flat_app.o
$CC -std=gnu11 -O2 -fPIC -I$GLUE -c $GLUE/android_native_app_glue.c -o $A/glue.o
$CC -std=gnu11 -O2 -fPIC -Wall $INC -c android/aaudio_sdl.c -o $A/aaudio_sdl.o
$CC -std=gnu11 -O2 -fPIC $INC -c android/prep/stubs.c -o $O/stubs.o
OBJS=$(ls $O/*.o | grep -v main_exe.o)
$CXX -shared -o $L/liblasrecomp.so -Wl,-soname,liblasrecomp.so -Wl,--exclude-libs,ALL -u ANativeActivity_onCreate $OBJS \
    $A/flat_app.o $A/glue.o $A/aaudio_sdl.o \
    -L$A/prefix/lib -lSDL3 -L$A/shaderc-build/libshaderc -lshaderc_combined \
    -L$A/ffprefix/lib -lavformat -lavcodec -lswscale -lswresample -lavutil -lz \
    $AT/build/libadrenotools.a $AT/build/lib/linkernsbypass/liblinkernsbypass.a \
    -lvulkan -laaudio -llog -landroid -ldl -lm -static-libstdc++
$CC -shared -o $L/liblasgame.so -Wl,--unresolved-symbols=ignore-all -Wl,--no-as-needed ${GOBJ:-$A/gobj}/main/*.o ${GOBJ:-$A/gobj}/sdk/*.o \
    ${GOBJ:-$A/gobj}/gamelib_entry.o -L$L -llasrecomp
cp $A/prefix/lib/libSDL3.so $L/
cp $AT/build/src/hook/libmain_hook.so $AT/build/src/hook/libhook_impl.so $AT/build/src/hook/libfile_redirect_hook.so \
   $AT/build/src/hook/libgsl_alloc_hook.so $L/  # (libadrenotools' hooks)
$CC -std=gnu11 -O2 -fPIC -shared -o $L/libguestreserve.so android/guestreserve.c -llog  # (wrap.sh preloads it)
cp android/wrap.sh $L/wrap.sh && chmod 755 $L/wrap.sh
for f in $L/*.so; do $TC/llvm-strip --strip-unneeded $f; done
$BT/aapt2 link -o $A/apk-tablet/base.apk --manifest android/AndroidManifest-tablet.xml -I $SDK/platforms/android-34/android.jar \
    --min-sdk-version 30 --target-sdk-version 34
(cd $A/apk-tablet && zip -qr base.apk lib)
$BT/zipalign -p -f 4 $A/apk-tablet/base.apk $A/apk-tablet/aligned.apk
$BT/apksigner sign --ks "${ANDROID_KEYSTORE:-$HOME/.android/debug.keystore}" --ks-pass pass:android --key-pass pass:android \
    --out $A/lasrecomp-tablet.apk $A/apk-tablet/aligned.apk
ls -la $A/lasrecomp-tablet.apk
echo TABLET_APK_DONE
