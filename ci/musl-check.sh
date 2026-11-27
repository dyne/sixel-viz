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
printf 'Work directory %s\nTarget SDK %s\nCompiler sysroot %s\n' "$work" "$target" "$sysroot"
"$CC" --version
git -C "$source_dir" rev-parse HEAD

unset CPATH C_INCLUDE_PATH CPLUS_INCLUDE_PATH LIBRARY_PATH LD_LIBRARY_PATH SI_CELL_SIZE
export PKG_CONFIG_LIBDIR="$target/lib/pkgconfig" PKG_CONFIG_PATH='' PKG_CONFIG_SYSROOT_DIR=''
for module in execinfo uuid alsa zlib librsvg-2.0; do
  printf '%s ' "$module"
  pkg-config --static --print-errors --modversion "$module"
done

xtd_sdk=$work/xtd-sdk
sixel_sdk=$work/libsixel-sdk
ffmpeg_modules=(libavformat libavcodec libavutil libswscale)
if pkg-config --static --exists "${ffmpeg_modules[@]}"; then
  ffmpeg_sdk=$target
  printf 'Using FFmpeg from the installed musl bundle\n'
else
  ffmpeg_sdk=$work/ffmpeg-sdk
  printf 'Bundle lacks FFmpeg metadata. Building the verified FFmpeg 7.1.5 source with bundled librsvg and zlib.\n'
  bash "$source_dir/ci/musl-ffmpeg.sh" "$work/ffmpeg" "$ffmpeg_sdk"
fi
for library in "${ffmpeg_modules[@]}"; do
  test -r "$ffmpeg_sdk/lib/$library.a"
done

static_link_flags="-static -L$target/lib"
toolchain=(
  -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_SYSTEM_NAME=Linux -DCMAKE_SYSTEM_PROCESSOR=x86_64
  "-DCMAKE_C_COMPILER=$CC" "-DCMAKE_CXX_COMPILER=$CXX" "-DCMAKE_SYSROOT=$sysroot"
  "-DCMAKE_C_FLAGS=-I$target/include" "-DCMAKE_CXX_FLAGS=-I$target/include"
  "-DCMAKE_EXE_LINKER_FLAGS=$static_link_flags"
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

checkout_sdk https://github.com/gammasoft71/xtd.git 3ea373359f7a92885d8abd88d397fc23fa1cad7e "$work/xtd"
git -C "$work/xtd" apply "$source_dir/ci/xtd-core.patch"
git -C "$work/xtd" apply "$source_dir/ci/xtd-musl.patch"
cmake -S "$work/xtd" -B "$work/xtd-build" "${toolchain[@]}" \
  "-DCMAKE_FIND_ROOT_PATH=$target;$sysroot" "-DCMAKE_INSTALL_PREFIX=$xtd_sdk" \
  -DXTD_BUILD_CPP_STANDARD=20 -DXTD_BUILD_TOOLS=OFF -DXTD_INSTALL_EXAMPLES=OFF -DXTD_INSTALL_RESOURCES=OFF
cmake --build "$work/xtd-build" --target xtd.core --parallel "$jobs"
cmake --install "$work/xtd-build"

checkout_sdk https://github.com/saitoha/libsixel.git 72ed7d6b4a2252a6245567908c32c4e8e8c226ea "$work/libsixel"
mkdir "$work/libsixel-build"
(
  cd "$work/libsixel-build"
  CFLAGS="-O2 -I$target/include" LDFLAGS="$static_link_flags" \
    "$work/libsixel/configure" --host="$triple" --prefix="$sixel_sdk" --disable-shared --enable-static \
    --disable-img2sixel --disable-sixel2png --disable-python --without-libcurl --without-jpeg --without-png
  make -j "$jobs"
  make install
)

export PKG_CONFIG_LIBDIR="$ffmpeg_sdk/lib/pkgconfig:$sixel_sdk/lib/pkgconfig:$target/lib/pkgconfig"
for module in "${ffmpeg_modules[@]}" libsixel; do
  printf '%s ' "$module"
  pkg-config --static --print-errors --modversion "$module"
done
# The LLVM unwinder used by librsvg needs the ELF frame table for C++ exceptions.
cmake -S "$source_dir" -B "$work/build" "${toolchain[@]}" -DPKG_CONFIG_ARGN=--static \
  "-DCMAKE_EXE_LINKER_FLAGS=$static_link_flags -Wl,--eh-frame-hdr" \
  "-DCMAKE_FIND_ROOT_PATH=$xtd_sdk;$ffmpeg_sdk;$sixel_sdk;$target;$sysroot" "-Dxtd_DIR=$xtd_sdk/cmake"
cmake --build "$work/build" --parallel "$jobs"
# The missing-SDK case runs its own CMake compiler check.
LDFLAGS="$static_link_flags" ctest --test-dir "$work/build" -C Release --output-on-failure --no-tests=error
ctest --test-dir "$work/build" -C Release -V --no-tests=error -R '^sdk\.decode\.(svg|gif)$'
cmake --install "$work/build" --prefix "$work/stage"
program=$work/stage/bin/si
readelf -W -h -l -d "$program" > "$work/si.elf.txt"
grep -F 'Advanced Micro Devices X86-64' "$work/si.elf.txt"
grep -E '^[[:blank:]]+GNU_EH_FRAME[[:blank:]]' "$work/si.elf.txt"
if grep -E 'INTERP|\(NEEDED\)' "$work/si.elf.txt"; then
  printf 'Expected a static x86-64 ELF: %s\n' "$program" >&2
  exit 1
fi
printf 'Static ELF has no interpreter or dynamic library dependencies: %s\n' "$program"
"$program" --help
python3 "$source_dir/tests/installed_media.py" --program "$program" --work-root "$work"
