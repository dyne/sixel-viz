#!/usr/bin/env bash
set -euo pipefail

root=${1:?Pass an external work directory}
asset=${2:-}
work=$(mktemp -d "$root/si-musl.XXXXXX")
source_dir=$(cd -- "$(dirname -- "$0")/.." && pwd -P)
jobs=${SI_BUILD_JOBS:-2}
: "${CC:?Use the dyne/musl-action CC output}"
: "${CXX:?Use the dyne/musl-action CXX output}"
triple=$("$CC" -dumpmachine)
test "$("$CXX" -dumpmachine)" = "$triple"
case "$triple" in
  x86_64-linux-musl) processor=x86_64; machine='Advanced Micro Devices X86-64'; emulator=qemu-x86_64 ;;
  aarch64-linux-musl) processor=aarch64; machine=AArch64; emulator=qemu-aarch64 ;;
  arm-linux-musleabihf) processor=arm; machine=ARM; emulator=qemu-arm ;;
  riscv64-linux-musl) processor=riscv64; machine=RISC-V; emulator=qemu-riscv64 ;;
  *) printf 'Unsupported musl target: %s\n' "$triple" >&2; exit 2 ;;
esac

runner=()
media_args=()
app_options=()
host_arch=$(uname -m)
if [[ $host_arch != "$processor" && ! ( $processor == arm && $host_arch == armv[78]l ) ]]; then
  emulator_path=$(command -v "$emulator") || {
    printf 'Install %s to run the cross-built viewer.\n' "$emulator" >&2
    exit 2
  }
  runner=("$emulator_path")
  media_args=(--emulator "$emulator_path")
  # Some CTest cases spawn target executables or exercise a native PTY.
  app_options+=(-DBUILD_TESTING=OFF)
fi

sysroot=$("$CC" -print-sysroot)
target="$(dirname "$(dirname "$(dirname "$CC")")")/$triple"
if [[ $processor == arm ]]; then
  # The SDK defaults to ARMv5TE. The SIGINT handler needs lock-free atomics.
  app_options+=("-DCMAKE_CXX_FLAGS=-I$target/include -march=armv7-a -mfpu=vfpv3-d16")
  app_options+=(-DSI_USE_GCC_UNWINDER=ON)
fi
printf 'Work directory %s\nTarget SDK %s\nCompiler sysroot %s\n' "$work" "$target" "$sysroot"
"$CC" --version
git -C "$source_dir" rev-parse HEAD

unset CPATH C_INCLUDE_PATH CPLUS_INCLUDE_PATH LIBRARY_PATH LD_LIBRARY_PATH SI_CELL_SIZE
export PKG_CONFIG_LIBDIR="$target/lib/pkgconfig" PKG_CONFIG_PATH='' PKG_CONFIG_SYSROOT_DIR=''
ffmpeg_modules=(libavformat libavcodec libavutil libswscale)
for module in zlib librsvg-2.0 "${ffmpeg_modules[@]}"; do
  version=$(pkg-config --static --print-errors --modversion "$module")
  printf '%s %s\n' "$module" "$version"
done
for library in "${ffmpeg_modules[@]}"; do
  if [[ ! -r "$target/lib/$library.a" ]]; then
    printf 'Missing bundled FFmpeg archive: %s\n' "$target/lib/$library.a" >&2
    exit 1
  fi
done

sixel_sdk=$work/libsixel-sdk
static_link_flags="-static -L$target/lib"
toolchain=(
  -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_SYSTEM_NAME=Linux "-DCMAKE_SYSTEM_PROCESSOR=$processor"
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

checkout_sdk https://github.com/saitoha/libsixel.git 72ed7d6b4a2252a6245567908c32c4e8e8c226ea "$work/libsixel"
mkdir "$work/libsixel-build"
(
  cd "$work/libsixel-build"
  # The SDK's musl returns non-null for malloc(0) and realloc(NULL, 0).
  # Autoconf otherwise guesses no for these target runtime probes when crossing.
  ac_cv_func_malloc_0_nonnull=yes ac_cv_func_realloc_0_nonnull=yes \
  CFLAGS="-O2 -I$target/include" LDFLAGS="$static_link_flags" \
    "$work/libsixel/configure" --build="$(sh "$work/libsixel/config.guess")" --host="$triple" \
    --prefix="$sixel_sdk" --disable-shared --enable-static \
    --disable-img2sixel --disable-sixel2png --disable-python --without-libcurl --without-jpeg --without-png
  make -j "$jobs"
  make install
)

export PKG_CONFIG_LIBDIR="$sixel_sdk/lib/pkgconfig:$target/lib/pkgconfig"
pkg-config --static --print-errors --modversion libsixel
# The LLVM unwinder used by librsvg needs the ELF frame table for C++ exceptions.
cmake -S "$source_dir" -B "$work/build" "${toolchain[@]}" -DPKG_CONFIG_ARGN=--static \
  "${app_options[@]}" \
  "-DCMAKE_EXE_LINKER_FLAGS=$static_link_flags -Wl,--eh-frame-hdr" \
  "-DCMAKE_FIND_ROOT_PATH=$sixel_sdk;$target;$sysroot"
cmake --build "$work/build" --parallel "$jobs"
if ((${#runner[@]} == 0)); then
  # The missing-SDK case runs its own CMake compiler check.
  LDFLAGS="$static_link_flags" ctest --test-dir "$work/build" -C Release --output-on-failure --no-tests=error
  ctest --test-dir "$work/build" -C Release -V --no-tests=error -R '^sdk\.decode\.(svg|gif)$'
fi
cmake --install "$work/build" --prefix "$work/stage"
program=$work/stage/bin/si
LC_ALL=C readelf -W -h -l -d "$program" > "$work/si.elf.txt"
grep -E "^ *Machine: +$machine$" "$work/si.elf.txt"
if [[ $processor == arm ]]; then
  grep -F 'hard-float ABI' "$work/si.elf.txt"
  # Older binutils displays the PT_ARM_EXIDX segment as EXIDX.
  grep -E '^[[:blank:]]+(ARM_)?EXIDX[[:blank:]]' "$work/si.elf.txt"
else
  grep -E '^[[:blank:]]+GNU_EH_FRAME[[:blank:]]' "$work/si.elf.txt"
fi
if grep -E 'INTERP|\(NEEDED\)' "$work/si.elf.txt"; then
  printf 'Expected a static %s ELF: %s\n' "$processor" "$program" >&2
  exit 1
fi
printf 'Static ELF has no interpreter or dynamic library dependencies: %s\n' "$program"
"${runner[@]}" "$program" --help
python3 "$source_dir/tests/installed_media.py" --program "$program" --work-root "$work" "${media_args[@]}"
if [[ -n $asset ]]; then
  tar -C "$work/stage/bin" -czf "$asset" si
fi
