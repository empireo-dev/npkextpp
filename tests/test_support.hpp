#pragma once

// Tiny test harness and synthetic-data builders. No game data is used: the
// archives are written with the same rules the extractor undoes, so a passing
// round trip shows the reader and the format description agree.

#include "npk/decoder.hpp"
#include "npk/entry.hpp"
#include "npk/hash.hpp"
#include "npk/paths.hpp"

#include <libdeflate.h>
#include <lz4.h>
#include <zstd.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <functional>
#include <random>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace test {

namespace fs = std::filesystem;

// --- Harness -------------------------------------------------------------------

struct Registry {
    std::vector<std::pair<const char*, void (*)()>> tests;
    int failures = 0;
    static Registry& get()
    {
        static Registry registry;
        return registry;
    }
};

struct Registrar {
    Registrar(const char* name, void (*fn)()) { Registry::get().tests.emplace_back(name, fn); }
};

#define TEST(name)                                                  \
    static void name();                                             \
    static const ::test::Registrar registrar_##name(#name, &name); \
    static void name()

#define CHECK(cond)                                                                          \
    do {                                                                                     \
        if (!(cond)) {                                                                       \
            std::fprintf(stderr, "  %s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); \
            ++::test::Registry::get().failures;                                              \
        }                                                                                    \
    } while (0)

#define CHECK_EQ(a, b)                                                                                     \
    do {                                                                                                   \
        const auto& check_a_ = (a);                                                                        \
        const auto& check_b_ = (b);                                                                        \
        if (!(check_a_ == check_b_)) {                                                                     \
            std::fprintf(stderr, "  %s:%d: CHECK_EQ failed: %s == %s\n", __FILE__, __LINE__, #a, #b);    \
            ++::test::Registry::get().failures;                                                            \
        }                                                                                                  \
    } while (0)

inline int run_all()
{
    auto& registry = Registry::get();
    int failed_tests = 0;
    for (const auto& [name, fn] : registry.tests) {
        const int before = registry.failures;
        try {
            fn();
        } catch (const std::exception& error) {
            std::fprintf(stderr, "  exception: %s\n", error.what());
            ++registry.failures;
        }
        const bool ok = registry.failures == before;
        std::printf("%s %s\n", ok ? "[ ok ]" : "[FAIL]", name);
        if (!ok) ++failed_tests;
    }
    std::printf("%zu tests, %d failed\n", registry.tests.size(), failed_tests);
    return failed_tests == 0 ? 0 : 1;
}

// --- Data helpers ------------------------------------------------------------------

using Bytes = std::vector<std::byte>;

inline Bytes bytes(std::string_view text)
{
    const auto view = std::as_bytes(std::span(text.data(), text.size()));
    return {view.begin(), view.end()};
}

inline Bytes repeat(std::string_view text, std::size_t times)
{
    std::string out;
    for (std::size_t i = 0; i < times; ++i) out += text;
    return bytes(out);
}

inline Bytes random_bytes(std::size_t size, std::uint32_t seed, bool compressible = true)
{
    std::mt19937 rng(seed);
    Bytes out(size);
    for (std::size_t i = 0; i < size; ++i) {
        const auto value = compressible ? (rng() % 16) + 'a' : rng() & 0xFF;
        out[i] = static_cast<std::byte>(value);
    }
    return out;
}

inline Bytes compress(const Bytes& content, npk::Compression mode)
{
    Bytes out;
    switch (mode) {
    case npk::Compression::none: return content;
    case npk::Compression::zlib: {
        libdeflate_compressor* c = libdeflate_alloc_compressor(6);
        out.resize(libdeflate_zlib_compress_bound(c, content.size()));
        out.resize(libdeflate_zlib_compress(c, content.data(), content.size(), out.data(), out.size()));
        libdeflate_free_compressor(c);
        return out;
    }
    case npk::Compression::lz4: {
        out.resize(static_cast<std::size_t>(LZ4_compressBound(static_cast<int>(content.size()))));
        const int n = LZ4_compress_default(reinterpret_cast<const char*>(content.data()), reinterpret_cast<char*>(out.data()),
                                           static_cast<int>(content.size()), static_cast<int>(out.size()));
        out.resize(static_cast<std::size_t>(n));
        return out;
    }
    case npk::Compression::zstd: {
        out.resize(ZSTD_compressBound(content.size()));
        out.resize(ZSTD_compress(out.data(), out.size(), content.data(), content.size(), 3));
        return out;
    }
    }
    throw std::runtime_error("bad mode");
}

inline void put32(Bytes& out, std::uint32_t value)
{
    for (int i = 0; i < 4; ++i) out.push_back(static_cast<std::byte>((value >> (8 * i)) & 0xFF));
}

struct FileSpec {
    std::string inner_path;
    Bytes content;
    npk::Compression mode = npk::Compression::lz4;
    bool encrypted = true;
};

// The bytes of an NXPK archive holding `files` (stored in the given order).
inline Bytes build_npk(const std::vector<FileSpec>& files)
{
    struct Row {
        std::uint32_t id, offset, packed, raw, crc_packed, crc_raw, flags;
    };
    Bytes body;
    std::vector<Row> rows;
    for (const FileSpec& file : files) {
        Bytes stored = compress(file.content, file.mode);
        if (file.encrypted) npk::decrypt(stored);   // XOR is symmetric: decrypt() also encrypts
        const std::uint32_t flags = static_cast<std::uint32_t>(file.mode) | (file.encrypted ? npk::Entry::kEncryptedFlag : 0u);
        rows.push_back({npk::path_id(file.inner_path), static_cast<std::uint32_t>(0x18 + body.size()),
                        static_cast<std::uint32_t>(stored.size()), static_cast<std::uint32_t>(file.content.size()),
                        npk::murmur3_32(stored), npk::murmur3_32(file.content), flags});
        body.insert(body.end(), stored.begin(), stored.end());
    }
    std::ranges::sort(rows, {}, &Row::id);

    Bytes out = bytes("NXPK");
    put32(out, static_cast<std::uint32_t>(rows.size()));
    put32(out, 0);
    put32(out, 0);
    put32(out, 0);
    put32(out, static_cast<std::uint32_t>(0x18 + body.size()));
    out.insert(out.end(), body.begin(), body.end());
    for (const Row& row : rows) {
        for (const std::uint32_t v : {row.id, row.offset, row.packed, row.raw, row.crc_packed, row.crc_raw, row.flags}) {
            put32(out, v);
        }
    }
    return out;
}

inline void write_file(const fs::path& path, const Bytes& data)
{
    fs::create_directories(path.parent_path());
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out.write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(data.size()));
}

inline Bytes read_file(const fs::path& path)
{
    std::ifstream in(path, std::ios::binary);
    const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    return bytes(text);
}

inline fs::path write_npk(const fs::path& path, const std::vector<FileSpec>& files)
{
    write_file(path, build_npk(files));
    return path;
}

inline fs::path write_resdb(const fs::path& path, const std::vector<std::string>& resource_paths)
{
    std::string text;
    for (std::size_t i = 0; i < resource_paths.size(); ++i) {
        if (i) text += '\n';
        text += "0 " + resource_paths[i];
    }
    Bytes out = bytes("CCZ!");
    out.resize(16);
    const Bytes packed = compress(bytes(text), npk::Compression::zlib);
    out.insert(out.end(), packed.begin(), packed.end());
    write_file(path, out);
    return path;
}

// A fresh empty folder, removed when the object goes away.
class TempDir {
public:
    TempDir()
    {
        static int counter = 0;
        std::random_device rd;
        path_ = fs::temp_directory_path() / ("npkextpp-test-" + std::to_string(rd()) + "-" + std::to_string(counter++));
        fs::create_directories(path_);
    }
    ~TempDir()
    {
        std::error_code error;
        fs::remove_all(path_, error);
    }
    TempDir(const TempDir&) = delete;
    TempDir& operator=(const TempDir&) = delete;
    [[nodiscard]] const fs::path& path() const { return path_; }

private:
    fs::path path_;
};

// The miniature installation of npkext's tests (conftest.fake_install):
//   res/resdb                    names for everything below
//   res.npk                      common\x.txt, and its own (different) copy of ui\login\bg.png
//   res/ui.npk                   login\bg.png (old), icons\a.png
//   res/model/hero.npk           body.mesh
//   Documents/res/ui.npk         login\bg.png (new), an unlisted file
//   script.npk                   one file not in the manifest
//   Documents/script_patch.npk   a newer copy of that file
inline fs::path fake_install(const fs::path& base)
{
    const fs::path root = base / "install";
    fs::create_directories(root / "neox_bin");
    write_resdb(root / "res" / "resdb", {"ui\\login\\bg.png", "ui\\icons\\a.png", "model\\hero\\body.mesh", "common\\x.txt"});
    write_npk(root / "res.npk", {{"common\\x.txt", repeat("X", 100)}, {"ui\\login\\bg.png", repeat("ROOT", 50)}});
    write_npk(root / "res" / "ui.npk", {{"login\\bg.png", repeat("OLD", 50)}, {"icons\\a.png", repeat("A", 300)}});
    Bytes mesh = bytes("\x34\x80\xc8\xbb");
    const Bytes m = repeat("m", 500);
    mesh.insert(mesh.end(), m.begin(), m.end());
    write_npk(root / "res" / "model" / "hero.npk", {{"body.mesh", mesh}});
    write_npk(root / "Documents" / "res" / "ui.npk",
              {{"login\\bg.png", repeat("NEW", 50)}, {"unlisted.xml", bytes("<?xml version='1.0'?><a/>")}});
    Bytes script = bytes("\x1d\x04");
    for (int i = 0; i < 200; ++i) script.push_back(static_cast<std::byte>(i));
    write_npk(root / "script.npk", {{"anything", script}});
    Bytes patched = bytes("\x1d\x04");
    const Bytes p = repeat("patched", 30);
    patched.insert(patched.end(), p.begin(), p.end());
    write_npk(root / "Documents" / "script_patch.npk", {{"anything", patched}});
    return root;
}

} // namespace test
