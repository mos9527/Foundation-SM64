#!/usr/bin/env python3
"""Pack Foundation-SM64 runtime artifacts from a CMake build directory.

Collects from <build>/bin/:
  - Data/ (recursively)
  - *.exe, *.dll, *.so, *.dylib
  - other executable files (Unix binaries with the execute bit)

Writes packages/Foundation-sm64-<git-hash>.zip at the repository root.
"""

from __future__ import annotations

import argparse
import os
import stat
import subprocess
import sys
import zipfile
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent
DEFAULT_BUILD_DIR = REPO_ROOT / "cmake-build-debug"

SHARED_LIB_SUFFIXES = {".dll", ".so", ".dylib"}
# Also match versioned shared objects: libfoo.so.1, libfoo.so.1.2.3
SHARED_LIB_PATTERNS = (".so.",)
# Build-side leftovers that sometimes land under bin/Data (e.g. slangc .spv.d).
SKIP_SUFFIXES = {".d", ".pdb", ".ilk", ".exp"}


def git_hash(repo: Path) -> str:
    try:
        out = subprocess.check_output(
            ["git", "rev-parse", "--short", "HEAD"],
            cwd=repo,
            stderr=subprocess.DEVNULL,
            text=True,
        ).strip()
        if out:
            return out
    except (subprocess.CalledProcessError, FileNotFoundError, OSError):
        pass
    return "unknown"


def is_shared_lib(path: Path) -> bool:
    name = path.name
    suffix = path.suffix.lower()
    if suffix in SHARED_LIB_SUFFIXES:
        return True
    # libfoo.so.1 / libfoo.so.1.2.3
    return any(p in name for p in SHARED_LIB_PATTERNS) and ".so" in name


def is_unix_executable(path: Path) -> bool:
    """True for non-extension binaries with an execute bit (or Windows PE without .exe)."""
    if path.suffix:
        return False
    try:
        mode = path.stat().st_mode
    except OSError:
        return False
    if mode & (stat.S_IXUSR | stat.S_IXGRP | stat.S_IXOTH):
        return True
    # Fallback: ELF magic (Linux/BSD) when the execute bit was stripped by copy.
    try:
        with path.open("rb") as f:
            magic = f.read(4)
        return magic == b"\x7fELF"
    except OSError:
        return False


def collect_files(bin_dir: Path) -> list[tuple[Path, str]]:
    """Return (absolute path, archive-relative path) pairs under bin/."""
    if not bin_dir.is_dir():
        raise FileNotFoundError(f"build bin directory not found: {bin_dir}")

    selected: dict[str, Path] = {}

    data_dir = bin_dir / "Data"
    if data_dir.is_dir():
        for path in data_dir.rglob("*"):
            if not path.is_file():
                continue
            if path.suffix.lower() in SKIP_SUFFIXES:
                continue
            rel = path.relative_to(bin_dir).as_posix()
            selected[rel] = path

    for path in bin_dir.iterdir():
        if not path.is_file():
            continue
        name_lower = path.name.lower()
        take = False
        if name_lower.endswith(".exe"):
            take = True
        elif is_shared_lib(path):
            take = True
        elif is_unix_executable(path):
            take = True
        if take:
            selected[path.name] = path

    # Also pick up shared libs nested under bin/ (some generators stage them
    # beside Data/ in subfolders other than Data/).
    for path in bin_dir.rglob("*"):
        if not path.is_file():
            continue
        rel_path = path.relative_to(bin_dir)
        if rel_path.parts and rel_path.parts[0] == "Data":
            continue  # already gathered
        if is_shared_lib(path) or path.suffix.lower() == ".exe" or is_unix_executable(path):
            selected[rel_path.as_posix()] = path

    return sorted(selected.items(), key=lambda kv: kv[0].lower())


def pack(build_dir: Path, out_zip: Path) -> int:
    bin_dir = build_dir / "bin"
    files = collect_files(bin_dir)
    if not files:
        print(f"error: nothing to pack under {bin_dir}", file=sys.stderr)
        return 1

    out_zip.parent.mkdir(parents=True, exist_ok=True)
    if out_zip.exists():
        out_zip.unlink()

    with zipfile.ZipFile(out_zip, "w", compression=zipfile.ZIP_DEFLATED) as zf:
        for rel, path in files:
            zf.write(path, arcname=rel)
            print(f"  + {rel}")

    size_mb = out_zip.stat().st_size / (1024 * 1024)
    print(f"packed {len(files)} files -> {out_zip} ({size_mb:.1f} MiB)")
    return 0


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "build_dir",
        nargs="?",
        type=Path,
        default=DEFAULT_BUILD_DIR,
        help=f"CMake build directory (default: {DEFAULT_BUILD_DIR.name})",
    )
    parser.add_argument(
        "-o",
        "--output",
        type=Path,
        default=None,
        help="Output zip path (default: packages/Foundation-sm64-<git-hash>.zip)",
    )
    args = parser.parse_args(argv)

    build_dir = args.build_dir
    if not build_dir.is_absolute():
        build_dir = (REPO_ROOT / build_dir).resolve()
    else:
        build_dir = build_dir.resolve()

    if not build_dir.is_dir():
        print(f"error: build directory not found: {build_dir}", file=sys.stderr)
        return 1

    short = git_hash(REPO_ROOT)
    out_zip = args.output
    if out_zip is None:
        out_zip = REPO_ROOT / "packages" / f"Foundation-sm64-{short}.zip"
    elif not out_zip.is_absolute():
        out_zip = (REPO_ROOT / out_zip).resolve()

    print(f"build:  {build_dir}")
    print(f"output: {out_zip}")
    return pack(build_dir, out_zip)


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
