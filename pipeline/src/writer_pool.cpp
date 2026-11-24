#include "npk/pipeline/writer_pool.hpp"

namespace npk::pipeline {

WriterPool::WriterPool(const std::filesystem::path& output_root, BoundedQueue<WriteJob>& in,
                       Tunables& tunables, WritebackTracker& writeback, Progress& progress)
    : output_root_(output_root)
    , in_(in)
    , tunables_(tunables)
    , writeback_(writeback)
    , progress_(progress)
{
}

void WriterPool::start(std::stop_token stop)
{
    (void)stop; // TODO
}

void WriterPool::join() { threads_.clear(); }

void WriterPool::allowed_changed() { park_.notify_all(); }

void WriterPool::run(unsigned index, StageClock& clock, std::stop_token stop)
{
    (void)index;
    (void)clock;
    (void)stop;
    (void)in_;
    (void)tunables_;
    (void)writeback_;
    (void)progress_;
}

} // namespace npk::pipeline
