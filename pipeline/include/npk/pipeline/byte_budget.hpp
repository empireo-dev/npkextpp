#pragma once

#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <stop_token>

namespace npk::pipeline {

// A memory limit counted in bytes. acquire(n) blocks until n bytes are free.
// Requests are granted in arrival order (a ticket queue), so a big request
// cannot be starved by a stream of small ones. A request larger than the whole
// capacity is granted once nothing else is held, so one huge entry can never
// deadlock the pipeline.
class ByteBudget {
public:
    explicit ByteBudget(std::size_t capacity);

    // false if stop was requested while waiting (nothing is held then).
    bool acquire(std::size_t bytes, std::stop_token stop = {});
    void release(std::size_t bytes) noexcept;

    [[nodiscard]] std::size_t capacity() const noexcept { return capacity_; }
    [[nodiscard]] std::size_t in_use() const;

private:
    std::size_t capacity_;
    std::size_t used_ = 0;
    std::uint64_t next_ticket_ = 0;
    std::uint64_t serving_ = 0;
    mutable std::mutex mutex_;
    std::condition_variable_any changed_;
};

// Bytes held from a budget, returned on destruction.
class BudgetLease {
public:
    BudgetLease() = default;
    BudgetLease(ByteBudget& budget, std::size_t bytes) noexcept : budget_(&budget), bytes_(bytes) {}
    BudgetLease(BudgetLease&& other) noexcept;
    BudgetLease& operator=(BudgetLease&& other) noexcept;
    BudgetLease(const BudgetLease&) = delete;
    BudgetLease& operator=(const BudgetLease&) = delete;
    ~BudgetLease();

    [[nodiscard]] std::size_t bytes() const noexcept { return bytes_; }

private:
    ByteBudget* budget_ = nullptr;
    std::size_t bytes_ = 0;
};

} // namespace npk::pipeline
