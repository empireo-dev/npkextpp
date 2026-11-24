#pragma once

#include "npk/error.hpp"
#include "npk/pipeline/progress.hpp"
#include "npk/plan.hpp"

#include <cstddef>
#include <functional>
#include <string>

namespace npk::pipeline {

struct Options {
    bool verify = true;              // check crc_packed and crc_raw
    std::size_t memory_budget = 0;   // 0 = automatic (min(1 GiB, RAM / 8))
    std::function<void(const std::string&)> log;   // tuner decisions etc.
};

// Extracts every entry of the plan on all cores. Creates the folders, runs
// Reader → Decoders → Writers with the Tuner alongside, then writes the index.
// Per-entry results end up in PlannedEntry::status; only fatal errors (output
// unwritable, disk full) are returned.
[[nodiscard]] Result<void> run(Plan& plan, const Options& options, Progress& progress);

} // namespace npk::pipeline
