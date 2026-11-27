#include "npk/install.hpp"

#include "npk/mounts.hpp"
#include "npk/paths.hpp"

#include <algorithm>

namespace fs = std::filesystem;

namespace npk {

namespace {

fs::path resolved(const fs::path& path)
{
    std::error_code error;
    fs::path out = fs::weakly_canonical(fs::absolute(path, error), error);
    return error ? fs::absolute(path).lexically_normal() : out;
}

bool is_dir(const fs::path& path)
{
    std::error_code error;
    return fs::is_directory(path, error);
}

bool is_file(const fs::path& path)
{
    std::error_code error;
    return fs::is_regular_file(path, error);
}

void add_unique(std::vector<std::string>& list, std::string item)
{
    if (std::ranges::find(list, item) == list.end()) list.push_back(std::move(item));
}

} // namespace

Result<Installation> Installation::open(const fs::path& folder)
{
    const fs::path root = resolved(folder);
    if (!is_dir(root)) return make_error(Errc::invalid_input, "installation folder not found: " + to_utf8(folder));
    Installation installation(root);
    if (!installation.looks_valid()) {
        return make_error(Errc::invalid_input, to_utf8(root) + " does not look like a game installation "
                                                               "(expected neox_bin/ or res/resdb + Documents/)");
    }
    installation.scan_archives();
    return installation;
}

std::optional<Installation> Installation::find_above(const fs::path& path)
{
    fs::path folder = resolved(path);
    while (folder.has_parent_path() && folder.parent_path() != folder) {
        folder = folder.parent_path();
        Installation candidate(folder);
        if (candidate.looks_valid()) {
            candidate.scan_archives();
            return candidate;
        }
    }
    return std::nullopt;
}

bool Installation::looks_valid() const
{
    const bool has_engine = is_dir(root_ / "neox_bin");
    const bool has_resources = is_file(root_ / "res" / "resdb") && is_dir(root_ / "Documents");
    return has_engine || has_resources;
}

void Installation::scan_archives()
{
    std::vector<fs::path> found;
    std::vector<fs::path> folders{root_};
    while (!folders.empty()) {
        const fs::path folder = std::move(folders.back());
        folders.pop_back();
        std::error_code error;
        fs::directory_iterator it(folder, error);
        if (error) {
            std::string rel = to_utf8(folder.lexically_relative(root_));
            std::ranges::replace(rel, '\\', '/');
            add_unique(unreadable_, rel.empty() ? std::string(".") : rel);
            continue;
        }
        for (; !error && it != fs::directory_iterator(); it.increment(error)) {
            std::error_code type_error;
            if (it->is_symlink(type_error)) continue;
            if (it->is_directory(type_error)) folders.push_back(it->path());
            else if (it->is_regular_file(type_error) && iequals_ascii(to_utf8(it->path().extension()), ".npk")) {
                found.push_back(it->path());
            }
        }
    }
    std::ranges::sort(found);
    std::ranges::sort(unreadable_);
    archives_ = std::move(found);
}

std::vector<fs::path> Installation::resdb_files() const
{
    std::vector<fs::path> files;
    for (const char* location : kResdbLocations) {
        fs::path candidate = root_ / from_utf8(location);
        if (is_file(candidate)) files.push_back(std::move(candidate));
    }
    return files;
}

std::vector<std::string> Installation::unreadable_resdb() const
{
    std::vector<std::string> out;
    for (const char* location : kResdbLocations) {
        std::error_code error;
        const fs::path candidate = root_ / from_utf8(location);
        if (is_file(candidate)) continue;
        (void)fs::status(candidate, error);
        if (error == std::errc::permission_denied) out.emplace_back(location);
    }
    return out;
}

std::optional<fs::path> Installation::relative(const fs::path& path) const
{
    const fs::path rel = resolved(path).lexically_relative(root_);
    if (rel.empty() || *rel.begin() == "..") return std::nullopt;
    return rel;
}

std::optional<int> Installation::mount_rank(const fs::path& archive) const
{
    const auto rel = relative(archive);
    if (!rel) return std::nullopt;
    const auto placement = place(*rel);
    if (!placement) return std::nullopt;
    return npk::mount_rank(to_utf8(placement->root));
}

} // namespace npk
