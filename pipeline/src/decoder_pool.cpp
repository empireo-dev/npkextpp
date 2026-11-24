#include "npk/pipeline/decoder_pool.hpp"

namespace npk::pipeline {

DecoderPool::DecoderPool(BoundedQueue<DecodeTask>& in, BoundedQueue<WriteJob>& out,
                         ByteBudget& output_budget, Progress& progress, bool verify)
    : in_(in)
    , out_(out)
    , output_budget_(output_budget)
    , progress_(progress)
    , verify_(verify)
{
}

void DecoderPool::start(unsigned threads, std::stop_token stop)
{
    (void)threads;
    (void)stop; // TODO
}

void DecoderPool::join() { threads_.clear(); }

void DecoderPool::run(StageClock& clock, std::stop_token stop)
{
    (void)clock;
    (void)stop;
    (void)in_;
    (void)out_;
    (void)output_budget_;
    (void)progress_;
    (void)verify_;
}

} // namespace npk::pipeline
