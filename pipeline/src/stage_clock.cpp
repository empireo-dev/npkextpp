#include "npk/pipeline/stage_clock.hpp"

namespace npk::pipeline {

void StageClock::enter(StageState state) noexcept
{
    (void)state; // TODO
}

std::array<StageClock::Nanos, static_cast<std::size_t>(StageState::count_)> StageClock::totals() const noexcept
{
    std::array<Nanos, static_cast<std::size_t>(StageState::count_)> out{};
    for (std::size_t i = 0; i < out.size(); ++i) out[i] = totals_[i].load(std::memory_order_relaxed);
    return out;
}

} // namespace npk::pipeline
