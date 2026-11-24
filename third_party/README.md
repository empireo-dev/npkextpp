# Third-party code

Every dependency is kept here as its **unchanged release archive** plus its
license, so a build never touches the network. CMake (FetchContent) unpacks the
archives into the build tree and checks their SHA-256.

| Library | Version | License | Source | Used for |
|---|---|---|---|---|
| LZ4 | 1.10.0 | BSD-2-Clause | https://github.com/lz4/lz4/releases/tag/v1.10.0 | compression mode 2 (most entries) |
| Zstandard | 1.5.7 | BSD-3-Clause (dual BSD/GPLv2, used under BSD) | https://github.com/facebook/zstd/releases/tag/v1.5.7 | compression mode 3 |
| libdeflate | 1.26 | MIT | https://github.com/ebiggers/libdeflate/releases/tag/v1.26 | compression mode 1 (zlib) and resdb |
| CLI11 | 2.7.2 | BSD-3-Clause | https://github.com/CLIUtils/CLI11/releases/tag/v2.7.2 | command line (app only), single header |

All are compatible with npkextpp's MIT license and with GPLv3/AGPLv3 if the
project is relicensed or linked into nxscriptpp. Their license texts are
embedded in the executable (`npkextpp --licenses`).

To update one: replace the archive, update `URL`/`URL_HASH` in
`CMakeLists.txt` here, and copy the new license file.
