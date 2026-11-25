#include "dashboard.hpp"

#include "console.hpp"

#include <algorithm>
#include <ftxui/screen/screen.hpp>
#include <cmath>
#include <cstdio>

using namespace ftxui;

namespace npkextpp {

namespace {

constexpr double kFileCost = 64 * 1024;          // a file costs about as much as 64 KiB of data (ETA only)
constexpr double kSmoothing = 0.1;               // per 0.5 s sample: about 5 s of memory
constexpr auto kSample = std::chrono::milliseconds(500);
constexpr double kEtaAfter = 3;                  // seconds before an ETA is shown
constexpr int kGauge = 12;
constexpr int kStatus = 13;
constexpr int kMinName = 16;
constexpr const char* kColumnGap = " │ ";

std::string fmt(const char* format, auto... args)
{
    char text[160];
    std::snprintf(text, sizeof text, format, args...);
    return text;
}

std::string percent(double share)
{
    return fmt("%3.0f %%", std::clamp(share, 0.0, 1.0) * 100);
}

std::string short_duration(double seconds)
{
    const auto total = static_cast<long long>(std::max(seconds, 0.0) + 0.5);
    if (total < 60) return std::to_string(total) + " s";
    if (total < 3600) return fmt("%lld:%02lld", total / 60, total % 60);
    return fmt("%lld:%02lld:%02lld", total / 3600, (total % 3600) / 60, total % 60);
}

// "Documents/res_dlc_scene/character.npk" -> "Documents/re...character.npk"
std::string ellipsize(const std::string& text, int width)
{
    const auto w = static_cast<std::size_t>(std::max(width, 4));
    if (text.size() <= w) return text;
    const std::size_t tail = (w - 3) * 2 / 3;   // keep more of the end: the archive's name
    const std::size_t head = w - 3 - tail;
    return text.substr(0, head) + "..." + text.substr(text.size() - tail);
}

Element fixed(Element element, int width)
{
    return std::move(element) | size(WIDTH, EQUAL, width);
}

Element right(const std::string& value, int width)
{
    return fixed(text(value) | align_right, width);
}

} // namespace

Element logo(std::string_view version)
{
    return vbox({
               hbox({text("npkextpp") | bold | color(Color::Cyan), text(" v" + std::string(version)) | dim}),
               text("NeoX NXPK archive extractor"),
           }) |
           borderRounded | color(Color::Cyan);
}

Element disk_test(const npk::pipeline::DiskProbe& probe)
{
    using npk::pipeline::DiskKind;
    Elements lines;
    std::string reads = probe.reads > 0
                            ? "half under " + fmt("%.2f ms", probe.median_read_ms) + ", 9 in 10 under " +
                                  fmt("%.2f ms", probe.slow_read_ms) + " (" + std::to_string(probe.reads) +
                                  " random 4 KiB reads of " + probe.read_from + ")"
                            : "could not be measured";
    std::string writes = probe.write_mb_per_s > 0 ? fmt("%.0f MB/s", probe.write_mb_per_s) + " (16 MiB, flushed)"
                                                  : "could not be measured";
    lines.push_back(hbox({text("Disk test     ") | bold, text("reads   "), text(reads)}));
    lines.push_back(hbox({text("              "), text("writes  "), text(writes)}));
    std::string kind = probe.kind == DiskKind::hdd   ? "HDD"
                       : probe.kind == DiskKind::ssd ? "SSD"
                                                     : "unknown (" + probe.problem + "), tuning as for an SSD";
    std::string why = probe.kind == DiskKind::unknown ? ""
                      : probe.kind == DiskKind::hdd   ? "  (1 read in 10 took 1 ms or more)"
                                                      : "  (9 reads in 10 took under 1 ms)";
    lines.push_back(hbox({text("              "), text("Assuming drive type: "), text(kind) | bold | color(Color::Cyan),
                          text(why) | dim}));
    return vbox(std::move(lines));
}

Dashboard::Dashboard(const npk::Plan& plan, std::span<const std::string> missing,
                     const npk::pipeline::Progress& progress, npk::pipeline::DiskKind disk)
    : progress_(progress)
    , disk_(disk)
{
    std::size_t index = 0;
    std::uint32_t largest = 0;
    for (const npk::OutputGroup& group : plan.groups) {
        for (const npk::ArchiveSource& source : group.sources) {
            Archive archive{source.label(), 0, 0, 0, index++, false};
            for (const npk::PlannedEntry& planned : source.entries) {
                if (planned.disposition == npk::Disposition::skip) continue;
                if (planned.status == npk::EntryStatus::existing) {
                    ++archive.existing;
                    continue;
                }
                ++archive.files;
                archive.bytes += source.entry(planned).raw_size;
            }
            total_files_ += archive.files;
            total_bytes_ += archive.bytes;
            existing_files_ += archive.existing;
            largest = std::max(largest, archive.files);
            archives_.push_back(std::move(archive));
        }
    }
    for (const std::string& label : missing) archives_.push_back(Archive{label, 0, 0, 0, 0, true});
    const std::string widest = format_count(largest);
    count_width_ = static_cast<int>(widest.size() * 2 + 1);
}

void Dashboard::measure()
{
    const auto now = Clock::now();
    if (now - sampled_at_ < kSample) return;
    const double dt = std::chrono::duration<double>(now - sampled_at_).count();
    const std::uint64_t files = progress_.files_written.load() + progress_.files_failed.load();
    const std::uint64_t bytes = progress_.bytes_written.load();
    const double work = static_cast<double>(bytes - sampled_bytes_) + kFileCost * static_cast<double>(files - sampled_files_);
    const double work_now = work / dt;
    const double bytes_now = static_cast<double>(bytes - sampled_bytes_) / dt;
    const bool first = work_rate_ == 0 && byte_rate_ == 0;
    work_rate_ = first ? work_now : work_rate_ + kSmoothing * (work_now - work_rate_);
    byte_rate_ = first ? bytes_now : byte_rate_ + kSmoothing * (bytes_now - byte_rate_);
    sampled_at_ = now;
    sampled_files_ = files;
    sampled_bytes_ = bytes;
}

// Time left, from the recent rate of work: bytes plus a fixed cost per file
// (a stretch of tiny files and a stretch of textures move at different MB/s).
double Dashboard::eta_seconds(std::uint64_t files_left, std::uint64_t bytes_left) const
{
    const double elapsed = std::chrono::duration<double>(Clock::now() - started_).count();
    if (elapsed < kEtaAfter || work_rate_ <= 0) return -1;
    return (static_cast<double>(bytes_left) + kFileCost * static_cast<double>(files_left)) / work_rate_;
}

Dashboard::State Dashboard::state_of(const Archive& archive, bool finished) const
{
    if (archive.missing) return State::missing;
    if (archive.files == 0) return State::existing;
    const auto& counts = progress_.sources()[archive.index];
    const std::uint32_t done = counts.files_done.load(std::memory_order_relaxed);
    if (done >= archive.files) return counts.files_failed.load(std::memory_order_relaxed) ? State::failed : State::done;
    if (finished) return State::not_reached;
    return done > 0 ? State::active : State::waiting;
}

Element Dashboard::pipeline_box() const
{
    const auto& p = progress_;
    const std::string block = std::to_string(p.read_block_kib.load() >> 10) + " MiB";
    const std::string disk = disk_ == npk::pipeline::DiskKind::hdd   ? "HDD"
                             : disk_ == npk::pipeline::DiskKind::ssd ? "SSD"
                                                                     : "unknown";
    const auto row = [](const std::string& stage, const npk::pipeline::StageLoad& load, const char* item, bool finished) {
        if (finished) return hbox({fixed(text(stage), 10), text("finished") | dim});
        const double ms = load.ms_per_item.load(std::memory_order_relaxed);
        const std::string per = ms > 0 ? (ms >= 10 ? fmt("%.0f ms", ms) : ms >= 0.1 ? fmt("%.2f ms", ms) : fmt("%.0f us", ms * 1000)) +
                                             " per " + item
                                       : "";
        return hbox({fixed(text(stage), 10), right(percent(load.working), 8), right(percent(load.waiting_input), 10),
                     right(percent(load.waiting_output), 10), text("   "), text(per) | dim});
    };
    return vbox({
        hbox({text("Reading buffer size ") | dim, text(block) | bold, text("    Number of writers ") | dim,
              text(std::to_string(p.writers_active.load())) | bold, text("    Tuned for ") | dim, text(disk) | bold}),
        hbox({fixed(text("last 2 s"), 10), right("busy", 8), right("starved", 10), right("blocked", 10),
              text("   average time")}) | dim,
        row("Reader", p.reader, "read", p.reading_done.load()),
        row("Decoders", p.decoders, "file", p.decoding_done.load()),
        row("Writers", p.writers, "file", false),
    });
}

Element Dashboard::progress_box() const
{
    const std::uint64_t files = progress_.files_written.load() + progress_.files_failed.load();
    const std::uint64_t failed = progress_.files_failed.load();
    const std::uint64_t bytes = progress_.bytes_written.load();
    const double share = total_files_ ? static_cast<double>(files) / static_cast<double>(total_files_) : 1.0;
    const double eta = eta_seconds(total_files_ - std::min(files, total_files_), total_bytes_ - std::min(bytes, total_bytes_));
    const double elapsed = std::chrono::duration<double>(Clock::now() - started_).count();

    Elements details = {
        text(format_count(files) + " / " + format_count(total_files_) + " files"),
        text(format_bytes(bytes) + " / " + format_bytes(total_bytes_)) | dim,
        text(format_bytes(static_cast<std::uint64_t>(byte_rate_)) + "/s"),
        text("elapsed " + short_duration(elapsed)) | dim,
        hbox({text("ETA ") | dim, text(eta < 0 ? "--" : short_duration(eta)) | bold}),
    };
    if (failed) details.push_back(text(format_count(failed) + " failed") | color(Color::Red));
    if (existing_files_) details.push_back(text(format_count(existing_files_) + " already there") | dim);
    return vbox({
        hbox({gauge(static_cast<float>(share)) | flex | color(Color::Cyan), right(fmt("%.1f %%", share * 100), 9)}),
        flexbox(std::move(details), FlexboxConfig().SetGap(3, 0)),   // wraps on a narrow terminal
    });
}

Element Dashboard::cell(const Cell& c, int width) const
{
    if (!c.archive) {   // a line standing for several archives
        Color tone = c.state == State::missing ? Color::Red : c.state == State::done ? Color::Green : Color::GrayDark;
        return fixed(text(c.text) | color(tone), width);
    }
    const Archive& a = *c.archive;
    const int name_width = width - (2 + 1 + kGauge + 1 + count_width_ + 1 + kStatus);
    const npk::pipeline::SourceProgress* counts = a.missing ? nullptr : &progress_.sources()[a.index];
    const std::uint32_t done = counts ? std::min(counts->files_done.load(std::memory_order_relaxed), a.files) : 0;
    const std::uint64_t bytes = counts ? counts->bytes_done.load(std::memory_order_relaxed) : 0;

    std::string icon = "·";
    std::string status;
    Color tone = Color::GrayDark;
    float share = a.files ? static_cast<float>(done) / static_cast<float>(a.files) : 1.0F;
    switch (c.state) {
    case State::waiting: status = "waiting"; break;
    case State::active: {
        icon = "▸";
        tone = Color::Yellow;
        const double eta = eta_seconds(a.files - done, a.bytes - std::min(bytes, a.bytes));
        status = eta < 0 ? "" : "ETA " + short_duration(eta);
        break;
    }
    case State::done: icon = "✓", tone = Color::Green, status = "done"; break;
    case State::failed:
        icon = "✗", tone = Color::Red;
        status = format_count(counts ? counts->files_failed.load() : 0) + " failed";
        break;
    case State::existing: icon = "✓", status = a.existing ? "already there" : "nothing new"; break;
    case State::missing: icon = "✗", tone = Color::Red, status = "missing", share = 0; break;
    case State::not_reached: icon = "-", status = done ? "stopped" : "not reached"; break;
    }
    const std::string count = a.missing                    ? ""
                              : c.state == State::existing ? format_count(a.existing)
                                                           : format_count(done) + "/" + format_count(a.files);
    return hbox({
        fixed(text(icon) | color(tone), 2),
        fixed(text(ellipsize(a.label, name_width)) | (c.state == State::waiting ? dim : nothing), std::max(name_width, 4)),
        text(" "),
        fixed(a.missing ? text("") : gauge(share) | color(tone), kGauge),
        text(" "),
        right(count, count_width_),
        text(" "),
        fixed(text(status) | color(tone), kStatus),
    });
}

Element Dashboard::archives_box(int width, int rows, bool finished) const
{
    const int inner = width - 2;
    const int fixed_part = 2 + 1 + kGauge + 1 + count_width_ + 1 + kStatus;
    const int gap = 3;
    const int columns = (inner - gap) / 2 >= fixed_part + kMinName && archives_.size() > 1 ? 2 : 1;
    const int cell_width = columns == 2 ? (inner - gap) / 2 : inner;

    std::vector<Cell> cells;
    for (const Archive& a : archives_) cells.push_back({&a, {}, state_of(a, finished)});

    const auto capacity = rows > 0 ? static_cast<std::size_t>(std::max(rows, 1)) * static_cast<std::size_t>(columns) : 0;
    if (capacity > 0 && cells.size() > capacity) {
        // Too many to show: fold the finished ones and the missing ones into one
        // line each, keep the active ones, then as many waiting ones as fit.
        std::vector<Cell> finished_cells, active, waiting, missing;
        std::uint64_t finished_files = 0;
        for (Cell& c : cells) {
            switch (c.state) {
            case State::done:
            case State::failed:
            case State::existing:
                finished_files += c.archive->files + c.archive->existing;
                finished_cells.push_back(c);
                break;
            case State::active: active.push_back(c); break;
            case State::missing: missing.push_back(c); break;
            default: waiting.push_back(c); break;
            }
        }
        std::vector<Cell> shown;
        const std::size_t failed = static_cast<std::size_t>(std::ranges::count_if(finished_cells, [](const Cell& c) {
            return c.state == State::failed;
        }));
        if (!finished_cells.empty()) {
            std::string line = "✓ " + format_count(finished_cells.size()) +
                               (finished_cells.size() == 1 ? " archive done (" : " archives done (") +
                               format_count(finished_files) + " files)";
            if (failed) line += ", " + format_count(failed) + " with failures";
            shown.push_back({nullptr, line, State::done});
        }
        const std::size_t tail = (missing.empty() ? 0 : 1);
        std::size_t room = capacity - shown.size() - tail;
        for (const Cell& c : active) {
            if (room == 0) break;
            shown.push_back(c);
            --room;
        }
        std::size_t taken = 0;
        while (taken < waiting.size() && room > (waiting.size() - taken > room ? 1u : 0u)) {
            shown.push_back(waiting[taken++]);
            --room;
        }
        if (taken < waiting.size()) {
            shown.push_back({nullptr, "… " + format_count(waiting.size() - taken) + " more waiting", State::waiting});
        }
        if (!missing.empty()) {
            std::string names;
            for (const Cell& c : missing) names += (names.empty() ? "" : ", ") + c.archive->label;
            shown.push_back({nullptr, ellipsize("✗ " + format_count(missing.size()) + " missing: " + names, cell_width),
                             State::missing});
        }
        cells = std::move(shown);
    }

    // Column by column, top to bottom.
    const std::size_t per_column = (cells.size() + static_cast<std::size_t>(columns) - 1) / static_cast<std::size_t>(columns);
    Elements lines;
    for (std::size_t r = 0; r < per_column; ++r) {
        Elements line;
        for (int col = 0; col < columns; ++col) {
            const std::size_t i = static_cast<std::size_t>(col) * per_column + r;
            if (col > 0) line.push_back(text(kColumnGap) | dim);
            line.push_back(i < cells.size() ? cell(cells[i], cell_width) : fixed(text(""), cell_width));
        }
        lines.push_back(hbox(std::move(line)));
    }
    if (lines.empty()) lines.push_back(text("no archives") | dim);
    return window(text(" Archives ") | bold, vbox(std::move(lines)));
}

Element Dashboard::frame(int width, int rows)
{
    measure();
    Element top = vbox({
        window(text(" Pipeline ") | bold, pipeline_box()),
        window(text(" Progress ") | bold, progress_box()),
    });
    int archive_rows = 0;
    if (rows > 0) {   // what is left for the archive list, inside its borders
        auto measured = Screen::Create(Dimension::Fixed(width), Dimension::Fixed(rows));
        Render(measured, top);   // lays it out at this width (the progress details may wrap)
        archive_rows = std::max(rows - top->requirement().min_y - 2, 3);
    }
    return vbox({std::move(top), archives_box(width, archive_rows, false)});
}

Element Dashboard::archive_list(int width, bool finished)
{
    return archives_box(width, 0, finished);
}

std::string Dashboard::progress_line()
{
    measure();
    const std::uint64_t files = progress_.files_written.load() + progress_.files_failed.load();
    const std::uint64_t bytes = progress_.bytes_written.load();
    const double share = total_files_ ? static_cast<double>(files) / static_cast<double>(total_files_) : 1.0;
    const double eta = eta_seconds(total_files_ - std::min(files, total_files_), total_bytes_ - std::min(bytes, total_bytes_));
    return fmt("%5.1f %%  ", share * 100) + format_count(files) + " / " + format_count(total_files_) + " files  " +
           format_bytes(static_cast<std::uint64_t>(byte_rate_)) + "/s  " + std::to_string(progress_.writers_active.load()) +
           " writers  " + std::to_string(progress_.read_block_kib.load() >> 10) + " MiB reads  ETA " +
           (eta < 0 ? std::string("--") : short_duration(eta));
}

} // namespace npkextpp
