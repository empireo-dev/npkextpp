# npkextpp

C++ rewrite of [npkext](../npkext): extracts NeoX **NXPK** (`.npk`) archives,
recovers file names from the game's `resdb` manifest, rebuilds the game's file
tree, and records every archive in `_archives/` so it can be rebuilt. The output
is byte-identical to npkext's, including the index.

Whole installations are unpacked on all cores (see [docs/pipeline.md](docs/pipeline.md)).
A single file is extracted on the calling thread.

```bash
# everything
npkextpp --install "path/to/Magic Awakened" --out unpacked

# archives by name, searched in the whole installation (all copies merged)
npkextpp --install "path/to/Magic Awakened" --in character.npk ui --out unpacked

# one file, by its path in the game's tree
npkextpp --install "path/to/Magic Awakened" --entry ui/login/bg.png --out .
```

`npkextpp --help` lists every option. The options and exit codes are the same as
npkext's (0 ok, 1 some entries failed, 2 bad input, 130 interrupted), plus:

- `--scan` looks for archives in the whole installation. Without it, the app only
  checks which archives of the game's known list
  ([core/data/known_archives.txt](core/data/known_archives.txt), compiled in) are
  there, and lists the others as missing. While no archive of that list is found
  (the list is still empty), it scans on its own.
- `--overwrite` writes every file again. Without it, files already in the output
  folder with the right size are kept, so a second run, or one after an
  interrupted run, writes only what is missing.

What the screen shows, in a terminal (redrawn every 50 ms, drawn with
[FTXUI](third_party/README.md)):

1. the name in a box, then the installation, the archives found, the names and the plan;
2. the disk test: the median of random 4 KiB reads and the write speed, and the
   drive type assumed from them (SSD or HDD), which decides how the tuner searches;
3. **Pipeline:** the reading buffer size and the number of writers as the tuner
   sets them, and for the reader, the decoders and the writers the share of the
   last 2 s spent busy, starved (waiting for input) and blocked (waiting for
   output), with the average time per block or file;
4. **Progress:** a bar, the percentage, files and bytes done, speed, ETA;
5. **Archives:** every archive in two columns (one on a narrow terminal), each with
   its own bar, files done and ETA; missing ones are marked. When they don't fit
   the terminal, finished ones fold into one line and the waiting ones are cut short.

Redirected to a file, it prints the same without colours, a progress line every
10 s instead of the live part, and the archive list at the end.

## Layout

| Folder | Target | What it is |
|---|---|---|
| `core/` | `npk::core` (static lib) | The NXPK format: TOC, decrypt, decompress, checksums, resdb, mount rules, names, planning. No threads, no I/O policy. Take this into other projects (nxscriptpp). |
| `pipeline/` | `npk::pipeline` (static lib) | Multi-threaded extraction of a whole plan: Reader → Decoders (all cores) → Writers (count tuned at run time). Design: [docs/pipeline.md](docs/pipeline.md). |
| `app/` | `npkextpp` (exe) | Command line front end. One self-contained executable. |
| `tests/` | `core_tests`, `pipeline_tests`, `core_extract` | Built with `-DNPKEXTPP_BUILD_TESTS=ON`. |
| `third_party/` | | Vendored dependency archives, see [third_party/README.md](third_party/README.md). |

### The core in one example

```cpp
auto installation = npk::Installation::open(root);                    // Result<Installation>
auto manifest = npk::ResourceManifest::from_files(installation->resdb_files());
npk::Planner planner(*manifest, &*installation);
npk::Plan plan = planner.build(installation->archives(), {});         // who writes what, where

npk::Decoder decoder;                                                 // one per thread, reused
auto archive = npk::Archive::open(path);
auto bytes = decoder.read(*archive, *archive->find("login\\bg.png")); // check, decrypt, decompress, check
```

## Build

Needs CMake 3.25+ and a C++23 compiler (GCC 13+, Clang 17+, MSVC 2022 17.6+). No network access.

```bash
cmake -S . -B build -G Ninja
cmake --build build
```

The result is a single file, `build/app/npkextpp` (`npkextpp.exe`), with no
DLLs, configs or resources next to it:

- MSVC: static runtime (`/MT`).
- MinGW: `-static`.
- Linux: libstdc++/libgcc linked in, glibc dynamic. `-DNPKEXTPP_FULLY_STATIC=ON` links fully statically.

## Tests

```bash
cmake -S . -B build -G Ninja -DNPKEXTPP_BUILD_TESTS=ON
cmake --build build
ctest --test-dir build
```

Comparison with the Python npkext on random synthetic installations (needs npkext
and its Python dependencies):

```bash
python3 tests/compare_with_npkext.py --npkext ../npkext --extractor build/app/npkextpp --index --seeds 20
```

Benchmark on a synthetic ~2 GB installation (put it on the disk you want to measure):

```bash
python3 tests/bench/make_install.py --npkext ../npkext --out build/bench
python3 tests/bench/run_bench.py --npkext ../npkext --install build/bench/install --work build/bench \
    --core build/tests/core_extract --app build/app/npkextpp
```

The tuner against a simulated SSD or HDD, writing nothing
([docs/simulated_disk.md](docs/simulated_disk.md)):

```bash
build/tests/tune_sim --disk ssd --curve --file-size 65536
build/tests/tune_sim --disk hdd --install build/bench/install --set switch_cost=300us
```

## Using the core from another project

```cmake
set(NPKEXTPP_BUILD_APP OFF)          # default when not top-level
set(NPKEXTPP_BUILD_PIPELINE OFF)     # only if you don't need the threaded extractor
add_subdirectory(path/to/npkextpp)
target_link_libraries(your_target PRIVATE npk::core)
```
