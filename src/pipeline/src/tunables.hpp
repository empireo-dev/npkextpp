#pragma once

#include "npk/pipeline/pipeline.hpp"

#include <atomic>
#include <cstddef>

namespace npk::pipeline {

// How the tuner searches for the writer count on each kind of disk
// (docs/pipeline.md §7). An SSD gains 2-3x from many writers, so the search is
// wide and takes small gains. A hard disk gains little or loses from more
// writers, and its measurements are noisy, so it starts at one, stays within
// a few, and moves only on a clear gain.
struct WriterProfile {
    unsigned initial;   // writers to start with
    unsigned max;       // the search stays at or below this
    double tie;         // closer than this: the same; fewer writers win
};
constexpr WriterProfile kSsdProfile{4, 32, 0.03};
constexpr WriterProfile kHddProfile{1, 4, 0.10};

// An unknown disk is searched like an SSD: too few writers on an SSD costs far
// more than too many on a hard disk.
constexpr const WriterProfile& profile_for(DiskKind kind) noexcept
{
    return kind == DiskKind::hdd ? kHddProfile : kSsdProfile;
}

// Settings the Tuner changes while the pipeline runs; read by the stages.
struct Tunables {
    static constexpr std::size_t kMinReadBlock = std::size_t{4} << 20;
    static constexpr std::size_t kMaxReadBlock = std::size_t{32} << 20;
    static constexpr unsigned kMaxWriters = 32;

    std::atomic<std::size_t> read_block{std::size_t{8} << 20};
    std::atomic<unsigned> writers_allowed{2};
};

} // namespace npk::pipeline
