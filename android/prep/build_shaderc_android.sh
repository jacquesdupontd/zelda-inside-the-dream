#!/bin/bash
# build_shaderc_android.sh: shaderc (GLSL -> SPIR-V, what the Vulkan backend compiles its shaders with) for the Quest,
# as a static library: google/shaderc and the dependencies its own git-sync-deps pins (glslang, SPIRV-Tools, SPIRV-Headers)
set -e
cd ~/LASRecomp/build/android/src
[ -d shaderc ] || git clone --depth 1 https://github.com/google/shaderc.git
cd shaderc
[ -d third_party/glslang ] || python3 utils/git-sync-deps
NDK=$HOME/android-sdk/ndk/27.2.12479018
CM=$HOME/android-sdk/cmake/3.22.1/bin/cmake
$CM -S . -B ../../shaderc-build -G Ninja -DCMAKE_MAKE_PROGRAM=$HOME/android-sdk/cmake/3.22.1/bin/ninja \
    -DCMAKE_TOOLCHAIN_FILE=$NDK/build/cmake/android.toolchain.cmake -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=android-32 \
    -DANDROID_STL=c++_static -DCMAKE_BUILD_TYPE=Release -DSHADERC_SKIP_TESTS=ON -DSHADERC_SKIP_EXAMPLES=ON \
    -DSHADERC_SKIP_COPYRIGHT_CHECK=ON -DSPIRV_SKIP_EXECUTABLES=ON -DENABLE_GLSLANG_BINARIES=OFF
$CM --build ../../shaderc-build --target shaderc_combined -j8
ls -la ../../shaderc-build/libshaderc/libshaderc_combined.a
echo SHADERC_DONE
