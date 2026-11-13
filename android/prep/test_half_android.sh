#!/bin/bash
# Build the standalone arithmetic probe; --run executes it on an idle Quest.
set -euo pipefail
cd "$(git -C "$(dirname "$0")" rev-parse --show-toplevel)"
source android/prep/env.sh
out=build/test_vk_half
mkdir -p "$out"
inc=(-Iruntime -Iruntime/nvn -Ibuild/deps-vk/include -Ibuild/android/src/shaderc/libshaderc/include)
for src in tools/test_vk_half.c runtime/nvn/vulkan/shaders.c runtime/nvn/maxwell/translate.c; do
    obj=$out/$(basename "$src").o
    "$CC" -O2 -Wno-unused-function "${inc[@]}" -c "$src" -o "$obj"
done
"$CC" -O2 -Ibuild/inc -Ithird_party/smaa/Textures -c runtime/nvn/smaa_data.c -o "$out/smaa_data.o"
"$CXX" "$out/test_vk_half.c.o" "$out/shaders.c.o" "$out/translate.c.o" "$out/smaa_data.o" \
    build/android/shaderc-build/libshaderc/libshaderc_combined.a \
    -lvulkan -llog -landroid -ldl -lm -static-libstdc++ -o "$out/test_vk_half"
"$TC/llvm-strip" "$out/test_vk_half"
if [ "${1:-}" = --run ]; then
    serial=${ANDROID_SERIAL:?set ANDROID_SERIAL}
    if adb -s "$serial" shell pidof com.lasrecomp.quest >/dev/null; then
        printf 'Close LASRecomp before running the standalone GPU probe.\n' >&2
        exit 2
    fi
    trap 'adb -s "$serial" shell rm -f /data/local/tmp/lasrecomp-test-vk-half >/dev/null 2>&1 || true' EXIT
    adb -s "$serial" push "$out/test_vk_half" /data/local/tmp/lasrecomp-test-vk-half >/dev/null
    adb -s "$serial" shell chmod 755 /data/local/tmp/lasrecomp-test-vk-half
    adb -s "$serial" shell env LARECOMP_VK_NO_SPIRV_CACHE=1 /data/local/tmp/lasrecomp-test-vk-half 2>&1 | tee "$out/quest-probe.log"
fi
