#pragma once

#include <atomic>
#include <cstddef>

namespace npk::pipeline {

// Settings the Tuner changes while the pipeline runs; read by the stages.
struct Tunables {
    static constexpr std::size_t kMinReadBlock = std::size_t{4} << 20;
    static constexpr std::size_t kMaxReadBlock = std::size_t{32} << 20;
    static constexpr unsigned kMaxWriters = 32;

    std::atomic<std::size_t> read_block{std::size_t{8} << 20};
    std::atomic<unsigned> writers_allowed{2};
};

} // namespace npk::pipeline
