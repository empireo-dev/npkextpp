#include "npk/filetypes.hpp"

#include <array>
#include <cstring>
#include <utility>

namespace npk {

namespace {

using namespace std::string_view_literals;

// (signature, extension), checked in order, so more specific signatures come first.
constexpr std::array<std::pair<std::string_view, std::string_view>, 19> kSignatures{{
    {"DDS "sv, "dds"sv},
    {"\x89PNG"sv, "png"sv},
    {"\xff\xd8\xff"sv, "jpg"sv},
    {"\xabKTX"sv, "ktx"sv},
    {"\x34\x80\xc8\xbb"sv, "mesh"sv},         // NeoX CMesh
    {"SKEL"sv, "skeleton"sv},                 // NeoX skeleton
    {"RAWA"sv, "rawanimation"sv},             // NeoX raw animation
    {"\xc1\x59\x41\x0d"sv, "bxml"sv},         // NeoX binary XML (materials, prefabs, scenes)
    {"RIFF"sv, "wem"sv},                      // Wwise audio
    {"BKHD"sv, "bnk"sv},                      // Wwise sound bank
    {"OggS"sv, "ogg"sv},
    {"NTRK"sv, "ntrk"sv},
    {"TZif"sv, "tzif"sv},                     // time zone data
    {"PK\x03\x04"sv, "zip"sv},
    {"\x1d\x04"sv, "enc"sv},                  // game script, still encrypted by the script layer
    {"<?xml"sv, "xml"sv},
    {"\xef\xbb\xbf<"sv, "xml"sv},
    {"<"sv, "xml"sv},
    {"{"sv, "json"sv},
}};

} // namespace

std::string_view guess_extension(std::span<const std::byte> data) noexcept
{
    for (const auto& [signature, extension] : kSignatures) {
        if (data.size() >= signature.size() && std::memcmp(data.data(), signature.data(), signature.size()) == 0) {
            return extension;
        }
    }
    return kUnknownExtension;
}

} // namespace npk
