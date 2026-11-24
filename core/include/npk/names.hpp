#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace npk {

// Recovers file names: an entry id is the murmur3 of the path relative to where
// the archive is mounted, so for a mount prefix we hash every manifest path
// under that prefix (with the prefix stripped) and look the id up.
class NameResolver {
public:
    // Adds manifest paths (resdb lines, backslash-separated, original case).
    void add_paths(std::vector<std::string> paths);

    // Builds the id → name table for one mount prefix ("" for the tree root,
    // "model\\character\\" for res/model/character.npk). Cheap if already built.
    void prepare_prefix(std::string_view prefix);

    // The path inside the archive for an id, if the manifest lists it.
    [[nodiscard]] std::optional<std::string_view> resolve(std::string_view prefix,
                                                          std::uint32_t id) const;

private:
    std::vector<std::string> paths_;
};

} // namespace npk
