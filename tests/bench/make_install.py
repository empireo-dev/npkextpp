#!/usr/bin/env python3
"""Build a synthetic installation for benchmarks (no game data).

Shaped like the real one: a few large LZ4 texture archives, tens of thousands of
small files, some zlib and stored entries, and a patch layer that overrides part
of the tree. --scale 1 is about 2 GB of output in ~55,000 files.

    python3 tests/bench/make_install.py --npkext ../npkext --out /path/to/bench --scale 1
"""

from __future__ import annotations

import argparse
import os
import random
import sys
from pathlib import Path


def content(rng: random.Random, size: int, compressible: float) -> bytes:
    """`size` bytes of which roughly `compressible` share is easy to compress."""
    hard = int(size * (1 - compressible))
    return os.urandom(hard) + bytes(rng.getrandbits(3) for _ in range(min(64, size - hard))) * ((size - hard) // 64 + 1)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--npkext", type=Path, required=True)
    parser.add_argument("--out", type=Path, required=True)
    parser.add_argument("--scale", type=float, default=1.0)
    args = parser.parse_args()
    sys.path[:0] = [str(args.npkext / "src"), str(args.npkext / "tests")]
    from conftest import write_npk, write_resdb
    from npkext.archive import Compression

    rng = random.Random(1)
    root = args.out / "install"
    (root / "neox_bin").mkdir(parents=True, exist_ok=True)
    names: list[str] = []

    def n(count: int) -> int:
        return max(1, int(count * args.scale))

    def build(path: str, prefix: str, count: int, sizes: tuple[int, int], mode, compressible: float,
              encrypted: bool = True) -> list[str]:
        files = {}
        for i in range(count):
            inner = f"d{i % 37}\\f{i}.bin"
            files[inner] = content(rng, rng.randint(*sizes), compressible)
            names.append(prefix + inner)
        write_npk(root / path, files, compression=mode, encrypted=encrypted)
        print(f"  {path}: {count} files", flush=True)
        return list(files)

    for part in range(2):
        build(f"res/model/texture{part}.npk", f"model\\texture{part}\\", n(1250), (100_000, 1_500_000),
              Compression.LZ4, 0.5)
    ui = build("res/ui.npk", "ui\\", n(30_000), (500, 20_000), Compression.LZ4, 0.6)
    build("res/levelsets/lake.npk", "levelsets\\lake\\", n(20_000), (1_000, 5_000), Compression.ZLIB, 0.7, False)
    build("res/sound.npk", "sound\\", n(500), (100_000, 300_000), Compression.STORED, 0.0, False)

    patch = {inner: content(rng, rng.randint(500, 20_000), 0.6) for inner in rng.sample(ui, len(ui) // 6)}
    write_npk(root / "Documents" / "res" / "ui.npk", patch)
    print(f"  Documents/res/ui.npk: {len(patch)} overrides", flush=True)

    write_resdb(root / "res" / "resdb", names)
    size = sum(p.stat().st_size for p in root.rglob("*.npk"))
    print(f"installation: {size / 2**30:.2f} GiB of archives at {root}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
