#include "npk/names.hpp"

#include <utility>

namespace npk {

void NameResolver::add_paths(std::vector<std::string> paths)
{
    paths_.insert(paths_.end(), std::make_move_iterator(paths.begin()),
                  std::make_move_iterator(paths.end()));
}

void NameResolver::prepare_prefix(std::string_view prefix)
{
    (void)prefix;
}

std::optional<std::string_view> NameResolver::resolve(std::string_view prefix, std::uint32_t id) const
{
    (void)prefix;
    (void)id;
    return std::nullopt;
}

} // namespace npk
