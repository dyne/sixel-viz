#!/usr/bin/env bash
set -euo pipefail

root=${1:?Pass an external work directory}
work=$(mktemp -d "$root/si-musl.XXXXXX")
source_dir=$(cd -- "$(dirname -- "$0")/.." && pwd -P)
jobs=${SI_BUILD_JOBS:-2}
: "${CC:?Use the dyne/musl-action CC output}"
: "${CXX:?Use the dyne/musl-action CXX output}"
triple=$("$CC" -dumpmachine)
test "$triple" = x86_64-linux-musl
test "$("$CXX" -dumpmachine)" = "$triple"
sysroot=$("$CC" -print-sysroot)
target="$(dirname "$(dirname "$(dirname "$CC")")")/$triple"
test -d "$target/lib"
printf 'Work directory %s\nTarget SDK %s\nCompiler sysroot %s\n' "$work" "$target" "$sysroot"
"$CC" --version

unset CPATH C_INCLUDE_PATH CPLUS_INCLUDE_PATH LIBRARY_PATH LD_LIBRARY_PATH
export PKG_CONFIG_LIBDIR="$target/lib/pkgconfig" PKG_CONFIG_PATH='' PKG_CONFIG_SYSROOT_DIR=''

verify_static() {
  local binary=$1 elf="$1.elf.txt"
  readelf -W -h -l -d "$binary" > "$elf"
  grep -F 'Advanced Micro Devices X86-64' "$elf"
  if grep -E 'INTERP|\(NEEDED\)' "$elf"; then
    printf 'Expected a static x86-64 ELF: %s\n' "$binary" >&2
    return 1
  fi
  printf 'Static ELF has no interpreter or dynamic library dependencies: %s\n' "$binary"
}

"$CXX" -std=c++20 -static -x c++ - -o "$work/toolchain-probe" <<'CPP'
#include <string_view>
#ifdef __GLIBC__
#error glibc headers in the musl toolchain probe
#endif
int main() { return !std::string_view{"sixel"}.starts_with("si"); }
CPP
verify_static "$work/toolchain-probe"
"$work/toolchain-probe"

printf 'Full static SVG probe using only the musl target SDK\n'
svg_link=0
"$CC" -static -L"$target/lib" -x c - -lrsvg-2 -o "$work/svg-probe" <<'C' || svg_link=$?
extern void rsvg_handle_new_from_data(void);
void (*volatile symbol)(void) = rsvg_handle_new_from_data;
int main(void) { return symbol == 0; }
C
svg_metadata=0
pkg-config --print-errors --static --exists librsvg-2.0 || svg_metadata=$?
printf 'SVG static link exit %d, isolated pkg-config exit %d\n' "$svg_link" "$svg_metadata"
svg_options=()
if ((svg_link == 0 && svg_metadata == 0)); then
  svg_options=(--enable-librsvg --enable-demuxer=svg_pipe)
else
  printf 'Full static SVG is unavailable from this SDK. The remaining attempt is reduced and excludes SVG.\n'
fi

toolchain=(
  -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_SYSTEM_NAME=Linux -DCMAKE_SYSTEM_PROCESSOR=x86_64
  "-DCMAKE_C_COMPILER=$CC" "-DCMAKE_CXX_COMPILER=$CXX" "-DCMAKE_SYSROOT=$sysroot"
  "-DCMAKE_C_FLAGS=-I$target/include" "-DCMAKE_CXX_FLAGS=-I$target/include"
  "-DCMAKE_EXE_LINKER_FLAGS=-static -L$target/lib"
  -DCMAKE_FIND_ROOT_PATH_MODE_PROGRAM=NEVER -DCMAKE_FIND_ROOT_PATH_MODE_LIBRARY=ONLY
  -DCMAKE_FIND_ROOT_PATH_MODE_INCLUDE=ONLY -DCMAKE_FIND_ROOT_PATH_MODE_PACKAGE=ONLY
)
if command -v ccache >/dev/null; then
  toolchain+=(-DCMAKE_C_COMPILER_LAUNCHER=ccache -DCMAKE_CXX_COMPILER_LAUNCHER=ccache)
fi

checkout_sdk() {
  local url=$1 revision=$2 path=$3
  git init -q "$path"
  git -C "$path" fetch --depth=1 "$url" "$revision"
  git -C "$path" checkout --quiet --detach FETCH_HEAD
  test "$(git -C "$path" rev-parse HEAD)" = "$revision"
}

printf 'Building the pinned xtd core with the musl compiler\n'
checkout_sdk https://github.com/gammasoft71/xtd.git 3ea373359f7a92885d8abd88d397fc23fa1cad7e "$work/xtd"
git -C "$work/xtd" apply "$source_dir/ci/xtd-core.patch"
cmake -S "$work/xtd" -B "$work/xtd-build" "${toolchain[@]}" \
  "-DCMAKE_FIND_ROOT_PATH=$target;$sysroot" "-DCMAKE_INSTALL_PREFIX=$work/xtd-sdk" \
  -DXTD_BUILD_CPP_STANDARD=20 -DXTD_BUILD_TOOLS=OFF -DXTD_INSTALL_EXAMPLES=OFF -DXTD_INSTALL_RESOURCES=OFF
cmake --build "$work/xtd-build" --target xtd.core --parallel "$jobs"
cmake --install "$work/xtd-build"

media_sdk=$work/media-sdk
checkout_sdk https://github.com/saitoha/libsixel.git 72ed7d6b4a2252a6245567908c32c4e8e8c226ea "$work/libsixel"
mkdir "$work/libsixel-build"
(
  cd "$work/libsixel-build"
  CFLAGS="-O2 -I$target/include" LDFLAGS="-static -L$target/lib" \
    "$work/libsixel/configure" --host="$triple" --prefix="$media_sdk" --disable-shared --enable-static \
    --disable-img2sixel --disable-sixel2png --disable-python --without-libcurl --without-jpeg --without-png
  make -j "$jobs"
  make install
)

(
  cd "$work"
  archive=ffmpeg-7.1.5.tar.xz
  curl -fsSL --max-time 120 "https://ffmpeg.org/releases/$archive" -o "$archive"
  printf '%s  %s\n' de668509caf9e35e3cd162473441fdb29538c6d96ed080292b3cf9e6fc5d558f "$archive" | sha256sum --check
  tar -xf "$archive"
  mkdir ffmpeg-build
  cd ffmpeg-build
  ../ffmpeg-7.1.5/configure --prefix="$media_sdk" --cc="$CC" --cxx="$CXX" \
    --arch=x86_64 --target-os=linux --enable-cross-compile --disable-x86asm \
    --extra-cflags="-I$target/include" --extra-ldflags="-static -L$target/lib" --pkg-config-flags=--static \
    --disable-shared --enable-static --disable-autodetect --disable-programs --disable-doc \
    --disable-avdevice --disable-avfilter --disable-postproc --disable-network --disable-everything \
    --enable-zlib --enable-protocol=file --enable-parser=h264 \
    --enable-decoder=png,gif,h264 --enable-demuxer=png_pipe,gif,matroska "${svg_options[@]}"
  make -j "$jobs"
  make install
)

export PKG_CONFIG_LIBDIR="$media_sdk/lib/pkgconfig:$target/lib/pkgconfig"
cmake -S "$source_dir" -B "$work/build" "${toolchain[@]}" -DBUILD_TESTING=OFF -DPKG_CONFIG_ARGN=--static \
  "-DCMAKE_FIND_ROOT_PATH=$work/xtd-sdk;$media_sdk;$target;$sysroot" "-Dxtd_DIR=$work/xtd-sdk/cmake"
cmake --build "$work/build" --target si --parallel "$jobs"
cmake --install "$work/build" --prefix "$work/stage"
program=$work/stage/bin/si
verify_static "$program"
"$program" --help
if ((${#svg_options[@]} != 0)); then
  python3 "$source_dir/tests/installed_media.py" --program "$program" --work-root "$work"
else
  python3 - "$source_dir" "$program" "$work" <<'PY'
from pathlib import Path
import runpy
import sys

source, program, work = map(Path, sys.argv[1:])
checks = runpy.run_path(str(source / "tests/installed_media.py"))
checks["verify_file"](program, work, source / "tests/fixtures/alpha.png",
                      (8, 4), [[(1, 2, (255, 32, 64))]])
print("Reduced static si passed a PNG smoke test. The full SVG build remains unavailable.")
PY
fi
