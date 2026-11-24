#include "npk/resdb.hpp"

namespace npk {

Result<std::vector<std::string>> read_resdb(const std::filesystem::path& path)
{
    (void)path;
    return make_error(Errc::not_implemented, "read_resdb");
}

Result<std::vector<std::string>> parse_resdb(std::span<const std::byte> data)
{
    (void)data;
    return make_error(Errc::not_implemented, "parse_resdb");
}

} // namespace npk
