#pragma once

#include <atomic>
#include <cstddef>

namespace npk::pipeline {

// Settings the Tuner changes while the pipeline runs; read by the stages.
struct Tunables {
    static constexpr std::size_t kMinReadBlock = 4u << 20;
    static constexpr std::size_t kMaxReadBlock = 32u << 20;

    std::atomic<std::size_t> read_block{8u << 20};
    std::atomic<unsigned> writers_allowed{2};
};

} // namespace npk::pipeline
