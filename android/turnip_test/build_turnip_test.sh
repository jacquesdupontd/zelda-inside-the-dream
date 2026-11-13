#!/bin/bash
# android/turnip_test/build_turnip_test.sh: the Turnip-under-OpenXR test app -> build/android/turnip-test.apk
# (xr_turnip.c says what it tests). Needs libadrenotools built for arm64 (ADRENOTOOLS=<its tree>, built into build/
# with the NDK's cmake: see the README beside this script) and the OpenXR headers + loader in build/android/xr.
# On the headset: adb install -r build/android/turnip-test.apk; adb push <driver>.so
#   /sdcard/Android/data/com.lasrecomp.turniptest/files/turnip/vulkan.ad07xx.so ; start it ; read
#   /sdcard/Android/data/com.lasrecomp.turniptest/files/turnip_test.log (or logcat -s xrturnip).
set -e
cd "$(git -C "$(dirname "$0")" rev-parse --show-toplevel)"
NDK=$HOME/android-sdk/ndk/27.2.12479018
TC=$NDK/toolchains/llvm/prebuilt/darwin-x86_64/bin
BT=$HOME/android-sdk/build-tools/34.0.0
AT=${ADRENOTOOLS:-$HOME/turnip-quest/libadrenotools}
A=build/android
X=$A/xr
O=$A/turnip_test
L=$O/apk/lib/arm64-v8a
GLUE=$NDK/sources/android/native_app_glue
rm -rf $O && mkdir -p $L
CC=$TC/aarch64-linux-android32-clang
CXX=$TC/aarch64-linux-android32-clang++
INC="-I$X/include -I/opt/homebrew/opt/vulkan-headers/include -I$AT/include -I$GLUE"
$CC -std=gnu11 -O2 -g -fPIC -Wall -Wno-missing-braces $INC -c android/turnip_test/xr_turnip.c -o $O/xr_turnip.o
$CC -std=gnu11 -O2 -fPIC -I$GLUE -c $GLUE/android_native_app_glue.c -o $O/glue.o
$CXX -shared -o $L/libxrturnip.so -Wl,-soname,libxrturnip.so -u ANativeActivity_onCreate $O/xr_turnip.o $O/glue.o \
    $AT/build/libadrenotools.a $AT/build/lib/linkernsbypass/liblinkernsbypass.a \
    -L$X/lib -lopenxr_loader -landroid -llog -ldl -lm -static-libstdc++
cp $X/lib/libopenxr_loader.so $L/
cp $AT/build/src/hook/libmain_hook.so $AT/build/src/hook/libhook_impl.so $AT/build/src/hook/libfile_redirect_hook.so \
   $AT/build/src/hook/libgsl_alloc_hook.so $L/
$BT/aapt2 link -o $O/apk/base.apk --manifest android/turnip_test/AndroidManifest.xml -I $HOME/android-sdk/platforms/android-34/android.jar \
    --min-sdk-version 32 --target-sdk-version 32
(cd $O/apk && zip -qr base.apk lib)
$BT/zipalign -p -f 4 $O/apk/base.apk $O/apk/aligned.apk
$BT/apksigner sign --ks $HOME/.android/debug.keystore --ks-pass pass:android --key-pass pass:android \
    --out $A/turnip-test.apk $O/apk/aligned.apk
ls -la $A/turnip-test.apk
