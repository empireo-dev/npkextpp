#pragma once

// Tells a hard disk from an SSD before the run. pipeline::run() calls it when
// Options::disk_kind is not set; the app calls it itself to show the result
// first. See docs/pipeline.md §7.
//
// It times random 4 KiB reads that bypass the OS cache. A hard disk has to move
// its head and wait for the platter for each one, which takes 2 ms at the very
// least (usually 5-15 ms); an SSD answers in 0.05-0.3 ms. The threshold of 1 ms
// sits between the two with room on both sides.
//
// What it reads: the largest archive, when it is on the same volume as the
// output folder and big enough (no writing needed, and it is long enough for
// real seeks); otherwise a 16 MiB file it writes into the output folder and
// deletes. That file is written in any case, to show the write speed. It takes
// about 0.7 s on a hard disk and well under 0.1 s on an SSD.

#include "npk/pipeline/pipeline.hpp"

#include <filesystem>
#include <string>
#include <vector>

namespace npk::pipeline {

struct DiskProbe {
    DiskKind kind = DiskKind::unknown;   // unknown: the probe could not run
    double median_read_ms = 0;
    std::size_t reads = 0;
    double write_mb_per_s = 0;           // writing the 16 MiB test file, flushed (0: could not)
    std::string read_from;               // "an archive" or "a test file"
    std::string problem;                 // why it could not run
};

constexpr double kHddReadMs = 1.0;       // median random read time from which a disk is a hard disk

[[nodiscard]] DiskKind classify(double median_read_ms) noexcept;

[[nodiscard]] DiskProbe probe_disk(const std::filesystem::path& output_root,
                                   const std::vector<std::filesystem::path>& archives);

} // namespace npk::pipeline
