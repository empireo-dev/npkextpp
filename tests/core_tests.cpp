// Tests for npk_core. Cases mirror npkext's pytest suite where one exists.
#include "test_support.hpp"

#include "npk/archive.hpp"
#include "npk/encoder.hpp"
#include "npk/archive_update.hpp"
#include "npk/decoder.hpp"
#include "npk/filetypes.hpp"
#include "npk/hash.hpp"
#include "npk/inputs.hpp"
#include "npk/install.hpp"
#include "npk/mounts.hpp"
#include "npk/names.hpp"
#include "npk/paths.hpp"
#include "npk/plan.hpp"
#include "npk/resdb.hpp"

#include <map>
#include <set>
#if !defined(_WIN32)
#include <unistd.h>
#endif
#include <optional>

using namespace test;
namespace fs = std::filesystem;

namespace {

std::vector<std::string> strings(std::initializer_list<const char*> list)
{
    return {list.begin(), list.end()};
}

const npk::OutputGroup* group_named(const npk::Plan& plan, std::string_view folder)
{
    for (const auto& group : plan.groups) {
        if (group.folder == folder) return &group;
    }
    return nullptr;
}

const npk::PlannedEntry* planned_entry(const npk::ArchiveSource& source, std::string_view inner_path)
{
    const std::uint32_t id = npk::path_id(inner_path);
    for (const auto& planned : source.entries) {
        if (source.entry(planned).id == id) return &planned;
    }
    return nullptr;
}

const npk::ArchiveSource* source_labelled(const npk::OutputGroup& group, std::string_view label)
{
    for (const auto& source : group.sources) {
        if (source.label() == label) return &source;
    }
    return nullptr;
}

npk::Plan plan_install(const fs::path& root, const npk::PlanOptions& options, npk::ResourceManifest& manifest_out,
                       std::optional<npk::Installation>& installation_out)
{
    installation_out = *npk::Installation::open(root, npk::ArchiveSearch::scan);
    manifest_out = *npk::ResourceManifest::from_files(installation_out->resdb_files());
    npk::Planner planner(manifest_out, &*installation_out);
    const auto archives = installation_out->archives();
    return planner.build(archives, options);
}

} // namespace

// --- Hash ------------------------------------------------------------------------

TEST(murmur3_matches_reference_vectors)
{
    // Values from Python's mmh3 (the library npkext uses).
    CHECK_EQ(npk::murmur3_32("", 0), 0x0u);
    CHECK_EQ(npk::murmur3_32("", 1), 0x514e28b7u);
    CHECK_EQ(npk::murmur3_32("", 0xffffffffu), 0x81f16f39u);
    CHECK_EQ(npk::murmur3_32(std::string_view("\0\0\0\0", 4), 0), 0x2362f9deu);
    CHECK_EQ(npk::murmur3_32("a"), 0x7fa09ea6u);
    CHECK_EQ(npk::murmur3_32("aa"), 0x5d211726u);
    CHECK_EQ(npk::murmur3_32("aaa"), 0x283e0130u);
    CHECK_EQ(npk::murmur3_32("aaaa"), 0x5a97808au);
    CHECK_EQ(npk::murmur3_32("abcd"), 0xf0478627u);
    CHECK_EQ(npk::murmur3_32("Hello, world!"), 0x24884cbau);
    CHECK_EQ(npk::murmur3_32("The quick brown fox jumps over the lazy dog"), 0x2fa826cdu);
    CHECK_EQ(npk::path_id("login\\bg.png"), 0x24c8037eu);
}

// --- Decrypt / decode ---------------------------------------------------------------

TEST(decrypt_is_the_06_ramp_on_128_bytes_and_its_own_inverse)
{
    Bytes data(200, std::byte{0});
    npk::decrypt(data);
    CHECK_EQ(data[0], std::byte{0x06});
    CHECK_EQ(data[1], std::byte{0x07});
    CHECK_EQ(data[127], std::byte{0x85});
    CHECK_EQ(data[128], std::byte{0x00});
    npk::decrypt(data);
    CHECK(std::ranges::all_of(data, [](std::byte b) { return b == std::byte{0}; }));

    Bytes shorter(5, std::byte{0});   // shorter than 128: only what is there
    npk::decrypt(shorter);
    CHECK_EQ(shorter[4], std::byte{0x0a});
}

TEST(every_compression_mode_round_trips)
{
    TempDir dir;
    const Bytes text = random_bytes(100'000, 1);
    const Bytes noise = random_bytes(5'000, 2, false);
    std::vector<FileSpec> files;
    for (const auto mode : {npk::Compression::none, npk::Compression::zlib, npk::Compression::lz4, npk::Compression::zstd}) {
        for (const bool encrypted : {false, true}) {
            const std::string tag = std::to_string(static_cast<int>(mode)) + (encrypted ? "e" : "p");
            files.push_back({"text\\" + tag, text, mode, encrypted});
            files.push_back({"noise\\" + tag, noise, mode, encrypted});
            files.push_back({"tiny\\" + tag, bytes("x"), mode, encrypted});
            files.push_back({"empty\\" + tag, {}, mode, encrypted});
        }
    }
    const fs::path path = write_npk(dir.path() / "all.npk", files);

    auto archive = npk::Archive::open(path);
    CHECK(archive.has_value());
    if (!archive) return;
    CHECK_EQ(archive->entries().size(), files.size());

    npk::Decoder decoder;
    for (const FileSpec& file : files) {
        const npk::Entry* entry = archive->find(file.inner_path);
        CHECK(entry != nullptr);
        if (!entry) continue;
        CHECK_EQ(entry->compression(), file.mode);
        CHECK_EQ(entry->encrypted(), file.encrypted);
        auto decoded = decoder.read(*archive, *entry);
        CHECK(decoded.has_value());
        if (decoded) CHECK(*decoded == file.content);
    }
}

TEST(damaged_entries_are_reported)
{
    TempDir dir;
    const Bytes content = random_bytes(10'000, 3);
    Bytes npk = build_npk({{"a", content, npk::Compression::lz4, true}});
    const fs::path path = dir.path() / "a.npk";

    write_file(path, npk);
    auto archive = npk::Archive::open(path);
    CHECK(archive.has_value());
    npk::Decoder decoder;
    npk::Entry entry = archive->entries()[0];

    // A flipped stored byte: crc_packed catches it.
    Bytes stored(entry.packed_size);
    CHECK(archive->read_stored(entry, stored).has_value());
    stored[200] ^= std::byte{1};
    Bytes out(entry.raw_size);
    auto result = decoder.decode(stored, entry, out);
    CHECK(!result && result.error().code == npk::Errc::crc_packed_mismatch);

    // Without verification the same damage surfaces as wrong data or a codec error, never a crash.
    CHECK(archive->read_stored(entry, stored).has_value());
    stored[200] ^= std::byte{1};
    (void)decoder.decode(stored, entry, out, false);

    // A wrong crc_raw: the decoded data is rejected.
    npk::Entry wrong = entry;
    wrong.crc_raw ^= 1;
    CHECK(archive->read_stored(entry, stored).has_value());
    result = decoder.decode(stored, wrong, out);
    CHECK(!result && result.error().code == npk::Errc::crc_raw_mismatch);

    // A wrong raw_size: size mismatch.
    npk::Entry small = entry;
    small.raw_size -= 1;
    CHECK(archive->read_stored(entry, stored).has_value());
    Bytes out_small(small.raw_size);
    result = decoder.decode(stored, small, out_small, false);
    CHECK(!result);

    // An unknown compression mode.
    npk::Entry mode9 = entry;
    mode9.flags = 9;
    CHECK(archive->read_stored(entry, stored).has_value());
    result = decoder.decode(stored, mode9, out, false);
    CHECK(!result && result.error().code == npk::Errc::unsupported);
}

TEST(truncated_and_foreign_files_are_not_archives)
{
    TempDir dir;
    write_file(dir.path() / "short.npk", bytes("NXPK"));
    write_file(dir.path() / "other.npk", bytes("PK\x03\x04 this is a zip file, not an archive"));
    Bytes npk = build_npk({{"a", random_bytes(1000, 4)}});
    npk.resize(npk.size() - 10);   // TOC cut off
    write_file(dir.path() / "cut.npk", npk);

    auto a = npk::Archive::open(dir.path() / "short.npk");
    CHECK(!a && a.error().code == npk::Errc::bad_format);
    auto b = npk::Archive::open(dir.path() / "other.npk");
    CHECK(!b && b.error().message.find("not an NXPK archive") != std::string::npos);
    auto c = npk::Archive::open(dir.path() / "cut.npk");
    CHECK(!c && c.error().message.find("runs past the end") != std::string::npos);
    auto d = npk::Archive::open(dir.path() / "missing.npk");
    CHECK(!d && d.error().code == npk::Errc::io_error);
}

TEST(entries_are_found_by_id)
{
    TempDir dir;
    std::vector<FileSpec> files;
    for (int i = 0; i < 50; ++i) files.push_back({"dir\\file" + std::to_string(i) + ".txt", bytes(std::to_string(i))});
    auto archive = npk::Archive::open(write_npk(dir.path() / "x.npk", files));
    CHECK(archive.has_value());
    CHECK(std::ranges::is_sorted(archive->entries(), {}, &npk::Entry::id));
    for (const auto& file : files) CHECK(archive->find(file.inner_path) != nullptr);
    CHECK(archive->find("dir\\missing.txt") == nullptr);
}

// --- resdb / names -------------------------------------------------------------------

TEST(read_resdb)
{
    TempDir dir;
    const fs::path resdb = write_resdb(dir.path() / "resdb", {"ui\\a.png", "model\\x\\b.mesh"});
    auto paths = npk::read_resdb(resdb);
    CHECK(paths.has_value());
    if (paths) CHECK(*paths == strings({"ui\\a.png", "model\\x\\b.mesh"}));

    // \r\n line ends and lines without a path.
    Bytes data = bytes("CCZ!");
    data.resize(16);
    const Bytes packed = compress(bytes("0 a\\b\r\n1 c\\d\r\nnoflag\n\n2 e"), npk::Compression::zlib);
    data.insert(data.end(), packed.begin(), packed.end());
    auto parsed = npk::parse_resdb(data);
    CHECK(parsed && *parsed == strings({"a\\b", "c\\d", "e"}));
}

TEST(read_resdb_rejects_other_files)
{
    TempDir dir;
    write_file(dir.path() / "resdb", bytes("not a manifest at all"));
    auto paths = npk::read_resdb(dir.path() / "resdb");
    CHECK(!paths && paths.error().message.find("CCZ!") != std::string::npos);
}

TEST(manifest_prefix_search_ignores_case)
{
    const npk::ResourceManifest manifest(strings({"ui\\b.png", "UI\\a.png", "uix\\c.png", "model\\d.mesh", "ui\\b.png"}));
    const auto under = manifest.paths_under("ui\\");
    CHECK(std::vector<std::string>(under.begin(), under.end()) == strings({"UI\\a.png", "ui\\b.png"}));
    CHECK_EQ(manifest.size(), 4u);
    CHECK_EQ(manifest.paths_under("").size(), 4u);
    CHECK(manifest.paths_under("zzz").empty());
}

TEST(resolver_names_entries_relative_to_the_archive)
{
    const npk::ResourceManifest manifest(strings({"ui\\login\\bg.png", "model\\x.mesh"}));
    npk::NameResolver resolver(manifest);
    const npk::ArchiveNames names = resolver.names_for(fs::path("game/res/ui.npk"));
    CHECK(names.find(npk::path_id("login\\bg.png")) == std::optional<std::string_view>("login\\bg.png"));
    CHECK(!names.find(npk::path_id("ui\\login\\bg.png")));
}

TEST(candidate_prefixes)
{
    const std::vector<std::pair<const char*, std::vector<std::string>>> cases = {
        {"game/res/ui.npk", strings({"ui\\"})},
        {"game/res/model/character.npk", strings({"model\\character\\"})},
        {"game/Documents/res2/model/character.npk", strings({"model\\character\\"})},
        {"game/res/levelsets/lake.npk", strings({"levelsets\\lake\\"})},
        {"game/res.npk", strings({""})},
        {"game/Documents/res_dlc/a/b/c.npk", strings({"a\\b\\c\\"})},   // res_dlc is mounted 3 deep
        {"game/script.npk", {}},                                           // scripts are not in resdb
        {"loose/ui.npk", strings({"", "ui\\"})},
    };
    for (const auto& [archive, expected] : cases) {
        const bool ok = npk::candidate_prefixes(archive) == expected;
        if (!ok) std::fprintf(stderr, "  candidate_prefixes(%s)\n", archive);
        CHECK(ok);
    }
}

// --- Mounts -------------------------------------------------------------------------

TEST(place_finds_the_mount_folder)
{
    struct Case {
        const char* archive;
        const char* mount;
        std::vector<std::string> branch;
    };
    const std::vector<Case> cases = {
        {"res.npk", "res", {}},
        {"Documents/res.npk", "Documents/res", {}},
        {"res/ui.npk", "res", strings({"ui"})},
        {"Documents/res4/model/character.npk", "Documents/res4", strings({"model", "character"})},
        {"Documents/res_silent1/a/b/c/d.npk", "Documents/res_silent1", strings({"a", "b", "c", "d"})},
        {"Documents/script_patch.npk", "Documents/script_patch", {}},
    };
    for (const auto& c : cases) {
        const auto placement = npk::place(c.archive);
        const bool ok = placement && npk::to_utf8(placement->root) == c.mount && placement->branch == c.branch;
        if (!ok) std::fprintf(stderr, "  place(%s)\n", c.archive);
        CHECK(ok);
    }
}

TEST(place_rejects_archives_outside_mount_rules)
{
    for (const char* archive : {"res/a/b/c.npk", "script/x.npk", "copies/ui.npk"}) {
        CHECK(!npk::place(archive).has_value());   // too deep for res, or no mount folder
    }
}

TEST(mount_priority_follows_the_log_order)
{
    CHECK(*npk::mount_rank("Documents/res4") < *npk::mount_rank("Documents/res"));
    CHECK(*npk::mount_rank("Documents/res") < *npk::mount_rank("res"));
    CHECK(*npk::mount_rank("res") < *npk::mount_rank("res2"));
    CHECK(*npk::mount_rank("Documents/script_patch") < *npk::mount_rank("script"));
    CHECK(!npk::mount_rank("copies").has_value());
    CHECK_EQ(npk::mount_rank("documents/RES").value_or(-1), npk::mount_rank("Documents/res").value_or(-2));
}

TEST(output_folder_mirrors_the_resource_tree)
{
    const std::vector<std::pair<const char*, const char*>> cases = {
        {"GAME/res.npk", "res"},
        {"GAME/Documents/res.npk", "res"},
        {"GAME/script.npk", "script"},
        {"GAME/Documents/script_patch.npk", "script"},
        {"GAME/res/ui.npk", "res/ui"},
        {"GAME/Documents/res2/ui.npk", "res/ui"},
        {"GAME/Documents/res4/model/character.npk", "res/model/character"},
        {"GAME/res/levelsets/lake.npk", "res/levelsets/lake"},
        {"copies/loose.npk", "loose"},
    };
    for (const auto& [archive, folder] : cases) {
        const bool ok = npk::locate(archive, nullptr).folder == folder;
        if (!ok) std::fprintf(stderr, "  output folder of %s\n", archive);
        CHECK(ok);
    }
}

// --- Paths ----------------------------------------------------------------------------

TEST(safe_relative_path)
{
    CHECK(npk::safe_relative_path("a\\b\\c.dds") == std::optional<std::string>("a/b/c.dds"));
    CHECK(npk::safe_relative_path("a\\\\.\\b") == std::optional<std::string>("a/b"));
    CHECK(!npk::safe_relative_path("..\\escape.txt"));
    CHECK(!npk::safe_relative_path("C:\\abs.txt"));
    CHECK(!npk::safe_relative_path(""));
    CHECK(!npk::safe_relative_path("\\\\"));
}

TEST(extended_length_path)
{
    CHECK_EQ(npk::extended_length_path("C:\\out"), std::string("\\\\?\\C:\\out"));
    CHECK_EQ(npk::extended_length_path("\\\\server\\share\\x"), std::string("\\\\?\\UNC\\server\\share\\x"));
    CHECK_EQ(npk::extended_length_path("\\\\?\\C:\\out"), std::string("\\\\?\\C:\\out"));
}

TEST(folder_tag)
{
    CHECK_EQ(npk::folder_tag("Documents/res/ui.npk"), std::string("Documents_res_ui.npk"));
    CHECK_EQ(npk::folder_tag("C:\\\\games//x.npk"), std::string("C_games_x.npk"));
    CHECK_EQ(npk::folder_tag("/home/me/copies/ui.npk"), std::string("home_me_copies_ui.npk"));
}

TEST(guess_extension)
{
    const std::vector<std::pair<std::string, std::string_view>> cases = {
        {"DDS \x7c", "dds"}, {"\x89PNG\r\n", "png"}, {"\x34\x80\xc8\xbbxx", "mesh"}, {"\xc1\x59\x41\x0d", "bxml"},
        {"RIFF....WAVE", "wem"}, {"<?xml version", "xml"}, {"<a/>", "xml"}, {"{\"a\":1}", "json"},
        {"\x1d\x04xx", "enc"}, {"??", "bin"}, {"", "bin"},
    };
    for (const auto& [data, ext] : cases) CHECK_EQ(npk::guess_extension(bytes(data)), ext);
}

// --- Inputs -----------------------------------------------------------------------------

TEST(wildcard_match)
{
    CHECK(npk::wildcard_match("ui.npk", "ui.npk"));
    CHECK(npk::wildcard_match("res_dlc_y2.npk", "res_dlc_*"));
    CHECK(npk::wildcard_match("abc", "a?c"));
    CHECK(npk::wildcard_match("a1", "a[0-9]"));
    CHECK(!npk::wildcard_match("ab", "a[!b]"));
    CHECK(npk::wildcard_match("ac", "a[!b]"));
    CHECK(npk::wildcard_match("a[", "a["));
    CHECK(npk::wildcard_match("", "*"));
    CHECK(!npk::wildcard_match("ui.npk", "ui"));
    CHECK(npk::wildcard_match("aXbXc", "*b*c"));
    CHECK(!npk::wildcard_match("aXbXd", "*b*c"));
}

TEST(close_matches_like_difflib)
{
    const auto names = strings({"hero.npk", "res.npk", "ui.npk", "script.npk", "script_patch.npk"});
    // Expected values from difflib.get_close_matches(word, candidates, n=4, cutoff=0.5), as npkext calls it.
    CHECK(npk::close_matches("heroo.npk", names) == strings({"hero.npk", "res.npk", "ui.npk", "script.npk"}));
    const auto textures = strings({"texture.npk", "texture2.npk", "tex.npk", "ui.npk"});
    CHECK(npk::close_matches("textur", textures) == strings({"texture.npk", "texture2.npk"}));
}

TEST(filter_by_name)
{
    TempDir dir;
    const fs::path root = fake_install(dir.path());
    const auto installation = npk::Installation::open(root, npk::ArchiveSearch::scan);
    CHECK(installation.has_value());
    const auto archives = installation->archives();
    for (const char* pattern : {"ui.npk", "ui", "UI", "u?.npk", "u*"}) {
        auto selected = npk::filter_by_name(archives, strings({pattern}));
        CHECK(selected.has_value());
        if (!selected) continue;
        CHECK_EQ(selected->size(), 2u);   // res/ui.npk and Documents/res/ui.npk
        for (const auto& path : *selected) CHECK_EQ(npk::to_utf8(path.filename()), std::string("ui.npk"));
    }
    auto missing = npk::filter_by_name(archives, strings({"heroo.npk"}));
    CHECK(!missing && missing.error().message.find("did you mean hero.npk") != std::string::npos);
}

TEST(resolve_inputs)
{
    TempDir dir;
    const fs::path root = fake_install(dir.path());
    const auto installation = npk::Installation::open(root, npk::ArchiveSearch::scan);
    CHECK(installation.has_value());

    auto by_path = npk::resolve_inputs(strings({"res/ui.npk"}), &*installation);
    CHECK(by_path && by_path->size() == 1);

    auto by_glob = npk::resolve_inputs(strings({"Documents/*.npk"}), &*installation);
    CHECK(by_glob && by_glob->size() == 1 && by_glob->front().filename() == "script_patch.npk");

    auto deep = npk::resolve_inputs(strings({"**/ui.npk"}), &*installation);
    CHECK(deep && deep->size() == 2);

    auto by_name = npk::resolve_inputs(strings({"ui", "hero", "ui.npk"}), &*installation);
    CHECK(by_name && by_name->size() == 3);   // duplicates dropped

    auto direct = npk::resolve_inputs(strings({npk::to_utf8(root / "res.npk").c_str()}), nullptr);
    CHECK(direct && direct->size() == 1);

    auto need_install = npk::resolve_inputs(strings({"ui"}), nullptr);
    CHECK(!need_install && need_install.error().message.find("--install") != std::string::npos);
    auto bad_path = npk::resolve_inputs(strings({"nowhere/x.npk"}), &*installation);
    CHECK(!bad_path && bad_path.error().message.find("no archive at") != std::string::npos);
}

// --- Installation -------------------------------------------------------------------------

TEST(known_archive_list_is_checked_instead_of_scanning)
{
    TempDir dir;
    const fs::path root = fake_install(dir.path());
    const auto known = strings({"res/ui.npk", "res/model/hero.npk", "res/gone.npk", "Documents/res/ui.npk"});

    const auto listed = npk::Installation::open(root, npk::ArchiveSearch::known, known);
    CHECK(listed.has_value());
    CHECK(!listed->scanned());
    CHECK_EQ(listed->known_count(), 4u);
    CHECK_EQ(listed->archives().size(), 3u);   // res.npk and the scripts are not on the list: not looked for
    CHECK(listed->missing_archives() == strings({"res/gone.npk"}));

    const auto scanned = npk::Installation::open(root, npk::ArchiveSearch::scan, known);
    CHECK(scanned && scanned->scanned() && scanned->archives().size() == 6u && scanned->missing_archives().empty());

    // None of the list is here (an empty list, another game version): scan after all.
    const auto unknown = npk::Installation::open(root, npk::ArchiveSearch::known, strings({"res/other.npk"}));
    CHECK(unknown && unknown->scanned() && unknown->archives().size() == 6u && unknown->missing_archives().empty());
}

TEST(folders_that_cannot_be_opened_are_reported)
{
#if !defined(_WIN32)
    if (::geteuid() == 0) return;   // root opens everything
    TempDir dir;
    const fs::path root = fake_install(dir.path());
    const auto known = strings({"res/ui.npk", "res/model/hero.npk", "Documents/res/ui.npk"});
    fs::permissions(root / "res" / "model", fs::perms::none);
    fs::permissions(root / "Documents", fs::perms::none);

    const auto listed = npk::Installation::open(root, npk::ArchiveSearch::known, known);
    const auto scanned = npk::Installation::open(root, npk::ArchiveSearch::scan, known);
    const auto blocked_resdb = listed ? listed->unreadable_resdb() : std::vector<std::string>{};
    fs::permissions(root / "res" / "model", fs::perms::owner_all);
    fs::permissions(root / "Documents", fs::perms::owner_all);

    CHECK(listed && listed->archives().size() == 1u);
    CHECK(listed->missing_archives().size() == 2u);
    CHECK(listed->unreadable_folders() == strings({"Documents", "res/model"}));
    CHECK(blocked_resdb == strings({"Documents/res/resdb"}));
    CHECK(scanned && scanned->unreadable_folders() == strings({"Documents", "res/model"}));
    CHECK_EQ(scanned->archives().size(), 3u);   // res.npk, res/ui.npk, script.npk

    const auto fine = npk::Installation::open(root, npk::ArchiveSearch::scan, known);
    CHECK(fine && fine->unreadable_folders().empty() && fine->unreadable_resdb().empty());
#endif
}

TEST(files_already_in_the_output_are_left_out_of_the_plan)
{
    TempDir dir;
    const fs::path root = fake_install(dir.path());
    char unnamed[64];
    std::snprintf(unnamed, sizeof unnamed, "res/ui/_unnamed/%08x", npk::path_id("unlisted.xml"));

    // On disk: one file complete, one cut short (an interrupted run), the unnamed one with its extension.
    const fs::path out = dir.path() / "out";
    const auto put = [&](const std::string& relative, std::size_t size) {
        fs::create_directories((out / relative).parent_path());
        std::ofstream(out / relative, std::ios::binary) << std::string(size, 'x');
    };
    put("res/ui/icons/a.png", 300);
    put("res/model/hero/body.mesh", 3);
    put("res/ui/login/bg.png.bak", 150);
    put(std::string(unnamed) + ".xml", 0);   // size fixed below, once the plan says it
    put("_archives/index.csv", 10);          // the index is not output

    npk::ResourceManifest manifest;
    std::optional<npk::Installation> installation;
    const npk::Plan fresh = plan_install(root, {}, manifest, installation);
    const auto& documents_ui = *source_labelled(*group_named(fresh, "res/ui"), "Documents/res/ui.npk");
    const std::size_t unnamed_size = documents_ui.entry(*planned_entry(documents_ui, "unlisted.xml")).raw_size;
    put(std::string(unnamed) + ".xml", unnamed_size);

    const npk::ExistingFiles existing = npk::ExistingFiles::scan(out);
    CHECK_EQ(existing.size(), 4u);
    npk::PlanOptions options;
    options.existing = &existing;
    const npk::Plan plan = plan_install(root, options, manifest, installation);

    const auto& ui = *group_named(plan, "res/ui");
    const auto* icon = planned_entry(*source_labelled(ui, "res/ui.npk"), "icons\\a.png");
    CHECK(icon && icon->status == npk::EntryStatus::existing);
    const auto* unlisted = planned_entry(*source_labelled(ui, "Documents/res/ui.npk"), "unlisted.xml");
    CHECK(unlisted && unlisted->status == npk::EntryStatus::existing && unlisted->path == std::string(unnamed) + ".xml");
    const auto* mesh = planned_entry(group_named(plan, "res/model/hero")->sources[0], "body.mesh");
    CHECK(mesh && mesh->status == npk::EntryStatus::pending);   // wrong size: written again
    CHECK_EQ(plan.totals.existing, 2u);
    CHECK_EQ(plan.totals.files_to_write, fresh.totals.files_to_write - 2);
}


TEST(installation_layout)
{
    TempDir dir;
    const fs::path root = fake_install(dir.path());
    const auto installation = npk::Installation::open(root, npk::ArchiveSearch::scan);
    CHECK(installation.has_value());
    CHECK_EQ(installation->archives().size(), 6u);
    CHECK_EQ(installation->resdb_files().size(), 1u);
    CHECK(installation->mount_rank(root / "Documents/res/ui.npk") < installation->mount_rank(root / "res/ui.npk"));
    CHECK(!installation->relative(dir.path() / "elsewhere.npk"));

    const auto found = npk::Installation::find_above(root / "Documents" / "res" / "ui.npk");
    CHECK(found && found->root() == installation->root());

    CHECK(!npk::Installation::open(dir.path()).has_value());   // not an installation
}

// --- Planner ---------------------------------------------------------------------------------

TEST(plan_merges_same_named_archives_highest_priority_first)
{
    TempDir dir;
    npk::ResourceManifest manifest;
    std::optional<npk::Installation> installation;
    const npk::Plan plan = plan_install(fake_install(dir.path()), {}, manifest, installation);

    std::vector<std::string> folders;
    for (const auto& group : plan.groups) folders.push_back(group.folder);
    CHECK(folders == strings({"res/model/hero", "res/ui", "res", "script"}));

    const auto* ui = group_named(plan, "res/ui");
    CHECK(ui != nullptr && ui->sources.size() == 2);
    if (!ui) return;
    CHECK_EQ(ui->sources[0].label(), std::string("Documents/res/ui.npk"));
    CHECK_EQ(ui->sources[1].label(), std::string("res/ui.npk"));
    CHECK(plan.unreadable.empty());
}

TEST(plan_decides_every_entry)
{
    TempDir dir;
    npk::ResourceManifest manifest;
    std::optional<npk::Installation> installation;
    const npk::Plan plan = plan_install(fake_install(dir.path()), {}, manifest, installation);

    const auto& ui = *group_named(plan, "res/ui");
    const auto& documents_ui = *source_labelled(ui, "Documents/res/ui.npk");
    const auto& res_ui = *source_labelled(ui, "res/ui.npk");

    const auto* bg_new = planned_entry(documents_ui, "login\\bg.png");
    CHECK(bg_new && bg_new->disposition == npk::Disposition::write && bg_new->path == "res/ui/login/bg.png");

    const auto* unlisted = planned_entry(documents_ui, "unlisted.xml");
    CHECK(unlisted && unlisted->unnamed);
    if (unlisted) {
        char expected[64];
        std::snprintf(expected, sizeof expected, "res/ui/_unnamed/%08x", npk::path_id("unlisted.xml"));
        CHECK_EQ(unlisted->path, std::string(expected));
        CHECK_EQ(npk::output_path(*unlisted, bytes("<?xml")), std::string(expected) + ".xml");
    }

    const auto* bg_old = planned_entry(res_ui, "login\\bg.png");
    CHECK(bg_old && bg_old->disposition == npk::Disposition::skip && bg_old->path.empty());
    if (bg_old) CHECK_EQ(plan.labels.at(static_cast<std::size_t>(bg_old->replaced_by)), std::string("Documents/res/ui.npk"));

    const auto* icon = planned_entry(res_ui, "icons\\a.png");
    CHECK(icon && icon->disposition == npk::Disposition::write && icon->path == "res/ui/icons/a.png");

    // res.npk's own copy of ui\login\bg.png loses to the nested archive (Documents/res/ui.npk ranks first).
    const auto& res = *group_named(plan, "res");
    const auto* root_bg = planned_entry(res.sources[0], "ui\\login\\bg.png");
    CHECK(root_bg && root_bg->disposition == npk::Disposition::skip);
    if (root_bg) CHECK_EQ(plan.labels.at(static_cast<std::size_t>(root_bg->replaced_by)), std::string("Documents/res/ui.npk"));
    const auto* x = planned_entry(res.sources[0], "common\\x.txt");
    CHECK(x && x->path == "res/common/x.txt");

    // Scripts: the patch wins; nothing is named.
    const auto& script = *group_named(plan, "script");
    CHECK_EQ(script.sources[0].label(), std::string("Documents/script_patch.npk"));
    CHECK(script.sources[0].entries[0].unnamed && script.sources[0].entries[0].disposition == npk::Disposition::write);
    CHECK(script.sources[1].entries[0].disposition == npk::Disposition::skip);

    CHECK_EQ(plan.totals.entries, 9u);
    CHECK_EQ(plan.totals.overridden, 3u);
    CHECK_EQ(plan.totals.files_to_write, 6u);
    CHECK(std::ranges::find(plan.directories, std::string("res/ui/login")) != plan.directories.end());
}

TEST(keep_overridden_writes_older_copies_aside)
{
    TempDir dir;
    npk::ResourceManifest manifest;
    std::optional<npk::Installation> installation;
    npk::PlanOptions options;
    options.keep_overridden = true;
    const npk::Plan plan = plan_install(fake_install(dir.path()), options, manifest, installation);

    const auto& ui = *group_named(plan, "res/ui");
    const auto* bg_old = planned_entry(*source_labelled(ui, "res/ui.npk"), "login\\bg.png");
    CHECK(bg_old && bg_old->disposition == npk::Disposition::write_copy);
    if (bg_old) CHECK_EQ(bg_old->path, std::string("res/ui/_overridden/res_ui.npk/login/bg.png"));
    CHECK_EQ(plan.totals.files_to_write, 9u);
}

TEST(limit_takes_the_first_entries_in_storage_order)
{
    TempDir dir;
    std::vector<FileSpec> files;
    for (int i = 0; i < 10; ++i) files.push_back({"f" + std::to_string(i), bytes(std::to_string(i))});
    const fs::path path = write_npk(dir.path() / "loose.npk", files);

    const npk::ResourceManifest manifest;
    npk::Planner planner(manifest, nullptr);
    npk::PlanOptions options;
    options.limit = 3;
    const std::vector<fs::path> archives{path};
    const npk::Plan plan = planner.build(archives, options);
    CHECK_EQ(plan.groups.size(), 1u);
    CHECK_EQ(plan.groups[0].folder, std::string("loose"));
    const auto& source = plan.groups[0].sources[0];
    CHECK_EQ(source.entries.size(), 3u);
    for (std::size_t i = 0; i < source.entries.size(); ++i) {
        CHECK_EQ(source.entry(source.entries[i]).id, npk::path_id("f" + std::to_string(i)));
    }
    CHECK(source.location.record.starts_with("_outside/"));
}

TEST(a_copy_of_a_nested_archive_gets_its_names)
{
    // res/model/hero.npk holds "body.mesh", listed in resdb as model\hero\body.mesh.
    // Copied out of the installation, its place no longer says the prefix.
    TempDir dir;
    const fs::path path = write_npk(dir.path() / "copy" / "hero.npk",
                                    {{"body.mesh", bytes("M")}, {"skin.png", bytes("P")}, {"unlisted", bytes("U")}});
    const npk::ResourceManifest manifest(
        std::vector<std::string>{"model\\hero\\body.mesh", "model\\hero\\skin.png", "ui\\body.mesh"});
    npk::Planner planner(manifest, nullptr);
    const std::vector<fs::path> archives{path};
    const npk::Plan plan = planner.build(archives, {});
    CHECK_EQ(plan.groups.size(), 1u);
    std::set<std::string> written;
    for (const auto& planned : plan.groups[0].sources[0].entries) written.insert(planned.path);
    CHECK(written.contains("hero/body.mesh"));
    CHECK(written.contains("hero/skin.png"));
    CHECK_EQ(plan.totals.unnamed, 1u);
}

TEST(encoded_entries_decode_to_the_original)
{
    const Bytes content = random_bytes(5000, 3);
    npk::Decoder decoder;
    for (const npk::Compression mode : {npk::Compression::none, npk::Compression::zlib, npk::Compression::lz4,
                                        npk::Compression::zstd}) {
        for (const bool scrambled : {false, true}) {
            const std::uint32_t flags = static_cast<std::uint32_t>(mode) | (scrambled ? npk::Entry::kEncryptedFlag : 0u);
            auto stored = npk::encode(content, flags);
            CHECK(stored.has_value());
            if (!stored) continue;
            const npk::Entry entry = npk::make_entry(1, 0x18, content, *stored, flags);
            Bytes out(content.size());
            CHECK(decoder.decode(*stored, entry, out, true).has_value());
            CHECK(out == content);
        }
    }
}

namespace {

// Every file of the archive, decoded, by id.
std::map<std::uint32_t, Bytes> archive_files(const fs::path& path)
{
    std::map<std::uint32_t, Bytes> files;
    auto archive = npk::Archive::open(path);
    if (!archive) return files;
    npk::Decoder decoder;
    for (const npk::Entry& entry : archive->entries()) {
        auto content = decoder.read(*archive, entry, true);
        files[entry.id] = content ? *content : bytes("<broken>");
    }
    return files;
}

} // namespace

TEST(a_file_is_replaced_in_place_or_at_the_end)
{
    TempDir dir;
    const fs::path path = write_npk(dir.path() / "ui.npk", {{"login\\bg.png", random_bytes(3000, 1, false)},
                                                            {"icons\\a.png", random_bytes(800, 2), npk::Compression::zlib},
                                                            {"icons\\b.png", random_bytes(500, 4), npk::Compression::none, false}});
    auto expected = archive_files(path);
    const auto size = fs::file_size(path);

    // Smaller than the old version: written over it, the archive keeps its size.
    const Bytes small = repeat("SMALL", 20);
    auto first = npk::replace_entry(path, npk::path_id("login\\bg.png"), small);
    CHECK(first.has_value() && first->in_place);
    CHECK_EQ(fs::file_size(path), size);
    expected[npk::path_id("login\\bg.png")] = small;
    CHECK(archive_files(path) == expected);

    // Larger: written where the table was, the table after it.
    const Bytes large = random_bytes(4000, 9, false);
    auto second = npk::replace_entry(path, npk::path_id("icons\\b.png"), large);
    CHECK(second.has_value() && !second->in_place);
    CHECK(second->after.offset % 4 == 0);
    CHECK(second->after.compression() == npk::Compression::none && !second->after.encrypted());   // as before
    CHECK(fs::file_size(path) > size);
    expected[npk::path_id("icons\\b.png")] = large;
    CHECK(archive_files(path) == expected);

    // Not in the archive.
    auto missing = npk::replace_entry(path, npk::path_id("icons\\c.png"), small);
    CHECK(!missing && missing.error().code == npk::Errc::not_found);
}

TEST(a_file_is_found_by_any_path_ending_in_its_inner_path)
{
    TempDir dir;
    const fs::path path = write_npk(dir.path() / "ui.npk", {{"login\\bg.png", bytes("x")}});
    for (const char* given : {"login\\bg.png", "login/bg.png", "ui/login/bg.png", "res/ui/login/bg.png", "/login/bg.png"}) {
        auto found = npk::find_entry(path, given);
        CHECK(found.has_value() && found->inner_path == "login\\bg.png" && found->id == npk::path_id("login\\bg.png"));
    }
    CHECK(!npk::find_entry(path, "login/BG.png"));   // the game's ids are case-sensitive
    CHECK(!npk::find_entry(path, "bg.png"));
}

TEST(unreadable_archives_are_listed_not_fatal)
{
    TempDir dir;
    write_file(dir.path() / "broken.npk", bytes("garbage"));
    const npk::ResourceManifest manifest;
    npk::Planner planner(manifest, nullptr);
    const std::vector<fs::path> archives{dir.path() / "broken.npk"};
    const npk::Plan plan = planner.build(archives, {});
    CHECK(plan.groups.empty());
    CHECK_EQ(plan.unreadable.size(), 1u);
}

TEST(planned_files_decode_to_the_packed_content)
{
    TempDir dir;
    npk::ResourceManifest manifest;
    std::optional<npk::Installation> installation;
    const npk::Plan plan = plan_install(fake_install(dir.path()), {}, manifest, installation);

    std::map<std::string, Bytes> written;
    npk::Decoder decoder;
    for (const auto& group : plan.groups) {
        for (const auto& source : group.sources) {
            auto archive = npk::Archive::open(source.path);
            CHECK(archive.has_value());
            for (const auto& planned : source.entries) {
                if (planned.disposition == npk::Disposition::skip) continue;
                auto data = decoder.read(*archive, source.entry(planned));
                CHECK(data.has_value());
                if (data) written[npk::output_path(planned, *data)] = *data;
            }
        }
    }
    CHECK(written.at("res/ui/login/bg.png") == repeat("NEW", 50));
    CHECK(written.at("res/ui/icons/a.png") == repeat("A", 300));
    CHECK(written.at("res/common/x.txt") == repeat("X", 100));
    CHECK(written.contains("res/model/hero/body.mesh"));
    CHECK(!written.contains("res/ui/login/bg.png.old"));
    std::size_t scripts = 0;
    for (const auto& [path, data] : written) {
        if (path.starts_with("script/_unnamed/")) {
            ++scripts;
            CHECK(path.ends_with(".enc"));
        }
    }
    CHECK_EQ(scripts, 1u);
}

int main()
{
    return test::run_all();
}
