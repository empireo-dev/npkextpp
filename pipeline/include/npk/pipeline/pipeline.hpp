#pragma once

#include "npk/error.hpp"
#include "npk/pipeline/fake_disk.hpp"
#include "npk/pipeline/progress.hpp"
#include "npk/plan.hpp"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <stop_token>
#include <string_view>
#include <vector>

namespace npk::pipeline {

// See docs/pipeline.md for how the pipeline works.
struct Options {
    std::filesystem::path output_root;
    bool verify = true;              // check crc_packed and crc_raw of every entry
    bool write_index = true;         // write _archives/ when done (also after a stop)

    // Defaults suit real use; tests change them.
    std::size_t memory_budget = 0;   // bytes for read + decoded data; 0 = min(1 GiB, RAM / 8)
    unsigned decoder_threads = 0;    // 0 = one per hardware thread
    unsigned initial_writers = 2;    // low enough for hard disks; the tuner climbs on faster ones
    bool adaptive = true;            // run the tuner

    // Tests only: writers hand file sizes to this instead of writing files, and
    // no output folders are created. See docs/simulated_disk.md.
    FakeDisk* fake_disk = nullptr;
    // Tests only: instead of tuning, cycle through these writer counts, each for
    // `schedule_period` seconds.
    std::vector<unsigned> writer_schedule;
    double schedule_period = 10;
    // Tests only: log a "trace" line with the tuner's raw readings every window.
    bool trace = false;

    // Tuner decisions and other details, one line each, from a pipeline thread.
    std::function<void(std::string_view)> log;
};

struct Summary {
    std::uint64_t files_written = 0;
    std::uint64_t files_failed = 0;
    std::uint64_t files_not_reached = 0;   // stopped before they were written
    std::uint64_t bytes_written = 0;
    double seconds = 0;
    bool cancelled = false;                // the caller's stop token fired
    std::optional<Error> fatal;            // what stopped the run (disk full, ...)
    std::optional<Error> index_error;      // _archives/ could not be written
    unsigned writers = 0;                  // the tuner's final choices
    std::size_t read_block = 0;
};

// Extracts every entry of the plan that is to be written, on all cores. Each
// entry's status (and error) is set in the plan; skipped entries are left alone.
// `cancel` stops the run early (e.g. Ctrl+C).
[[nodiscard]] Summary run(Plan& plan, const Options& options, Progress& progress, std::stop_token cancel = {});

} // namespace npk::pipeline
