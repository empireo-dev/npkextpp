#include "npk/install.hpp"

namespace npk {

Result<Installation> Installation::locate(const std::filesystem::path& root)
{
    (void)root;
    return make_error(Errc::not_implemented, "Installation::locate");
}

std::vector<std::filesystem::path> Installation::resdb_files() const
{
    return {};
}

Result<std::vector<std::filesystem::path>> Installation::find_archives() const
{
    return make_error(Errc::not_implemented, "Installation::find_archives");
}

} // namespace npk
