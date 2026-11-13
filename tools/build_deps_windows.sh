#!/bin/bash
# make deps-windows (MSYS2 UCRT64 shell): what the shipped Windows build links against instead of MSYS2's packages,
# built from pinned sources into build/deps-win. MSYS2's FFmpeg is a GPLv3 build (x264, x265, xvid, ...); the
# release uses an LGPL-only FFmpeg with just what the game's movies need - WebM: VP9 video + Vorbis audio, plus
# VP8 / Opus / H.264 / AAC in WebM or MP4 (the same configuration as tools/build_deps_macos.sh) - and swscale.
# SDL3 (zlib licence) and the compiler runtime DLLs still come from MSYS2.
# Needs: the packages from tools/setup_windows.ps1 (clang, make, nasm). Skipped when its output exists
# (delete build/deps-win to redo). FFmpeg stores its configure line (with --prefix) in the DLLs, so it is configured
# for a neutral prefix and installed into build/deps-win with DESTDIR: no path of this machine ships.
set -euo pipefail
cd "$(dirname "$0")/.."
P=$PWD/build/deps-win
S=$P/src
mkdir -p "$S"
JOBS=$(nproc)
NEUTRAL=/opt/las-recomp-deps  # (FFmpeg's --prefix: never used at run time, the DLLs sit next to the exe)

fetch() {  # url sha256 file
    [ -f "$S/$3" ] || curl -fsSL -o "$S/$3.part" "$1" && { [ -f "$S/$3" ] || mv "$S/$3.part" "$S/$3"; }
    echo "$2  $S/$3" | sha256sum -c --quiet
}
unpack() { rm -rf "$S/$2" && mkdir -p "$S/$2" && tar -xf "$S/$1" -C "$S/$2" --strip-components 1; }

fetch https://ffmpeg.org/releases/ffmpeg-9.0.2.tar.xz \
      8c3850283eb25fa026482078a04051e0be17347b09ef81a0849bec15a96e002e ffmpeg-9.0.2.tar.xz

# The recorder (Inside The Dream, 28/10): the game pipes its frames into ffmpeg.exe, built here (LGPL, shipped next to
# the exe), as the Mac's recorder does with Homebrew's: ProRes 422 HQ and PCM (FFmpeg's own), H.264 / HEVC by the GPU
# through Media Foundation (Windows' own) and AMF (AMD: its headers, MIT, from MSYS2's amf-headers), in MOV; raw video
# and sound read from a pipe. A build without them (an older deps-win) is redone: the stamp says which one this is.
if [ ! -f "$P/lib/libavcodec.dll.a" ] || [ ! -f "$P/ffmpeg-recorder2.stamp" ]; then
    pacman -S --needed --noconfirm mingw-w64-ucrt-x86_64-amf-headers >/dev/null 2>&1 || echo "deps-win: no amf-headers (AMF left out)"
    AMF=$([ -f /ucrt64/include/AMF/core/Factory.h ] && echo "--enable-amf --enable-encoder=h264_amf,hevc_amf")
    unpack ffmpeg-9.0.2.tar.xz ffmpeg
    (cd "$S/ffmpeg" && ./configure --prefix="$NEUTRAL" --cc=clang --cxx=clang++ --arch="${MSYSTEM_CARCH:-x86_64}" --target-os=mingw32 \
        --enable-shared --disable-static \
        --disable-autodetect --disable-ffplay --disable-ffprobe --disable-doc --disable-network --disable-avdevice \
        --disable-everything \
        --enable-protocol=file,pipe --enable-demuxer=matroska,mov,rawvideo,pcm_f32le,pcm_s16le,wav \
        --enable-decoder=vp9,vp8,vorbis,opus,h264,aac,rawvideo,pcm_f32le,pcm_s16le --enable-parser=vp9,vp8,vorbis,opus,h264,aac \
        --enable-filter=scale,format,aformat,aresample,null,anull,buffer,buffersink,abuffer,abuffersink,setpts,asetpts \
        --enable-d3d11va --enable-mediafoundation --enable-encoder=prores_ks,aac,pcm_s16le,h264_mf,hevc_mf $AMF --enable-muxer=mov,mp4 \
        && make -j"$JOBS" && rm -rf "$S/ffmpeg-root" && make install DESTDIR="$S/ffmpeg-root") && touch "$P/ffmpeg-recorder2.stamp"
    cp -R "$S/ffmpeg-root$NEUTRAL"/. "$P"/
    sed -i "s|$NEUTRAL|$P|g" "$P"/lib/pkgconfig/lib{avformat,avcodec,avutil,swscale,avfilter,swresample}.pc  # (pkg-config: the real place)
fi
if [ "${CI:-}" != true ]; then  # (no path of this machine inside: FFmpeg's configure line; not on CI: no one's paths)
    for f in "$P"/bin/*.dll; do
        ! grep -qaiF -e "$PWD" -e "$(cygpath -m "$PWD")" -e "$(cygpath -w "$PWD")" "$f" || { echo "$f holds a path of this machine"; exit 1; }
    done
fi
# (the DLL's own license and configure line: avcodec_license(), avcodec_configuration(); its .pc file has neither)
grep -qa 'GPL version 3\|--enable-gpl' "$P"/bin/avcodec-*.dll && { echo "deps-win: FFmpeg is not LGPL"; exit 1; }
echo "deps-win: $(ls "$P/bin"/*.dll | xargs -n1 basename | tr '\n' ' ')"
