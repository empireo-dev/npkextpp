#include "npk/pipeline/byte_budget.hpp"

#include <utility>

namespace npk::pipeline {

ByteBudget::ByteBudget(std::size_t capacity) : capacity_(capacity) {}

bool ByteBudget::acquire(std::size_t bytes, std::stop_token stop)
{
    (void)bytes;
    (void)stop;
    return false; // TODO
}

void ByteBudget::release(std::size_t bytes) noexcept
{
    (void)bytes; // TODO
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
