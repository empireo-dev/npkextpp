#pragma once

#include "npk/pipeline/bounded_queue.hpp"
#include "npk/pipeline/byte_budget.hpp"
#include "npk/pipeline/progress.hpp"
#include "npk/pipeline/stage_clock.hpp"
#include "npk/pipeline/work_items.hpp"

#include <atomic>
#include <deque>
#include <thread>
#include <vector>

namespace npk::pipeline {

// Stage 2, one thread per hardware thread. Each thread owns a DecodeContext,
// decodes the entries of its task in place and allocates the output from the
// output budget. The last thread to finish closes the write queue.
class DecoderPool {
public:
    DecoderPool(BoundedQueue<DecodeTask>& in, BoundedQueue<WriteJob>& out,
                ByteBudget& output_budget, Progress& progress, bool verify);

    void start(unsigned threads, std::stop_token stop);
    void join();

    [[nodiscard]] const std::deque<StageClock>& clocks() const noexcept { return clocks_; }

private:
    void run(StageClock& clock, std::stop_token stop);

    BoundedQueue<DecodeTask>& in_;
    BoundedQueue<WriteJob>& out_;
    ByteBudget& output_budget_;
    Progress& progress_;
    bool verify_;
    std::deque<StageClock> clocks_;   // deque: stable addresses, not movable elements
    std::vector<std::jthread> threads_;
    std::atomic<unsigned> running_{0};
};

} // namespace npk::pipeline
