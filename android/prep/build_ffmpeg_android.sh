#!/bin/bash
# build_ffmpeg_android.sh: FFmpeg 7.0 (the major version of the desktop build's headers) for the Quest, reduced to what
# the movie module plays: the game's movies are WebM, VP9 1920x1080 + Vorbis
set -e
cd ~/LASRecomp/build/android
NDK=$HOME/android-sdk/ndk/27.2.12479018
TC=$NDK/toolchains/llvm/prebuilt/darwin-x86_64/bin
P=$PWD/ffprefix
[ -d ffmpeg-7.0 ] || { curl -sL https://ffmpeg.org/releases/ffmpeg-7.0.tar.xz -o ffmpeg-7.0.tar.xz; tar xf ffmpeg-7.0.tar.xz; }
cd ffmpeg-7.0
./configure --prefix=$P --target-os=android --arch=aarch64 --cpu=armv8-a --enable-cross-compile \
  --cc=$TC/aarch64-linux-android34-clang --cxx=$TC/aarch64-linux-android34-clang++ --nm=$TC/llvm-nm --ar=$TC/llvm-ar \
  --ranlib=$TC/llvm-ranlib --strip=$TC/llvm-strip --enable-pic --enable-static --disable-shared \
  --disable-programs --disable-doc --disable-avdevice --disable-avfilter --disable-network --disable-everything \
  --enable-decoder=vp9,vp8,vorbis,opus --enable-demuxer=matroska --enable-parser=vp9,vp8,vorbis,opus \
  --enable-protocol=file --enable-swscale --enable-swresample > ../ffconf.log 2>&1
make -j10 > ../ffmake.log 2>&1
make install >> ../ffmake.log 2>&1
ls -la $P/lib
