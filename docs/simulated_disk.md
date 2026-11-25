# Simulated disk

A fake disk for designing and testing the tuner (`docs/pipeline.md` §7)
without a real SSD or HDD. Writers hand it a file size and it sleeps for as
long as the modelled disk would take. Nothing is written anywhere.

It exists only for tests and tools. The app never uses it.

## 1. What it has to show

The tuner looks at three signals: throughput (bytes plus files per second),
how busy the writers are, and how long the decoders wait for output space.
The fake disk only has to make those signals behave like a real disk's:

- **SSD:** more writers help a lot with small files and a little with big
  ones, up to the drive's parallelism. Past that, more writers help nothing
  and slowly cost a bit.
- **HDD:** one head. Writers interleaving their data makes it seek, so past
  one or two writers, more writers make it slower. Reading the archives from
  the same disk competes with writing.
- **Both:** throughput is uneven. Real disks stall now and then, which is
  what made the first tuner draw wrong conclusions on the VM.

## 2. The model

One model with parameters; `ssd()` and `hdd()` are presets of it.

A file write of `n` bytes by writer `w`:

1. **File cost.** Take one of `channels` slots and hold it for
   `file_latency`. This is creating, opening and closing the file: metadata
   and the journal. Up to `channels` files pay it in parallel. Above
   `channels` writers, each extra writer adds `contention` (a fraction) to the
   file latency: queueing and lock contention in the drive and the
   filesystem.
2. **Data.** The bytes go through one shared pipe at `bandwidth`, in pieces
   of `chunk` bytes. When the piece before came from a different stream
   (another writer, or the reader), the pipe first spends `switch_cost`:
   the seek that is left after the OS and the drive have reordered the
   writes.

**Write cache.** Writers may run up to `buffer / bandwidth` ahead of the disk
(64 MiB by default) before they wait, as with a real OS write cache. Without
it, a writer that woke up late under full CPU load left the disk idle, which
made runs with few writers slow and different every time (one writer on the
HDD: 47, 33 and 25 MB/s in three runs; with the buffer 56.0 and 56.5). Reads
are not buffered.

Reading a block of `n` bytes is step 2 only, with the reader as its own
stream, when `shared_with_input` is set.

**Time.** Sleeps of tens of microseconds overshoot badly (by about 0.5 ms
on the VM), which made the first version several times slower than modelled.
So every stream (each writer, and the reader) keeps its own virtual clock,
moved on by the modelled costs, and only sleeps once it is more than 1 ms
ahead of the real clock. The shared parts (the channels and the pipe) book
their time from the real clock, so one stream running ahead doesn't hold the
others back, while a stream that overslept may claim back the time it lost.
A stream idle for more than 3 ms (waiting for work, not for the disk) starts
again from the real clock.

**Stalls.** On average every `stall_every` seconds of disk time, the pipe is
blocked for `stall_length` (exponentially distributed gaps, fixed seed).

| parameter            | SSD preset | HDD preset |
|----------------------|-----------:|-----------:|
| `channels`           | 8          | 1          |
| `file_latency`       | 80 µs      | 500 µs     |
| `contention`         | 0.02       | 0          |
| `bandwidth`          | 1500 MB/s  | 150 MB/s   |
| `chunk`              | 1 MiB      | 1 MiB      |
| `switch_cost`        | 0          | 1 ms       |
| `shared_with_input`  | no         | yes        |
| `stall_every`        | 5 s        | 10 s       |
| `stall_length`       | 20 ms      | 300 ms     |
| `buffer`             | 64 MiB     | 64 MiB     |

What that gives, measured with `tune_sim --curve` (score = MB/s plus 64 KiB
per file, as the tuner counts it):

| writers | SSD 4 KiB files | SSD 64 KiB | SSD 4 MiB | HDD 4 KiB | HDD 64 KiB | HDD 4 MiB |
|--------:|----------:|----------:|----------:|---------:|---------:|---------:|
| 1       | 820       | 1050      | 1470      | **130**  | **138**  | **150**  |
| 2       | 1540      | 1720      | 1510      | 73       | 93       | 133      |
| 4       | 2670      | 2540      | **1520**  | 68       | 91       | 133      |
| 8       | 4410      | 2910      | 1520      | 68       | 91       | 133      |
| 16      | **5410**  | 2980      | 1520      | 68       | 91       | 133      |
| 32      | 4630      | **2990**  | 1520      | 68       | 91       | 133      |

- **SSD:** small files gain almost linearly up to 8 writers, still gain to
  16 and lose at 32 (contention). Big files are nearly saturated by one
  writer.
- **HDD:** one writer is best. A second writer makes the pipe switch stream
  on every file, which costs more than the file costs it overlaps. With
  `switch_cost` below `file_latency` the HDD instead stays roughly flat.

Every parameter can be changed from the command line, so other disks (a
SATA SSD, a USB stick, a NAS) are a few numbers away.

## 3. Hooking it in

`npk::pipeline::FakeDisk` is a small interface in the pipeline library:

```cpp
class FakeDisk {
public:
    virtual ~FakeDisk() = default;
    virtual void write_file(unsigned writer, std::size_t bytes) = 0;   // blocks
    virtual void read(std::size_t bytes) = 0;                          // blocks
};
```

`Options::fake_disk` (null by default) points at one. When it's set:

- writers call `write_file` instead of creating files, and nothing goes to
  the writeback tracker;
- the reader still reads the real archives, then calls `read`, and keeps
  the archive pages in the OS cache instead of dropping them, so a run reads
  them from RAM after the first pass;
- no output folders are created.

The model itself (`SimulatedDisk`) lives in `tests/sim/`.

## 4. The tool

`tune_sim` runs the real pipeline (real reader, decoders and tuner) against a
simulated disk:

```
tune_sim --install DIR [--in NAME...] --disk ssd|hdd [--set name=value ...]
         [--writers N]       fixed writer count, no tuner
         [--repeat N]        go through the archives N times, for runs long enough to tune
         [--decoders N]
         [--schedule N N ... [--period S]]   cycle through fixed writer counts, S seconds each
         [--trace]           print the tuner's raw readings every 0.5 s
         [--input same|ram|ssd|hdd [--input-set name=value ...]]
                             where the archives are: the output disk (default),
                             free, or a second simulated disk
         [--kind ssd|hdd]    tune as for this kind of disk (default: the --disk preset)
         [--real DIR]        write real files into DIR instead (emptied first);
                             the archives are read cold and the disk probe runs
tune_sim --disk ssd|hdd --curve --file-size BYTES [--seconds S] [--set ...]
```

Values take units: `--set file_latency=200us --set bandwidth=500MB/s
--set chunk=256KiB --set shared_with_input=no`. The archives are read once
before the run so they come from the OS cache; they need to fit in RAM.

`--curve` measures the disk alone, without the pipeline: throughput for 1, 2,
4 … 32 writers writing files of one size. That shows what the right answer
is before asking the tuner.

Grading a tuner change: for each scenario, the best fixed writer count's
throughput is the target. The tuned run should get within about 10 % of it,
and never end up below the default of 2 writers.

## 5. What it doesn't model

- **CPU cost of writing.** Real writes cost CPU in the kernel; sleeping costs
  none. This matters when all cores are decoding.
- **The OS write cache.** The pipeline's writeback tracker keeps it small on
  Linux, so writes are treated as going straight to the disk.
- **Any particular drive.** The presets are plausible, not measured. One run
  on a real SSD at the end checks the model. The real hard disk behind this
  project's VM is slower (15 MB/s on the benchmark installation) and flatter
  (1, 2 and 4 writers within 5 %) than the HDD preset; `--set
  switch_cost=300us` is closer to it.
- **The disk probe.** A simulated disk has no random reads to time, so
  `tune_sim` names the kind of disk itself (`--kind`); `--real` runs the probe.
