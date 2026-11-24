#pragma once

#include <condition_variable>
#include <cstddef>
#include <deque>
#include <mutex>
#include <optional>
#include <stop_token>

namespace npk::pipeline {

// Multi-producer / multi-consumer queue with a fixed capacity. push() blocks
// while full, pop() blocks while empty. close() wakes everyone: further pushes
// fail and pop() returns nullopt once the queue is drained.
template <class T>
class BoundedQueue {
public:
    explicit BoundedQueue(std::size_t capacity) : capacity_(capacity) {}

    // false if the queue was closed or stop was requested.
    bool push(T item, std::stop_token stop = {})
    {
        (void)item;
        (void)stop;
        return false; // TODO
    }

    // nullopt once the queue is closed and empty, or stop was requested.
    std::optional<T> pop(std::stop_token stop = {})
    {
        (void)stop;
        return std::nullopt; // TODO
    }

    void close()
    {
        std::lock_guard lock(mutex_);
        closed_ = true;
        not_empty_.notify_all();
        not_full_.notify_all();
    }

private:
    std::size_t capacity_;
    std::deque<T> items_;
    bool closed_ = false;
    std::mutex mutex_;
    std::condition_variable_any not_empty_;
    std::condition_variable_any not_full_;
};

} // namespace npk::pipeline
