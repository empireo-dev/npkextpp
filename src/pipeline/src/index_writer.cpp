#include "npk/pipeline/index_writer.hpp"

#include "npk/input_file.hpp"
#include "npk/mounts.hpp"
#include "npk/paths.hpp"

#include <algorithm>
#include <cstdio>
#include <fstream>
#include <map>
#include <system_error>

namespace fs = std::filesystem;

namespace npk::pipeline {

namespace {

constexpr std::string_view kReadme = R"(This folder records where every unpacked file came from, so the original .npk
archives can be rebuilt. Written by npkextpp (same format as npkext).

index.csv     one row per archive unpacked into this output folder
  archive         path of the .npk, relative to the game installation
  mount           the mount folder it belongs to (Documents/res, res2, ...)
  priority        rank of that mount folder, 0 = highest (see mounts.csv)
  output_folder   where its files went, relative to the output folder
  entries         number of files in the archive
  looked_at       how many were processed (less than entries with --limit)
  size            size of the .npk in bytes
  record          its record file, relative to this folder

mounts.csv    the folders the game loads archives from, highest priority first
  depth           how many folder levels below the mount folder archives are found
  tree            res (resources) or script

<archive>.csv one record per archive, rows in the archive's own table order
  toc_index       position in the archive's table of contents
  id              hash of the file's path inside the archive (hex)
  offset          where the stored bytes start in the .npk (storage order)
  packed_size, raw_size, crc_packed, crc_raw, flags
                  the table-of-contents values, unchanged
  status          ok          written to path
                  overridden  another archive's copy was used (see note);
                              written to path only with --keep-overridden
                  failed      could not be decoded (see note)
                  skipped     not processed (--limit)
  path            the written file, relative to the output folder
  note            which archive replaced this copy, or the error
)";

const std::vector<std::string> kIndexColumns = {"archive", "mount",     "priority", "output_folder",
                                                "entries", "looked_at", "size",     "record"};

std::string hex8(std::uint32_t value)
{
    char text[9];
    std::snprintf(text, sizeof text, "%08x", value);
    return text;
}

std::string hex_flags(std::uint32_t value)   // Python's f"{value:#x}"
{
    char text[16];
    std::snprintf(text, sizeof text, "0x%x", value);
    return text;
}

Result<void> write_text(const fs::path& path, std::string_view text)
{
    std::error_code error;
    fs::create_directories(path.parent_path(), error);
    std::ofstream out(long_path(path), std::ios::binary | std::ios::trunc);
    out.write(text.data(), static_cast<std::streamsize>(text.size()));
    out.close();
    if (!out) return make_error(Errc::io_error, "cannot write " + to_utf8(path));
    return {};
}

struct Outcome {
    std::string status, path, note;
};

Outcome outcome_of(const PlannedEntry& planned, const Plan& plan)
{
    const std::string replaced = planned.replaced_by >= 0
                                     ? "replaced by " + plan.labels.at(static_cast<std::size_t>(planned.replaced_by))
                                     : std::string();
    if (planned.disposition == Disposition::skip) return {"overridden", "", replaced};
    switch (planned.status) {
    case EntryStatus::written:
    case EntryStatus::existing: return {replaced.empty() ? "ok" : "overridden", planned.path, replaced};
    case EntryStatus::failed: return {"failed", "", planned.error};
    case EntryStatus::pending: break;
    }
    return {"skipped", "", ""};
}

std::string record_csv(const ArchiveSource& source, const Plan& plan, std::size_t& looked_at)
{
    std::vector<Outcome> outcomes(source.toc.size(), Outcome{"skipped", "", ""});
    looked_at = 0;
    for (const PlannedEntry& planned : source.entries) {
        Outcome outcome = outcome_of(planned, plan);
        if (outcome.status != "skipped") ++looked_at;
        outcomes[planned.toc_index] = std::move(outcome);
    }

    std::string text = csv_row({"toc_index", "id", "offset", "packed_size", "raw_size", "crc_packed", "crc_raw",
                                "flags", "status", "path", "note"});
    for (std::size_t i = 0; i < source.toc.size(); ++i) {
        const Entry& e = source.toc[i];
        Outcome& o = outcomes[i];
        text += csv_row({std::to_string(i), hex8(e.id), std::to_string(e.offset), std::to_string(e.packed_size),
                         std::to_string(e.raw_size), hex8(e.crc_packed), hex8(e.crc_raw), hex_flags(e.flags),
                         std::move(o.status), std::move(o.path), std::move(o.note)});
    }
    return text;
}

} // namespace

std::string csv_row(const std::vector<std::string>& fields)
{
    std::string row;
    for (std::size_t i = 0; i < fields.size(); ++i) {
        if (i) row += ',';
        const std::string& field = fields[i];
        if (field.find_first_of(",\"\r\n") == std::string::npos) {
            row += field;
            continue;
        }
        row += '"';
        for (const char c : field) {
            if (c == '"') row += '"';
            row += c;
        }
        row += '"';
    }
    row += "\r\n";
    return row;
}

std::vector<std::vector<std::string>> parse_csv(std::string_view text)
{
    std::vector<std::vector<std::string>> rows;
    std::vector<std::string> row;
    std::string field;
    bool quoted = false;
    bool any = false;
    for (std::size_t i = 0; i < text.size(); ++i) {
        const char c = text[i];
        any = true;
        if (quoted) {
            if (c == '"') {
                if (i + 1 < text.size() && text[i + 1] == '"') {
                    field += '"';
                    ++i;
                } else {
                    quoted = false;
                }
            } else {
                field += c;
            }
        } else if (c == '"') {
            quoted = true;
        } else if (c == ',') {
            row.push_back(std::move(field));
            field.clear();
        } else if (c == '\r' || c == '\n') {
            if (c == '\r' && i + 1 < text.size() && text[i + 1] == '\n') ++i;
            row.push_back(std::move(field));
            field.clear();
            rows.push_back(std::move(row));
            row.clear();
            any = false;
        } else {
            field += c;
        }
    }
    if (any) {
        row.push_back(std::move(field));
        rows.push_back(std::move(row));
    }
    return rows;
}

Result<void> write_index(const Plan& plan, const fs::path& output_root)
{
    if (plan.groups.empty()) return {};   // nothing was unpacked
    const fs::path folder = output_root / from_utf8(kIndexFolder);

    // Rows of earlier runs, by archive.
    std::map<std::string, std::vector<std::string>> rows;
    if (auto existing = InputFile::read_all(folder / "index.csv")) {
        const std::string_view text(reinterpret_cast<const char*>(existing->data()), existing->size());
        auto table = parse_csv(text);
        if (!table.empty()) {
            const std::vector<std::string> header = table.front();
            for (std::size_t r = 1; r < table.size(); ++r) {
                std::vector<std::string> row;
                for (const std::string& column : kIndexColumns) {
                    const auto at = std::ranges::find(header, column);
                    const auto index = static_cast<std::size_t>(at - header.begin());
                    row.push_back(at != header.end() && index < table[r].size() ? table[r][index] : "");
                }
                if (!row[0].empty()) rows[row[0]] = std::move(row);
            }
        }
    }

    for (const OutputGroup& group : plan.groups) {
        for (const ArchiveSource& source : group.sources) {
            std::size_t looked_at = 0;
            const std::string record = record_csv(source, plan, looked_at);
            if (auto written = write_text(folder / from_utf8(source.location.record), record); !written) return written;
            const Location& location = source.location;
            rows[location.label] = {location.label,
                                    location.mount,
                                    location.mount.empty() ? "" : std::to_string(location.rank),
                                    location.folder,
                                    std::to_string(source.toc.size()),
                                    std::to_string(looked_at),
                                    std::to_string(source.file_size),
                                    location.record};
        }
    }

    std::vector<std::vector<std::string>> sorted;
    for (auto& [archive, row] : rows) sorted.push_back(std::move(row));
    std::ranges::stable_sort(sorted, {}, [](const std::vector<std::string>& row) { return lower_ascii(row[0]); });
    std::string index = csv_row(kIndexColumns);
    for (const auto& row : sorted) index += csv_row(row);
    if (auto written = write_text(folder / "index.csv", index); !written) return written;

    std::string mounts_csv = csv_row({"priority", "mount", "depth", "tree"});
    const auto all = mounts();
    for (std::size_t rank = 0; rank < all.size(); ++rank) {
        mounts_csv += csv_row({std::to_string(rank), std::string(all[rank].root), std::to_string(all[rank].depth),
                               std::string(tree_folder(all[rank].tree))});
    }
    if (auto written = write_text(folder / "mounts.csv", mounts_csv); !written) return written;

    // Python writes README.txt in text mode: \n line ends on POSIX, \r\n on Windows.
#if defined(_WIN32)
    std::string readme;
    for (const char c : kReadme) {
        if (c == '\n') readme += '\r';
        readme += c;
    }
    return write_text(folder / "README.txt", readme);
#else
    return write_text(folder / "README.txt", kReadme);
#endif
}

} // namespace npk::pipeline
