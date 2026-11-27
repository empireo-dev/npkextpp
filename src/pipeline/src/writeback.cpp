#include "npk/pipeline/writeback.hpp"

#include <utility>
#include <vector>

namespace npk::pipeline {

namespace {
#if defined(__linux__)
constexpr bool kTrackWriteback = true;
#else
constexpr bool kTrackWriteback = false;
#endif
} // namespace

WritebackTracker::WritebackTracker(std::size_t max_bytes, std::size_t max_files)
    : max_bytes_(max_bytes)
    , max_files_(max_files == 0 ? 1 : max_files)
{
}

WritebackTracker::~WritebackTracker() { drain(); }

void WritebackTracker::finish(Pending& pending) noexcept
{
    pending.file.finish_writeback();
    (void)pending.file.close();
    on_disk_.fetch_add(pending.size, std::memory_order_relaxed);
}

void WritebackTracker::submit(OutputFile file, std::uint64_t size)
{
    if constexpr (!kTrackWriteback) {
        Pending now{std::move(file), size};
        finish(now);
        return;
    }

    std::vector<Pending> due;
    {
        std::lock_guard lock(mutex_);
        pending_.push_back({std::move(file), size});
        pending_bytes_ += size;
        while (!pending_.empty() && (pending_bytes_ > max_bytes_ || pending_.size() > max_files_)) {
            pending_bytes_ -= pending_.front().size;
            due.push_back(std::move(pending_.front()));
            pending_.pop_front();
        }
    }
    for (Pending& pending : due) finish(pending);   // outside the lock: this may wait for the disk
}

void WritebackTracker::finish_oldest(std::size_t count)
{
    std::vector<Pending> due;
    {
        std::lock_guard lock(mutex_);
        while (!pending_.empty() && due.size() < count) {
            pending_bytes_ -= pending_.front().size;
            due.push_back(std::move(pending_.front()));
            pending_.pop_front();
        }
    }
    for (Pending& pending : due) finish(pending);
}

void WritebackTracker::drain()
{
    std::deque<Pending> all;
    {
        std::lock_guard lock(mutex_);
        all.swap(pending_);
        pending_bytes_ = 0;
    }
    for (Pending& pending : all) finish(pending);
}

} // namespace npk::pipeline
