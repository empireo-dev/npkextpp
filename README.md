# npkextpp

C++ rewrite of [npkext](../npkext): extracts NeoX **NXPK** (`.npk`) archives.
Status: skeleton — the structure is in place, the code is not written yet.

## Layout

| Folder | Target | What it is |
|---|---|---|
| `core/` | `npk::core` (static lib) | The NXPK format: TOC, decrypt, decompress, checksums, resdb, mount rules, names, planning. No threads, no I/O policy. Take this into other projects (nxscriptpp). |
| `pipeline/` | `npk::pipeline` (static lib) | Multi-threaded extraction of a whole plan: Reader → Decoders (all cores) → Writers (count tuned at run time). |
| `app/` | `npkextpp` (exe) | Command line front end. One self-contained executable. |
| `tests/` | `core_tests`, `pipeline_tests` | Built with `-DNPKEXTPP_BUILD_TESTS=ON`. |
| `third_party/` | | Vendored dependency archives, see [third_party/README.md](third_party/README.md). |

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

## Using the core from another project

```cmake
set(NPKEXTPP_BUILD_APP OFF)          # default when not top-level
set(NPKEXTPP_BUILD_PIPELINE OFF)     # only if you don't need the threaded extractor
add_subdirectory(path/to/npkextpp)
target_link_libraries(your_target PRIVATE npk::core)
```
