#!/usr/bin/env python3
"""Time npkext (Python), core_extract (C++, one thread) and npkextpp (pipeline)
on the same installation, and check that they write the same files.

Before each run the archives are dropped from the OS cache (posix_fadvise
DONTNEED, no root needed), so every tool reads them from the disk.

    python3 tests/bench/run_bench.py --npkext ../npkext --install build/bench/install \\
        --work build/bench --core build/tests/core_extract --app build/npkextpp
"""

from __future__ import annotations

import argparse
import filecmp
import os
import shutil
import subprocess
import sys
import time
import types
from pathlib import Path


def drop_cache(folder: Path) -> None:
    if not hasattr(os, "posix_fadvise"):
        return
    os.sync()
    for path in folder.rglob("*"):
        if path.is_file():
            with open(path, "rb") as f:
                os.posix_fadvise(f.fileno(), 0, 0, os.POSIX_FADV_DONTNEED)


def warm_cache(folder: Path) -> None:
    """Read every archive once, so the next run finds them all in the OS cache
    (the tools drop what they read, so this is needed before each run)."""
    for path in folder.rglob("*.npk"):
        with open(path, "rb") as f:
            while f.read(1 << 24):
                pass


def files_of(folder: Path) -> dict[str, int]:
    return {p.relative_to(folder).as_posix(): p.stat().st_size for p in folder.rglob("*")
            if p.is_file() and p.relative_to(folder).parts[0] != "_archives"}


def same_tree(a: Path, b: Path) -> str:
    fa, fb = files_of(a), files_of(b)
    if fa.keys() != fb.keys():
        return f"different file sets ({len(fa)} vs {len(fb)})"
    for name in fa:
        if not filecmp.cmp(a / name, b / name, shallow=False):
            return f"different bytes: {name}"
    return ""


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--npkext", type=Path, required=True)
    parser.add_argument("--install", type=Path, required=True)
    parser.add_argument("--work", type=Path, required=True, help="folder for the outputs (on the disk to test)")
    parser.add_argument("--core", type=Path)
    parser.add_argument("--app", type=Path, required=True)
    parser.add_argument("--skip-python", action="store_true")
    parser.add_argument("--app-args", nargs="*", default=[])
    parser.add_argument("--keep-cache", action="store_true",
                        help="leave the archives in the OS cache (measures the tools, not the disk)")
    args = parser.parse_args()

    sys.path[:0] = [str(args.npkext / "src")]
    try:
        import rich_argparse  # noqa: F401
    except ImportError:
        stub = types.ModuleType("rich_argparse")
        stub.RawDescriptionRichHelpFormatter = argparse.RawDescriptionHelpFormatter
        sys.modules["rich_argparse"] = stub
    from npkext.cli import main as npkext_main

    reference = args.work / "out-python"
    results = []

    def timed(name: str, out: Path, run) -> None:
        shutil.rmtree(out, ignore_errors=True)
        if args.keep_cache:
            warm_cache(args.install)
        else:
            drop_cache(args.install)
        started = time.monotonic()
        code = run(out)
        os.sync()   # count writing the data out, for every tool alike
        seconds = time.monotonic() - started
        size = sum(files_of(out).values())
        check = "" if out == reference or not reference.exists() else same_tree(reference, out)
        results.append((name, seconds, size, code, check))
        print(f"{name:<24} {seconds:8.1f} s  {size / 2**20 / seconds:8.1f} MiB/s  exit {code}  "
              f"{'IDENTICAL' if not check and out != reference else check}", flush=True)

    if not args.skip_python:
        def python(out: Path) -> int:
            with open(os.devnull, "w") as devnull:
                stdout, sys.stdout = sys.stdout, devnull
                try:
                    return npkext_main(["--install", str(args.install), "--out", str(out)])
                finally:
                    sys.stdout = stdout
        timed("npkext (Python)", reference, python)

    def tool(binary: Path, extra: list[str]):
        def run(out: Path) -> int:
            return subprocess.run([str(binary), "--install", str(args.install), "--out", str(out), *extra],
                                  stdout=subprocess.DEVNULL).returncode
        return run

    if args.core:
        timed("core_extract (1 thread)", args.work / "out-core", tool(args.core, []))
        shutil.rmtree(args.work / "out-core", ignore_errors=True)
    timed("npkextpp (pipeline)", args.work / "out-app", tool(args.app, args.app_args))
    shutil.rmtree(args.work / "out-app", ignore_errors=True)
    shutil.rmtree(reference, ignore_errors=True)
    return 0 if all(code == 0 and not check for _, _, _, code, check in results) else 1


if __name__ == "__main__":
    sys.exit(main())
