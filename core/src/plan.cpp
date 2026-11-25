#include "npk/plan.hpp"

#include "npk/archive.hpp"
#include "npk/filetypes.hpp"
#include "npk/hash.hpp"
#include "npk/mounts.hpp"
#include "npk/paths.hpp"

#include <algorithm>
#include <cstdio>
#include <map>
#include <numeric>
#include <set>
#include <tuple>
#include <unordered_map>
#include <unordered_set>

namespace fs = std::filesystem;

namespace npk {

namespace {

fs::path resolved(const fs::path& path)
{
    std::error_code error;
    fs::path out = fs::weakly_canonical(fs::absolute(path, error), error);
    return error ? path : out;
}

// The path as the OS writes it (backslashes on Windows), as UTF-8.
std::string native_utf8(const fs::path& path)
{
    const std::u8string text = path.u8string();
    return {reinterpret_cast<const char*>(text.data()), text.size()};
}

std::string hex8(std::uint32_t value)
{
    char text[9];
    std::snprintf(text, sizeof text, "%08x", value);
    return text;
}

std::vector<std::string_view> split(std::string_view text, char separator)
{
    std::vector<std::string_view> parts;
    std::size_t start = 0;
    while (start <= text.size()) {
        std::size_t end = text.find(separator, start);
        if (end == std::string_view::npos) end = text.size();
        parts.push_back(text.substr(start, end - start));
        start = end + 1;
    }
    return parts;
}

std::string join(std::span<const std::string_view> parts, char separator)
{
    std::string out;
    for (std::size_t i = 0; i < parts.size(); ++i) {
        if (i != 0) out += separator;
        out += parts[i];
    }
    return out;
}

std::string_view last_part(std::string_view folder)
{
    const auto slash = folder.rfind('/');
    return slash == std::string_view::npos ? folder : folder.substr(slash + 1);
}

// Sort key: alphabetical, but every folder after the folders nested in it
// (res/model/character, res/ui, res, then the top level).
std::vector<std::string> tree_order(std::string_view folder)
{
    std::vector<std::string> key;
    if (!folder.empty()) {
        for (const std::string_view part : split(folder, '/')) key.push_back(lower_ascii(part));
    }
    key.emplace_back("\xF4\x8F\xBF\xBF");   // U+10FFFF: after every real name
    return key;
}

struct GroupKey {
    std::vector<std::string> section;
    std::string name;
    auto operator<=>(const GroupKey&) const = default;
};

GroupKey group_order(const OutputGroup& group)
{
    return {tree_order(group.section()), lower_ascii(last_part(group.folder))};
}

// Every known archive by output folder, with their entry ids read on demand.
// Answers: does an archive nested below a parent folder hold this file?
class NestedArchives {
public:
    struct Holder {
        int rank;
        std::string label;
        fs::path path;
        auto operator<=>(const Holder&) const = default;
    };

    NestedArchives(std::span<const fs::path> archives, const Installation* installation)
    {
        for (const fs::path& path : archives) {
            Location location = locate(path, installation);
            by_folder_[lower_ascii(location.folder)].push_back({location.rank, std::move(location.label), path});
        }
        for (auto& [folder, holders] : by_folder_) std::ranges::sort(holders);
    }

    void remember(const fs::path& path, std::span<const Entry> toc)
    {
        auto& ids = ids_[native_utf8(path)];
        for (const Entry& entry : toc) ids.insert(entry.id);
    }

    [[nodiscard]] bool has_any_below(std::string_view folder) const
    {
        const std::string prefix = lower_ascii(folder) + "/";
        const auto it = by_folder_.lower_bound(prefix);
        return it != by_folder_.end() && it->first.starts_with(prefix);
    }

    // The highest-priority archive in a folder nested in `parent` that holds `name`
    // (a '/' path relative to `parent`). The deepest such folder decides.
    [[nodiscard]] const std::string* holder_of(std::string_view parent, std::string_view name)
    {
        const std::vector<std::string_view> parts = split(name, '/');
        for (std::size_t depth = parts.size() - 1; depth >= 1; --depth) {
            const std::string folder = std::string(parent) + "/" + join(std::span(parts).first(depth), '/');
            const auto it = by_folder_.find(lower_ascii(folder));
            if (it == by_folder_.end()) continue;
            const std::uint32_t inner_id = path_id(join(std::span(parts).subspan(depth), '\\'));
            for (const Holder& holder : it->second) {
                if (ids_of(holder.path).contains(inner_id)) return &holder.label;
            }
            return nullptr;
        }
        return nullptr;
    }

private:
    const std::unordered_set<std::uint32_t>& ids_of(const fs::path& path)
    {
        const std::string key = native_utf8(path);
        if (const auto it = ids_.find(key); it != ids_.end()) return it->second;
        auto& ids = ids_[key];
        if (auto archive = Archive::open(path)) {
            for (const Entry& entry : archive->entries()) ids.insert(entry.id);
        }
        return ids;
    }

    std::map<std::string, std::vector<Holder>> by_folder_;   // lower-case output folder
    std::unordered_map<std::string, std::unordered_set<std::uint32_t>> ids_;
};

class LabelTable {
public:
    std::int32_t index(const std::string& label)
    {
        const auto [it, added] = index_.try_emplace(label, static_cast<std::int32_t>(labels_.size()));
        if (added) labels_.push_back(label);
        return it->second;
    }
    std::vector<std::string> take() { return std::move(labels_); }

private:
    std::unordered_map<std::string, std::int32_t> index_;
    std::vector<std::string> labels_;
};

std::string parent_folder(std::string_view path)
{
    const auto slash = path.rfind('/');
    return std::string(slash == std::string_view::npos ? std::string_view{} : path.substr(0, slash));
}

} // namespace

Location locate(const fs::path& archive, const Installation* installation, int order)
{
    const std::optional<fs::path> relative = installation ? installation->relative(archive) : std::nullopt;
    const std::optional<Placement> placement = place(relative ? *relative : archive);
    const std::optional<int> rank =
        (placement && relative) ? mount_rank(to_utf8(placement->root)) : std::nullopt;

    Location location;
    location.folder = placement ? placement->output_folder() : to_utf8(archive.stem());
    location.label = relative ? to_utf8(*relative) : native_utf8(archive);
    location.rank = rank ? *rank : static_cast<int>(mounts().size()) + order;
    location.mount = (placement && relative) ? to_utf8(placement->root) : std::string();
    location.record = relative ? to_utf8(*relative) + ".csv"
                               : std::string(kOutsideRecords) + "/" + folder_tag(native_utf8(resolved(archive))) + ".csv";
    return location;
}

std::string output_path(const PlannedEntry& entry, std::span<const std::byte> data)
{
    if (!entry.unnamed) return entry.path;
    std::string path = entry.path;
    path += '.';
    path += guess_extension(data);
    return path;
}

namespace {

std::string existing_key(std::string_view relative)
{
#if defined(_WIN32)
    return lower_ascii(relative);   // NTFS ignores case
#else
    return std::string(relative);
#endif
}

} // namespace

ExistingFiles ExistingFiles::scan(const fs::path& output_root)
{
    ExistingFiles files;
    std::error_code error;
    if (!fs::is_directory(output_root, error)) return files;
    const fs::path root = long_path(output_root);
    fs::recursive_directory_iterator it(root, fs::directory_options::skip_permission_denied, error);
    for (; !error && it != fs::recursive_directory_iterator(); it.increment(error)) {
        std::error_code entry_error;
        if (it.depth() == 0 && it->is_directory(entry_error) && it->path().filename() == kIndexFolderName) {
            it.disable_recursion_pending();   // the index is rewritten anyway
            continue;
        }
        if (!it->is_regular_file(entry_error)) continue;
        const std::uint64_t size = it->file_size(entry_error);
        if (entry_error) continue;
        files.add(to_utf8(it->path().lexically_relative(root)), size);
    }
    return files;
}

void ExistingFiles::add(std::string_view relative, std::uint64_t size)
{
    ++count_;
    // An unnamed file is planned without its extension (it is guessed from the data).
    const std::size_t slash = relative.rfind('/');
    const std::string_view folder = slash == std::string_view::npos ? std::string_view{} : relative.substr(0, slash);
    const bool in_unnamed = folder == kUnnamedFolder || folder.ends_with("/" + std::string(kUnnamedFolder));
    if (in_unnamed) {
        const std::size_t dot = relative.rfind('.');
        if (dot != std::string_view::npos && dot > slash + 1) {
            files_.insert_or_assign(existing_key(relative.substr(0, dot)), File{size, std::string(relative.substr(dot))});
            return;
        }
    }
    files_.insert_or_assign(existing_key(relative), File{size, {}});
}

std::optional<std::string> ExistingFiles::find(std::string_view planned_path, bool unnamed, std::uint64_t size) const
{
    const auto it = files_.find(existing_key(planned_path));
    if (it == files_.end() || it->second.size != size) return std::nullopt;
    if (!unnamed) return std::string(planned_path);
    if (it->second.extension.empty()) return std::nullopt;
    return std::string(planned_path) + it->second.extension;
}

std::string_view OutputGroup::section() const noexcept
{
    const auto slash = folder.rfind('/');
    return slash == std::string::npos ? std::string_view{} : std::string_view(folder).substr(0, slash);
}

std::size_t Plan::source_count() const noexcept
{
    std::size_t count = 0;
    for (const OutputGroup& group : groups) count += group.sources.size();
    return count;
}

Planner::Planner(const ResourceManifest& manifest, const Installation* installation)
    : names_(manifest)
    , installation_(installation)
{
}

Plan Planner::build(std::span<const fs::path> archives, const PlanOptions& options)
{
    Plan plan;

    // 1. Read every table of contents and group the archives by output folder.
    std::map<std::string, OutputGroup> by_folder;   // lower-case folder
    for (std::size_t order = 0; order < archives.size(); ++order) {
        const fs::path& path = archives[order];
        auto archive = Archive::open(path);
        if (!archive) {
            plan.unreadable.emplace_back(path, archive.error().message);
            continue;
        }

        ArchiveSource source;
        source.path = path;
        source.location = locate(path, installation_, static_cast<int>(order));
        source.file_size = archive->file_size();
        source.toc = archive->entries();
        archive->close();

        // Storage order: reading front to back is much faster than table order.
        std::vector<std::uint32_t> selected(source.toc.size());
        std::iota(selected.begin(), selected.end(), std::uint32_t{0});
        std::ranges::stable_sort(selected, {}, [&](std::uint32_t i) { return source.toc[i].offset; });
        if (options.limit && *options.limit < selected.size()) selected.resize(*options.limit);

        source.entries.reserve(selected.size());
        for (const std::uint32_t index : selected) {
            PlannedEntry planned;
            planned.toc_index = index;
            source.entries.push_back(std::move(planned));
            source.packed_bytes += source.toc[index].packed_size;
            source.raw_bytes += source.toc[index].raw_size;
        }

        OutputGroup& group = by_folder[lower_ascii(source.location.folder)];
        if (group.folder.empty()) group.folder = source.location.folder;
        group.sources.push_back(std::move(source));
    }

    for (auto& [key, group] : by_folder) {
        std::ranges::stable_sort(group.sources, [](const ArchiveSource& a, const ArchiveSource& b) {
            return std::tie(a.location.rank, a.location.label) < std::tie(b.location.rank, b.location.label);
        });
        plan.groups.push_back(std::move(group));
    }
    std::ranges::stable_sort(plan.groups, {}, group_order);

    // 2. Every archive that could be mounted below a planned folder, also those
    //    not being unpacked: a nested archive's copy of a file beats the parent's.
    std::vector<fs::path> known = installation_ ? installation_->archives()
                                                : std::vector<fs::path>(archives.begin(), archives.end());
    for (const OutputGroup& group : plan.groups) {
        for (const ArchiveSource& source : group.sources) known.push_back(source.path);
    }
    std::ranges::sort(known);
    known.erase(std::ranges::unique(known).begin(), known.end());
    NestedArchives nested(known, installation_);
    for (const OutputGroup& group : plan.groups) {
        for (const ArchiveSource& source : group.sources) nested.remember(source.path, source.toc);
    }

    // 3. Decide every entry.
    LabelTable labels;
    for (OutputGroup& group : plan.groups) {
        const bool has_nested = nested.has_any_below(group.folder);
        std::unordered_map<std::uint32_t, std::int32_t> provided;   // id -> label of the source that writes it

        for (ArchiveSource& source : group.sources) {
            const std::int32_t self = labels.index(source.label());
            const std::optional<fs::path> relative = installation_ ? installation_->relative(source.path) : std::nullopt;
            const ArchiveNames names = names_.names_for(relative ? *relative : source.path);

            for (PlannedEntry& planned : source.entries) {
                const Entry& entry = source.toc[planned.toc_index];

                std::optional<std::string> name;
                if (const auto inner = names.find(entry.id)) name = safe_relative_path(*inner);

                std::int32_t winner = -1;
                if (const auto it = provided.find(entry.id); it != provided.end()) {
                    winner = it->second;
                } else if (name && has_nested) {
                    if (const std::string* holder = nested.holder_of(group.folder, *name)) winner = labels.index(*holder);
                }

                if (winner >= 0) {
                    planned.replaced_by = winner;
                    if (!options.keep_overridden) {
                        planned.disposition = Disposition::skip;
                        continue;
                    }
                    planned.disposition = Disposition::write_copy;
                }

                planned.unnamed = !name;
                std::string inner = name ? std::move(*name) : std::string(kUnnamedFolder) + "/" + hex8(entry.id);
                if (winner >= 0) {
                    inner = std::string(kOverriddenFolder) + "/" + folder_tag(source.label()) + "/" + inner;
                } else {
                    provided.emplace(entry.id, self);
                }
                planned.path = group.folder + "/" + inner;
            }
        }
    }

    // 4. One entry per output path: writes run in parallel, so two entries for one
    //    path would race. npkext wrote them one after the other, so the last one in
    //    plan order is what it left on disk; keep that one. (Happens only in odd
    //    layouts: a parent and a nested archive both keeping a file, or names that
    //    differ only in case on a case-insensitive filesystem.)
    std::unordered_map<std::string, std::pair<PlannedEntry*, std::int32_t>> by_path;
    for (OutputGroup& group : plan.groups) {
        for (ArchiveSource& source : group.sources) {
            const std::int32_t self = labels.index(source.label());
            for (PlannedEntry& planned : source.entries) {
                if (planned.disposition == Disposition::skip) continue;
#if defined(_WIN32)
                std::string key = lower_ascii(planned.path);
#else
                const std::string& key = planned.path;
#endif
                const auto [it, added] = by_path.try_emplace(key, &planned, self);
                if (!added) {
                    PlannedEntry& earlier = *it->second.first;
                    earlier.disposition = Disposition::skip;
                    earlier.replaced_by = self;
                    earlier.path.clear();
                    it->second = {&planned, self};
                }
            }
        }
    }

    // 5. Files already in the output folder (from an earlier run) are left alone.
    if (options.existing && options.existing->size() > 0) {
        for (OutputGroup& group : plan.groups) {
            for (ArchiveSource& source : group.sources) {
                for (PlannedEntry& planned : source.entries) {
                    if (planned.disposition == Disposition::skip) continue;
                    auto found = options.existing->find(planned.path, planned.unnamed, source.entry(planned).raw_size);
                    if (!found) continue;
                    planned.status = EntryStatus::existing;
                    planned.path = std::move(*found);
                }
            }
        }
    }

    // 6. Totals and the folders to create.
    std::set<std::string> directories;
    for (const OutputGroup& group : plan.groups) {
        for (const ArchiveSource& source : group.sources) {
            for (const PlannedEntry& planned : source.entries) {
                plan.totals.entries += 1;
                if (planned.replaced_by >= 0) plan.totals.overridden += 1;
                if (planned.disposition == Disposition::skip) continue;
                const Entry& entry = source.entry(planned);
                if (planned.status == EntryStatus::existing) {
                    plan.totals.existing += 1;
                    plan.totals.existing_bytes += entry.raw_size;
                    continue;
                }
                if (planned.unnamed && planned.disposition == Disposition::write) plan.totals.unnamed += 1;
                plan.totals.files_to_write += 1;
                plan.totals.packed_bytes += entry.packed_size;
                plan.totals.raw_bytes += entry.raw_size;
                directories.insert(parent_folder(planned.path));
            }
        }
    }

    plan.labels = labels.take();
    plan.directories.assign(directories.begin(), directories.end());
    return plan;
}

} // namespace npk
