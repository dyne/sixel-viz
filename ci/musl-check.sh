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

printf 'Checking libuuid headers, archive and runtime with the musl compiler\n'
if [[ -r $target/lib/libuuid.a ]]; then
  printf 'libuuid archive %s\n' "$target/lib/libuuid.a"
fi
uuid_status=0
"$CC" -static -I"$target/include" -L"$target/lib" -x c - -luuid -o "$work/uuid-probe" <<'C' || uuid_status=$?
#include <uuid/uuid.h>
int main(void) {
    uuid_t id;
    uuid_generate(id);
    return uuid_is_null(id);
}
C
if ((uuid_status == 0)); then
  verify_static "$work/uuid-probe"
  "$work/uuid-probe" || uuid_status=$?
fi
printf 'libuuid compile/link/run exit %d\n' "$uuid_status"

printf 'Checking Linux APIs used by the pinned xtd core\n'
header_status=0
printf '#include <execinfo.h>\n' |
  "$CC" -I"$target/include" -x c -fsyntax-only - || header_status=$?
printf 'Header execinfo.h exit %d\n' "$header_status"
backtrace_status=0
backtrace_archive="$target/lib/libexecinfo.a"
backtrace_flags=()
if [[ -f $backtrace_archive ]]; then backtrace_flags=(-lexecinfo); fi
"$CC" -static -L"$target/lib" -x c - "${backtrace_flags[@]}" -o "$work/backtrace-probe" <<'C' || backtrace_status=$?
extern int backtrace(void **, int);
int (*volatile symbol)(void **, int) = backtrace;
int main(void) { return symbol == 0; }
C
printf 'backtrace static symbol exit %d (libexecinfo lookup %s)\n' "$backtrace_status" "$backtrace_archive"
api_status=0
"$CC" -static -Werror=implicit-function-declaration -x c - -o "$work/posix-probe" <<'C' || api_status=$?
#define _GNU_SOURCE
#include <dlfcn.h>
#include <link.h>
#include <pthread.h>
static int callback(struct dl_phdr_info *info, size_t size, void *data) { return 1; }
int main(void) {
    cpu_set_t mask;
    Dl_info info;
    CPU_ZERO(&mask);
    return pthread_setname_np(pthread_self(), "si-probe") +
           pthread_setaffinity_np(pthread_self(), sizeof mask, &mask) +
           dladdr((void *) callback, &info) + dl_iterate_phdr(callback, 0);
}
C
printf 'pthread naming/affinity and dladdr/dl_iterate_phdr compile/link exit %d\n' "$api_status"

printf 'Full static SVG probe using only the musl target SDK\n'
svg_header="$target/include/librsvg-2.0/librsvg/rsvg.h"
svg_archive="$target/lib/librsvg-2.a"
if [[ -r $svg_header ]]; then
  printf 'FFmpeg SVG header present: %s\n' "$svg_header"
else
  printf 'FFmpeg SVG header missing: %s\n' "$svg_header"
fi
if [[ -r $svg_archive ]]; then
  printf 'librsvg archive present: %s\n' "$svg_archive"
else
  printf 'librsvg archive missing: %s\n' "$svg_archive"
fi
svg_link=0
cat > "$work/svg-probe.c" <<'C'
extern void rsvg_handle_new_from_data(void);
void (*volatile symbol)(void) = rsvg_handle_new_from_data;
int main(void) { return symbol == 0; }
C
svg_metadata=0
pkg-config --print-errors --static --exists librsvg-2.0 || svg_metadata=$?
if ((svg_metadata == 0)); then
  python3 - "$CC" "$work" "$target" <<'PY' || svg_link=$?
from pathlib import Path
import shlex
import subprocess
import sys

compiler, work, target = sys.argv[1:]
flags = shlex.split(subprocess.check_output(
    ["pkg-config", "--static", "--cflags", "--libs", "librsvg-2.0"], text=True,
))
subprocess.run([compiler, "-static", "-L" + target + "/lib", str(Path(work) / "svg-probe.c"),
                *flags, "-o", str(Path(work) / "svg-probe")], check=True)
PY
else
  "$CC" -static -L"$target/lib" "$work/svg-probe.c" -lrsvg-2 -o "$work/svg-probe" || svg_link=$?
fi
printf 'SVG static link exit %d, isolated pkg-config exit %d\n' "$svg_link" "$svg_metadata"
printf 'Checking metadata for known librsvg dependencies; link closure is unproven when librsvg is absent\n'
for module in glib-2.0 gio-2.0 gdk-pixbuf-2.0 cairo cairo-gobject cairo-png freetype2 \
  harfbuzz libxml-2.0 pangocairo pangoft2 gmodule-2.0 fontconfig pixman-1 fribidi libpng; do
  module_status=0
  pkg-config --print-errors --static --exists "$module" || module_status=$?
  printf 'librsvg dependency %s, target pkg-config exit %d\n' "$module" "$module_status"
done
svg_options=()
if [[ -r $svg_header ]] && ((svg_link == 0 && svg_metadata == 0)); then
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

build_xtd() {
  checkout_sdk https://github.com/gammasoft71/xtd.git 3ea373359f7a92885d8abd88d397fc23fa1cad7e "$work/xtd"
  git -C "$work/xtd" apply "$source_dir/ci/xtd-core.patch"
  if ((${#svg_options[@]} == 0)); then
    printf 'Reduced experiment uses xtd without optional ALSA and GSound audio\n'
    git -C "$work/xtd" apply --unidiff-zero "$source_dir/ci/xtd-musl-reduced.patch"
  fi
  cmake -S "$work/xtd" -B "$work/xtd-build" "${toolchain[@]}" \
    "-DCMAKE_FIND_ROOT_PATH=$target;$sysroot" "-DCMAKE_INSTALL_PREFIX=$work/xtd-sdk" \
    -DXTD_BUILD_CPP_STANDARD=20 -DXTD_BUILD_TOOLS=OFF -DXTD_INSTALL_EXAMPLES=OFF -DXTD_INSTALL_RESOURCES=OFF
  cmake --build "$work/xtd-build" --target xtd.core --parallel "$jobs" -- -k 0
  cmake --install "$work/xtd-build"
}

media_sdk=$work/media-sdk
build_libsixel() {
  checkout_sdk https://github.com/saitoha/libsixel.git 72ed7d6b4a2252a6245567908c32c4e8e8c226ea "$work/libsixel"
  mkdir "$work/libsixel-build"
  cd "$work/libsixel-build"
  CFLAGS="-O2 -I$target/include" LDFLAGS="-static -L$target/lib" \
    "$work/libsixel/configure" --host="$triple" --prefix="$media_sdk" --disable-shared --enable-static \
    --disable-img2sixel --disable-sixel2png --disable-python --without-libcurl --without-jpeg --without-png
  make -j "$jobs"
  make install
}

build_ffmpeg() {
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
    --enable-decoder=png,gif,h264 --enable-demuxer=png_pipe,gif,matroska "${svg_options[@]}" || {
      status=$?
      if [[ -r ffbuild/config.log ]]; then tail -n 100 ffbuild/config.log >&2; fi
      return "$status"
    }
  make -j "$jobs"
  make install
}

run_phase() {
  local phase=$1
  printf 'Starting SDK phase %s\n' "$phase"
  set +e
  ( set -e; "$phase" )
  phase_status=$?
  set -e
  printf 'SDK phase %s exit %d\n' "$phase" "$phase_status"
}

run_phase build_xtd
xtd_status=$phase_status
run_phase build_libsixel
sixel_status=$phase_status
run_phase build_ffmpeg
ffmpeg_status=$phase_status
if ((xtd_status != 0 || sixel_status != 0 || ffmpeg_status != 0)); then
  printf 'Static si build blocked by SDK phases: xtd=%d libsixel=%d FFmpeg=%d\n' \
    "$xtd_status" "$sixel_status" "$ffmpeg_status"
  exit 1
fi

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
