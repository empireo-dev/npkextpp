#!/usr/bin/env python3
"""Checks the tuner against simulated disks (docs/simulated_disk.md).

For every scenario it first runs the pipeline at fixed writer counts to learn
the right answer (cached in --work), then runs the tuner with several stall
seeds. A tuned run passes when the count it ends at is within 10 % of the best
fixed throughput, and (where given) the scenario's own check holds.

    python3 tests/bench/tuner_scenarios.py --install out/bench/install \
        --tune-sim out/bin/tests/tune_sim --work out/bench/tuner [--only ssd hdd] [--seeds 3]
"""
import argparse
import json
import re
import subprocess
import sys
from pathlib import Path

# name: (tune_sim arguments, passes over the archives for tuned runs, fixed counts to measure)
SCENARIOS = {
    "ssd": (["--disk", "ssd"], 40, [1, 2, 4, 8, 12, 16, 24, 32]),
    "hdd": (["--disk", "hdd"], 2, [1, 2, 3, 4]),
    "hdd-flat": (["--disk", "hdd", "--set", "switch_cost=300us"], 2, [1, 2, 3, 4]),
    "hdd-big": (["--disk", "hdd", "--in", "texture0.npk", "texture1.npk"], 3, [1, 2, 3, 4]),
    "ssd-input-hdd": (["--disk", "ssd", "--input", "hdd"], 2, [1, 2, 4, 8]),
}
FIXED_PASSES = {"ssd": 6}   # fixed runs only need to be long enough to measure
TOLERANCE = 0.10

RESULT = re.compile(r"result: ([\d.]+) s, ([\d.]+) MB/s, ([\d.]+) files/s, \d+ files, (\d+) writers at the end, (\d+) MiB reads")


def run(tune_sim, install, args):
    out = subprocess.run([tune_sim, "--install", str(install), *args], capture_output=True, text=True, check=True).stdout
    match = RESULT.search(out)
    if not match:
        sys.exit(f"no result line in:\n{out}")
    seconds, mbs, files, writers, block = match.groups()
    return {"seconds": float(seconds), "mbs": float(mbs), "files": float(files), "writers": int(writers),
            "block": int(block), "log": [l for l in out.splitlines() if "tuner:" in l]}


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--install", type=Path, required=True)
    parser.add_argument("--tune-sim", required=True)
    parser.add_argument("--work", type=Path, required=True)
    parser.add_argument("--only", nargs="*")
    parser.add_argument("--seeds", type=int, default=3)
    parser.add_argument("--verbose", action="store_true", help="print the tuner's log of every run")
    options = parser.parse_args()
    options.work.mkdir(parents=True, exist_ok=True)

    failures = 0
    for name, (args, passes, counts) in SCENARIOS.items():
        if options.only and name not in options.only:
            continue
        cache = options.work / f"fixed-{name}.json"
        fixed = json.loads(cache.read_text()) if cache.exists() else {}
        for count in counts:
            if str(count) not in fixed:
                r = run(options.tune_sim, options.install,
                        [*args, "--repeat", str(FIXED_PASSES.get(name, passes)), "--writers", str(count)])
                fixed[str(count)] = r["mbs"]
                cache.write_text(json.dumps(fixed, indent=1))
        best = max(fixed.values())
        good = sorted(int(c) for c, mbs in fixed.items() if mbs >= best * (1 - TOLERANCE))
        print(f"\n{name}: fixed MB/s " + ", ".join(f"{c}: {fixed[c]:.0f}" for c in sorted(fixed, key=int)) +
              f"; good counts {good}")

        for seed in range(1, options.seeds + 1):
            r = run(options.tune_sim, options.install, [*args, "--repeat", str(passes), "--set", f"seed={seed}"])
            w = r["writers"]
            # The nearest measured counts around the one it ended at.
            ok = any(lo <= w <= hi for lo, hi in ranges(good, counts))
            extra = ""
            if name == "ssd-input-hdd":   # reading is the limit: no writer changes, bigger reads
                changed = any("trying" in l for l in r["log"])
                ok = ok and not changed and r["block"] > 8
                extra = f", read block {r['block']} MiB, {'probed writers' if changed else 'no probes'}"
            failures += not ok
            print(f"  seed {seed}: {'ok  ' if ok else 'FAIL'} ends at {w} writers, {r['mbs']:.0f} MB/s over the run "
                  f"({r['mbs'] / best * 100:.0f} % of the best fixed), {r['seconds']:.0f} s{extra}")
            if options.verbose or not ok:
                for line in r["log"]:
                    print("      " + line.strip())
    print(f"\n{failures} failed")
    return 1 if failures else 0


def ranges(good, counts):
    """Count ranges that count as good: a good measured count, and the unmeasured counts
    between two good measured ones."""
    counts = sorted(counts)
    out = []
    for i, c in enumerate(counts):
        if c not in good:
            continue
        hi = c
        if i + 1 < len(counts) and counts[i + 1] in good:
            hi = counts[i + 1]
        out.append((c, hi))
    return out


if __name__ == "__main__":
    sys.exit(main())
