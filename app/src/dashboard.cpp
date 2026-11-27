#include "dashboard.hpp"

#include "console.hpp"

#include <algorithm>
#include <ftxui/screen/screen.hpp>
#include <cmath>
#include <cstdio>

using namespace ftxui;

namespace npkextpp {

namespace {

constexpr double kFileCost = 64 * 1024;          // a file costs about as much as 64 KiB of data (time left only)
constexpr double kSmoothing = 0.1;               // shown speed: per 0.5 s sample, about 5 s of memory
constexpr auto kSample = std::chrono::milliseconds(500);
constexpr auto kWorkSample = std::chrono::seconds(1);     // time left: one sample a second...
constexpr double kWorkWindow = 300;                       // ... weighted over the last 5 minutes
constexpr double kEtaAfter = 5;                  // seconds before a time left is shown
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

// 0:07, 12:34, 1:02:03
std::string clock_time(double seconds)
{
    const auto total = static_cast<long long>(std::max(seconds, 0.0) + 0.5);
    if (total < 3600) return fmt("%lld:%02lld", total / 60, total % 60);
    return fmt("%lld:%02lld:%02lld", total / 3600, (total % 3600) / 60, total % 60);
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

Dashboard::Dashboard(const npk::Plan& plan, const npk::pipeline::Progress& progress, npk::pipeline::DiskKind disk)
    : progress_(progress)
    , disk_(disk)
{
    std::size_t index = 0;
    std::uint32_t largest = 0;
    for (const npk::OutputGroup& group : plan.groups) {
        for (const npk::ArchiveSource& source : group.sources) {
            Archive archive{source.label(), 0, 0, 0, index++};
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
    const std::string widest = format_count(largest);
    count_width_ = static_cast<int>(widest.size() * 2 + 1);
}

void Dashboard::measure()
{
    const auto now = Clock::now();
    const std::uint64_t files = progress_.files_written.load() + progress_.files_failed.load();
    const std::uint64_t bytes = progress_.bytes_written.load();

    // The speed shown: a few seconds' worth.
    if (now - sampled_at_ >= kSample) {
        const double dt = std::chrono::duration<double>(now - sampled_at_).count();
        const double bytes_now = static_cast<double>(bytes - sampled_bytes_) / dt;
        byte_rate_ = byte_rate_ == 0 ? bytes_now : byte_rate_ + kSmoothing * (bytes_now - byte_rate_);
        sampled_at_ = now;
        sampled_bytes_ = bytes;
    }

    // The rate behind the time left: the work of every second of the last 5
    // minutes, weighted by (1 - age / 5 min)^2, so the last minute counts most
    // and a stall of a few seconds hardly moves it. 300 samples, weighed once a second.
    if (samples_.empty()) samples_.push_back({started_, 0});
    if (now - samples_.back().at < kWorkSample) return;
    samples_.push_back({now, static_cast<double>(bytes) + kFileCost * static_cast<double>(files)});
    const auto age = [&now](const Sample& sample) { return std::chrono::duration<double>(now - sample.at).count(); };
    while (samples_.size() > 2 && age(samples_[1]) > kWorkWindow) samples_.pop_front();
    double work = 0;
    double seconds = 0;
    for (std::size_t i = 1; i < samples_.size(); ++i) {
        const double fresh = std::max(0.0, 1.0 - age(samples_[i]) / kWorkWindow);
        const double weight = fresh * fresh;
        work += weight * (samples_[i].work - samples_[i - 1].work);
        seconds += weight * std::chrono::duration<double>(samples_[i].at - samples_[i - 1].at).count();
    }
    work_rate_ = seconds > 0 ? work / seconds : 0;
}

// Time left, from the recent rate of work: bytes plus a fixed cost per file
// (a stretch of tiny files and a stretch of textures move at different MB/s).
double Dashboard::eta_seconds(std::uint64_t files_left, std::uint64_t bytes_left) const
{
    const double elapsed = std::chrono::duration<double>(Clock::now() - started_).count();
    if (elapsed < kEtaAfter || work_rate_ <= 0) return -1;
    return (static_cast<double>(bytes_left) + kFileCost * static_cast<double>(files_left)) / work_rate_;
}

void Dashboard::scroll(Scroll how, int lines)
{
    const int page = std::max(shown_rows_ - 1, 1);
    switch (how) {
    case Scroll::line_up: top_row_ -= lines; break;
    case Scroll::line_down: top_row_ += lines; break;
    case Scroll::page_up: top_row_ -= page; break;
    case Scroll::page_down: top_row_ += page; break;
    case Scroll::top: top_row_ = 0; break;
    case Scroll::bottom: top_row_ = total_rows_; break;
    case Scroll::follow: follow_ = true; return;
    }
    follow_ = false;
    top_row_ = std::clamp(top_row_, 0, std::max(total_rows_ - shown_rows_, 0));
}

Dashboard::State Dashboard::state_of(const Archive& archive, bool finished) const
{
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
    const auto field = [](const std::string& label, const std::string& value) {
        return hbox({text(label + " ") | dim, text(value) | bold});
    };

    Elements details = {
        field("Files", format_count(files) + " / " + format_count(total_files_)),
        field("Data", format_bytes(bytes) + " / " + format_bytes(total_bytes_)),
        field("Speed", format_bytes(static_cast<std::uint64_t>(byte_rate_)) + "/s"),
    };
    if (failed) details.push_back(text(format_count(failed) + " failed") | color(Color::Red));
    if (existing_files_) details.push_back(text(format_count(existing_files_) + " already there") | dim);
    return vbox({
        hbox({gauge(static_cast<float>(share)) | flex | color(Color::Cyan), right(fmt("%.1f %%", share * 100), 9)}),
        hbox({field("Elapsed", clock_time(elapsed)), text("   "),
              field("ETA", eta < 0 ? "estimating..." : clock_time(eta))}),
        flexbox(std::move(details), FlexboxConfig().SetGap(3, 0)),   // wraps on a narrow terminal
    });
}

Element Dashboard::cell(const Cell& c, int width) const
{
    const Archive& a = *c.archive;
    const int name_width = width - (2 + 1 + kGauge + 1 + count_width_ + 1 + kStatus);
    const npk::pipeline::SourceProgress& counts = progress_.sources()[a.index];
    const std::uint32_t done = std::min(counts.files_done.load(std::memory_order_relaxed), a.files);
    const std::uint64_t bytes = counts.bytes_done.load(std::memory_order_relaxed);

    std::string icon = "·";
    std::string status;
    Color tone = Color::GrayDark;
    const float share = a.files ? static_cast<float>(done) / static_cast<float>(a.files) : 1.0F;
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
        status = format_count(counts.files_failed.load()) + " failed";
        break;
    case State::existing: icon = "✓", status = a.existing ? "already there" : "nothing new"; break;
    case State::not_reached: icon = "-", status = done ? "stopped" : "not reached"; break;
    }
    const std::string count =
        c.state == State::existing ? format_count(a.existing) : format_count(done) + "/" + format_count(a.files);
    return hbox({
        fixed(text(icon) | color(tone), 2),
        fixed(text(ellipsize(a.label, name_width)) | (c.state == State::waiting ? dim : nothing), std::max(name_width, 4)),
        text(" "),
        fixed(gauge(share) | color(tone), kGauge),
        text(" "),
        right(count, count_width_),
        text(" "),
        fixed(text(status) | color(tone), kStatus),
    });
}

Element Dashboard::archives_box(int width, int rows, bool finished)
{
    const int inner = width - 2;
    const int fixed_part = 2 + 1 + kGauge + 1 + count_width_ + 1 + kStatus;
    const int gap = 3;
    const int columns = (inner - gap) / 2 >= fixed_part + kMinName && archives_.size() > 1 ? 2 : 1;
    const int cell_width = columns == 2 ? (inner - gap) / 2 : inner;

    std::vector<Cell> cells;
    for (const Archive& a : archives_) cells.push_back({&a, state_of(a, finished)});

    // Row by row, left to right, so a window of rows shows neighbours.
    const int total = (static_cast<int>(cells.size()) + columns - 1) / columns;
    int first = 0;
    int count = total;
    std::string title = " Archives ";
    if (rows > 0 && total > rows) {
        if (follow_) {   // keep the archive being unpacked in the top third
            const auto at = std::ranges::find_if(cells, [](const Cell& c) { return c.state == State::active; });
            const auto next = std::ranges::find_if(cells, [](const Cell& c) { return c.state == State::waiting; });
            const auto mark = at != cells.end() ? at : next != cells.end() ? next : cells.end() - 1;
            top_row_ = static_cast<int>(mark - cells.begin()) / columns - rows / 3;
        }
        top_row_ = std::clamp(top_row_, 0, total - rows);
        first = top_row_;
        count = rows;
        const auto from = static_cast<std::size_t>(first * columns) + 1;
        const auto to = std::min(static_cast<std::size_t>((first + count) * columns), cells.size());
        title += format_count(from) + "-" + format_count(to) + " of " + format_count(cells.size()) +
                 " · scroll: wheel ↑↓ PgUp PgDn " + (follow_ ? "" : "· F follows progress ");
    }
    shown_rows_ = count;
    total_rows_ = total;

    Elements lines;
    for (int r = first; r < first + count; ++r) {
        Elements line;
        for (int col = 0; col < columns; ++col) {
            const auto i = static_cast<std::size_t>(r * columns + col);
            if (col > 0) line.push_back(text(kColumnGap) | dim);
            line.push_back(i < cells.size() ? cell(cells[i], cell_width) : fixed(text(""), cell_width));
        }
        lines.push_back(hbox(std::move(line)));
    }
    if (lines.empty()) lines.push_back(text("no archives") | dim);
    return window(text(title) | bold, vbox(std::move(lines)));
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
           (progress_.writers_active.load() == 1 ? " writer  " : " writers  ") + std::to_string(progress_.read_block_kib.load() >> 10) + " MiB reads  elapsed " +
           clock_time(std::chrono::duration<double>(Clock::now() - started_).count()) + "  ETA " +
           (eta < 0 ? std::string("--") : clock_time(eta));
}

} // namespace npkextpp
