// core_extract: the simplest possible extractor, built only on npk_core and
// single-threaded. A reference for the pipeline and the C++ side of
// tests/compare_with_npkext.py. Writes the files only (no _archives/ index).
//
//   core_extract --install DIR --out DIR [--keep-overridden] [--limit N] [--in NAME...]
#include "npk/decoder.hpp"
#include "npk/inputs.hpp"
#include "npk/install.hpp"
#include "npk/names.hpp"
#include "npk/paths.hpp"
#include "npk/plan.hpp"

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;

int main(int argc, char** argv)
{
    fs::path install, out;
    npk::PlanOptions options;
    std::vector<std::string> inputs;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--install" && i + 1 < argc) install = argv[++i];
        else if (arg == "--out" && i + 1 < argc) out = argv[++i];
        else if (arg == "--keep-overridden") options.keep_overridden = true;
        else if (arg == "--limit" && i + 1 < argc) options.limit = std::stoul(argv[++i]);
        else if (arg == "--in") {
            while (i + 1 < argc && std::strncmp(argv[i + 1], "--", 2) != 0) inputs.emplace_back(argv[++i]);
        } else {
            std::fprintf(stderr, "usage: core_extract --install DIR --out DIR [--keep-overridden] [--limit N] [--in NAME...]\n");
            return 2;
        }
    }

    auto installation = npk::Installation::open(install, npk::ArchiveSearch::scan);
    if (!installation) {
        std::fprintf(stderr, "%s\n", installation.error().message.c_str());
        return 2;
    }
    auto manifest = npk::ResourceManifest::from_files(installation->resdb_files());
    if (!manifest) {
        std::fprintf(stderr, "%s\n", manifest.error().message.c_str());
        return 2;
    }
    std::vector<fs::path> archives;
    if (inputs.empty()) {
        archives = installation->archives();
    } else {
        auto resolved = npk::resolve_inputs(inputs, &*installation);
        if (!resolved) {
            std::fprintf(stderr, "%s\n", resolved.error().message.c_str());
            return 2;
        }
        archives = std::move(*resolved);
    }

    npk::Planner planner(*manifest, &*installation);
    npk::Plan plan = planner.build(archives, options);
    for (const auto& [path, reason] : plan.unreadable) std::fprintf(stderr, "unreadable: %s\n", reason.c_str());

    for (const std::string& directory : plan.directories) fs::create_directories(out / npk::from_utf8(directory));

    npk::Decoder decoder;
    std::size_t written = 0, failed = 0;
    for (auto& group : plan.groups) {
        for (auto& source : group.sources) {
            auto archive = npk::Archive::open(source.path);
            if (!archive) {
                std::fprintf(stderr, "%s\n", archive.error().message.c_str());
                ++failed;
                continue;
            }
            for (auto& planned : source.entries) {
                if (planned.disposition == npk::Disposition::skip) continue;
                auto data = decoder.read(*archive, source.entry(planned));
                if (!data) {
                    planned.status = npk::EntryStatus::failed;
                    planned.error = data.error().message;
                    std::fprintf(stderr, "%s: %s\n", planned.path.c_str(), planned.error.c_str());
                    ++failed;
                    continue;
                }
                std::ofstream file(out / npk::from_utf8(npk::output_path(planned, *data)), std::ios::binary | std::ios::trunc);
                file.write(reinterpret_cast<const char*>(data->data()), static_cast<std::streamsize>(data->size()));
                planned.status = npk::EntryStatus::written;
                ++written;
            }
        }
    }
    std::printf("%zu files written, %zu failed\n", written, failed);
    return failed == 0 ? 0 : 1;
}
