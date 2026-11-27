#include "npk/install.hpp"

#include "npk/mounts.hpp"
#include "npk/paths.hpp"

#include <algorithm>
#include <fstream>
#include <string_view>

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

// A file named resdb that starts like a name manifest.
bool is_manifest(const fs::path& path)
{
    if (!iequals_ascii(to_utf8(path.filename()), "resdb")) return false;
    std::ifstream file(path, std::ios::binary);
    char magic[4] = {};
    return file.read(magic, sizeof magic) && std::string_view(magic, sizeof magic) == "CCZ!";
}

} // namespace

std::string relative_text(const fs::path& path, const fs::path& root)
{
    std::string rel = to_utf8(path.lexically_relative(root));
    std::ranges::replace(rel, '\\', '/');
    return rel.empty() || rel == "." ? std::string(".") : rel;
}

FolderScan scan_folder(const fs::path& root_folder, const ScanProgress& progress)
{
    const fs::path root = resolved(root_folder);
    FolderScan found;
    std::vector<fs::path> folders{root};
    std::vector<fs::path> archives, manifests, below;   // of one folder
    while (!folders.empty()) {
        const fs::path folder = std::move(folders.back());
        folders.pop_back();
        std::error_code error;
        fs::directory_iterator it(folder, error);
        if (error) {
            found.unreadable.push_back(relative_text(folder, root));
            continue;
        }
        archives.clear(), manifests.clear(), below.clear();
        bool marked = false;   // has a .noscan file: the whole subtree is left out
        for (; !error && it != fs::directory_iterator(); it.increment(error)) {
            std::error_code type_error;
            if (it->is_symlink(type_error)) continue;
            if (it->is_directory(type_error)) {
                below.push_back(it->path());
            } else if (it->is_regular_file(type_error)) {
                const fs::path name = it->path().filename();
                if (name == kNoScanFileName) marked = true;
                else if (iequals_ascii(to_utf8(it->path().extension()), ".npk")) archives.push_back(it->path());
                else if (is_manifest(it->path())) manifests.push_back(it->path());
            }
        }
        if (marked) {
            found.skipped.push_back(relative_text(folder, root));
        } else {
            found.archives.insert(found.archives.end(), archives.begin(), archives.end());
            found.manifests.insert(found.manifests.end(), manifests.begin(), manifests.end());
            folders.insert(folders.end(), below.begin(), below.end());
        }
        if (progress) progress(found);
    }
    std::ranges::sort(found.archives);
    std::ranges::sort(found.manifests);
    std::ranges::sort(found.unreadable);
    std::ranges::sort(found.skipped);
    return found;
}

Result<void> mark_no_scan(const fs::path& folder)
{
    const fs::path path = folder / kNoScanFileName;
    std::error_code error;
    if (fs::is_regular_file(path, error)) return {};
    fs::create_directories(folder, error);
    std::ofstream file(long_path(path), std::ios::binary | std::ios::trunc);
    file << kNoScanText;
    file.close();
    if (!file) return make_error(Errc::io_error, "cannot write " + to_utf8(path));
    return {};
}

Result<Installation> Installation::open(const fs::path& folder, const ScanProgress& progress)
{
    const fs::path root = resolved(folder);
    if (!is_dir(root)) return make_error(Errc::invalid_input, "installation folder not found: " + to_utf8(folder));
    Installation installation(root);
    if (!installation.looks_valid()) {
        return make_error(Errc::invalid_input, to_utf8(root) + " does not look like a game installation "
                                                               "(expected neox_bin/ or res/resdb + Documents/)");
    }
    installation.scan(progress);
    return installation;
}

std::optional<Installation> Installation::find_above(const fs::path& path)
{
    fs::path folder = resolved(path);
    while (folder.has_parent_path() && folder.parent_path() != folder) {
        folder = folder.parent_path();
        Installation candidate(folder);
        if (candidate.looks_valid()) {
            candidate.scan({});
            return candidate;
        }
    }
    return std::nullopt;
}

bool Installation::looks_like_one(const fs::path& folder)
{
    return Installation(resolved(folder)).looks_valid();
}

bool Installation::looks_valid() const
{
    const bool has_engine = is_dir(root_ / "neox_bin");
    const bool has_resources = is_file(root_ / "res" / "resdb") && is_dir(root_ / "Documents");
    return has_engine || has_resources;
}

void Installation::scan(const ScanProgress& progress)
{
    FolderScan found = scan_folder(root_, progress);
    archives_ = std::move(found.archives);
    manifests_ = std::move(found.manifests);
    unreadable_ = std::move(found.unreadable);
    skipped_ = std::move(found.skipped);
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
