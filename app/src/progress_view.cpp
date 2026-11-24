#include "progress_view.hpp"

namespace npkextpp {

ProgressView::ProgressView(const npk::pipeline::Progress& progress, std::uint64_t total_files,
                           std::uint64_t total_bytes)
    : progress_(progress)
    , total_files_(total_files)
    , total_bytes_(total_bytes)
{
}

void ProgressView::run(std::stop_token stop)
{
    (void)stop; // TODO
    (void)progress_;
    (void)total_files_;
    (void)total_bytes_;
}

void ProgressView::finish() {}

} // namespace npkextpp
