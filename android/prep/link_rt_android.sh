#!/bin/bash
# link_rt_android.sh: the runtime objects (build_rt_android.sh) linked into a native executable for the Quest,
# build/android/lasrecomp (a test bed run over adb before the OpenXR app: LARECOMP_OFFSCREEN)
cd "$(git -C "$(dirname "$0")" rev-parse --show-toplevel)"  # (the tree this script is in: worktrees too)
NDK=$HOME/android-sdk/ndk/27.2.12479018
TC=$NDK/toolchains/llvm/prebuilt/darwin-x86_64/bin
CC="$TC/aarch64-linux-android34-clang"
CXX="$TC/aarch64-linux-android34-clang++"
O=build/android/obj
INC="-Iruntime -Iruntime/nvn -Ibuild/android/prefix/include -I/opt/homebrew/opt/vulkan-headers/include -Ibuild/deps-vk/include"
$CC -std=gnu11 -O2 -g -fPIC -fno-strict-aliasing -Wno-format $INC -c runtime/main.c -o $O/main_exe.o
$CC -std=gnu11 -O2 -fPIC $INC -c build/android/stubs.c -o $O/stubs.o
OBJS=$(ls $O/*.o | grep -v "runtime_main.c.o")
$CXX -o build/android/lasrecomp $OBJS -Wl,--export-dynamic -Lbuild/android/prefix/lib -lSDL3 \
    -Lbuild/android/shaderc-build/libshaderc -lshaderc_combined -Lbuild/android/ffprefix/lib -lavformat -lavcodec -lswscale -lswresample -lavutil -lz -lvulkan -llog -landroid -ldl -lm -static-libstdc++ 2>&1 | \
    grep -o "undefined symbol: [A-Za-z0-9_]*" | sort | uniq -c | sort -rn 
ls -la build/android/lasrecomp 2>/dev/null
