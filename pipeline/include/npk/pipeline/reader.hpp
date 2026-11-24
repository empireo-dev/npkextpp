#pragma once

#include "npk/pipeline/bounded_queue.hpp"
#include "npk/pipeline/byte_budget.hpp"
#include "npk/pipeline/progress.hpp"
#include "npk/pipeline/stage_clock.hpp"
#include "npk/pipeline/tunables.hpp"
#include "npk/pipeline/work_items.hpp"
#include "npk/plan.hpp"

#include <stop_token>

namespace npk::pipeline {

// Stage 1, one thread. Walks the archives in plan order and each archive's
// entries by offset, merging neighbours into reads of Tunables::read_block
// bytes, and cuts every chunk into ~1 MB DecodeTasks. Blocks on the read budget.
class Reader {
public:
    Reader(Plan& plan, ByteBudget& budget, BoundedQueue<DecodeTask>& out,
           const Tunables& tunables, Progress& progress);

    void run(std::stop_token stop);   // closes `out` when done

    [[nodiscard]] const StageClock& clock() const noexcept { return clock_; }

private:
    Plan& plan_;
    ByteBudget& budget_;
    BoundedQueue<DecodeTask>& out_;
    const Tunables& tunables_;
    Progress& progress_;
    StageClock clock_;
};

} // namespace npk::pipeline
