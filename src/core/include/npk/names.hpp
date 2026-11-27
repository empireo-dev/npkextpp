#pragma once

#include "npk/error.hpp"

#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace npk {

// Every resource path of the merged resdb manifests, sorted case-insensitively
// once, so that all paths under a mount prefix are one contiguous range.
class ResourceManifest {
public:
    ResourceManifest() = default;
    explicit ResourceManifest(std::vector<std::string> paths);

    // Merges the paths of several resdb files (the shipped and the patched one).
    [[nodiscard]] static Result<ResourceManifest> from_files(std::span<const std::filesystem::path> files);

    [[nodiscard]] std::size_t size() const noexcept { return paths_.size(); }
    [[nodiscard]] std::span<const std::string> paths() const noexcept { return paths_; }

    // Every path that starts with `prefix`, ignoring upper/lower case.
    [[nodiscard]] std::span<const std::string> paths_under(std::string_view prefix) const;

private:
    std::vector<std::string> paths_;   // sorted by keys_
    std::vector<std::string> keys_;    // lower case
};

// id -> path inside the archive (backslash separated, views into the manifest).
using NameTable = std::unordered_map<std::uint32_t, std::string_view>;

// The names of one archive: the tables of its candidate prefixes, first one wins.
class ArchiveNames {
public:
    ArchiveNames() = default;
    explicit ArchiveNames(std::vector<const NameTable*> tables) : tables_(std::move(tables)) {}

    [[nodiscard]] std::optional<std::string_view> find(std::uint32_t id) const;

private:
    std::vector<const NameTable*> tables_;
};

// Where in the resource tree the files of an archive are listed: the mount
// prefix ("ui\\", "model\\character\\", "" for the root). Empty for archives resdb
// does not cover (scripts). A loose copy outside every mount folder is tried as
// a root archive and as a top-level branch: {"", "<stem>\\"}.
[[nodiscard]] std::vector<std::string> candidate_prefixes(const std::filesystem::path& archive);

// Recovers file names: an entry id is the murmur3 of the path relative to where
// its archive is mounted, so for a prefix every manifest path under it is
// hashed with the prefix stripped. Tables are cached per prefix (several
// archives mount at the same place). Not thread-safe; the manifest must outlive it.
class NameResolver {
public:
    explicit NameResolver(const ResourceManifest& manifest) : manifest_(&manifest) {}

    [[nodiscard]] const NameTable& table_for(std::string_view prefix);
    [[nodiscard]] ArchiveNames names_for(std::span<const std::string> prefixes);
    [[nodiscard]] ArchiveNames names_for(const std::filesystem::path& archive);

    // For an archive copied out of the installation, where its place does not say
    // the prefix: the manifest folder under which the most of `ids` are listed
    // (every manifest path is hashed once per folder level), and how many. Empty
    // if no id is listed below any folder.
    [[nodiscard]] std::pair<std::string, std::size_t> guess_prefix(std::span<const std::uint32_t> ids) const;

private:
    const ResourceManifest* manifest_;
    std::unordered_map<std::string, NameTable> tables_;
};

} // namespace npk
