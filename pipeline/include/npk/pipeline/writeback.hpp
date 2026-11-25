#pragma once

#include "npk/pipeline/output_file.hpp"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <mutex>

namespace npk::pipeline {

// Keeps written output from piling up in the OS page cache without a global
// sync (docs/pipeline.md §6). A writer hands over each file after
// start_writeback(); the tracker keeps it open in a FIFO. Once the FIFO holds
// more than max_bytes or max_files, the oldest files are finished on the
// calling writer's thread: wait until on disk, drop from the cache, close. A
// disk that falls behind therefore slows the writers down. Also the source of
// "bytes that really reached the disk" for the tuner.
//
// Where finish_writeback() is a no-op (not Linux) submit() simply closes the file.
class WritebackTracker {
public:
    static constexpr std::size_t kDefaultMaxBytes = std::size_t{64} << 20;
    static constexpr std::size_t kDefaultMaxFiles = 256;

    explicit WritebackTracker(std::size_t max_bytes = kDefaultMaxBytes, std::size_t max_files = kDefaultMaxFiles);
    ~WritebackTracker();
    WritebackTracker(const WritebackTracker&) = delete;
    WritebackTracker& operator=(const WritebackTracker&) = delete;

    void submit(OutputFile file, std::uint64_t size);
    void finish_oldest(std::size_t count);   // frees file handles (EMFILE)
    void drain();                            // finishes everything still pending

    [[nodiscard]] std::uint64_t bytes_on_disk() const noexcept { return on_disk_.load(std::memory_order_relaxed); }

private:
    struct Pending {
        OutputFile file;
        std::uint64_t size = 0;
    };
    void finish(Pending& pending) noexcept;

    std::size_t max_bytes_;
    std::size_t max_files_;
    std::mutex mutex_;
    std::deque<Pending> pending_;
    std::uint64_t pending_bytes_ = 0;
    std::atomic<std::uint64_t> on_disk_{0};
};

} // namespace npk::pipeline
