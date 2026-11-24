#pragma once

#include "npk/pipeline/bounded_queue.hpp"
#include "npk/pipeline/progress.hpp"
#include "npk/pipeline/stage_clock.hpp"
#include "npk/pipeline/tunables.hpp"
#include "npk/pipeline/work_items.hpp"
#include "npk/pipeline/writeback.hpp"

#include <condition_variable>
#include <deque>
#include <filesystem>
#include <mutex>
#include <thread>
#include <vector>

namespace npk::pipeline {

// Stage 3. Starts kMaxWriters threads up front; only Tunables::writers_allowed
// of them take jobs, the rest stay parked, so the tuner can change the number
// of writers without creating or destroying threads.
class WriterPool {
public:
    static constexpr unsigned kMaxWriters = 32;

    WriterPool(const std::filesystem::path& output_root, BoundedQueue<WriteJob>& in,
               Tunables& tunables, WritebackTracker& writeback, Progress& progress);

    void start(std::stop_token stop);
    void join();

    // Wakes parked writers after Tunables::writers_allowed changed.
    void allowed_changed();

    [[nodiscard]] const std::deque<StageClock>& clocks() const noexcept { return clocks_; }

private:
    void run(unsigned index, StageClock& clock, std::stop_token stop);

    std::filesystem::path output_root_;
    BoundedQueue<WriteJob>& in_;
    Tunables& tunables_;
    WritebackTracker& writeback_;
    Progress& progress_;
    std::deque<StageClock> clocks_;
    std::vector<std::jthread> threads_;
    std::mutex park_mutex_;
    std::condition_variable_any park_;
};

} // namespace npk::pipeline
