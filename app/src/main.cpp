// npkextpp: command line front end.
//
//   1. find the archives        (--install / --in, filtered by --unpack)
//   2. load the name manifest   (resdb from the installation, or --resdb)
//   3. build the plan           (read every table of contents, decide every entry)
//   4. unpack                   (npk::pipeline on all cores, or one --entry on this thread)
#include "cli_options.hpp"
#include "licenses.hpp"
#include "progress_view.hpp"

#include "npk/decoder.hpp"
#include "npk/inputs.hpp"
#include "npk/install.hpp"
#include "npk/names.hpp"
#include "npk/paths.hpp"
#include "npk/pipeline/index_writer.hpp"
#include "npk/pipeline/pipeline.hpp"
#include "npk/plan.hpp"

#include <algorithm>
#include <atomic>
#include <csignal>
#include <cstdio>
#include <fstream>
#include <iostream>
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
    if (options.install) inputs.installation = unwrap(npk::Installation::open(*options.install));
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

void show_plan(Console& console, const npk::Plan& plan, std::size_t names, std::size_t manifests,
               const std::optional<npk::Installation>& installation, bool verbose)
{
    if (installation) console.line("Installation  " + npk::to_utf8(installation->root()));
    console.line("Names         " + format_count(names) + " paths from " + std::to_string(manifests) + " manifest" +
                 (manifests == 1 ? "" : "s"));
    console.line("Plan          " + format_count(plan.source_count()) + " archives into " +
                 format_count(plan.groups.size()) + " folders: " + format_count(plan.totals.files_to_write) +
                 " files to write (" + format_bytes(plan.totals.raw_bytes) + "), " +
                 format_count(plan.totals.overridden) + " overridden, " + format_count(plan.totals.unnamed) +
                 " unnamed");
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
    std::uint64_t folders = 0, files = 0, named = 0, unnamed = 0, overridden = 0, failed = 0, bytes = 0;
    void add(const Counts& other)
    {
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
        std::snprintf(text, sizeof text, "%-28s %8s %10s %10s %9s %11s %7s %11s", name.c_str(),
                      format_count(c.folders).c_str(), format_count(c.files).c_str(), format_count(c.named).c_str(),
                      format_count(c.unnamed).c_str(), format_count(c.overridden).c_str(),
                      format_count(c.failed).c_str(), format_bytes(c.bytes).c_str());
        console.line(text);
    };
    console.line("");
    char header[256];
    std::snprintf(header, sizeof header, "%-28s %8s %10s %10s %9s %11s %7s %11s", "Section", "Folders", "Files",
                  "Named", "Unnamed", "Overridden", "Failed", "Written");
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
    console.line("\nDone in " + format_duration(summary.seconds) + " (" + format_bytes(static_cast<std::uint64_t>(rate)) +
                 "/s, " + std::to_string(summary.writers) + " writers, " + std::to_string(summary.read_block >> 20) +
                 " MiB reads). Output: " + npk::to_utf8(out));
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

int run(const CliOptions& options, Console& console)
{
    if (!options.out && !options.dry_run) throw InputError("--out DIR is required (or use --dry-run to only look)");

    Inputs inputs = find_archives(options);
    std::vector<fs::path> resdb_files = options.resdb;
    if (resdb_files.empty() && inputs.installation) resdb_files = inputs.installation->resdb_files();
    for (const fs::path& resdb : resdb_files) {
        std::error_code error;
        if (!fs::is_regular_file(resdb, error)) throw InputError("resdb file not found: " + npk::to_utf8(resdb));
    }

    const npk::ResourceManifest manifest = unwrap(npk::ResourceManifest::from_files(resdb_files));
    npk::PlanOptions plan_options;
    plan_options.keep_overridden = options.keep_overridden;
    plan_options.limit = options.limit;
    npk::Planner planner(manifest, inputs.installation ? &*inputs.installation : nullptr);
    npk::Plan plan = planner.build(inputs.archives, plan_options);
    show_plan(console, plan, manifest.size(), resdb_files.size(), inputs.installation, options.verbose);

    if (options.entry) {
        if (!options.out) throw InputError("--entry needs --out DIR");
        return extract_entry(console, plan, *options.entry, *options.out, options.verify);
    }
    if (options.dry_run || plan.groups.empty()) return kExitOk;

    npk::pipeline::Options pipeline_options;
    pipeline_options.output_root = *options.out;
    pipeline_options.verify = options.verify;
    if (options.verbose) pipeline_options.log = [&console](std::string_view line) { console.line(line); };

    npk::pipeline::Progress progress;
    std::stop_source cancel;
    std::signal(SIGINT, on_interrupt);
    npk::pipeline::Summary summary;
    {
        std::jthread view([&](std::stop_token stop) {
            console.show_progress(progress, plan.totals.files_to_write, plan.totals.raw_bytes, stop, [&] {
                if (g_interrupted.load()) cancel.request_stop();
            });
        });
        summary = npk::pipeline::run(plan, pipeline_options, progress, cancel.get_token());
    }
    std::signal(SIGINT, SIG_DFL);

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
        return run(options, console);
    } catch (const InputError& error) {
        console.error(error.what());
        return kExitBadInput;
    } catch (const std::exception& error) {
        console.error(std::string("unexpected: ") + error.what());
        return kExitEntriesFailed;
    }
}
