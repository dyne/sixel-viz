#!/usr/bin/env bash
set -euo pipefail

if [[ $# != 2 ]]; then
  printf 'Usage: bash ci/native-check.sh {linux|macos|windows} WORK_ROOT\n' >&2
  exit 2
fi
platform=$1
root=$2
source_dir=$(cd -- "$(dirname -- "$0")/.." && pwd -P)
jobs=${SI_BUILD_JOBS:-2}
ffmpeg_prefix=
program=si

case "$platform:$(uname -s)" in
  linux:Linux|macos:Darwin) ;;
  windows:MINGW*|windows:MSYS*|windows:UCRT*)
    if [[ ${MSYSTEM:-} != UCRT64 ]]; then
      printf 'Open an MSYS2 UCRT64 shell.\n' >&2
      exit 2
    fi
    root=$(cygpath -u "$root")
    ffmpeg_prefix=$MINGW_PREFIX
    program=si.exe
    ;;
  *)
    printf 'Run the check on the named native platform.\n' >&2
    exit 2
    ;;
esac

root=$(cd -- "$root" && pwd -P)
if [[ $root == "$source_dir" || $root == "$source_dir/"* ]]; then
  printf 'WORK_ROOT must be outside the source tree.\n' >&2
  exit 2
fi
work=$(mktemp -d "$root/si-native.XXXXXX")
printf 'Work directory: %s\n' "$work"

native_path() {
  if [[ $platform == windows ]]; then
    cygpath -am "$1"
  else
    printf '%s\n' "$1"
  fi
}

checkout_sdk() {
  local repo=$1 revision=$2 destination=$3
  git init -q "$destination"
  git -C "$destination" fetch --depth=1 "$repo" "$revision"
  git -C "$destination" -c core.autocrlf=false checkout --quiet --detach FETCH_HEAD
  test "$(git -C "$destination" rev-parse HEAD)" = "$revision"
  printf '%s %s\n' "$repo" "$revision"
}

unset SI_CELL_SIZE
uname -sm
cmake --version
c++ --version
git -C "$source_dir" rev-parse HEAD
case "$platform" in
  linux)
    dpkg-query -W cmake g++ librsvg2-2
    pkg-config --modversion --print-errors libavformat libavcodec libavutil libswscale
    ;;
  macos)
    sw_vers
    xcrun --sdk macosx --show-sdk-version
    brew list --versions librsvg jpeg-xl
    ;;
  windows)
    pacman -Q "${MINGW_PACKAGE_PREFIX}-gcc" "${MINGW_PACKAGE_PREFIX}-ffmpeg" "${MINGW_PACKAGE_PREFIX}-librsvg"
    ;;
esac

cmake_args=(-G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_EXPORT_COMPILE_COMMANDS=ON)
if command -v ccache >/dev/null; then
  cmake_args+=(-DCMAKE_C_COMPILER_LAUNCHER=ccache -DCMAKE_CXX_COMPILER_LAUNCHER=ccache)
fi

checkout_sdk https://github.com/gammasoft71/xtd.git 3ea373359f7a92885d8abd88d397fc23fa1cad7e "$work/xtd"
git -C "$work/xtd" apply "$source_dir/ci/xtd-core.patch"
cmake -S "$work/xtd" -B "$work/xtd-build" "${cmake_args[@]}" \
  "-DCMAKE_INSTALL_PREFIX=$(native_path "$work/xtd-sdk")" \
  -DXTD_BUILD_CPP_STANDARD=20 -DXTD_BUILD_TOOLS=OFF \
  -DXTD_INSTALL_EXAMPLES=OFF -DXTD_INSTALL_RESOURCES=OFF
cmake --build "$work/xtd-build" --config Release --target xtd.core --parallel "$jobs"
cmake --install "$work/xtd-build" --config Release

checkout_sdk https://github.com/saitoha/libsixel.git 72ed7d6b4a2252a6245567908c32c4e8e8c226ea "$work/libsixel"
mkdir "$work/libsixel-build"
(
  cd "$work/libsixel-build"
  CFLAGS="-O2 -std=gnu17" "$work/libsixel/configure" --prefix="$work/libsixel-sdk" \
    --disable-img2sixel --disable-sixel2png --disable-python --disable-static \
    --without-libcurl --without-jpeg --without-png
  make -j "$jobs"
  make install
)

if [[ $platform == macos ]]; then
  ffmpeg_prefix=$work/ffmpeg-sdk
  release=ffmpeg-7.1.5
  archive=$release.tar.xz
  (
    cd "$work"
    curl --fail --location --silent --show-error --output "$archive" \
      "https://ffmpeg.org/releases/$archive"
    # The official tarball also matches Debian trixie's FFmpeg source package.
    printf '%s  %s\n' de668509caf9e35e3cd162473441fdb29538c6d96ed080292b3cf9e6fc5d558f "$archive" |
      shasum -a 256 --check
    tar -xf "$archive"
    mkdir ffmpeg-build
    cd ffmpeg-build
    pkg_config_dirs="$(brew --prefix librsvg)/lib/pkgconfig:$(brew --prefix jpeg-xl)/lib/pkgconfig"
    PKG_CONFIG_PATH="$pkg_config_dirs${PKG_CONFIG_PATH:+:$PKG_CONFIG_PATH}" \
      "../$release/configure" --prefix="$ffmpeg_prefix" \
      --disable-autodetect --enable-librsvg --enable-libjxl --enable-zlib \
      --enable-shared --disable-static --disable-debug --disable-doc --disable-programs \
      --disable-encoders --disable-muxers --disable-avdevice --disable-avfilter --disable-postproc
    make -j "$jobs"
    make install
  )
fi

case "$platform" in
  linux) export LD_LIBRARY_PATH="$work/libsixel-sdk/lib" ;;
  macos) export DYLD_LIBRARY_PATH="$ffmpeg_prefix/lib:$work/libsixel-sdk/lib" ;;
  windows) export PATH="$work/libsixel-sdk/bin:$MINGW_PREFIX/bin:$PATH" ;;
esac

cd "$source_dir"
cmake -S . -B "$work/build" "${cmake_args[@]}" \
  "-Dxtd_DIR=$(native_path "$work/xtd-sdk/cmake")" \
  "-DFFmpeg_ROOT=${ffmpeg_prefix:+$(native_path "$ffmpeg_prefix")}" \
  "-DLibSixel_ROOT=$(native_path "$work/libsixel-sdk")" -DCMAKE_INSTALL_BINDIR=bin
cmake --build "$work/build" --config Release --parallel "$jobs"
ctest --test-dir "$work/build" -C Release --output-on-failure --no-tests=error
ctest --test-dir "$work/build" -C Release -V --no-tests=error -R '^sdk\.(smoke|decode\.)'
cmake --install "$work/build" --config Release --prefix "$work/stage"
python3 tests/installed_media.py --program "$(native_path "$work/stage/bin/$program")" \
  --work-root "$(native_path "$work")"
