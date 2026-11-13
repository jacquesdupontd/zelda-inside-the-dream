#!/bin/bash
# movie_android.sh: the movie module for the Quest, against the FFmpeg 7.0 built by build_ffmpeg_android.sh
cd "$(git -C "$(dirname "$0")" rev-parse --show-toplevel)"  # (the tree this script is in: worktrees too)
source android/prep/env.sh || exit 1
$CC -std=gnu11 -O2 -g -fPIC -fno-strict-aliasing -Wno-format -Iruntime -Iruntime/nvn -Ibuild/android/prefix/include \
    -Ibuild/android/ffprefix/include -c runtime/hle/movie.c -o build/android/obj/runtime_hle_movie.c.o
