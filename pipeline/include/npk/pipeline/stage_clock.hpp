#pragma once

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>

namespace npk::pipeline {

enum class StageState : std::uint8_t {
    working,
    waiting_input,   // upstream queue empty
    waiting_output,  // downstream queue full / budget exhausted
    parked,          // writer not currently allowed to work
    count_,
};

// Per-thread time accounting. The owning thread calls enter() on each state
// change; the tuner thread reads totals() at any time.
class StageClock {
public:
    using Nanos = std::int64_t;

    void enter(StageState state) noexcept;
    [[nodiscard]] std::array<Nanos, static_cast<std::size_t>(StageState::count_)> totals() const noexcept;

private:
    std::array<std::atomic<Nanos>, static_cast<std::size_t>(StageState::count_)> totals_{};
    std::atomic<std::uint8_t> state_{static_cast<std::uint8_t>(StageState::waiting_input)};
    std::atomic<Nanos> since_{0};
};

} // namespace npk::pipeline
