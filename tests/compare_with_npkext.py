#!/usr/bin/env python3
"""Compare npkextpp's output with the Python npkext on a synthetic installation.

Builds a randomized miniature installation with npkext's own test helpers
(several mount layers, nested archives, overrides, unnamed files, every
compression mode), unpacks it with npkext and with a C++ extractor, and checks
that both produce the same files with the same bytes. _archives/ is not compared.

    python3 tests/compare_with_npkext.py --npkext ../npkext --extractor out/bin/tests/core_extract

No game data is involved.
"""

from __future__ import annotations

import argparse
import filecmp
import os
import random
import subprocess
import sys
import tempfile
from pathlib import Path


def build_installation(root: Path, seed: int, write_npk, write_resdb, Compression) -> None:
    rng = random.Random(seed)
    modes = [Compression.STORED, Compression.ZLIB, Compression.LZ4, Compression.ZSTD]

    branches = {
        "": "",                                   # root archives (res.npk)
        "ui": "ui\\",
        "model/character": "model\\character\\",
        "levelsets/lake": "levelsets\\lake\\",
        "a/b/c": "a\\b\\c\\",                     # 3 deep: only res_dlc mounts that far
    }
    names = {prefix: [] for prefix in branches.values()}
    for prefix in branches.values():
        for i in range(rng.randint(15, 30)):
            folder = rng.choice(["", "sub\\", "sub\\deep\\", "x\\"])
            names[prefix].append(f"{prefix}{folder}file{i}.{rng.choice(['dds', 'xml', 'bin', 'mesh'])}")
    # Root archives also hold copies of files under nested branches.
    everything = [p for paths in names.values() for p in paths]

    listed = set(rng.sample(everything, int(len(everything) * 0.9)))   # 10 % unnamed
    shipped = sorted(listed)[: len(listed) // 2]
    write_resdb(root / "res" / "resdb", shipped)
    write_resdb(root / "Documents" / "res" / "resdb", sorted(listed))

    def archive(path: str, prefix: str, pool: list[str], count: int) -> None:
        files = {}
        for full in rng.sample(pool, min(count, len(pool))):
            inner = full[len(prefix):]
            size = rng.choice([0, 1, 50, 1000, 20000])
            body = (f"{path}:{full}:".encode() * (size // 20 + 1))[:size]
            if rng.random() < 0.2:
                body = bytes(rng.getrandbits(8) for _ in range(size))
            files[inner] = body
        for i in range(rng.randint(0, 3)):     # unlisted entries
            files[f"unlisted_{path.replace('/', '_')}_{i}"] = rng.choice([b"DDS " + bytes(100), b"<?xml?>", b"\x00\x01"])
        mode = rng.choice(modes)
        write_npk(root / path, files, compression=mode, encrypted=rng.random() < 0.7)

    (root / "neox_bin").mkdir(parents=True, exist_ok=True)
    archive("res.npk", "", everything, 60)
    archive("Documents/res.npk", "", everything, 30)
    for layer in ["res", "res2", "Documents/res", "Documents/res2", "Documents/res4"]:
        archive(f"{layer}/ui.npk", "ui\\", names["ui\\"], rng.randint(8, 20))
        archive(f"{layer}/model/character.npk", "model\\character\\", names["model\\character\\"], rng.randint(8, 20))
    archive("res/levelsets/lake.npk", "levelsets\\lake\\", names["levelsets\\lake\\"], 20)
    archive("Documents/res_dlc/a/b/c.npk", "a\\b\\c\\", names["a\\b\\c\\"], 20)
    archive("extra/loose.npk", "", everything, 10)          # outside every mount folder
    write_npk(root / "script.npk", {"s1": b"\x1d\x04old", "s2": b"\x1d\x04two"})
    write_npk(root / "Documents" / "script_patch.npk", {"s1": b"\x1d\x04new"})


def tree(folder: Path, index: bool = False) -> set[str]:
    """Files below `folder`; the _archives/ index only with `index` (and never its README)."""
    files = set()
    for p in folder.rglob("*"):
        relative = p.relative_to(folder)
        if not p.is_file() or relative.as_posix() == "_archives/README.txt":
            continue
        if relative.parts[0] == "_archives" and not index:
            continue
        files.add(relative.as_posix())
    return files


def compare(a: Path, b: Path, index: bool = False) -> list[str]:
    files_a, files_b = tree(a, index), tree(b, index)
    problems = [f"only in npkext: {p}" for p in sorted(files_a - files_b)]
    problems += [f"only in npkextpp: {p}" for p in sorted(files_b - files_a)]
    problems += [f"different bytes: {p}" for p in sorted(files_a & files_b)
                 if not filecmp.cmp(a / p, b / p, shallow=False)]
    return problems


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--npkext", type=Path, required=True, help="the npkext source folder")
    parser.add_argument("--extractor", type=Path, required=True, help="C++ extractor (core_extract or npkextpp)")
    parser.add_argument("--seeds", type=int, default=5, help="number of random installations")
    parser.add_argument("--extra", nargs="*", default=[], help="extra arguments for the C++ extractor")
    parser.add_argument("--index", action="store_true", help="also compare the _archives/ index (npkextpp writes it)")
    args = parser.parse_args()

    sys.path[:0] = [str(args.npkext / "src"), str(args.npkext / "tests")]
    try:
        import rich_argparse  # noqa: F401  (only colours npkext's --help)
    except ImportError:
        import types
        stub = types.ModuleType("rich_argparse")
        stub.RawDescriptionRichHelpFormatter = argparse.RawDescriptionHelpFormatter
        sys.modules["rich_argparse"] = stub
    from conftest import write_npk, write_resdb   # npkext's test helpers
    from npkext.archive import Compression
    from npkext.cli import main as npkext_main

    failures = 0
    for seed in range(args.seeds):
        for keep in (False, True):
            with tempfile.TemporaryDirectory(prefix="npkextpp-compare-") as tmp:
                tmp = Path(tmp)
                root = tmp / "install"
                build_installation(root, seed, write_npk, write_resdb, Compression)
                extra = ["--keep-overridden"] if keep else []

                with open(os.devnull, "w") as devnull:
                    stdout, sys.stdout = sys.stdout, devnull
                    try:
                        code = npkext_main(["--install", str(root), "--out", str(tmp / "py"), *extra])
                    finally:
                        sys.stdout = stdout
                cpp = subprocess.run([str(args.extractor), "--install", str(root), "--out", str(tmp / "cpp"),
                                      *extra, *args.extra], capture_output=True, text=True)
                problems = compare(tmp / "py", tmp / "cpp", args.index)
                label = f"seed {seed}{' --keep-overridden' if keep else ''}"
                count = len(tree(tmp / "py"))
                if code != 0 or cpp.returncode != 0 or problems:
                    failures += 1
                    print(f"FAIL {label}: npkext exit {code}, C++ exit {cpp.returncode}\n{cpp.stderr}")
                    for problem in problems[:20]:
                        print("   ", problem)
                else:
                    print(f"ok   {label}: {count} files identical")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
