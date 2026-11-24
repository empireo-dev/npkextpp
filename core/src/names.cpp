#include "npk/names.hpp"

#include "npk/hash.hpp"
#include "npk/mounts.hpp"
#include "npk/paths.hpp"
#include "npk/resdb.hpp"

#include <algorithm>
#include <numeric>

namespace npk {

ResourceManifest::ResourceManifest(std::vector<std::string> paths)
{
    std::ranges::sort(paths);
    const auto duplicates = std::ranges::unique(paths);
    paths.erase(duplicates.begin(), duplicates.end());

    std::vector<std::string> keys;
    keys.reserve(paths.size());
    for (const std::string& path : paths) keys.push_back(lower_ascii(path));

    std::vector<std::size_t> order(paths.size());
    std::iota(order.begin(), order.end(), std::size_t{0});
    std::ranges::stable_sort(order, {}, [&keys](std::size_t i) -> const std::string& { return keys[i]; });

    paths_.reserve(paths.size());
    keys_.reserve(paths.size());
    for (const std::size_t i : order) {
        paths_.push_back(std::move(paths[i]));
        keys_.push_back(std::move(keys[i]));
    }
}

Result<ResourceManifest> ResourceManifest::from_files(std::span<const std::filesystem::path> files)
{
    std::vector<std::string> all;
    for (const auto& file : files) {
        auto paths = read_resdb(file);
        if (!paths) return std::unexpected(std::move(paths.error()));
        all.insert(all.end(), std::make_move_iterator(paths->begin()), std::make_move_iterator(paths->end()));
    }
    return ResourceManifest(std::move(all));
}

std::span<const std::string> ResourceManifest::paths_under(std::string_view prefix) const
{
    const std::string key = lower_ascii(prefix);
    const auto first = std::ranges::lower_bound(keys_, key);
    auto last = first;
    while (last != keys_.end() && last->starts_with(key)) ++last;
    const auto begin = static_cast<std::size_t>(first - keys_.begin());
    return std::span(paths_).subspan(begin, static_cast<std::size_t>(last - first));
}

std::optional<std::string_view> ArchiveNames::find(std::uint32_t id) const
{
    for (const NameTable* table : tables_) {
        if (const auto it = table->find(id); it != table->end()) return it->second;
    }
    return std::nullopt;
}

std::vector<std::string> candidate_prefixes(const std::filesystem::path& archive)
{
    if (const auto placement = place(archive)) {
        auto prefix = placement->name_prefix();
        if (!prefix) return {};
        return {std::move(*prefix)};
    }
    return {std::string(), to_utf8(archive.stem()) + "\\"};
}

const NameTable& NameResolver::table_for(std::string_view prefix)
{
    if (const auto it = tables_.find(std::string(prefix)); it != tables_.end()) return it->second;

    NameTable table;
    const auto paths = manifest_->paths_under(prefix);
    table.reserve(paths.size());
    for (const std::string& full_path : paths) {
        const std::string_view inner = std::string_view(full_path).substr(prefix.size());
        table.insert_or_assign(path_id(inner), inner);   // later paths win, as in a dict
    }
    return tables_.emplace(std::string(prefix), std::move(table)).first->second;
}

ArchiveNames NameResolver::names_for(std::span<const std::string> prefixes)
{
    std::vector<const NameTable*> tables;
    tables.reserve(prefixes.size());
    for (const std::string& prefix : prefixes) tables.push_back(&table_for(prefix));
    return ArchiveNames(std::move(tables));
}

ArchiveNames NameResolver::names_for(const std::filesystem::path& archive)
{
    const std::vector<std::string> prefixes = candidate_prefixes(archive);
    return names_for(std::span<const std::string>(prefixes));
}

} // namespace npk
