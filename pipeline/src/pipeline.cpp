#include "npk/pipeline/pipeline.hpp"

#include "npk/pipeline/decoder_pool.hpp"
#include "npk/pipeline/index_writer.hpp"
#include "npk/pipeline/reader.hpp"
#include "npk/pipeline/tuner.hpp"
#include "npk/pipeline/writer_pool.hpp"

namespace npk::pipeline {

Result<void> run(Plan& plan, const Options& options, Progress& progress)
{
    (void)plan;
    (void)options;
    (void)progress;
    return make_error(Errc::not_implemented, "pipeline::run");
}

} // namespace npk::pipeline
