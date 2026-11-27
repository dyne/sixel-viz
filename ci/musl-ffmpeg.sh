#!/usr/bin/env bash
set -euo pipefail

work=${1:?Pass a fresh FFmpeg work directory}
prefix=${2:?Pass an FFmpeg SDK install prefix}
: "${CC:?Use the dyne/musl-action CC output}"
: "${CXX:?Use the dyne/musl-action CXX output}"
triple=$("$CC" -dumpmachine)
test "$triple" = x86_64-linux-musl
target="$(dirname "$(dirname "$(dirname "$CC")")")/$triple"
test -r "$target/include/librsvg-2.0/librsvg/rsvg.h"
test -r "$target/lib/librsvg-2.a"
test -r "$target/lib/libz.a"
unset CPATH C_INCLUDE_PATH CPLUS_INCLUDE_PATH LIBRARY_PATH LD_LIBRARY_PATH
export PKG_CONFIG_LIBDIR="$target/lib/pkgconfig" PKG_CONFIG_PATH='' PKG_CONFIG_SYSROOT_DIR=''
pkg-config --static --print-errors --modversion librsvg-2.0 zlib

mkdir "$work"
cd "$work"
archive=ffmpeg-7.1.5.tar.xz
curl -fsSL --max-time 120 "https://ffmpeg.org/releases/$archive" -o "$archive"
printf '%s  %s\n' de668509caf9e35e3cd162473441fdb29538c6d96ed080292b3cf9e6fc5d558f "$archive" | sha256sum --check
tar -xf "$archive"
mkdir build
cd build

decoders=aac,bmp,ffv1,gif,h264,librsvg,mjpeg,pcm_s16le,png,ppm,qoi,tiff,webp
demuxers=concat,gif,h264,image_bmp_pipe,image_jpeg_pipe,image_png_pipe,image_ppm_pipe
demuxers+=,image_qoi_pipe,image_svg_pipe,image_tiff_pipe,image_webp_pipe,matroska,mov,wav
# FFmpeg 7.1.5 spells its SVG include relative to the SDK's include root.
if ../ffmpeg-7.1.5/configure --prefix="$prefix" --cc="$CC" --cxx="$CXX" \
  --arch=x86_64 --target-os=linux --enable-cross-compile --disable-x86asm \
  --extra-cflags="-I$target/include" --extra-ldflags="-static -L$target/lib" --pkg-config-flags=--static \
  --disable-shared --enable-static --disable-autodetect --disable-programs --disable-doc \
  --disable-avdevice --disable-avfilter --disable-postproc --disable-network --disable-everything \
  --enable-zlib --enable-librsvg --enable-protocol=file --enable-parser=gif,h264,mjpeg,png \
  --enable-decoder="$decoders" --enable-demuxer="$demuxers"; then
  for component in LIBRSVG_DECODER GIF_PARSER IMAGE_PNG_PIPE_DEMUXER IMAGE_SVG_PIPE_DEMUXER; do
    grep -Fx "#define CONFIG_${component} 1" config_components.h
  done
else
  status=$?
  if [[ -r ffbuild/config.log ]]; then tail -n 100 ffbuild/config.log >&2; fi
  exit "$status"
fi
make -j "${SI_BUILD_JOBS:-2}"
make install
