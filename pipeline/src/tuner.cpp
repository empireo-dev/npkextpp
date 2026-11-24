#include "npk/pipeline/tuner.hpp"

#include <utility>

namespace npk::pipeline {

Tuner::Tuner(Tunables& tunables, const Reader& reader, const DecoderPool& decoders,
             WriterPool& writers, const WritebackTracker& writeback, Progress& progress, Log log)
    : tunables_(tunables)
    , reader_(reader)
    , decoders_(decoders)
    , writers_(writers)
    , writeback_(writeback)
    , progress_(progress)
    , log_(std::move(log))
{
}

void Tuner::run(std::stop_token stop)
{
    (void)stop; // TODO
    (void)tunables_;
    (void)reader_;
    (void)decoders_;
    (void)writers_;
    (void)writeback_;
    (void)progress_;
}

} // namespace npk::pipeline
