# Extraction pipeline

How `npk::pipeline` unpacks a whole `Plan` on all cores. The core library
(`npk::core`) decides *what* goes *where*; the pipeline only makes that happen
as fast as the machine allows. A single entry never needs the pipeline:
`npk::Decoder::read()` does it on the calling thread.

## 1. Goals and constraints

- **Same result as npkext.** Byte-identical files at the same paths, and the same
  `_archives/` index. `tests/compare_with_npkext.py` checks this.
- **No tuning knobs for the user.** Thread counts and read sizes are found at run
  time (§7). Most users can't judge their disk, and they shouldn't have to.
- **Bounded memory.** RAM use is fixed by a byte budget, whatever the archive sizes
  and however slow the disk. The OS file cache is kept small as well (§6).
- **Many small files.** An installation has hundreds of thousands of entries. The
  cost is mostly per file (open, write, close, Defender on Windows), not per byte.
  LZ4 decodes at several GB/s per core, so the CPU is rarely the limit.
- **Portable.** Plain `std::jthread`, mutexes and condition variables. The OS work
  sits behind two small classes (`InputFile` in the core, `OutputFile` here).

## 2. Overview

```
 main thread                              pipeline threads
 ───────────                              ────────────────
 Plan (from npk::Planner)
   │
   ├─ create every output folder
   │
   └─ start ──►  Reader (1) ──► DecodeQueue ──► Decoders (N = cores) ──► WriteQueue ──► Writers (W of 32)
                   │  ▲                            │  ▲                                   │
                   │  └──────── ReadBudget ◄───────┘  └───────── OutputBudget ◄───────────┘
                   │                                                                      │
                   │                                         WritebackTracker ◄───────────┘
                   │                                                │
                 Tuner (1): every 500 ms reads the stage clocks, sets W and the read block size
                   │
               Progress (atomics) ──► UI thread
   │
   └─ join ──► write _archives/ index ──► Summary
```

Every arrow is a bounded queue or a byte budget, so a slow stage simply makes the
stages before it wait. No stage can run ahead and fill the memory.

## 3. Work items

| Item | Made by | Holds | Freed when |
|---|---|---|---|
| `ReadChunk` | Reader | one buffer with the stored bytes of neighbouring entries of one archive, and a `ReadBudget` lease | the last task or job that points into it is destroyed (`shared_ptr`) |
| `DecodeTask` | Reader | `shared_ptr<ReadChunk>`, a run of entries in it (~1 MiB of stored data) | its decoder finishes it |
| `WriteJob` | Decoder | the planned entry, its final path, and the data: an owned buffer with an `OutputBudget` lease, or a view into the chunk (stored entries) | its writer finishes it |

Only the `Plan` itself is shared. The pipeline writes nothing into it except each
entry's `status` and `error`. An entry is handled by one thread at a time (reader,
then decoder, then writer), and the hand-over goes through a queue's mutex. So
these fields need no locks.

## 4. Stages

### 4.1 Prepare (main thread)
- Creates every folder in `Plan::directories`, so writers never call `mkdir` in
  the hot path.
- Raises the open-file limit to the hard limit (Linux), because the writeback
  tracker keeps recently written files open (§6).
- Sizes the memory budget: `min(1 GiB, RAM / 8)`, at least 64 MiB. A third goes
  to reading and two thirds to decoded output.

### 4.2 Reader: one thread
Goes through the sources in plan order: group by group, highest priority first.
In each archive it takes the entries to write in storage (offset) order, so the
disk is read front to back.

**Merging reads.** Neighbouring entries go into one `ReadChunk` until it would pass
`read_block` bytes (4–32 MiB, chosen by the tuner, 8 MiB at the start). A gap of
up to 512 KiB of unwanted data between them is read through. Such gaps are
overridden entries; reading them costs less than another read call or a seek. A
larger gap starts a new chunk. An entry bigger than `read_block` gets a chunk of
its own.

**Budget.** Before allocating a chunk, the reader takes its size from the
`ReadBudget`. When the budget is used up, the reader waits. That is the
backpressure for the whole pipeline.

**After the read**, `InputFile::forget()` drops the range from the OS cache: the
data is in our buffer now and is never read again.

**Chunk to tasks.** The chunk is split into `DecodeTask`s of about 1 MiB of stored
data. A task per entry would cost more in hand-overs than tiny entries take to
decode.

**Entries that share bytes.** A TOC may point two entries at overlapping or
identical bytes. Decryption works in place on the first 128 bytes, so decrypting
one would corrupt the other. The reader marks every entry whose range overlaps
another entry in the same chunk as `shared`, and the decoder works on a private
copy of those.

**Errors.** If an archive can't be opened, all its entries fail. If a chunk can't
be read, its entries fail. An entry whose range runs past the end of the file fails
on its own. The reader then carries on with the rest.

### 4.3 Decoders: one thread per hardware thread
Each thread owns one `npk::Decoder`: the zstd context, the libdeflate decompressor
and a scratch buffer, reused for every entry. For each entry of its task:

- **Stored entries** (mode 0, not shared, not empty) are not copied. The decoder
  checks `crc_packed`, decrypts in place if needed, checks `crc_raw`, and the
  `WriteJob` points into the chunk. The chunk then stays alive until the file is
  written.
- **Everything else** takes `raw_size` bytes from the `OutputBudget`, allocates
  the buffer without zero-filling it, and calls `Decoder::decode()`.
- **Unnamed entries** get their extension from the decoded bytes here
  (`npk::output_path`).
- **A failed entry** gets `status = failed` and the error message. Its buffer goes
  back to the budget at once.

### 4.4 Writers: up to 32 threads, `writers_allowed` of them active
All 32 threads start at once. A writer whose index is at or above
`Tunables::writers_allowed` parks on a condition variable. Changing the number of
writers therefore never creates or destroys a thread. Having more writer threads
than cores is fine, because a writer spends most of its time inside the OS.

For each job a writer:
1. opens `<out>/<path>` (create, truncate). On Windows it uses the extended-length
   path form, so paths over 260 characters work.
2. writes the data, retrying short writes. If the folder has vanished (`ENOENT`),
   it creates it and tries once more. If too many files are open (`EMFILE`), it
   lets the writeback tracker close some and tries once more.
3. on Linux, calls `sync_file_range(SYNC_FILE_RANGE_WRITE)`. This starts writing
   the file to disk without waiting for it.
4. hands the open file to the `WritebackTracker` (§6). On Windows and other
   systems it just closes it.
5. sets `status = written` and updates `Progress`. Destroying the job frees the
   buffer and its budget, or its hold on the chunk.

Fatal errors stop the whole pipeline: disk full, quota exceeded, read-only
filesystem (§8). Any other write error only fails that file.

## 5. Memory, backpressure and why it cannot deadlock

There are two budgets, both counted in **bytes**:

| Budget | Taken by | Given back by |
|---|---|---|
| `ReadBudget` (⅓) | Reader, per chunk | last owner of the chunk (decoder, or a writer for stored entries) |
| `OutputBudget` (⅔) | Decoder, per decoded entry | writer, after writing |

**Fairness and oversized requests.** `ByteBudget` grants requests in arrival order
(a ticket queue). A request larger than the whole capacity is granted once nothing
else is held. Without the ordering, a 300 MB entry could wait forever while small
requests keep slipping in ahead of it. With it, the budget drains, the big entry
gets its turn, and the small ones follow.

**No deadlock.** Writers only ever wait for input, never for a budget. So whatever
is in the WriteQueue is always eventually written, and its output bytes come back.
A decoder that waits for output bytes is therefore waiting for writers, and they
make progress. A decoder never waits for read bytes. The reader waits for read
bytes, which come back when decoders and writers finish with the chunks. The waits
form a chain (reader → decoders → writers) with no cycle. The one exception, a
request bigger than the whole budget, is handled by the "granted when nothing else
is held" rule above.

The queues are bounded too (DecodeQueue 4096 tasks, WriteQueue 8192 jobs). Their
contents are already limited by the budgets; the bounds are only a safety net.

## 6. Writeback and the OS file cache

On Linux, every byte written sits in RAM as a dirty page until the kernel writes it
out. After that it stays cached in case it is read again, which it never is. With
~60 GB written, the cache pushes everything else out of RAM. npkext dealt with this
with a global `sync()` every 256 MB. That stalls every disk in the system, and the
extractor waits for it.

`WritebackTracker` handles each file on its own instead:
1. The writer has already started that file's writeback (`SYNC_FILE_RANGE_WRITE`).
2. The tracker keeps the file open in a FIFO.
3. When the FIFO holds more than 64 MiB or 256 files, the oldest file is waited on
   (`sync_file_range` with `WAIT_BEFORE | WRITE | WAIT_AFTER`). This is usually
   instant, since its writeback started long ago. Its pages are then dropped
   (`posix_fadvise(DONTNEED)`) and the file is closed. The writer that submitted the
   newest file does this outside the tracker's lock. A disk that falls behind
   therefore slows the writers down, as it should.
4. Bytes count as **on disk** only after step 3 (`Progress::bytes_on_disk`). The
   tuner doesn't use this number, because it trails the writes by up to 64 MiB. It
   uses the bytes handed to the OS. Because of step 3, at most 64 MiB / 256 files
   can be ahead of the disk, so after the first second that rate *is* the disk's
   rate, without the lag.

`drain()` at the end waits for everything still in the FIFO, so "done" means the
data really is on the disk (as far as `sync_file_range` can promise; no `fsync`).

The Windows file cache works differently and needs none of this. There, `submit()`
closes the file and counts its bytes.

## 7. The tuner

Every thread keeps a `StageClock`: the nanoseconds it spent **working**, **waiting
for input**, **waiting for output space** and **parked**. A reading taken mid-wait
includes the wait so far, so a writer blocked in a 2-second write shows up at once.

Every 500 ms (one *window*) the tuner takes the difference since the last look. For
each stage it computes the share of time spent in each state; parked writers are
left out.

| Pattern | Meaning | Action |
|---|---|---|
| writers work > 85 %, **or** decoders wait for output > 50 % | writing is the limit | probe the number of writers |
| reader works > 85 %, decoders wait for input > 50 % | reading is the limit | double the read block, up to 32 MiB |
| decoders work > 85 % | CPU is the limit | nothing to tune |
| otherwise | balanced, or waiting on something outside | nothing |

Either signal alone means "writing is the limit":
- **Busy writers** catch a hard disk shared by reading and writing. There the reader
  and the writers are both busy and the decoders wait for *input*, yet fewer writers
  (fewer seeks) may still help.
- **Decoders waiting for output** catch the many-tiny-files case. In a measurement
  with 600,000 files of 1–8 KB, two writers showed only 56 % "working": the rest
  went into taking jobs off the queue while 8 decoders kept it full. Meanwhile the
  decoders spent 95 % of their time waiting to hand work on, and 8 writers were 30 %
  faster.

**Probing writers (hill climbing).** Throughput is scored as
`bytes written + 64 KiB × files written` (§6.4 explains why written bytes are
honest). The 64 KiB is a rough per-file cost, so that a stretch of tiny files and a
stretch of big textures score alike. Scores are compared in **samples** of several
windows. A probe works like this:

1. Take the mean of the last 4 samples at the current W (the baseline).
2. **Too uneven to compare?** If the samples vary by more than 35 % (standard
   deviation over the mean), double the sample length and collect again: 1 s, 2 s,
   4 s, up to 8 s. Only if 8 s samples are still that uneven does the tuner give up,
   keep W, and look again in 30 s. The VirtualBox disk used in development stalls
   for whole seconds; on it, half-second samples gave readings from −99 % to +206 %
   for changes that make no real difference (§13).
3. Change W by **doubling or halving**: 2 → 4 → 8 → 16 (at most 32), or 2 → 1 (at
   least 1). Throughput grows roughly with the logarithm of W. With +1 steps each
   step gained ~5 %, too little to tell from noise, and the climb stalled at 2 while
   8 was 30 % faster.
4. Skip 1 window to let it settle, then measure 4 samples.
5. Keep the change if it gains more than 5 % **and** more than the baseline's own
   variation (a smaller gain could be luck). Then probe again in the same direction
   straight away, with the samples just measured as the new baseline. If the
   probe's own samples vary by more than 35 %, the result is *inconclusive* and
   treated as "did not help". Otherwise go back to the previous W, turn the
   direction around, and wait 30 s.

The data mix and the disk change during a run (texture archives versus thousands of
tiny bxml files, an SSD's fast cache filling up). Probing therefore never stops: it
resumes after the 30 s pause and whenever a big archive starts. Writers start at
**2**: low enough not to swamp a hard disk, while fast storage climbs to 8 within
the first ~20 s. With `--verbose` every decision is logged:

```
tuner: trying 4 writers instead of 2
tuner: writers 2 -> 4 (+12 %)
tuner: trying 8 writers instead of 4
```

This is a heuristic. The thresholds come from the measurements in §13 and may
change. They are constants in `tuner.cpp`, not options.

## 8. Errors and stopping

- **Per entry** (checksum, codec, unreadable range, write error): `status = failed`,
  `error` set, and the pipeline goes on.
- **Fatal** (disk full, quota, read-only filesystem, output folder unusable): the
  first such error is recorded and `request_stop()` is called on the pipeline's
  stop source. Every wait uses that stop token, so all threads stop promptly.
  Entries not reached keep `status = pending`.
- **Cancellation from outside** (Ctrl+C in the app) uses the same stop source.
- The `_archives/` index is written either way. Entries not reached are recorded
  as `skipped`, as npkext does when it is interrupted.

## 9. Correctness rules the pipeline relies on

- **One entry per output path.** Writers run in parallel, so two jobs for the same
  path would race. The planner guarantees there are none. If two planned writes
  still land on the same path (an archive and a nested one both keep a file, or
  names differing only in case on Windows), the planner keeps the one npkext would
  have left on disk: the last in plan order. The earlier one becomes `skip` with
  `replaced_by` set. On Windows the comparison ignores case.
- **Decisions are made before extraction.** Overrides are fixed by the plan.
  npkext lets a lower-priority copy fill in when the winning copy fails to decode.
  The pipeline doesn't: the failure is reported instead. Damaged entries are rare,
  and handling them would tie writers to an order.
- **Output order doesn't matter.** Files may finish in any order. Only the set of
  files and their bytes are guaranteed.

## 10. Public interface

```cpp
npk::pipeline::Options options;
options.output_root = "out";
options.verify = true;                // check crc_packed and crc_raw
options.log = [](std::string_view line) { ... };   // tuner decisions (verbose)

npk::pipeline::Progress progress;     // atomics, read by a UI thread
std::stop_source stop;                // e.g. Ctrl+C
npk::pipeline::Summary summary = npk::pipeline::run(plan, options, progress, stop.get_token());
```

`Summary` holds the files written, failed and not reached, the bytes, the elapsed
time, the first fatal error, the tuner's final settings, and whether the index was
written.

## 11. Testing

- `pipeline_tests`:
  - queue and budget behaviour (blocking, close, stop, fairness, oversized requests);
  - an end-to-end run on the miniature installation;
  - a stress run with a tiny memory budget, so every entry is oversized and the
    queues are always full;
  - archives whose entries share stored bytes;
  - damaged entries;
  - a fatal error forced by a read-only output folder.
- `compare_with_npkext.py --extractor build/app/npkextpp` runs the real executable
  against npkext on random installations, including the `_archives/` index.
- `tune_sim` runs the pipeline and the tuner against a simulated SSD or HDD
  (`docs/simulated_disk.md`), for tuner work without real disks.

## 12. Later

- **io_uring (Linux) / IOCP (Windows)** in place of writer threads, when threads
  turn out to be the limit.
- **Preallocating output files** (`fallocate`, `FileAllocationInfo`) to reduce
  fragmentation on hard disks and NTFS.
- **A second reader** when the archives sit on different physical disks.

## 13. Measurements

All on one development machine: a VirtualBox VM with 8 cores and 15 GB RAM. Its
virtual disk writes ~31 MB/s sequentially and reads ~60 MB/s, and stalls in
bursts. The data is synthetic (`tests/bench/make_install.py`): 2.3 GiB in 53,000
files (two LZ4 texture archives, 30,000 small UI files, 20,000 zlib level files,
stored audio, and a patch layer overriding 5,000 files). Every result below was
byte-identical to npkext's output.

**What the tools cost** (archives in the OS cache, output on a RAM disk; the disk
taken out):

| Tool | Time | vs npkext |
|---|---|---|
| npkext (Python) | 31.4 s | 1× |
| core_extract (C++, one thread) | 4.1 s | 7.7× |
| npkextpp (pipeline, 8 cores) | 2.1 s | 15× |

**On the VM's disk** every tool takes the same time, because the disk is the limit:

| Tool | Time |
|---|---|
| npkext (Python) | 170 s |
| core_extract (C++, one thread) | 158 s |
| npkextpp, with every tuner version tried during development | 157–173 s |

The rewrite only pays off when the disk is faster than the old tool. A real SSD is,
by a wide margin: npkext manages ~14–76 MiB/s, while the pipeline produced
1.1 GiB/s when the disk was out of the way.

**Writer counts on the VM disk** (texture + UI subset, fixed W, the tuner off):

| W | first pass | repeat 1 | repeat 2 |
|---|---|---|---|
| 2 | 107 s | 81 s | 79 s |
| 4 | 84 s | 84 s | 85 s |

The first pass suggested 4 writers were 22 % faster. The repeats showed that was
noise from the VM: the writer count makes at most ~5 % difference on this disk.
Lessons: never conclude from one run on this machine. And the tuner itself needs
long samples and noise checks (§7) so it doesn't chase such bursts.

**Writer counts on fast storage** (600,000 files of 1–8 KB, 2.6 GiB, RAM disk; two
rounds each, both agree):

| | Time |
|---|---|
| W = 2 fixed | 17.2–17.5 s |
| W = 8 fixed | 12.2 s |
| tuned (starts at 2, reaches 4, tries 8 before the run ends) | 15.4–15.9 s |

The tuner needs about 10 s per step (warm-up, baseline, probe), so it pays off on
long runs, which is what it is for: a full installation takes far longer than that
even on fast hardware. On the VM disk it leaves W where it is or makes changes that
don't matter there.

Not measured yet: a real hard disk, a real SSD/NVMe, and Windows (Defender scanning
every new file is expected to make more writers worthwhile). The numbers above
should be redone there.
