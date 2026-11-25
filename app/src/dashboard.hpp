#pragma once

// What the screen shows: the logo, the disk test, and the live frame (pipeline
// settings and stage loads, overall progress, every archive with its own
// progress). Builds FTXUI elements; Console prints them.

#include "npk/pipeline/disk_probe.hpp"
#include "npk/pipeline/progress.hpp"
#include "npk/plan.hpp"

#include <ftxui/dom/elements.hpp>

#include <chrono>
#include <cstdint>
#include <deque>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace npkextpp {

[[nodiscard]] ftxui::Element logo(std::string_view version);

// The disk test's readings and the drive type it led to.
[[nodiscard]] ftxui::Element disk_test(const npk::pipeline::DiskProbe& probe);

// `missing`: archives of the known list that are not in the installation.
class Dashboard {
public:
    Dashboard(const npk::Plan& plan, std::span<const std::string> missing, const npk::pipeline::Progress& progress,
              npk::pipeline::DiskKind disk);

    // One live frame, at most `rows` rows high. When the archives don't fit, the
    // list shows a window of them that follows the archive being unpacked, or
    // stays where the user scrolled it.
    [[nodiscard]] ftxui::Element frame(int width, int rows);

    enum class Scroll { line_up, line_down, page_up, page_down, top, bottom, follow };
    void scroll(Scroll how, int lines = 1);

    // Every archive (for the end of the run, or a log without a terminal).
    // `finished`: the run is over, so archives not done were not reached.
    [[nodiscard]] ftxui::Element archive_list(int width, bool finished);

    // One plain line of overall progress (for logs).
    [[nodiscard]] std::string progress_line();

private:
    using Clock = std::chrono::steady_clock;

    struct Archive {
        std::string label;
        std::uint32_t files = 0;      // to write in this run
        std::uint32_t existing = 0;   // already in the output folder
        std::uint64_t bytes = 0;
        std::size_t index = 0;        // into Progress::sources()
        bool missing = false;
    };
    enum class State { waiting, active, done, failed, existing, missing, not_reached };
    struct Cell {   // one place in the archive list
        const Archive* archive = nullptr;
        State state = State::waiting;
    };
    struct Sample {   // for the time left
        Clock::time_point at;
        double work = 0;   // done so far, in work units
    };

    void measure();   // updates the rates
    [[nodiscard]] State state_of(const Archive& archive, bool finished) const;
    [[nodiscard]] double eta_seconds(std::uint64_t files_left, std::uint64_t bytes_left) const;
    [[nodiscard]] ftxui::Element pipeline_box() const;
    [[nodiscard]] ftxui::Element progress_box() const;
    [[nodiscard]] ftxui::Element archives_box(int width, int rows, bool finished);
    [[nodiscard]] ftxui::Element cell(const Cell& cell, int width) const;

    const npk::pipeline::Progress& progress_;
    npk::pipeline::DiskKind disk_;
    std::vector<Archive> archives_;
    std::uint64_t total_files_ = 0;
    std::uint64_t total_bytes_ = 0;
    std::uint64_t existing_files_ = 0;
    int count_width_ = 0;   // of "done/total" for the largest archive

    Clock::time_point started_ = Clock::now();
    Clock::time_point sampled_at_ = started_;
    std::uint64_t sampled_bytes_ = 0;
    double byte_rate_ = 0;          // bytes per second, over the last few seconds (shown)
    std::deque<Sample> samples_;    // one a second, the last 5 minutes
    double work_rate_ = 0;          // work units per second, recent seconds weighted most (time left)

    // The archive list's window: its first row, and how it was laid out last.
    int top_row_ = 0;
    bool follow_ = true;
    int shown_rows_ = 0;
    int total_rows_ = 0;
};

} // namespace npkextpp
