#include "npk/pipeline/output_file.hpp"

#include <utility>

namespace npk::pipeline {

OutputFile::OutputFile(OutputFile&& other) noexcept : handle_(std::exchange(other.handle_, -1)) {}

OutputFile& OutputFile::operator=(OutputFile&& other) noexcept
{
    if (this != &other) {
        (void)close();
        handle_ = std::exchange(other.handle_, -1);
    }
    return *this;
}

OutputFile::~OutputFile() { (void)close(); }

Result<OutputFile> OutputFile::create(const std::filesystem::path& path, std::uint64_t size)
{
    (void)path;
    (void)size;
    return make_error(Errc::not_implemented, "OutputFile::create");
}

Result<void> OutputFile::write(std::span<const std::byte> data)
{
    (void)data;
    return make_error(Errc::not_implemented, "OutputFile::write");
}

void OutputFile::start_writeback() noexcept {}

Result<void> OutputFile::close()
{
    handle_ = -1; // TODO
    return {};
}

} // namespace npk::pipeline
