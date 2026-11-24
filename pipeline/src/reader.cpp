#include "npk/pipeline/reader.hpp"

namespace npk::pipeline {

Reader::Reader(Plan& plan, ByteBudget& budget, BoundedQueue<DecodeTask>& out,
               const Tunables& tunables, Progress& progress)
    : plan_(plan)
    , budget_(budget)
    , out_(out)
    , tunables_(tunables)
    , progress_(progress)
{
}

void Reader::run(std::stop_token stop)
{
    (void)stop;
    (void)plan_;
    (void)budget_;
    (void)tunables_;
    (void)progress_;
    out_.close(); // TODO
}

} // namespace npk::pipeline
