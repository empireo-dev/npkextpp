#include "npk/pipeline/writeback.hpp"

#include <utility>

namespace npk::pipeline {

void WritebackTracker::submit(OutputFile file, std::uint64_t size)
{
    (void)file;
    (void)size; // TODO
}

void WritebackTracker::drain() {}

std::uint64_t WritebackTracker::bytes_on_disk() const noexcept { return on_disk_; }

} // namespace npk::pipeline
