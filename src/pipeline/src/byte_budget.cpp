#include "npk/pipeline/byte_budget.hpp"

#include <utility>

namespace npk::pipeline {

ByteBudget::ByteBudget(std::size_t capacity) : capacity_(capacity) {}

bool ByteBudget::acquire(std::size_t bytes, std::stop_token stop)
{
    std::unique_lock lock(mutex_);
    const std::uint64_t ticket = next_ticket_++;
    const bool granted = changed_.wait(lock, stop, [&] {
        return ticket == serving_ && (used_ + bytes <= capacity_ || used_ == 0);
    });
    if (!granted) return false;   // stopping: the pipeline is shutting down, nobody waits behind us
    used_ += bytes;
    ++serving_;
    lock.unlock();
    changed_.notify_all();   // the next ticket may fit too
    return true;
}

void ByteBudget::release(std::size_t bytes) noexcept
{
    {
        std::lock_guard lock(mutex_);
        used_ -= bytes;
    }
    changed_.notify_all();
}

std::size_t ByteBudget::in_use() const
{
    std::lock_guard lock(mutex_);
    return used_;
}

BudgetLease::BudgetLease(BudgetLease&& other) noexcept
    : budget_(std::exchange(other.budget_, nullptr))
    , bytes_(std::exchange(other.bytes_, 0))
{
}

BudgetLease& BudgetLease::operator=(BudgetLease&& other) noexcept
{
    if (this != &other) {
        if (budget_) budget_->release(bytes_);
        budget_ = std::exchange(other.budget_, nullptr);
        bytes_ = std::exchange(other.bytes_, 0);
    }
    return *this;
}

BudgetLease::~BudgetLease()
{
    if (budget_) budget_->release(bytes_);
}

} // namespace npk::pipeline
