// npkextpp: command line front end.
//
//   1. find the archives        (--install / --in, filtered by --unpack)
//   2. load the name manifest   (resdb from the installation, or --resdb)
//   3. build the plan           (read every table of contents, decide every entry)
//   4. unpack                   (npk::pipeline on all cores, or one --entry on this thread)
#include "cli_options.hpp"
#include "console.hpp"
#include "dashboard.hpp"
#include "licenses.hpp"

#include "npk/archive_update.hpp"
#include "npk/decoder.hpp"
#include "npk/input_file.hpp"
#include "npk/inputs.hpp"
#include "npk/install.hpp"
#include "npk/names.hpp"
#include "npk/paths.hpp"
#include "npk/pipeline/disk_probe.hpp"
#include "npk/pipeline/index_writer.hpp"
#include "npk/pipeline/pipeline.hpp"
#include "npk/plan.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <csignal>
#include <cstdio>
#include <fstream>
#include <functional>
#include <iostream>
#include <map>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace fs = std::filesystem;
using namespace npkextpp;

namespace {

constexpr std::size_t kMaxFailuresShown = 20;

std::atomic<bool> g_interrupted{false};

extern "C" void on_interrupt(int)
{
    g_interrupted.store(true);
}

// Input the user has to fix; main() prints it and exits with kExitBadInput.
struct InputError : std::runtime_error {
    using std::runtime_error::runtime_error;
};

template <class T>
T unwrap(npk::Result<T> result)
{
    if (!result) throw InputError(result.error().message);
    return std::move(*result);
}

std::string hex8(std::uint32_t value)
{
    char text[9];
    std::snprintf(text, sizeof text, "%08x", value);
    return text;
}

// --- 1. Archives -------------------------------------------------------------------

struct Inputs {
    std::optional<npk::Installation> installation;
    std::vector<fs::path> archives;
};

Inputs find_archives(const CliOptions& options)
{
    Inputs inputs;
    const auto search = options.scan ? npk::ArchiveSearch::scan : npk::ArchiveSearch::known;
    if (options.install) inputs.installation = unwrap(npk::Installation::open(*options.install, search));
    const npk::Installation* installation = inputs.installation ? &*inputs.installation : nullptr;

    if (!options.inputs.empty()) {
        inputs.archives = unwrap(npk::resolve_inputs(options.inputs, installation));
        if (!inputs.installation) inputs.installation = npk::Installation::find_above(inputs.archives.front());
    } else if (inputs.installation) {
        inputs.archives = inputs.installation->archives();
        if (inputs.archives.empty()) {
            throw InputError("no .npk files found in " + npk::to_utf8(inputs.installation->root()));
        }
    } else {
        throw InputError("nothing to unpack: give --install DIR and/or --in ...");
    }

    if (!options.unpack.empty()) inputs.archives = unwrap(npk::filter_by_name(inputs.archives, options.unpack));
    return inputs;
}

// --- Reports ---------------------------------------------------------------------------

std::string section_name(std::string_view section)
{
    return section.empty() ? std::string("(top level)") : std::string(section) + "/";
}

// Archives of the known list that are not there, if the whole installation is unpacked.
std::vector<std::string> missing_archives(const CliOptions& options, const std::optional<npk::Installation>& installation)
{
    std::vector<std::string> missing;
    if (!installation || !options.inputs.empty()) return missing;
    for (const std::string& path : installation->missing_archives()) {
        const std::string name = npk::lower_ascii(path.substr(path.rfind('/') + 1));
        bool wanted = options.unpack.empty();
        for (const std::string& pattern : options.unpack) {
            std::string p = npk::lower_ascii(pattern);
            if (!p.ends_with(".npk")) p += ".npk";
            wanted = wanted || npk::wildcard_match(name, p);
        }
        if (wanted) missing.push_back(path);
    }
    return missing;
}

void show_sources(Console& console, const std::optional<npk::Installation>& installation, std::size_t selected,
                  std::size_t missing)
{
    if (!installation) return;
    console.line("Installation  " + npk::to_utf8(installation->root()));
    const std::size_t found = installation->archives().size();
    const std::string chosen = selected == found ? "" : ", " + format_count(selected) + " selected";
    if (installation->scanned()) {
        const std::string why = installation->known_count() == 0
                                    ? " (the built-in list of the game's archives is empty)"
                                    : " (none of the " + format_count(installation->known_count()) +
                                          " known archives is here)";
        console.line("Archives      " + format_count(found) + " found by scanning the installation" + chosen +
                     (why.empty() ? "" : why));
    } else {
        console.line("Archives      " + format_count(found) + " of " + format_count(installation->known_count()) +
                     " known archives here" + chosen + (missing ? ", " + format_count(missing) + " missing" : "") +
                     " (--scan looks for others)");
    }
    const auto& blocked = installation->unreadable_folders();
    if (!blocked.empty()) {
        std::string names;
        for (std::size_t i = 0; i < blocked.size() && i < 8; ++i) names += (i ? ", " : "") + blocked[i];
        if (blocked.size() > 8) names += " and " + format_count(blocked.size() - 8) + " more";
        console.line("Warning       " + format_count(blocked.size()) + (blocked.size() == 1 ? " folder" : " folders") +
                     " of the installation cannot be opened (permission denied), so the archives in " +
                     (blocked.size() == 1 ? "it" : "them") + " are left out: " + names);
    }
}

// Says why there are no names, if there are none.
void show_missing_names(Console& console, const CliOptions& options, const std::optional<npk::Installation>& installation,
                        std::size_t manifests)
{
    if (manifests != 0 || !options.resdb.empty()) return;
    if (!installation) {
        console.line("Warning       no name manifest: files keep their hashed names. Give --resdb with the game's "
                     "res/resdb (and Documents/res/resdb) to get names.");
        return;
    }
    const std::vector<std::string> blocked = installation->unreadable_resdb();
    std::string where;
    for (const char* location : npk::kResdbLocations) where += (where.empty() ? "" : " or ") + std::string(location);
    console.line("Warning       " +
                 (blocked.empty() ? "no name manifest in the installation (" + where + ")"
                                  : "the name manifest cannot be opened (permission denied)") +
                 ": files keep their hashed names. --resdb FILE gives one.");
}

void show_plan(Console& console, const npk::Plan& plan, std::size_t names, std::size_t manifests, bool verbose)
{
    console.line("Names         " + format_count(names) + " paths from " + std::to_string(manifests) + " manifest" +
                 (manifests == 1 ? "" : "s"));
    console.line("Plan          " + format_count(plan.source_count()) + " archives into " +
                 format_count(plan.groups.size()) + " folders: " + format_count(plan.totals.files_to_write) +
                 " files to write (" + format_bytes(plan.totals.raw_bytes) + "), " +
                 format_count(plan.totals.overridden) + " overridden, " + format_count(plan.totals.unnamed) +
                 " unnamed");
    if (plan.totals.existing) {
        console.line("              " + format_count(plan.totals.existing) + " files (" +
                     format_bytes(plan.totals.existing_bytes) +
                     ") are already in the output folder and are kept (--overwrite writes them again)");
    }
    if (verbose) {
        for (const npk::OutputGroup& group : plan.groups) {
            std::string sources;
            for (const npk::ArchiveSource& source : group.sources) {
                sources += (sources.empty() ? "" : ", ") + source.label();
            }
            console.line("  " + group.folder + "/  <-  " + sources);
        }
    }
    for (const auto& [path, reason] : plan.unreadable) console.line("cannot read " + reason);
}

struct Counts {
    std::uint64_t folders = 0, files = 0, named = 0, unnamed = 0, overridden = 0, failed = 0, bytes = 0, kept = 0;
    void add(const Counts& other)
    {
        kept += other.kept;
        folders += other.folders;
        files += other.files;
        named += other.named;
        unnamed += other.unnamed;
        overridden += other.overridden;
        failed += other.failed;
        bytes += other.bytes;
    }
};

Counts count(const npk::OutputGroup& group)
{
    Counts counts;
    counts.folders = 1;
    for (const npk::ArchiveSource& source : group.sources) {
        for (const npk::PlannedEntry& planned : source.entries) {
            if (planned.replaced_by >= 0) ++counts.overridden;
            if (planned.status == npk::EntryStatus::failed) ++counts.failed;
            if (planned.status == npk::EntryStatus::existing) ++counts.kept;
            if (planned.status != npk::EntryStatus::written) continue;
            counts.bytes += source.entry(planned).raw_size;
            if (planned.disposition != npk::Disposition::write) continue;
            ++counts.files;
            ++(planned.unnamed ? counts.unnamed : counts.named);
        }
    }
    return counts;
}

void show_summary(Console& console, const npk::Plan& plan, const npk::pipeline::Summary& summary, const fs::path& out)
{
    std::vector<std::pair<std::string, Counts>> sections;
    Counts total;
    for (const npk::OutputGroup& group : plan.groups) {
        const std::string name = section_name(group.section());
        if (sections.empty() || sections.back().first != name) sections.emplace_back(name, Counts{});
        const Counts counts = count(group);
        sections.back().second.add(counts);
        total.add(counts);
    }

    const auto row = [&](const std::string& name, const Counts& c) {
        char text[256];
        std::snprintf(text, sizeof text, "%-28s %8s %10s %10s %9s %11s %7s %11s %10s", name.c_str(),
                      format_count(c.folders).c_str(), format_count(c.files).c_str(), format_count(c.named).c_str(),
                      format_count(c.unnamed).c_str(), format_count(c.overridden).c_str(),
                      format_count(c.failed).c_str(), format_bytes(c.bytes).c_str(), format_count(c.kept).c_str());
        console.line(text);
    };
    console.line("");
    char header[256];
    std::snprintf(header, sizeof header, "%-28s %8s %10s %10s %9s %11s %7s %11s %10s", "Section", "Folders", "Files",
                  "Named", "Unnamed", "Overridden", "Failed", "Written", "Kept");
    console.line(header);
    for (const auto& [name, counts] : sections) row(name, counts);
    row("Total", total);

    std::size_t shown = 0;
    for (const npk::OutputGroup& group : plan.groups) {
        for (const npk::ArchiveSource& source : group.sources) {
            for (const npk::PlannedEntry& planned : source.entries) {
                if (planned.status != npk::EntryStatus::failed) continue;
                if (shown++ < kMaxFailuresShown) {
                    console.line("failed " + source.label() + " entry " + hex8(source.entry(planned).id) + ": " +
                                 planned.error);
                }
            }
        }
    }
    if (shown > kMaxFailuresShown) {
        console.line("... and " + format_count(shown - kMaxFailuresShown) +
                     " more failures (status 'failed' in the records in _archives/)");
    }
    if (summary.files_not_reached > 0) {
        console.line(format_count(summary.files_not_reached) + " files were not reached (the run was stopped)");
    }

    const double rate = summary.seconds > 0 ? static_cast<double>(summary.bytes_written) / summary.seconds : 0;
    std::string how = "nothing new to write";
    if (summary.bytes_written > 0) {
        how = format_bytes(static_cast<std::uint64_t>(rate)) + "/s, ";
        if (summary.disk != npk::pipeline::DiskKind::unknown) how += std::string(npk::pipeline::name_of(summary.disk)) + ", ";
        how += std::to_string(summary.writers) + (summary.writers == 1 ? " writer, " : " writers, ") + std::to_string(summary.read_block >> 20) + " MiB reads";
    }
    console.line("\nDone in " + format_duration(summary.seconds) + " (" + how + "). Output: " + npk::to_utf8(out));
    console.line("_archives/ records every archive and where each of its files went; see _archives/README.txt.");
}

// --- 4a. One entry ------------------------------------------------------------------------

int extract_entry(Console& console, npk::Plan& plan, std::string wanted, const fs::path& out, bool verify)
{
    std::ranges::replace(wanted, '\\', '/');
    while (!wanted.empty() && wanted.front() == '/') wanted.erase(0, 1);
    const std::string as_given = npk::lower_ascii(wanted);
    const std::string in_resources = "res/" + as_given;

    for (npk::OutputGroup& group : plan.groups) {
        for (npk::ArchiveSource& source : group.sources) {
            for (npk::PlannedEntry& planned : source.entries) {
                if (planned.disposition != npk::Disposition::write) continue;
                const std::string path = npk::lower_ascii(planned.path);
                if (path != as_given && path != in_resources) continue;
                if (planned.status == npk::EntryStatus::existing) {
                    console.line("Already there: " + npk::to_utf8(out / npk::from_utf8(planned.path)) +
                                 " (--overwrite writes it again)");
                    return kExitOk;
                }

                auto archive = npk::Archive::open(source.path);
                if (!archive) throw InputError(archive.error().message);
                npk::Decoder decoder;
                auto data = decoder.read(*archive, source.entry(planned), verify);
                if (!data) {
                    console.error(source.label() + ": " + data.error().message);
                    return kExitEntriesFailed;
                }
                const std::string relative = npk::output_path(planned, *data);
                const fs::path target = out / npk::from_utf8(relative);
                std::error_code error;
                fs::create_directories(target.parent_path(), error);
                std::ofstream file(npk::long_path(target), std::ios::binary | std::ios::trunc);
                file.write(reinterpret_cast<const char*>(data->data()), static_cast<std::streamsize>(data->size()));
                file.close();
                if (!file) {
                    console.error("cannot write " + npk::to_utf8(target));
                    return kExitEntriesFailed;
                }
                console.line("Wrote " + npk::to_utf8(target) + " (" + format_bytes(data->size()) + ") from " +
                             source.label());
                return kExitOk;
            }
        }
    }
    throw InputError("no file '" + wanted + "' in the selected archives (paths are relative to the game's tree, "
                     "e.g. ui/login/bg.png or script/...)");
}

// --- 4b. Everything ---------------------------------------------------------------------------

// Redraws the live frame every 50 ms (a terminal), or prints a progress line
// every 10 s (a log), until stopped. Every tick checks for Ctrl+C; between
// ticks, keys and the mouse wheel scroll the archive list.
void show_live(Console& console, Dashboard& dashboard, std::stop_token stop, const std::function<void()>& on_tick)
{
    using namespace std::chrono_literals;
    using Key = Console::Key;
    using Scroll = Dashboard::Scroll;
    auto next_line = std::chrono::steady_clock::now() + 10s;
    if (console.interactive()) console.start_input();
    while (!stop.stop_requested()) {
        on_tick();
        if (console.interactive()) {
            console.draw(dashboard.frame(console.width(), console.height() - 1));
        } else if (std::chrono::steady_clock::now() >= next_line) {
            console.line(dashboard.progress_line());
            next_line += 10s;
        }
        for (const Key key : console.read_keys(50ms)) {
            switch (key) {
            case Key::up: dashboard.scroll(Scroll::line_up); break;
            case Key::down: dashboard.scroll(Scroll::line_down); break;
            case Key::wheel_up: dashboard.scroll(Scroll::line_up, 3); break;
            case Key::wheel_down: dashboard.scroll(Scroll::line_down, 3); break;
            case Key::page_up: dashboard.scroll(Scroll::page_up); break;
            case Key::page_down: dashboard.scroll(Scroll::page_down); break;
            case Key::home: dashboard.scroll(Scroll::top); break;
            case Key::end: dashboard.scroll(Scroll::bottom); break;
            case Key::follow: dashboard.scroll(Scroll::follow); break;
            }
        }
    }
    console.stop_input();
}

int run(const CliOptions& options, Console& console)
{
    console.print(logo(NPKEXTPP_VERSION));
    if (!options.out && !options.dry_run) throw InputError("--out DIR is required (or use --dry-run to only look)");

    Inputs inputs = find_archives(options);
    const std::vector<std::string> missing = missing_archives(options, inputs.installation);
    show_sources(console, inputs.installation, inputs.archives.size(), missing.size());

    std::vector<fs::path> resdb_files = options.resdb;
    if (resdb_files.empty() && inputs.installation) resdb_files = inputs.installation->resdb_files();
    for (const fs::path& resdb : resdb_files) {
        std::error_code error;
        if (!fs::is_regular_file(resdb, error)) throw InputError("resdb file not found: " + npk::to_utf8(resdb));
    }
    const npk::ResourceManifest manifest = unwrap(npk::ResourceManifest::from_files(resdb_files));
    show_missing_names(console, options, inputs.installation, resdb_files.size());

    // What an earlier run already wrote stays (unless --overwrite).
    npk::ExistingFiles existing;
    if (options.out && !options.overwrite) {
        console.line("Output        " + npk::to_utf8(*options.out) + " (looking for files from earlier runs...)");
        existing = npk::ExistingFiles::scan(*options.out);
    } else if (options.out) {
        console.line("Output        " + npk::to_utf8(*options.out) + " (--overwrite: every file is written again)");
    }

    npk::PlanOptions plan_options;
    plan_options.keep_overridden = options.keep_overridden;
    plan_options.limit = options.limit;
    plan_options.existing = &existing;
    npk::Planner planner(manifest, inputs.installation ? &*inputs.installation : nullptr);
    npk::Plan plan = planner.build(inputs.archives, plan_options);
    show_plan(console, plan, manifest.size(), resdb_files.size(), options.verbose);

    if (options.entry) {
        if (!options.out) throw InputError("--entry needs --out DIR");
        return extract_entry(console, plan, *options.entry, *options.out, options.verify);
    }
    if (options.dry_run || plan.groups.empty()) return kExitOk;

    std::error_code space_error;
    fs::create_directories(*options.out, space_error);
    const auto space = fs::space(*options.out, space_error);
    if (!space_error) {
        console.line("              " + format_bytes(plan.totals.raw_bytes) + " to write, " +
                     format_bytes(space.available) + " free");
        if (space.available < plan.totals.raw_bytes) console.line("Warning: not enough free disk space for everything.");
    }

    npk::pipeline::Options pipeline_options;
    pipeline_options.output_root = *options.out;
    pipeline_options.verify = options.verify;
    if (options.verbose) pipeline_options.log = [&console](std::string_view line) { console.line(line); };

    // The disk test decides how the writer count is searched for.
    npk::pipeline::DiskKind disk = npk::pipeline::DiskKind::unknown;
    if (plan.totals.files_to_write > 0) {
        std::vector<fs::path> archives;
        for (const npk::OutputGroup& group : plan.groups) {
            for (const npk::ArchiveSource& source : group.sources) archives.push_back(source.path);
        }
        console.line("");
        const npk::pipeline::DiskProbe probe = npk::pipeline::probe_disk(*options.out, archives);
        console.print(disk_test(probe));
        disk = probe.kind;
    }
    pipeline_options.disk_kind = disk;

    npk::pipeline::Progress progress(plan.source_count());
    Dashboard dashboard(plan, missing, progress, disk);
    std::stop_source cancel;
    std::signal(SIGINT, on_interrupt);
    npk::pipeline::Summary summary;
    {
        std::jthread live([&](std::stop_token stop) {
            show_live(console, dashboard, stop, [&] {
                if (g_interrupted.load()) cancel.request_stop();
            });
        });
        summary = npk::pipeline::run(plan, pipeline_options, progress, cancel.get_token());
    }
    std::signal(SIGINT, SIG_DFL);

    console.clear_frame();
    console.print(dashboard.archive_list(console.width(), true));
    show_summary(console, plan, summary, *options.out);
    if (summary.index_error) console.error("cannot write the index: " + summary.index_error->message);
    if (summary.fatal) {
        console.error(summary.fatal->message);
        return kExitEntriesFailed;
    }
    if (summary.cancelled) {
        console.error("interrupted");
        return kExitInterrupted;
    }
    return (summary.files_failed > 0 || summary.index_error) ? kExitEntriesFailed : kExitOk;
}

std::string compression_name(const npk::Entry& entry)
{
    std::string name;
    switch (entry.compression()) {
    case npk::Compression::none: name = "stored"; break;
    case npk::Compression::zlib: name = "zlib"; break;
    case npk::Compression::lz4: name = "LZ4"; break;
    case npk::Compression::zstd: name = "zstd"; break;
    default: name = "mode " + std::to_string(entry.flags & 0xFFu); break;
    }
    return name + (entry.encrypted() ? ", scrambled" : "");
}

std::string slashes(std::string path)
{
    std::ranges::replace(path, '\\', '/');
    return path;
}

void show_replaced(Console& console, const npk::ReplaceResult& result, const std::vector<std::string>& names)
{
    std::size_t replaced = 0, at_end = 0, unchanged = 0, listed = 0;
    for (const npk::Replacement& file : result.files) {
        if (file.unchanged) ++unchanged;
        else ++replaced, at_end += file.in_place ? 0 : 1;
    }
    constexpr std::size_t kListed = 20;
    for (std::size_t i = 0; i < result.files.size(); ++i) {
        const npk::Replacement& file = result.files[i];
        if (file.unchanged || ++listed > kListed) continue;
        console.line("  " + names[i] + "  " + format_bytes(file.before.raw_size) + " -> " + format_bytes(file.after.raw_size) +
                     (file.in_place ? "" : ", at the end"));
    }
    if (replaced > kListed) console.line("  ... and " + format_count(replaced - kListed) + " more");
    if (replaced == 0) {
        console.line("Nothing to do: " + std::string(unchanged == 1 ? "the file is" : "every file is") +
                     " the same as in the archive");
        return;
    }
    std::string line = "Replaced      " + format_count(replaced) + (replaced == 1 ? " file" : " files");
    if (unchanged) line += ", " + format_count(unchanged) + " unchanged";
    console.line(line);
    console.line(at_end == 0 ? "              all written over their old versions; the archive stays " +
                                   format_bytes(result.size_after)
                             : "              " +
                                   (at_end == 1 ? std::string("1 was too big for its old place and went")
                                                : format_count(at_end) + " were too big for their old places and went") +
                                   " to the end; the archive grew from " + format_bytes(result.size_before) + " to " +
                                   format_bytes(result.size_after));
    console.line("Checked       read back from the archive: identical to the new versions");
}

// npkextpp update: files of one archive replaced by new versions, one given by
// its path or every file of a folder laid out like the archive.
int run_update(const UpdateOptions& update, Console& console)
{
    console.print(logo(NPKEXTPP_VERSION));
    console.line("Archive       " + npk::to_utf8(update.target));

    if (update.new_file) {
        const npk::FoundEntry found = unwrap(npk::find_entry(update.target, *update.path));
        const std::string shown = slashes(found.inner_path);
        console.line("File          " + shown + " (id " + hex8(found.id) + ")" +
                     (shown == slashes(*update.path) ? "" : ", found from " + *update.path));
        const std::vector<std::byte> content = unwrap(npk::InputFile::read_all(*update.new_file));
        console.line("New version   " + npk::to_utf8(*update.new_file) + " (" + format_bytes(content.size()) + ")");
        const npk::ReplaceResult result = unwrap(npk::replace_entry(update.target, found.id, content));
        console.line("Stored as     " + compression_name(result.files.front().after) + ", like the old version");
        show_replaced(console, result, {shown});
        return kExitOk;
    }

    // A folder: every file in it, by its path relative to the folder.
    const std::vector<npk::FolderFile> files = unwrap(npk::folder_files(*update.folder));
    console.line("Folder        " + npk::to_utf8(*update.folder) + " (" + format_count(files.size()) +
                 (files.size() == 1 ? " file)" : " files)"));
    if (files.empty()) throw InputError("the folder has no files");
    {
        const npk::Archive archive = unwrap(npk::Archive::open(update.target));
        std::vector<std::string> absent;
        std::map<std::uint32_t, std::string> seen;
        for (const npk::FolderFile& file : files) {
            if (archive.find(file.id) == nullptr) absent.push_back(slashes(file.inner_path));
            const auto [it, added] = seen.emplace(file.id, file.inner_path);
            if (!added) {
                throw InputError(slashes(it->second) + " and " + slashes(file.inner_path) + " are the same file of the archive");
            }
        }
        if (!absent.empty()) {
            std::string list;
            for (std::size_t i = 0; i < absent.size() && i < 10; ++i) list += "\n  " + absent[i];
            if (absent.size() > 10) list += "\n  ... and " + format_count(absent.size() - 10) + " more";
            throw InputError(format_count(absent.size()) + (absent.size() == 1 ? " file of the folder is" : " files of the folder are") +
                             " not in the archive (adding files is not supported yet), so nothing was changed:" + list);
        }
    }
    std::vector<npk::Change> changes;
    std::vector<std::string> names;
    for (const npk::FolderFile& file : files) {
        changes.push_back({file.id, [path = file.file] { return npk::InputFile::read_all(path); }});
        names.push_back(slashes(file.inner_path));
    }
    const npk::ReplaceResult result = unwrap(npk::replace_entries(update.target, changes));
    show_replaced(console, result, names);
    return kExitOk;
}

} // namespace

int main(int argc, char** argv)
{
    int exit_code = kExitOk;
    const std::optional<CliOptions> parsed = parse_cli(argc, argv, exit_code);
    if (!parsed) return exit_code;
    const CliOptions& options = *parsed;

    if (options.show_licenses) {
        std::cout << third_party_licenses();
        return kExitOk;
    }

    Console console;
    try {
        if (options.update) return run_update(*options.update, console);
        return run(options, console);
    } catch (const InputError& error) {
        console.error(error.what());
        return kExitBadInput;
    } catch (const std::exception& error) {
        console.error(std::string("unexpected: ") + error.what());
        return kExitEntriesFailed;
    }
}
