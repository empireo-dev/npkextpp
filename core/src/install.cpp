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
    return installation;
}

std::optional<Installation> Installation::find_above(const fs::path& path)
{
    fs::path folder = resolved(path);
    while (folder.has_parent_path() && folder.parent_path() != folder) {
        folder = folder.parent_path();
        Installation candidate(folder);
        if (candidate.looks_valid()) return candidate;
    }
    return std::nullopt;
}

bool Installation::looks_valid() const
{
    const bool has_engine = is_dir(root_ / "neox_bin");
    const bool has_resources = is_file(root_ / "res" / "resdb") && is_dir(root_ / "Documents");
    return has_engine || has_resources;
}

std::vector<fs::path> Installation::archives() const
{
    std::vector<fs::path> found;
    std::error_code error;
    fs::recursive_directory_iterator it(root_, fs::directory_options::skip_permission_denied, error);
    for (; !error && it != fs::recursive_directory_iterator(); it.increment(error)) {
        std::error_code type_error;
        if (!it->is_regular_file(type_error)) continue;
        if (iequals_ascii(to_utf8(it->path().extension()), ".npk")) found.push_back(it->path());
    }
    std::ranges::sort(found);
    return found;
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
