#pragma once

#include "npk/pipeline/output_file.hpp"

#include <cstdint>
#include <deque>
#include <mutex>

namespace npk::pipeline {

// Keeps written output from piling up in the OS page cache without a global
// sync. A writer hands over each file after start_writeback(); once enough
// newer data has followed, the oldest files are waited on (sync_file_range
// WAIT), dropped from the cache (posix_fadvise DONTNEED) and closed. Also the
// source of "bytes that really reached the disk" for the tuner.
// On Windows submit() simply closes the file.
class WritebackTracker {
public:
    void submit(OutputFile file, std::uint64_t size);
    void drain();   // waits for and closes everything still pending

    [[nodiscard]] std::uint64_t bytes_on_disk() const noexcept;

private:
    struct Pending {
        OutputFile file;
        std::uint64_t size = 0;
    };
    std::mutex mutex_;
    std::deque<Pending> pending_;
    std::uint64_t pending_bytes_ = 0;
    std::uint64_t on_disk_ = 0;
};

} // namespace npk::pipeline
