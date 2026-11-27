"""Check redirected SIXEL output from a freshly installed si."""

import argparse
from pathlib import Path
import re
import shutil
import subprocess
import tempfile


PICTURE = re.compile(rb'\x1bP[0-9;]*q"(\d+);(\d+);(\d+);(\d+)([^\x1b]*)\x1b\\')
COMMAND = re.compile(rb"#(\d+)(?:;2;(\d+);(\d+);(\d+))?|\$|-|(?:!(\d+))?([?-~])")

RED, GREEN, BLUE = (255, 0, 0), (0, 255, 0), (0, 0, 255)
BLACK, WHITE, YELLOW = (0, 0, 0), (255, 255, 255), (255, 255, 0)


def require(condition, message):
    if not condition:
        raise ValueError(message)


def read_picture(output, offset, size):
    picture = PICTURE.match(output, offset)
    require(picture, f"expected complete SIXEL picture at byte {offset}")
    pan, pad, width, height = map(int, picture.groups()[:4])
    require(pan > 0 and pad > 0, "invalid raster aspect")
    require((width, height) == size, f"expected raster {size}, got {(width, height)}")

    palette = {}
    pixels = [[None] * width for _ in range(height)]
    color = x = y = 0
    raster = picture[5]
    cursor = 0
    while cursor < len(raster):
        command = COMMAND.match(raster, cursor)
        require(command, f"invalid SIXEL command at picture data byte {cursor}")
        cursor = command.end()
        if command[1] is not None:
            color = int(command[1])
            require(color < 256, f"invalid palette index {color}")
            if command[2] is not None:
                percentages = tuple(map(int, command.groups()[1:4]))
                require(max(percentages) <= 100, "invalid RGB palette definition")
                palette[color] = tuple((value * 255 + 50) // 100 for value in percentages)
        elif command[0] == b"$":
            x = 0
        elif command[0] == b"-":
            x = 0
            y += 6
        else:
            count = int(command[5]) if command[5] is not None else 1
            require(0 < count <= width - x, "SIXEL columns extend beyond raster")
            mask = command[6][0] - ord("?")
            for bit in range(6):
                if mask & (1 << bit):
                    require(y + bit < height, "SIXEL rows extend beyond raster")
                    require(color in palette, f"undefined palette index {color}")
                    pixels[y + bit][x:x + count] = [palette[color]] * count
            x += count

    require(all(pixel is not None for row in pixels for pixel in row), "incomplete raster")
    return picture.end(), pixels


def verify_file(program, work, fixture, size, frames, tolerance=8):
    name = "夜空 café 🌌" + fixture.suffix
    shutil.copyfile(fixture, work / name)
    result = subprocess.run([str(program), name], cwd=work, capture_output=True, timeout=30)
    require(result.returncode == 0 and not result.stderr,
            f"exited {result.returncode}, stderr: {result.stderr!r}")
    offset = 0
    for index, samples in enumerate(frames, start=1):
        try:
            offset, pixels = read_picture(result.stdout, offset, size)
            for x, y, expected in samples:
                actual = pixels[y][x]
                require(all(abs(value - reference) <= tolerance
                            for value, reference in zip(actual, expected)),
                        f"RGB at ({x}, {y}) is {actual}, expected {expected} ± {tolerance}")
        except ValueError as failure:
            raise ValueError(f"picture {index}: {failure}") from failure
    require(offset == len(result.stdout),
            f"unexpected output after {len(frames)} pictures at byte {offset}")
    print(f"{fixture.name}: {len(frames)} complete {size[0]}x{size[1]} pictures, RGB samples matched")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--program", type=Path, required=True)
    parser.add_argument("--work-root", type=Path, required=True)
    args = parser.parse_args()
    program = args.program.resolve(strict=True)
    root = args.work_root.resolve(strict=True)
    tests = Path(__file__).resolve().parent
    require(not root.is_relative_to(tests.parent), "work root must be outside the source tree")
    # For GIF, sample a green checker cell and a blue cell plus unmodified red.
    # For H.264, the 2:1 sample aspect doubles the x coordinate of the marker.
    cases = (
        ("alpha.png", (8, 4),
         [[(1, 2, (255, 32, 64)), (3, 2, (120, 63, 10)), (5, 2, BLACK), (7, 2, BLUE)]], 8),
        ("two-colors.svg", (8, 4), [[(1, 2, RED), (6, 2, BLUE)]], 8),
        ("once-disposal.gif", (8, 4),
         [[(2, 0, green), (6, 1, blue), (0, 0, RED)]
          for green, blue in [(RED, RED), (GREEN, RED), (RED, BLUE)]], 8),
        ("limited-yuv.mkv", (64, 16),
         [[(48, 4, color), (8, 12, RED), (24, 12, GREEN), (40, 12, BLUE), (56, 12, WHITE)]
          for color in (RED, GREEN, BLUE, WHITE, BLACK, YELLOW)], 12),
    )
    failed = 0
    with tempfile.TemporaryDirectory(prefix="si-installed-", dir=root) as directory:
        work = Path(directory)
        print(f"Program: {program}\nWorking directory: {work}", flush=True)
        for name, size, frames, tolerance in cases:
            try:
                verify_file(program, work, tests / "fixtures" / name, size, frames, tolerance)
            except (OSError, subprocess.TimeoutExpired, ValueError) as failure:
                print(f"{name}: {failure}")
                failed += 1
    print(f"{len(cases) - failed}/{len(cases)} installed media checks passed")
    return bool(failed)


if __name__ == "__main__":
    raise SystemExit(main())
