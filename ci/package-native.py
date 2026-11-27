#!/usr/bin/env python3
"""Package and test the installed macOS or Windows viewer and its libraries."""

import argparse
from collections import deque
import os
from pathlib import Path
import re
import shutil
import stat
import subprocess
import sys
import tempfile


def output(*command):
    return subprocess.check_output(command, text=True)


def macho_dependencies(path):
    return [line.strip().split(" (compatibility version ", 1)[0]
            for line in output("otool", "-arch", "arm64", "-L", path).splitlines()
            if " (compatibility version " in line]


def macho_rpaths(path):
    return re.findall(r"^\s*cmd LC_RPATH\s*\n\s*cmdsize \d+\s*\n\s*path (.+) \(offset \d+\)$",
                      output("otool", "-arch", "arm64", "-l", path), re.MULTILINE)


def system_macho(path):
    return str(path).startswith(("/usr/lib/", "/System/Library/"))


def expand_macho(path, loader, executable):
    if path.startswith("@loader_path/"):
        return loader.parent / path.removeprefix("@loader_path/")
    if path.startswith("@executable_path/"):
        return executable.parent / path.removeprefix("@executable_path/")
    return Path(path)


def resolve_macho(name, loader, executable, rpaths, runtime_dirs):
    if name.startswith("@rpath/"):
        relative = name.removeprefix("@rpath/")
        candidates = [path / relative for path in [*rpaths, *runtime_dirs]]
    else:
        candidates = [expand_macho(name, loader, executable)]
    for path in candidates:
        if system_macho(path):
            return path
        if path.is_file():
            return path.resolve()
    raise FileNotFoundError(f"{loader.name}: could not locate {name}")


def bundle_macos(executable, bundle, runtime_dirs):
    library_dir = bundle / "lib"
    library_dir.mkdir()
    bundle_rpath = "@executable_path/lib"
    bundled = {executable: bundle / executable.name}
    queue = deque([(executable, [])])
    while queue:
        source, inherited_rpaths = queue.popleft()
        target = bundled[source]
        original_rpaths = macho_rpaths(source)
        rpaths = [expand_macho(path, source, executable) for path in original_rpaths]
        rpaths.extend(inherited_rpaths)
        dependencies = macho_dependencies(source)
        edits = []
        for path in dict.fromkeys(original_rpaths):
            if source != executable or path != bundle_rpath:
                edits.extend(["-delete_rpath", path])
        if source == executable:
            if bundle_rpath not in original_rpaths:
                edits.extend(["-add_rpath", bundle_rpath])
        else:
            # For a dylib, otool -L prints its install name before its dependencies.
            dependencies = dependencies[1:]
            edits.extend(["-id", f"@rpath/{target.name}"])
        for name in dict.fromkeys(dependencies):
            if system_macho(name):
                continue
            dependency = resolve_macho(name, source, executable, rpaths, runtime_dirs)
            if system_macho(dependency):
                reference = str(dependency)
            else:
                if dependency not in bundled:
                    destination = library_dir / Path(name).name
                    if destination in bundled.values():
                        raise ValueError(f"Multiple libraries would use {destination.name}")
                    bundled[dependency] = destination
                    queue.append((dependency, rpaths))
                reference = f"@rpath/{bundled[dependency].name}"
            if name != reference:
                edits.extend(["-change", name, reference])
        shutil.copy2(source, target)
        target.chmod(target.stat().st_mode | stat.S_IWUSR)
        if edits:
            subprocess.run(["install_name_tool", *edits, target], check=True)
    for target in [*library_dir.iterdir(), bundled[executable]]:
        for name in macho_dependencies(target):
            if system_macho(name):
                continue
            if not name.startswith("@rpath/") or not (library_dir / name[7:]).is_file():
                raise ValueError(f"{target.name}: dependency escaped the bundle: {name}")
        # Changing load commands invalidates the original signature on Apple Silicon.
        subprocess.run(["codesign", "--force", "--sign", "-", target], check=True)
        subprocess.run(["codesign", "--verify", target], check=True)


def bundle_windows(executable, bundle, runtime_dirs):
    system = Path(os.environ["SystemRoot"]) / "System32"
    dll_paths = {}
    for directory in runtime_dirs:
        for path in directory.glob("*.dll"):
            dll_paths.setdefault(path.name.lower(), path)
    queued = {executable.name.lower()}
    queue = deque([executable])
    while queue:
        source = queue.popleft()
        shutil.copy2(source, bundle / source.name)
        names = re.findall(r"^\s*DLL Name:\s*(\S+)\s*$",
                           output("objdump", "-p", source), re.MULTILINE)
        for name in names:
            key = name.lower()
            if key in queued or key.startswith(("api-ms-win-", "ext-ms-win-")):
                continue
            if (system / name).is_file():
                continue
            if key not in dll_paths:
                raise FileNotFoundError(f"{source.name}: could not locate {name}")
            queue.append(dll_paths[key])
            queued.add(key)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("platform", choices=("macos", "windows"))
    parser.add_argument("--program", type=Path, required=True)
    parser.add_argument("--archive", type=Path, required=True)
    parser.add_argument("--runtime-dir", type=Path, action="append", required=True)
    args = parser.parse_args()
    program = args.program.resolve(strict=True)
    archive = args.archive.resolve()
    directories = [path.resolve(strict=True) for path in args.runtime_dir]
    media_test = Path(__file__).resolve().parent.parent / "tests/installed_media.py"
    env = {key: value for key, value in os.environ.items()
           if key != "LD_LIBRARY_PATH" and not key.startswith("DYLD_") and key != "SI_CELL_SIZE"}
    with tempfile.TemporaryDirectory(prefix="native-package-", dir=archive.parent) as directory:
        work = Path(directory)
        bundle = work / "bundle"
        bundle.mkdir()
        if args.platform == "macos":
            bundle_macos(program, bundle, directories)
            env["PATH"] = "/usr/bin:/bin"
            archive_format = "gztar"
        else:
            bundle_windows(program, bundle, directories)
            windows = Path(os.environ["SystemRoot"])
            env["PATH"] = os.pathsep.join(map(str, (windows / "System32", windows)))
            archive_format = "zip"
        candidate = Path(shutil.make_archive(str(work / "si"), archive_format, root_dir=bundle))
        extracted = work / "extracted"
        shutil.unpack_archive(candidate, extracted)
        subprocess.run([sys.executable, media_test, "--program", extracted / program.name,
                        "--work-root", work], env=env, check=True)
        candidate.replace(archive)


if __name__ == "__main__":
    main()
