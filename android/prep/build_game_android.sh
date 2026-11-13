#!/bin/bash
# build_game_android.sh: the generated game code (the Mac's generated/: aarch64, guest SIMD as inline assembly) compiled
# for the Quest and linked into build/android/lasgame.so (the runtime functions it calls: resolved at load time)
cd "$(git -C "$(dirname "$0")" rev-parse --show-toplevel)"  # (the tree this script is in: worktrees too)
NDK=$HOME/android-sdk/ndk/27.2.12479018
export CC="$NDK/toolchains/llvm/prebuilt/darwin-x86_64/bin/aarch64-linux-android32-clang"
export O=${GOBJ:-build/android/gobj}
# GAMEFLAGS: the optimisation (default -O1; tried 21/10: -O2 -fwrapv -march=armv8.2-a, inline atomics)
export GAMEFLAGS="${GAMEFLAGS:--O1}"
mkdir -p $O/main $O/sdk
cat > $O/one.sh <<'ONE'
src=$1; o=$O/$(basename $(dirname $src))/$(basename $src .c).o
[ -f $o -a $o -nt $src ] && exit 0
$CC $GAMEFLAGS -fno-strict-aliasing -Wno-return-type -Wno-unused-label -fvisibility=hidden -fPIC -Iruntime -c $src -o $o 2>> $O/errors.txt || echo "FAILED $src" >> $O/errors.txt
ONE
ls generated/main/*.c generated/sdk/*.c | xargs -P 9 -n 1 bash $O/one.sh
ID=$(cat build/lasgame.dylib.id)
$CC -O1 -fno-strict-aliasing -fvisibility=hidden -fPIC -Iruntime -DGAMELIB_ID="\"$ID\"" -c tools/gamelib_entry.c -o $O/gamelib_entry.o
$CC -shared -Wl,--unresolved-symbols=ignore-all -o $O/../lasgame${GOBJ:+_alt}.so $O/main/*.o $O/sdk/*.o $O/gamelib_entry.o
ls -la build/android/lasgame.so
echo GAME_DONE
