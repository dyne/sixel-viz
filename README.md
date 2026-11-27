# sixel-viz

`si` is a command-line SIXEL image viewer and silent video player.

`si --help` prints usage. `si [--] FILE` takes one local file. Use `--` before
a filename that starts with a hyphen. Quote filenames containing spaces.
A still writes one complete SIXEL picture to stdout, including when redirected.
Usage errors exit 2. Media and output errors exit 1 and write diagnostics to
stderr. Exit 130 is reserved for interrupted playback.

## Terminal sizing

`si` can use the terminal's pixel dimensions or the dimensions of its visible
cell grid and font. It reserves one prompt row when the terminal has two or
more rows. For example, 800 × 600 pixels across 30 rows gives an 800 × 580
usable area. A one-row terminal keeps that row available.

On terminals that report the cell count alone, the fit is approximate. It
uses 8 × 16 pixels per cell by default. To calibrate it, divide the width and
height of the terminal's text area in pixels by its column and row counts,
respectively. Round to whole pixels and set `SI_CELL_SIZE`, for example to
`10x20`. Use positive decimal integers and a lowercase `x`. An invalid value
is a usage error. If no dimensions are available, the estimate uses 80 columns
and 24 rows. When stdout is redirected, `si` writes the still at its native
display size and sends no terminal query.

Stills shrink only after orientation and pixel-aspect correction. The native
display width rounds to the nearest whole pixel, with halves rounded up.
When shrinking, `si` uses the exact display ratio and rounds the scaled
dimension down, keeping at least one pixel on each axis. Resized pixels use
the nearest source pixel at each output pixel center.

## Development SDKs

Use CMake 3.20 or later and a C++20 toolchain. Use SDK headers and libraries for
the same architecture and toolchain as the application.

| SDK | Reference version | Discovery |
| --- | --- | --- |
| xtd | `3ea373359f7a92885d8abd88d397fc23fa1cad7e` | `xtd_DIR` contains the installed `xtdConfig.cmake` and `xtd.core` export. |
| FFmpeg | 7.1.5 (Debian 13 package `7:7.1.5-0+deb13u1`) | `pkg-config` modules `libavformat`, `libavcodec`, `libavutil`, and `libswscale`, or `FFmpeg_ROOT`. |
| libsixel | 1.8.7-r2, commit `72ed7d6b4a2252a6245567908c32c4e8e8c226ea` | `pkg-config` module `libsixel`, or `LibSixel_ROOT`. |

The FFmpeg libraries must support GIF and SVG decoding. Debian 13's shared
FFmpeg development packages link to an FFmpeg build with librsvg enabled. For a
source build, install the librsvg development SDK, configure FFmpeg with
`--enable-librsvg --enable-shared`, then build and install it. For explicit
prefix discovery, provide shared libraries or their Windows import libraries
along with the headers. CMake also searches architecture-specific include and
library directories beneath each prefix.

For a full Linux build of the pinned xtd SDK, install the GTK 3, ALSA, UUID, and
wxWidgets 3.2 development libraries. Check out the reference commit in a
separate directory and run these commands.

```sh
cmake -S /path/to/xtd -B /path/to/xtd-build -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_INSTALL_PREFIX=/path/to/xtd-sdk \
  -DXTD_BUILD_CPP_STANDARD=20 -DXTD_BUILD_TOOLS=OFF
cmake --build /path/to/xtd-build --config Release
cmake --install /path/to/xtd-build --config Release --prefix /path/to/xtd-sdk
```

The isolated Linux verification used a core-only SDK from the same pin. In a
disposable xtd copy, keep only the first `add_projects` block in
`src/CMakeLists.txt` and set the generated `xtd_LIBRARIES` list in the root
`CMakeLists.txt` to `xtd.core`. Set the default `TARGET_REGISTERED` to `OFF` in
`scripts/cmake/xtd_commands.cmake` if the build must avoid registering desktop
entries in the user directory. These changes affect packaging only. Build the
`xtd.core` target and use the same install command to produce the SDK export.

Build the workspace's libsixel checkout out of tree. The viewer only needs the
library and its headers.

```sh
mkdir -p /path/to/libsixel-build
cd /path/to/libsixel-build
/path/to/libsixel/configure --prefix=/path/to/libsixel-sdk \
  --disable-img2sixel --disable-sixel2png --disable-python
make
make install
```

## Build, test, and install

Run the commands from the application directory. Omit an explicit media SDK
prefix if the intended SDK is already available to pkg-config.

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release \
  -Dxtd_DIR=/path/to/xtd-sdk/cmake \
  -DFFmpeg_ROOT=/path/to/ffmpeg-sdk \
  -DLibSixel_ROOT=/path/to/libsixel-sdk
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure --no-tests=error
cmake --install build --config Release --prefix stage
```

For a custom runtime library location, make the SDK libraries visible to the
system loader when launching the installed executable or running install tests.
For an isolated Linux setup, set `LD_LIBRARY_PATH` for the command. On Windows, add the SDK DLL
directories to `PATH` or place the DLLs beside `si.exe`.

`ctest --test-dir build -C Release -V -R '^sdk\.'` prints the FFmpeg build
configuration, selected demuxers and decoders, and the fixture checks. The
reference Linux build uses Debian 13 amd64, GCC 14.2 and CMake 3.31.6. The SDK
probe exercises `gif/gif` and `svg_pipe/librsvg` through the linked libraries.
