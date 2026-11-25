#include "npk/pipeline/stage_clock.hpp"

#include <chrono>

namespace npk::pipeline {

double StageTimes::share(StageState state) const noexcept
{
    const std::int64_t total = (*this)[StageState::working] + (*this)[StageState::waiting_input] +
                               (*this)[StageState::waiting_output];
    if (total <= 0 || state == StageState::parked) return 0.0;
    return static_cast<double>((*this)[state]) / static_cast<double>(total);
}

double StageTimes::busy() const noexcept
{
    return share(StageState::working);
}

std::int64_t StageClock::now_ns() noexcept
{
    return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

StageClock::StageClock() noexcept : since_(now_ns()) {}

void StageClock::enter(StageState state) noexcept
{
    const std::int64_t now = now_ns();
    const auto previous = state_.exchange(static_cast<std::uint8_t>(state), std::memory_order_relaxed);
    const std::int64_t since = since_.exchange(now, std::memory_order_relaxed);
    totals_[previous].fetch_add(now - since, std::memory_order_relaxed);
}

StageTimes StageClock::times() const noexcept
{
    StageTimes out;
    for (std::size_t i = 0; i < kStageStates; ++i) out.ns[i] = totals_[i].load(std::memory_order_relaxed);
    const auto state = state_.load(std::memory_order_relaxed);
    const std::int64_t running = now_ns() - since_.load(std::memory_order_relaxed);
    if (running > 0) out.ns[state] += running;   // approximate: the owner may be switching right now
    return out;
}

} // namespace npk::pipeline
