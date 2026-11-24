#include "npk/paths.hpp"

#include <algorithm>

namespace npk {

std::string to_utf8(const std::filesystem::path& path)
{
    const std::u8string text = path.generic_u8string();
    return {reinterpret_cast<const char*>(text.data()), text.size()};
}

std::filesystem::path from_utf8(std::string_view text)
{
    return std::filesystem::path(std::u8string(reinterpret_cast<const char8_t*>(text.data()), text.size()));
}

namespace {
constexpr char lower_char(char c) noexcept
{
    return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
}
} // namespace

std::string lower_ascii(std::string_view text)
{
    std::string out(text);
    std::ranges::transform(out, out.begin(), lower_char);
    return out;
}

bool iequals_ascii(std::string_view a, std::string_view b) noexcept
{
    return a.size() == b.size() && istarts_with_ascii(a, b);
}

bool istarts_with_ascii(std::string_view text, std::string_view prefix) noexcept
{
    if (prefix.size() > text.size()) return false;
    for (std::size_t i = 0; i < prefix.size(); ++i) {
        if (lower_char(text[i]) != lower_char(prefix[i])) return false;
    }
    return true;
}

std::vector<std::string> path_parts(const std::filesystem::path& path)
{
    std::vector<std::string> parts;
    for (const auto& element : path) {
        std::string part = to_utf8(element);
        if (!part.empty()) parts.push_back(std::move(part));
    }
    return parts;
}

std::optional<std::string> safe_relative_path(std::string_view inner_path)
{
    std::string out;
    std::size_t start = 0;
    while (start <= inner_path.size()) {
        std::size_t end = inner_path.find_first_of("\\/", start);
        if (end == std::string_view::npos) end = inner_path.size();
        const std::string_view part = inner_path.substr(start, end - start);
        if (!part.empty() && part != ".") {
            if (part == ".." || part.find(':') != std::string_view::npos) return std::nullopt;
            if (!out.empty()) out += '/';
            out += part;
        }
        start = end + 1;
    }
    if (out.empty()) return std::nullopt;
    return out;
}

std::string folder_tag(std::string_view label)
{
    std::string out;
    bool in_run = false;
    for (const char c : label) {
        if (c == '\\' || c == '/' || c == ':') {
            if (!in_run) out += '_';
            in_run = true;
        } else {
            out += c;
            in_run = false;
        }
    }
    const auto first = out.find_first_not_of('_');
    if (first == std::string::npos) return {};
    const auto last = out.find_last_not_of('_');
    return out.substr(first, last - first + 1);
}

namespace {
template <class String>
String extended(const String& absolute, const String& prefix, const String& unc_prefix)
{
    if (absolute.starts_with(prefix)) return absolute;
    const String two_slashes = prefix.substr(0, 2);
    if (absolute.starts_with(two_slashes)) return unc_prefix + absolute.substr(2);
    return prefix + absolute;
}
} // namespace

std::string extended_length_path(std::string_view absolute)
{
    return extended(std::string(absolute), std::string("\\\\?\\"), std::string("\\\\?\\UNC\\"));
}

std::filesystem::path long_path(const std::filesystem::path& path)
{
#if defined(_WIN32)
    std::error_code error;
    std::filesystem::path absolute = std::filesystem::absolute(path, error);
    if (error) return path;
    absolute = absolute.lexically_normal();
    absolute.make_preferred();
    return extended(absolute.wstring(), std::wstring(L"\\\\?\\"), std::wstring(L"\\\\?\\UNC\\"));
#else
    return path;
#endif
}

} // namespace npk
