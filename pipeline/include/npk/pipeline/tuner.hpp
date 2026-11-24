#pragma once

#include "npk/pipeline/decoder_pool.hpp"
#include "npk/pipeline/progress.hpp"
#include "npk/pipeline/reader.hpp"
#include "npk/pipeline/tunables.hpp"
#include "npk/pipeline/writeback.hpp"
#include "npk/pipeline/writer_pool.hpp"

#include <functional>
#include <stop_token>
#include <string>

namespace npk::pipeline {

// Wakes every ~500 ms, compares how much time each stage spent working versus
// waiting, finds the bottleneck and adjusts:
//   writers busy, decoders waiting for output → try more writers (hill climb,
//     keep if throughput rises > ~5 %, otherwise revert and remember the limit)
//   reader busy, decoders waiting for input  → larger read blocks
//   decoders busy                            → CPU bound, nothing to tune
// Probes again periodically and when a new archive starts, because the data mix
// and the disk speed change during a run.
class Tuner {
public:
    using Log = std::function<void(const std::string&)>;

    Tuner(Tunables& tunables, const Reader& reader, const DecoderPool& decoders,
          WriterPool& writers, const WritebackTracker& writeback, Progress& progress, Log log = {});

    void run(std::stop_token stop);

private:
    Tunables& tunables_;
    const Reader& reader_;
    const DecoderPool& decoders_;
    WriterPool& writers_;
    const WritebackTracker& writeback_;
    Progress& progress_;
    Log log_;
};

} // namespace npk::pipeline
