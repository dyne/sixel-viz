"""Check redirected SIXEL output from a freshly installed si."""

import argparse
from datetime import datetime
import os
from pathlib import Path
import re
import shutil
import struct
import subprocess
import tempfile
import zlib


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


def set_modified(path, year, month, day):
    timestamp = datetime(year, month, day, 12).timestamp()
    os.utime(path, (timestamp, timestamp))


def caption(path, date, display_name, size=None):
    size = size if size is not None else f"{path.stat().st_size}B"
    return f"{date} {size} {display_name}".encode()


def run_si(command, work, *operands):
    env = dict(os.environ, SI_CELL_SIZE="8x16")
    result = subprocess.run([*command, *operands], cwd=work, capture_output=True,
                            timeout=30, env=env)
    require(result.returncode == 0 and not result.stderr,
            f"exited {result.returncode}, stderr: {result.stderr!r}")
    return result.stdout


def verify_file(command, work, fixture, size, frames, tolerance=8):
    name = "夜空 café 🌌" + fixture.suffix
    path = work / name
    shutil.copyfile(fixture, path)
    set_modified(path, 2020, 1, 2)
    output = run_si(command, work, name)
    offset = 0
    for index, samples in enumerate(frames, start=1):
        try:
            offset, pixels = read_picture(output, offset, size)
            for x, y, expected in samples:
                actual = pixels[y][x]
                require(all(abs(value - reference) <= tolerance
                            for value, reference in zip(actual, expected)),
                        f"RGB at ({x}, {y}) is {actual}, expected {expected} ± {tolerance}")
        except ValueError as failure:
            raise ValueError(f"picture {index}: {failure}") from failure
    suffix = b"" if fixture.suffix == ".mkv" else b"\n" + caption(path, "02/01/20", name) + b"\n\n"
    require(output[offset:] == suffix,
            f"unexpected output after {len(frames)} pictures at byte {offset}: {output[offset:offset + 90]!r}")
    print(f"{fixture.name}: {len(frames)} complete {size[0]}x{size[1]} pictures, RGB samples matched")


def exif_profile(order="<", date=b"2001:02:03 04:05:06\0"):
    endian = b"II" if order == "<" else b"MM"
    # TIFF header, IFD0 with its EXIF pointer, then the DateTimeOriginal IFD.
    return (endian + struct.pack(order + "HIH", 42, 8, 1)
            + struct.pack(order + "HHII", 0x8769, 4, 1, 26) + bytes(4)
            + struct.pack(order + "H", 1)
            + struct.pack(order + "HHII", 0x9003, 2, len(date), 44) + bytes(4) + date)


def jpeg_with_exif(jpeg):
    exif = b"Exif\0\0" + exif_profile()
    return jpeg[:2] + b"\xff\xe1" + struct.pack(">H", len(exif) + 2) + exif + jpeg[2:]


def verify_image_metadata(command, work, fixtures):
    png = (fixtures / "alpha.png").read_bytes()
    webp = (fixtures / "two-colors.webp").read_bytes()
    profile = exif_profile(">")

    def png_with_exif(profile):
        chunk = b"eXIf" + profile
        return png[:33] + struct.pack(">I", len(profile)) + chunk + struct.pack(">I", zlib.crc32(chunk)) + png[33:]

    def riff_chunk(tag, payload):
        return tag + struct.pack("<I", len(payload)) + payload + bytes(len(payload) % 2)

    # VP8X declares EXIF and the 8x4 canvas of the fixture's VP8L bitstream.
    riff = b"WEBP" + riff_chunk(b"VP8X", bytes([8, 0, 0, 0, 7, 0, 0, 3, 0, 0]))
    riff += webp[12:] + riff_chunk(b"EXIF", b"Exif\0\0" + exif_profile())
    cases = (
        ("capture.png", png_with_exif(profile), "03/02/01"),
        ("capture.webp", riff_chunk(b"RIFF", riff), "03/02/01"),
        ("invalid-date.png", png_with_exif(exif_profile(date=b"2001:02:99 04:05:06\0")), "02/01/20"),
    )
    for name, contents, date in cases:
        path = work / name
        path.write_bytes(contents)
        set_modified(path, 2020, 1, 2)
        output = run_si(command, work, name)
        end, _ = read_picture(output, 0, (8, 4))
        require(output[end:] == b"\n" + caption(path, date, name) + b"\n\n",
                f"incorrect capture date for {name}: {output[end:]!r}")
    print("Image metadata: PNG and WebP EXIF dates and invalid-date fallback matched")


def verify_gallery(command, work, fixtures):
    folder = work / "gallery"
    folder.mkdir()
    old, clip, poster = (folder / name for name in ("old.jpg", "clip.mkv", "poster.png"))
    old.write_bytes(jpeg_with_exif((fixtures / "oriented.jpg").read_bytes()))
    shutil.copyfile(fixtures / "limited-yuv.mkv", clip)
    poster.write_bytes((fixtures / "alpha.png").read_bytes().ljust(1536, b"\0"))
    (folder / "notes.txt").write_text("skip me")
    (folder / "nested").mkdir()
    shutil.copyfile(poster, folder / "nested" / "outside.png")
    set_modified(old, 2030, 4, 5)
    set_modified(clip, 2015, 6, 7)
    set_modified(poster, 2022, 8, 9)

    folder_output = run_si(command, work, "gallery")
    folder_glob = run_si(command, work, "galler?/*.*")
    explicit = run_si(command, work, str(poster), str(clip), str(old))
    require(folder_output == folder_glob == explicit, "batch input forms did not agree")
    offset, pixels = read_picture(folder_output, 0, (384, 16))
    # The second video frame is the first frame at or after 10% of the clip.
    expected_pixels = [(320 + 48, 4, GREEN), (320 + 29, 7, WHITE), (310, 7, BLACK)]
    for x, y, expected in expected_pixels:
        require(all(abs(actual - ref) <= 12 for actual, ref in zip(pixels[y][x], expected)),
                f"preview pixel ({x}, {y}) was {pixels[y][x]}, expected {expected}")
    old_label = caption(old, "03/02/01", "gallery/old.jpg")
    clip_label = caption(clip, "07/06/15", "gallery/clip.mkv", "1.5K")
    row = b"\n" + old_label.ljust(40) + clip_label + b"\n\n"
    require(folder_output.startswith(row, offset),
            f"incorrect date, order or alignment: {folder_output[offset:offset + 95]!r}")
    offset, _ = read_picture(folder_output, offset + len(row), (8, 4))
    require(folder_output[offset:] == b"\n09/08/22 1.5K gallery/poster.png\n\n",
            "expected the last preview with its caption and a blank line")

    mixed = run_si(command, work, str(old), str(poster))
    _, pixels = read_picture(mixed, 0, (328, 16))
    require(pixels[2][321] == BLACK and all(abs(actual - ref) <= 8
            for actual, ref in zip(pixels[14][321], (255, 32, 64))),
            "expected the shorter preview at the bottom of the composed raster")

    single_match = run_si(command, work, "gallery/[o]l?.jpg")
    picture = PICTURE.match(single_match)
    require(picture and single_match[picture.end():] == b"\n" + old_label + b"\n\n",
            "one-match wildcard did not select the expected image")
    print("Gallery: folder, wildcards, multiple paths, sorting, captions, and video preview matched")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--program", type=Path, required=True)
    parser.add_argument("--work-root", type=Path, required=True)
    parser.add_argument("--emulator", type=Path)
    args = parser.parse_args()
    program = args.program.resolve(strict=True)
    command = [str(program)]
    if args.emulator:
        command.insert(0, str(args.emulator.resolve(strict=True)))
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
        fixtures = tests / "fixtures"
        for name, size, frames, tolerance in cases:
            try:
                verify_file(command, work, fixtures / name, size, frames, tolerance)
            except (OSError, subprocess.TimeoutExpired, ValueError) as failure:
                print(f"{name}: {failure}")
                failed += 1
        extra_checks = (verify_image_metadata, verify_gallery)
        for verify in extra_checks:
            try:
                verify(command, work, fixtures)
            except (OSError, subprocess.TimeoutExpired, ValueError) as failure:
                print(f"{verify.__name__}: {failure}")
                failed += 1
    total = len(cases) + len(extra_checks)
    print(f"{total - failed}/{total} installed media checks passed")
    return bool(failed)


if __name__ == "__main__":
    raise SystemExit(main())
