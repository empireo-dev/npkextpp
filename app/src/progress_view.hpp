#pragma once

#include "npk/pipeline/progress.hpp"

#include <cstdint>
#include <stop_token>

namespace npkextpp {

// Redraws a one-line progress display from the pipeline counters a few times
// per second, on its own thread.
class ProgressView {
public:
    ProgressView(const npk::pipeline::Progress& progress, std::uint64_t total_files,
                 std::uint64_t total_bytes);

    void run(std::stop_token stop);
    void finish();   // final line + newline

private:
    const npk::pipeline::Progress& progress_;
    std::uint64_t total_files_;
    std::uint64_t total_bytes_;
};

} // namespace npkextpp
