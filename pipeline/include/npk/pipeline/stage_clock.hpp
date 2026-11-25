#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>

namespace npk::pipeline {

enum class StageState : std::uint8_t {
    working,
    waiting_input,   // upstream queue empty
    waiting_output,  // downstream queue full / budget exhausted
    parked,          // writer not currently allowed to work
    count_,
};

inline constexpr std::size_t kStageStates = static_cast<std::size_t>(StageState::count_);

// Time one thread spent in each state, in nanoseconds.
struct StageTimes {
    std::array<std::int64_t, kStageStates> ns{};

    [[nodiscard]] std::int64_t operator[](StageState state) const noexcept
    {
        return ns[static_cast<std::size_t>(state)];
    }
    StageTimes& operator+=(const StageTimes& other) noexcept
    {
        for (std::size_t i = 0; i < kStageStates; ++i) ns[i] += other.ns[i];
        return *this;
    }
    [[nodiscard]] StageTimes operator-(const StageTimes& other) const noexcept
    {
        StageTimes out;
        for (std::size_t i = 0; i < kStageStates; ++i) out.ns[i] = ns[i] - other.ns[i];
        return out;
    }
    // Share of non-parked time spent working (0 when nothing was measured).
    [[nodiscard]] double busy() const noexcept;
    [[nodiscard]] double share(StageState state) const noexcept;
};

// Per-thread time accounting. The owning thread calls enter() on each state
// change; any other thread may call times() at any time. times() includes the
// time spent so far in the current state, so a thread stuck in one long call
// shows up at once.
class StageClock {
public:
    StageClock() noexcept;

    void enter(StageState state) noexcept;
    [[nodiscard]] StageTimes times() const noexcept;

    [[nodiscard]] static std::int64_t now_ns() noexcept;

private:
    std::array<std::atomic<std::int64_t>, kStageStates> totals_{};
    std::atomic<std::uint8_t> state_{static_cast<std::uint8_t>(StageState::waiting_input)};
    std::atomic<std::int64_t> since_;
};

} // namespace npk::pipeline
