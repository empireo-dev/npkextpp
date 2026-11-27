#pragma once

#include <algorithm>
#include <condition_variable>
#include <cstddef>
#include <deque>
#include <mutex>
#include <optional>
#include <stop_token>

namespace npk::pipeline {

// Multi-producer / multi-consumer queue with a fixed capacity. push() blocks
// while full, pop() blocks while empty. close() wakes everyone: further pushes
// fail and pop() returns nullopt once the queue is drained. Both give up when
// the stop token is triggered.
template <class T>
class BoundedQueue {
public:
    explicit BoundedQueue(std::size_t capacity) : capacity_(std::max<std::size_t>(capacity, 1)) {}

    // false if the queue was closed or stop was requested (the item is dropped).
    bool push(T item, std::stop_token stop = {})
    {
        std::unique_lock lock(mutex_);
        not_full_.wait(lock, stop, [&] { return closed_ || items_.size() < capacity_; });
        if (closed_ || items_.size() >= capacity_) return false;
        items_.push_back(std::move(item));
        lock.unlock();
        not_empty_.notify_one();
        return true;
    }

    // nullopt once the queue is closed and empty, or when stop was requested.
    std::optional<T> pop(std::stop_token stop = {})
    {
        std::unique_lock lock(mutex_);
        not_empty_.wait(lock, stop, [&] { return closed_ || !items_.empty(); });
        if (items_.empty() || stop.stop_requested()) return std::nullopt;
        std::optional<T> item(std::move(items_.front()));
        items_.pop_front();
        lock.unlock();
        not_full_.notify_one();
        return item;
    }

    void close()
    {
        {
            std::lock_guard lock(mutex_);
            closed_ = true;
        }
        not_empty_.notify_all();
        not_full_.notify_all();
    }

    [[nodiscard]] bool closed() const
    {
        std::lock_guard lock(mutex_);
        return closed_;
    }

    [[nodiscard]] std::size_t size() const
    {
        std::lock_guard lock(mutex_);
        return items_.size();
    }

private:
    std::size_t capacity_;
    std::deque<T> items_;
    bool closed_ = false;
    mutable std::mutex mutex_;
    std::condition_variable_any not_empty_;
    std::condition_variable_any not_full_;
};

} // namespace npk::pipeline
