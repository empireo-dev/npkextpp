#pragma once

// Tells a hard disk from an SSD before the run. pipeline::run() calls it when
// Options::disk_kind is not set; the app calls it itself to show the result
// first. See docs/pipeline.md §7.
//
// It times random 4 KiB reads that bypass the OS cache. A hard disk has to move
// its head and wait for the platter for each one, which takes 2 ms at the very
// least (usually 5-15 ms); an SSD answers in 0.05-0.3 ms. It decides on the 90th
// percentile: a hard disk is one where at least a tenth of the reads take 1 ms or
// more. (Not the median: a disk's own cache, or a virtual machine host's, can
// answer most reads of a recently written file at SSD speed.)
//
// What it reads, the first that works: the largest archive, if it is on the
// output folder's volume; another existing file there (in the output folder or
// a folder above it); a 16 MiB file it writes into the output folder. Only files
// not written in the last 10 minutes count as existing, since a cache may still
// hold younger ones. The test file is written in any case, to show the write
// speed, and deleted. It all takes about 1 s on a hard disk, a fraction on an SSD.

#include "npk/pipeline/pipeline.hpp"

#include <filesystem>
#include <string>
#include <vector>

namespace npk::pipeline {

struct DiskProbe {
    DiskKind kind = DiskKind::unknown;   // unknown: the probe could not run
    double median_read_ms = 0;           // shown
    double slow_read_ms = 0;             // 90th percentile: decides
    std::size_t reads = 0;
    double write_mb_per_s = 0;           // writing the 16 MiB test file, flushed (0: could not)
    std::string read_from;               // "an archive", "an existing file" or "the test file"
    std::string problem;                 // why it could not run
};

constexpr double kHddReadMs = 1.0;       // 90th percentile read time from which a disk is a hard disk

[[nodiscard]] DiskKind classify(double slow_read_ms) noexcept;

[[nodiscard]] DiskProbe probe_disk(const std::filesystem::path& output_root,
                                   const std::vector<std::filesystem::path>& archives);

} // namespace npk::pipeline
