# Tuner redesign: deciding on disk cost, not on throughput

Status: **implemented**, with one change found on the way: the disk-cost model
decides well on an SSD but not on a hard disk, so a probe before the run picks
an SSD or a hard-disk search profile (`docs/pipeline.md` §7).

## 1. What is wrong

The current tuner changes the writer count W, measures throughput for a while,
and keeps the change if throughput went up by more than the noise. On the
simulated disks (`docs/simulated_disk.md`) it ends at 5–6 writers on the SSD,
where 16 is best, and it reads 2 writers on the HDD as 6–8 % *better* than 1,
when they are about a third worse. Tracing fixed writer schedules
(`tune_sim --schedule 1 2 --period 10 --trace`) shows why. Thresholds aren't the
problem; the quantity being compared is.

**F1. The noise is the workload, not the disk.** On the simulated HDD, the
average file size changes from 3 KB to 570 KB between consecutive 10-second
stretches, as the reader moves between archives of tiny files and archives of
textures. At one fixed writer count the score swings between 50 and 114. The
effects the tuner needs to see are 3–30 %. Comparing two periods compares two
different workloads first and two writer counts second.

**F2. The fixed weight of 64 KiB per file is wrong for every disk.** Fitting
the traces, a file really costs about 650 µs on the simulated HDD with 1 writer,
1150 µs with 2 (worth 70–130 KB of transfer), and 8–14 µs on the simulated SSD
(about 15 KB). So the score tilts with the mix: on the HDD, tiny-file stretches
score low whatever W is.

**F3. The HDD "+6 %" is F1 and F2, not a simulator bug.** Example from the
trace: one 10 s stretch at W = 1 averaged 297 KB per file and scored 85; the
next at W = 2 averaged 574 KB per file and scored 114, so "2 writers +35 %".

**F4. Measuring "before" again only cancels slow drift.** The workload changes
within seconds, so before → after → before still compares different
workloads. It also doubles the time per decision.

**F5. Near the top, one step is below what can be measured.** On the
simulated SSD, going from 6 to 16 writers gains about 12 % in all, around 1 % per
step. No threshold separates 1 % from noise of 10–30 %. Stepping by one can only
creep, and stalls.

## 2. The new method

### 2.1 Measure the disk, not the output

While writing is the limit, every half-second window's time is spent writing
what was written. A linear cost model fits that well:

```
window time = a[W] × files written + b × bytes written (+ c × bytes read)
```

- `a[W]`: disk seconds per file at W writers. This is what concurrency changes:
  an SSD hides it with parallelism, an HDD pays seeks for it.
- `b`: seconds per byte, the disk's bandwidth, shared by all W.
- `c`: seconds per byte read. It only matters when the archives are on the same
  disk, and it keeps reading from being charged to the writers.

With `b` and `c` shared, every window yields an estimate of `a[W]` directly. The
mix doesn't distort it: a window of big files charges its time to `b`, a window
of tiny files to `a`. Fitted to the traces, the model recovers the simulated
disks:

| | fitted | simulated truth |
|---|---|---|
| SSD bandwidth | 1430–1510 MB/s | 1500 MB/s |
| SSD `a` at 6 / 8 / 12 / 16 writers | 13.8 / 10.7 / 7.8 / 7.7 µs | best at 12–16 |
| HDD `a` at 1 / 2 writers | 650 / 1150 µs | 1 writer about ⅓ faster |

**Comparing two writer counts** is now done on the *current* mix: predicted time
`T[W] = a[W]·f + b·r + c·q` for the files `f`, written bytes `r` and read bytes
`q` of the last minute; the W with the lowest `T` wins. This is independent of
when each W was measured, so the tuner doesn't need to go back and measure
"before" again. Data gathered at a writer count stays useful when the tuner
returns to it later.

**Robustness.** Single windows still vary: stalls, a burst of reads. On the HDD,
estimates of `a` from 2-second chunks have a coefficient of variation around
0.5, but the outliers are one-sided spikes, and the medians separate 1 and 2
writers cleanly. So:
- `b` and `c` come from a least-squares fit over all saturated windows, with
  exponential forgetting (half-life about 2 minutes), so a disk that changes (an
  SSD's fast cache filling) is followed;
- `a[W]` is the **median** of its recent per-chunk estimates, with an
  uncertainty from their spread;
- windows where writing was not the limit are not used: writers mostly waiting
  for input say nothing about the disk.

### 2.2 Search: expand, then refine

Writer counts are compared through `T[W]`, not by single steps:

1. Start at 2 writers, collect until `a[2]` is known (about 6–10 s of
   saturated windows).
2. **Expand:** try W + 1. While each try predicts clearly lower `T`, keep
   going with growing steps: +1, +2, +4 … The 1 % per step that F5 can't see
   adds up to something visible over a step of 4 or 8.
3. **Refine:** once a step makes it worse, the best count lies between the last
   two tries. Check the counts in between one by one: the ±1 resolution you
   asked for, used where it pays.
4. **Downwards** the same way, if W + 1 was worse from the start (the HDD case:
   2 → 1).
5. **Ties go to fewer writers.** Within the uncertainty, or within 3 %, the
   lower count wins: fewer threads, kinder to hard disks.
6. **Keep watching.** Every minute or so, re-measure the neighbours W − 1 and
   W + 1. With forgetting, the table follows a disk that changes. A big change in
   `T` for the current mix (from tiny files to textures, say) triggers a new
   search, since the best count can differ by mix: with tiny files the SSD
   wants many writers, with textures one is nearly enough.

Starting, finishing and the "writing is the limit" test stay as they are. So
does the read block rule (it is independent of this).

### 2.3 What it costs

Per writer count tried, about 6–10 s of saturated windows. A climb from 2 to
12 on an SSD tries 2, 3, 5, 9, 17 (worse), 13, 11, 12: about 8 counts, one to
two minutes, against a 30–60 minute extraction. The model is a few dozen sums
updated every half second; the work is negligible.

## 3. Checking it

**On the simulated disks** (`tune_sim`), with runs long enough to matter:

| scenario | best fixed count | target |
|---|---|---|
| SSD preset | 12–16 | ≥ 90 % of the best throughput over the run |
| HDD preset | 1 | ends at 1, within 60 s |
| HDD, `switch_cost=300us` (flat) | 2–3 | within 5 % of the best |
| HDD with only big files | 1 | no climb |
| SSD, input on an HDD | reader is the limit | no writer changes; read block grows |

The last row needs a separate input disk in the simulator, a small addition
to `tune_sim`.

Each scenario runs 3 times with different stall seeds. A change to the tuner
passes when every scenario meets its target in every run, and no scenario ends
below the fixed default of 2 writers.

**On real disks**, once it passes on the simulated ones: one run on an SSD and
one on this VM's HDD, with `--verbose`. They check the model, not the tuning.

## 4. Risks

- **The model is linear.** Real disks have caches and queues, so the cost of
  a file can depend on what came before it. Forgetting and medians should
  absorb that; the real-SSD run will tell.
- **Bandwidth shared by all W** is a simplification. On a hard disk, many
  writers interleaving big files also lowers the effective bandwidth. That then
  shows up as a larger `a[W]` in big-file windows, which still ranks W
  correctly, but makes `a` noisier. The "HDD with only big files" scenario
  checks this.
- **Windows' file cache.** Without the Linux writeback control, writes return
  as soon as they are in the cache, until the cache is full. The model then
  measures the cache at first and the disk afterwards. Forgetting handles
  the change; the start may need a longer warm-up there.
