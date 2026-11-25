#pragma once

#include "npk/plan.hpp"

#include <atomic>
#include <cstdint>

namespace npk::pipeline {

// Counters updated by the pipeline threads, read by the UI at any time.
struct Progress {
    std::atomic<std::uint64_t> files_written{0};
    std::atomic<std::uint64_t> files_failed{0};
    std::atomic<std::uint64_t> bytes_read{0};      // stored bytes read from archives
    std::atomic<std::uint64_t> bytes_written{0};   // decoded bytes handed to the OS
    std::atomic<std::uint64_t> bytes_on_disk{0};   // ... of which confirmed written out (Linux), see WritebackTracker
    std::atomic<std::uint32_t> archives_started{0};
    std::atomic<std::uint32_t> writers_active{0};
    std::atomic<std::uint32_t> read_block_kib{0};
    std::atomic<const ArchiveSource*> current_source{nullptr};   // being read; lives in the Plan
};

} // namespace npk::pipeline
