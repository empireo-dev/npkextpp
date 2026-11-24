#pragma once

#include <atomic>
#include <cstdint>

namespace npk::pipeline {

// Counters updated by the pipeline threads, read by the UI.
struct Progress {
    std::atomic<std::uint64_t> files_written{0};
    std::atomic<std::uint64_t> files_failed{0};
    std::atomic<std::uint64_t> bytes_read{0};
    std::atomic<std::uint64_t> bytes_written{0};
    std::atomic<std::uint32_t> archives_done{0};
    std::atomic<std::uint32_t> writers_active{0};
    std::atomic<std::uint32_t> read_block_kib{0};
};

} // namespace npk::pipeline
