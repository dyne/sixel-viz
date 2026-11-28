# sixel-viz

`si` puts photos and video in your terminal using SIXEL. Open a file on its own
or browse a folder of images and video previews in two columns.

## Install si

### Linux and Apple silicon macOS

```sh
curl -fsSL https://raw.githubusercontent.com/dyne/sixel-viz/main/install.sh | bash
```

This installs `si` in `~/.local/bin` without root access. Add that directory
to your `PATH` or run `~/.local/bin/si` directly. Set `SI_PREFIX` for another
install prefix. Rerun the command to update.

### Windows x86-64

```powershell
$installer = Join-Path $env:TEMP 'sixel-viz-install.ps1'
Invoke-WebRequest -UseBasicParsing https://raw.githubusercontent.com/dyne/sixel-viz/main/install.ps1 -OutFile $installer
powershell -NoProfile -ExecutionPolicy Bypass -File $installer
```

The Windows installer prints the full path to `si.exe` under
`%LOCALAPPDATA%\sixel-viz`. Run it with `& 'full\path\to\si.exe'`.

Both installers verify the archive's SHA-256 and keep bundled libraries
with the executable.

### Install from an archive

Download an archive for your machine from the [latest release](https://github.com/dyne/sixel-viz/releases/latest).
Each release includes a `SHA256SUMS.txt` file.

| Platform | Archive contents |
| --- | --- |
| Linux | A static `si` binary for x86-64, AArch64, 32-bit ARM hard-float, or RISC-V 64-bit |
| macOS | `si` for Apple silicon, with its runtime libraries in `lib/` |
| Windows | `si.exe` for x86-64, with its runtime DLLs |

Extract the archive, then run `./si photo.jpg` in a SIXEL-capable terminal.
On macOS, keep `lib/` beside `si`. On Windows, keep the DLLs beside `si.exe`
and run `.\si.exe photo.jpg` in PowerShell.

## Use si

```sh
si photo.jpg
si clip.mp4
si photos/
si 'photos/*.jpg'
si 'first photo.jpg' 'second photo.jpg'
```

A single video or animated GIF plays silently once. Press Ctrl+C to stop
playback and return to the shell. For a folder, `si` shows the media files
one level deep. Folders, wildcard matches, and lists of files use two columns,
oldest first. Batch videos show a frame at about 10% of the clip with a play
symbol. When the timing or seek is unavailable, `si` uses the first frame.

A caption shows the capture date, or the file's modification date, followed
by its size and a path relative to the current directory. Sizes use B, K, M,
and G in steps of 1024. A blank line follows each row.

```text
03/02/01 1.5M photos/cat.jpg

```

Quote paths with spaces. Quote a pattern if you want `si` to expand it.
Patterns support `*`, `?`, and bracket sets or ranges. Use `--` before a
filename that starts with a hyphen. Run `si --help` for usage.

### Terminal size and redirected output

`si` fits images to the terminal and leaves room for the shell prompt. When
the terminal reports only rows and columns, `si` estimates each cell at
8 × 16 pixels. Set `SI_CELL_SIZE=10x20` to try a different cell size. The
setting takes whole pixels in `WIDTHxHEIGHT` form.

During playback, `si` refits the picture when you resize the window and
restores the shell screen when playback ends. With redirected stdout, a single
still keeps its native display size and motion writes every frame without
pacing. Batches use an estimated 80-column grid. Long captions can be shortened
to fit a terminal column. Redirected captions include the full paths.

Usage errors exit with status 2. Media or output errors exit with status 1 and
write a message to stderr. Interrupted playback exits with status 130.

## Build the static Linux release

The [release workflow](.github/workflows/release.yml) uses the
[Dyne musl toolchain](https://github.com/dyne/musl) through
[dyne/musl-action](https://github.com/dyne/musl-action). The pinned SDK,
v2.11.0, supplies the compilers, FFmpeg 9.1, and librsvg for all four Linux
targets. The recipe builds libsixel and links `si` against the FFmpeg libraries
from the SDK.

To reproduce the Linux build, install CMake 3.22 or newer, Ninja, make,
pkg-config, Python, Git, and binutils. Export `CC` and `CXX` with the absolute
paths to the SDK compilers. Install QEMU user emulation if you are building
for another architecture.

```sh
bash ci/musl-check.sh "$(mktemp -d)"
```

The [script](ci/musl-check.sh) prints its work directory, installs the viewer
at `stage/bin/si` under that directory, checks that the ELF is statically
linked, and runs the installed media checks.

## Build and test locally

The viewer uses standard C++ for console output, FFmpeg for decoding and
scaling, and libsixel to write SIXEL. Use CMake 3.20 or newer and a C++20
compiler. Use an FFmpeg build that can decode GIFs and SVGs. The recipes
build [libsixel 1.8.7-r2](https://github.com/saitoha/libsixel/tree/72ed7d6b4a2252a6245567908c32c4e8e8c226ea).

If pkg-config already finds FFmpeg and libsixel, run these commands from the
project root. For SDKs installed in custom locations, add
`-DFFmpeg_ROOT=/path/to/ffmpeg` and `-DLibSixel_ROOT=/path/to/libsixel` to
the configure command.

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure --no-tests=error
cmake --install build --config Release --prefix stage
```

The [native build recipe](ci/native-check.sh) installs into a temporary
`stage` directory, runs CTest, and checks the installed executable with media
fixtures. GitHub runs it on Apple silicon macOS for pull requests.
[Pull request workflow](.github/workflows/native.yml)

<details>
<summary>Native build commands by platform</summary>

**Debian 13.** The distro FFmpeg packages include SVG support through librsvg.

```sh
sudo apt-get update
sudo apt-get install build-essential ca-certificates cmake git \
  libavcodec-dev libavformat-dev libavutil-dev libswscale-dev \
  ninja-build pkgconf python3
bash ci/native-check.sh linux "$(mktemp -d)"
```

**Apple silicon macOS.** Install the Xcode command-line tools first. The
recipe builds shared FFmpeg 7.1.5 with Homebrew's librsvg and JPEG XL libraries.

```sh
brew install cmake jpeg-xl librsvg ninja pkgconf python
bash ci/native-check.sh macos "$(mktemp -d)"
```

**Windows.** Open an MSYS2 UCRT64 shell and update it with `pacman -Syu`.
The recipe uses the UCRT64 FFmpeg package. [MSYS2 environments](https://www.msys2.org/docs/environments/)

```sh
pacman -S --needed git make mingw-w64-ucrt-x86_64-{cmake,ffmpeg,gcc,ninja,pkgconf,python}
bash ci/native-check.sh windows "$(mktemp -d)"
```

To run a native build again from the work directory printed by the recipe,
include its SDK libraries in the loader path. On Linux, add `libsixel-sdk/lib`
to `LD_LIBRARY_PATH`. On macOS, add `ffmpeg-sdk/lib` and `libsixel-sdk/lib`
to `DYLD_LIBRARY_PATH`. On Windows, add `libsixel-sdk/bin` and the MSYS2
`ucrt64/bin` directory to `PATH`.

</details>

### Check rendering in a terminal

Try a wide photo, `tests/fixtures/two-colors.svg`, and a folder with several
images. The output should stay above the prompt. Play
`tests/fixtures/resize-motion.gif`, resize the window while it plays, and make
sure the shell is usable when it finishes. Repeat with a video and Ctrl+C.

To try SIXEL in XTerm, open it in VT340 mode.

```sh
xterm -ti vt340 -xrm '*decTerminalID: 340' -xrm '*sixelScrolling: true'
```

On macOS, use a SIXEL-capable iTerm2 profile. On Windows, run `si.exe` from a
SIXEL-capable Windows Terminal profile.

## How releases are published

GitHub Actions tags the tested commit on `main` and publishes the archives
after all six builds and installed-media checks pass. Versioning follows
conventional commits through the IETF Tools SemVer action.
[Release workflow](.github/workflows/release.yml)
