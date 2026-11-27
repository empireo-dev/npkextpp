# Building npkextpp

The result is one file, `npkextpp` (Linux) or `npkextpp.exe` (Windows), that runs
on its own: no installer, no DLLs or configs next to it. Ready-made builds are on
the project's GitHub Releases page; you only need this if you want to build it
yourself.

Everything the build needs is in this repository (the libraries are in
`third_party/`), so no internet connection is needed once you have the sources.

## 1. What you need

| tool | what it is for | minimum version |
|---|---|---|
| a C++ compiler | turns the source code into a program | GCC 13, Clang 17 or Visual Studio 2022 17.6 |
| CMake | prepares the build | 3.25 |
| Ninja | runs the build | any |
| Git | downloads the sources (optional: a ZIP from GitHub works too) | any |

### Windows

1. Install **Visual Studio 2022** ("Community" is free), or the smaller
   **Build Tools for Visual Studio 2022** if you don't want the editor. Both are on
   visualstudio.microsoft.com.
2. In the installer, tick the **Desktop development with C++** workload. It
   brings the compiler, CMake and Ninja; keep "C++ CMake tools for Windows"
   ticked in its list on the right.
3. Optional: install **Git for Windows** (git-scm.com).

Visual Studio's CMake and Ninja work only inside a prepared window: open the
Start menu and run **x64 Native Tools Command Prompt for VS 2022**. All Windows
commands below are typed there.

### Linux

Debian, Ubuntu (24.04 or newer):

```bash
sudo apt install build-essential cmake ninja-build git
```

Fedora:

```bash
sudo dnf install gcc-c++ cmake ninja-build git
```

Arch:

```bash
sudo pacman -S base-devel cmake ninja git
```

Older systems may come with a CMake or GCC that is too old (Ubuntu 22.04 has
CMake 3.22 and GCC 11). Check with `cmake --version` and `g++ --version`.

### macOS (not tested)

Install the Xcode command line tools (`xcode-select --install`), then CMake and
Ninja, for example with Homebrew: `brew install cmake ninja`. Use the Linux
commands.

## 2. Get the sources

```bash
git clone https://github.com/empireo-dev/npkextpp.git
cd npkextpp
```

Without Git: download the ZIP from the GitHub page ("Code", "Download ZIP"),
unpack it, and open a command window in that folder.

## 3. Build

Two commands: the first prepares the build, the second compiles it.

Windows (in the x64 Native Tools Command Prompt):

```bat
cmake --preset build-win
cmake --build --preset build-win
```

Linux:

```bash
cmake --preset build
cmake --build --preset build
```

The first build takes a few minutes; later ones only rebuild what changed. The
program is then at:

| | release | debug |
|---|---|---|
| Linux | `out/bin/npkextpp` | `out/bin-debug/npkextpp` |
| Windows | `out\bin-win\npkextpp.exe` | `out\bin-win-debug\npkextpp.exe` |

Copy that one file wherever you like. To update later, pull the new sources
(`git pull`) and run the second command again.

## 4. Presets

A preset is a named set of build settings (they are in `CMakePresets.json`). Each
system lists only its own; `cmake --list-presets` shows them.

| preset | system | kind | build files | program |
|---|---|---|---|---|
| `build` | Linux, macOS | release | `out/build` | `out/bin` |
| `build-debug` | Linux, macOS | debug | `out/build-debug` | `out/bin-debug` |
| `build-win` | Windows (MSVC) | release | `out/build-win` | `out/bin-win` |
| `build-win-debug` | Windows (MSVC) | debug | `out/build-win-debug` | `out/bin-win-debug` |

- **Release** is optimised: the one to use and to hand out.
- **Debug** is unoptimised, many times slower, and meant for a debugger. Build
  it yourself when you need it; releases never include it.

Everything a build makes stays in `out/`. Deleting `out/` (or one folder in it)
is always safe; the next build makes it again.

## 5. Options

Options go on the first command as `-DNAME=VALUE`, for example
`cmake --preset build -DNPKEXTPP_BUILD_TESTS=ON`. They are remembered: later
runs of the second command keep them. Change one by running the first command
again with the new value.

| option | default | what it does |
|---|---|---|
| `NPKEXTPP_BUILD_TESTS` | `OFF` | Also builds the tests and test tools, into `out/bin*/tests/`. See section 6. |
| `NPKEXTPP_FULLY_STATIC` | `OFF` | Linux: links everything into the program, the C library too, so it runs on any Linux distribution. Without it, it needs a glibc as new as the one it was built on. The Linux release uses it. |
| `NPKEXTPP_BIN_DIR` | per preset (table above) | Where the program and the test binaries are written. |
| `CMAKE_BUILD_TYPE` | `Release` | `Release`, `Debug`, `RelWithDebInfo` (optimised, with debug information) or `MinSizeRel`. The presets set it. |
| `NPKEXTPP_BUILD_PIPELINE` | `ON` | The multi-threaded unpacker (`npk::pipeline`). The program needs it. |
| `NPKEXTPP_BUILD_APP` | `ON` (`OFF` inside another project) | The program itself. Turn it off to build only the libraries. |

What is always the same, without options:

- **Windows (MSVC):** the C++ runtime is linked in (`/MT`), so no Visual C++
  redistributable is needed.
- **Linux:** libstdc++ and libgcc are linked in; only glibc is used from the system.
- **Release builds** use link-time optimisation when the compiler supports it.

## 6. Tests

```bash
cmake --preset build -DNPKEXTPP_BUILD_TESTS=ON
cmake --build --preset build
ctest --preset build
```

On Windows, use `build-win` in all three. That builds and runs `core_tests` and
`pipeline_tests` (a few seconds in release, about a minute in debug), and also
builds two tools:

- `core_extract`: a simple single-threaded unpacker, to compare the real one against.
- `tune_sim`: runs the unpacker against a simulated disk (see `docs/simulated_disk.md`).

The comparisons with the original Python npkext and the benchmarks need Python 3
and npkext; the commands are in the README.

## 7. Without presets

Presets need Ninja. To use another generator or compiler, give the build folder
yourself:

```bash
cmake -S . -B out/build-clang -G Ninja -DCMAKE_CXX_COMPILER=clang++ -DCMAKE_C_COMPILER=clang
cmake --build out/build-clang
```

A build folder named `out/build<something>` writes the program to
`out/bin<something>`; any other folder writes it to `<folder>/bin`.

Some useful variants:

- **Sanitizers** (finding memory and threading bugs, GCC or Clang):
  `cmake -S . -B out/build-asan -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo -DNPKEXTPP_BUILD_TESTS=ON -DCMAKE_CXX_FLAGS=-fsanitize=address,undefined -DCMAKE_EXE_LINKER_FLAGS=-fsanitize=address,undefined`
  (`-fsanitize=thread` for threading bugs, in its own folder).
- **Visual Studio solution:** `cmake -S . -B out/build-vs -G "Visual Studio 17 2022"`,
  then open the `.sln` in `out/build-vs`.
- **MinGW** (GCC on Windows, not tested): from a MinGW shell,
  `cmake -S . -B out/build-mingw -G Ninja`. The program is linked with `-static`.

## 8. Using it inside another project

npkextpp can be added to another CMake project with `add_subdirectory`. It then
builds only the libraries (the program is off), and leaves the output folders
and runtime settings to that project:

```cmake
add_subdirectory(npkextpp)
target_link_libraries(my_tool PRIVATE npk::core)       # the archive format
target_link_libraries(my_tool PRIVATE npk::pipeline)   # plus the multi-threaded unpacker
```

## When something goes wrong

- **`'cmake' is not recognized`, or `No CMAKE_CXX_COMPILER could be found`, on
  Windows:** the window is not the x64 Native Tools Command Prompt.
- **`CMake 3.25 or higher is required`:** your CMake is too old; install a newer one
  (cmake.org, or `pip install cmake`).
- **`Could not find Ninja`:** install Ninja (section 1).
- **Errors about the build folder** after moving the project or switching
  compilers: delete that folder in `out/` and build again.
