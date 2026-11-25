#pragma once

#include "npk/plan.hpp"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>

namespace npk::pipeline {

// One archive's share of the work, in plan order (groups, then their sources).
struct SourceProgress {
    std::atomic<std::uint32_t> files_done{0};     // written or failed
    std::atomic<std::uint32_t> files_failed{0};
    std::atomic<std::uint64_t> bytes_done{0};     // raw bytes of the written files
};

// How one stage spent its time over the last ~2 seconds (published by the tuner
// thread every 500 ms). Shares are of the time its threads were not parked.
struct StageLoad {
    std::atomic<float> working{0};
    std::atomic<float> waiting_input{0};
    std::atomic<float> waiting_output{0};
    std::atomic<float> ms_per_item{0};            // working time per block read / file decoded / file written
};

// Counters updated by the pipeline threads, read by the UI at any time.
struct Progress {
    // `sources`: plan.source_count(), to get per-archive counts (0: none).
    explicit Progress(std::size_t sources = 0)
        : sources_(sources ? std::make_unique<SourceProgress[]>(sources) : nullptr)
        , source_count_(sources)
    {
    }

    std::atomic<std::uint64_t> files_written{0};
    std::atomic<std::uint64_t> files_failed{0};
    std::atomic<std::uint64_t> files_decoded{0};
    std::atomic<std::uint64_t> blocks_read{0};
    std::atomic<std::uint64_t> bytes_read{0};      // stored bytes read from archives
    std::atomic<std::uint64_t> bytes_written{0};   // decoded bytes handed to the OS
    std::atomic<std::uint64_t> bytes_on_disk{0};   // ... of which confirmed written out (Linux), see WritebackTracker
    std::atomic<std::uint32_t> archives_started{0};
    std::atomic<std::uint32_t> writers_active{0};
    std::atomic<std::uint32_t> read_block_kib{0};
    std::atomic<const ArchiveSource*> current_source{nullptr};   // being read; lives in the Plan

    StageLoad reader, decoders, writers;
    std::atomic<bool> reading_done{false};    // the reader has read everything
    std::atomic<bool> decoding_done{false};   // ... and the decoders have decoded it

    [[nodiscard]] std::span<SourceProgress> sources() noexcept { return {sources_.get(), source_count_}; }
    [[nodiscard]] std::span<const SourceProgress> sources() const noexcept { return {sources_.get(), source_count_}; }

private:
    std::unique_ptr<SourceProgress[]> sources_;
    std::size_t source_count_ = 0;
};

} // namespace npk::pipeline
