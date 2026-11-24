#include "npk/archive.hpp"

namespace npk {

Result<Archive> Archive::open(const std::filesystem::path& path)
{
    (void)path;
    return make_error(Errc::not_implemented, "Archive::open");
}

Result<std::vector<Entry>> Archive::parse_toc(std::span<const std::byte> toc, std::uint32_t entry_count)
{
    (void)toc;
    (void)entry_count;
    return make_error(Errc::not_implemented, "Archive::parse_toc");
}

const Entry* Archive::find(std::uint32_t id) const noexcept
{
    (void)id;
    return nullptr;
}

Result<void> Archive::read_stored(const Entry& entry, std::span<std::byte> out) const
{
    (void)entry;
    (void)out;
    return make_error(Errc::not_implemented, "Archive::read_stored");
}

} // namespace npk
