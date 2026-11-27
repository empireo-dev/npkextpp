#pragma once

#include <cstddef>

namespace npk::pipeline {

// A stand-in for the output disk, for testing the tuner without writing
// anything (docs/simulated_disk.md). Calls block for as long as the modelled
// disk would take. Called from many threads at once.
class FakeDisk {
public:
    virtual ~FakeDisk() = default;
    virtual void write_file(unsigned writer, std::size_t bytes) = 0;
    virtual void read(std::size_t bytes) = 0;
};

} // namespace npk::pipeline
