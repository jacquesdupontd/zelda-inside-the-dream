#!/bin/bash
# build_rt_android.sh: the LASRecomp runtime (Vulkan backend) compiled for the Quest 3 (aarch64-linux-android32) into
# objects under build/android/obj - step 1 of A-FAIRE/lasrecomp-quest3-appsw.md. The Linux platform code serves
# (Android is Linux); SDL3 from build/android/prefix; main() renamed lasrecomp_main (the OpenXR app calls it).
cd "$(git -C "$(dirname "$0")" rev-parse --show-toplevel)"  # (the tree this script is in: worktrees too)
source android/prep/env.sh || exit 1
python3 tools/embed_sgsr2.py || exit 1
O=build/android/obj
INC="-Iruntime -Iruntime/nvn -Ibuild/android/prefix/include -I$VK_HEADERS -Ibuild/deps-vk/include \
     -Ibuild/inc -Ithird_party/smaa/Textures -Ithird_party/imgui -Ithird_party/imgui/backends \
     -Ibuild/android/src/shaderc/libshaderc/include"
CFLAGS="-O2 -g -fPIC -fno-strict-aliasing -Wno-format -Wno-unused-function $INC"
mkdir -p $O
fail=0; ok=0
ID=$(cat build/lasgame.dylib.id)  # (the game code's id: the runtime refuses a library built from other generated code)
compile() {  # src lang - skipped when its object is newer (FORCE=1: everything)
    local src=$1 out=$O/$(echo $1 | tr / _).o extra=$3
    case $src in
        runtime/gamelib.c|runtime/gamebuild.c) extra="$extra -DGAMELIB_ID=\"$ID\"" ;;
        *)
            if [ -z "$FORCE" ] && [ "$out" -nt "$src" ]; then
                if [ "$src" != runtime/nvn/vulkan/shaders.c ] || { [ "$out" -nt third_party/sgsr2/shaders.inc ] && [ "$out" -nt runtime/nvn/vulkan/xr_depth_ray.h ]; }; then return; fi
            fi ;;
    esac
    case $src in runtime/nvn/vulkan/*|third_party/imgui/backends/imgui_impl_vulkan.cpp) extra="$extra -include runtime/nvn/vulkan/vk_dispatch.h";; esac  # (every Vulkan command through a pointer: tools/gen_vk_dispatch.py)
    if [ "$2" = c ]; then $CC -std=gnu11 $CFLAGS $extra -c $src -o $out 2>> $O/errors.txt || { echo "FAILED $src" >> $O/errors.txt; fail=$((fail+1)); return; }
    else $CXX -std=c++17 $CFLAGS $extra -c $src -o $out 2>> $O/errors.txt || { echo "FAILED $src" >> $O/errors.txt; fail=$((fail+1)); return; }; fi
    ok=$((ok+1))
}
: > $O/errors.txt
for f in runtime/*.c runtime/hle/*.c runtime/services/*.c runtime/nvn/*.c runtime/nvn/maxwell/*.c runtime/nvn/vulkan/*.c runtime/platform/linux.c; do
    [ "$f" = runtime/hle/movie.c ] && continue
    extra=""; [ "$f" = runtime/main.c ] && extra="-Dmain=lasrecomp_main"
    compile $f c "$extra"
done
for f in runtime/*.cpp runtime/nvn/vulkan/*.cpp third_party/imgui/imgui.cpp third_party/imgui/imgui_draw.cpp third_party/imgui/imgui_tables.cpp \
         third_party/imgui/imgui_widgets.cpp third_party/imgui/backends/imgui_impl_sdl3.cpp third_party/imgui/backends/imgui_impl_vulkan.cpp; do
    compile $f cpp
done
for f in runtime/ui/*.mm; do [ $f = runtime/ui/launcher.mm ] && continue; compile $f cpp "-x c++"; done  # (no start screen: android/prep/stubs.c)
echo "compiled $ok, failed $fail"
grep -c "error:" $O/errors.txt || true
[ "$fail" -eq 0 ]
