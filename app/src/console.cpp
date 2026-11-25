#include "console.hpp"

#include <ftxui/screen/screen.hpp>
#include <ftxui/screen/terminal.hpp>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <string>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <io.h>
#include <windows.h>
#define NPK_ISATTY(fd) _isatty(fd)
#define NPK_FILENO(f) _fileno(f)
#else
#include <unistd.h>
#define NPK_ISATTY(fd) ::isatty(fd)
#define NPK_FILENO(f) ::fileno(f)
#endif

namespace npkextpp {

namespace {

constexpr int kWidthWithoutTerminal = 100;

bool is_terminal()
{
    if (NPK_ISATTY(NPK_FILENO(stdout)) == 0) return false;
    const char* term = std::getenv("TERM");
#if defined(_WIN32)
    // The Windows console has no TERM; it understands escape codes once asked to.
    const HANDLE out = ::GetStdHandle(STD_OUTPUT_HANDLE);
    DWORD mode = 0;
    if (!::GetConsoleMode(out, &mode)) return false;
    if (!::SetConsoleMode(out, mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING | ENABLE_PROCESSED_OUTPUT)) return false;
    ::SetConsoleOutputCP(CP_UTF8);
    return term == nullptr || std::string_view(term) != "dumb";
#else
    return term != nullptr && std::string_view(term) != "dumb";
#endif
}

// Drops escape sequences (colours, cursor moves, hyperlinks) for plain output.
std::string strip_escapes(const std::string& text)
{
    std::string out;
    out.reserve(text.size());
    for (std::size_t i = 0; i < text.size(); ++i) {
        if (text[i] != '\x1B') {
            if (text[i] != '\r') out += text[i];
            continue;
        }
        if (i + 1 < text.size() && text[i + 1] == '[') {   // CSI: ESC [ ... final byte
            i += 2;
            while (i < text.size() && !(text[i] >= 0x40 && text[i] <= 0x7E)) ++i;
        } else if (i + 1 < text.size() && text[i + 1] == ']') {   // OSC: ESC ] ... BEL or ESC '\'
            i += 2;
            while (i < text.size() && text[i] != '\x07' && !(text[i] == '\x1B' && i + 1 < text.size() && text[i + 1] == '\\')) ++i;
            if (i < text.size() && text[i] == '\x1B') ++i;
        } else {
            ++i;
        }
    }
    // Trailing spaces of each line (cells nobody drew on).
    std::string trimmed;
    std::size_t start = 0;
    while (start <= out.size()) {
        std::size_t end = out.find('\n', start);
        if (end == std::string::npos) end = out.size();
        std::size_t last = end;
        while (last > start && out[last - 1] == ' ') --last;
        trimmed.append(out, start, last - start);
        if (end < out.size()) trimmed += '\n';
        start = end + 1;
    }
    return trimmed;
}

} // namespace

std::string format_bytes(std::uint64_t bytes)
{
    const char* units[] = {"B", "KiB", "MiB", "GiB", "TiB"};
    double value = static_cast<double>(bytes);
    int unit = 0;
    while (value >= 1024 && unit < 4) {
        value /= 1024;
        ++unit;
    }
    char text[32];
    std::snprintf(text, sizeof text, unit == 0 ? "%.0f %s" : "%.1f %s", value, units[unit]);
    return text;
}

std::string format_count(std::uint64_t count)
{
    std::string digits = std::to_string(count);
    std::string out;
    for (std::size_t i = 0; i < digits.size(); ++i) {
        if (i != 0 && (digits.size() - i) % 3 == 0) out += ',';
        out += digits[i];
    }
    return out;
}

std::string format_duration(double seconds)
{
    char text[32];
    if (seconds < 60) {
        std::snprintf(text, sizeof text, "%.1f s", seconds);
    } else {
        const auto total = static_cast<long long>(seconds + 0.5);
        if (total >= 3600) std::snprintf(text, sizeof text, "%lld h %02lld min", total / 3600, (total % 3600) / 60);
        else std::snprintf(text, sizeof text, "%lld min %02lld s", total / 60, total % 60);
    }
    return text;
}

Console::Console() : interactive_(is_terminal()) {}

int Console::width() const
{
    if (!interactive_) return kWidthWithoutTerminal;
    return std::max(ftxui::Terminal::Size().dimx, 40);
}

int Console::height() const
{
    return interactive_ ? std::max(ftxui::Terminal::Size().dimy, 10) : 0;
}

std::string Console::render(const ftxui::Element& element, int width, int height) const
{
    element->ComputeRequirement();
    const auto needed = element->requirement();
    int rows = std::max(needed.min_y, 1);
    if (height > 0) rows = std::min(rows, height);
    auto screen = ftxui::Screen::Create(ftxui::Dimension::Fixed(width), ftxui::Dimension::Fixed(rows));
    ftxui::Render(screen, element);
    std::string text = screen.ToString();
    return interactive_ ? text : strip_escapes(text);
}

void Console::clear_frame_locked()
{
    if (frame_.empty()) return;
    std::fputs(reset_.c_str(), stdout);
    std::fflush(stdout);
    frame_.clear();
    reset_.clear();
}

void Console::line(std::string_view text)
{
    std::lock_guard lock(mutex_);
    const std::string frame = frame_;
    const std::string reset = reset_;
    clear_frame_locked();
    std::fprintf(stdout, "%.*s\n", static_cast<int>(text.size()), text.data());
    if (!frame.empty()) {   // put the frame back below the new line
        std::fputs(frame.c_str(), stdout);
        frame_ = frame;
        reset_ = reset;
    }
    std::fflush(stdout);
}

void Console::print(const ftxui::Element& element)
{
    element->ComputeRequirement();
    const int natural = std::max(element->requirement().min_x, 1);
    const std::string text = render(element, std::min(natural, width()), 0);
    std::lock_guard lock(mutex_);
    clear_frame_locked();
    std::fputs(text.c_str(), stdout);
    std::fputs("\n", stdout);
    std::fflush(stdout);
}

void Console::error(std::string_view text)
{
    std::lock_guard lock(mutex_);
    clear_frame_locked();
    std::fprintf(stderr, "error: %.*s\n", static_cast<int>(text.size()), text.data());
    std::fflush(stderr);
}

void Console::draw(const ftxui::Element& frame)
{
    if (!interactive_) return;
    const int rows_available = height() - 1;   // the cursor's line stays free
    const std::string text = render(frame, width(), rows_available);
    const auto rows = static_cast<int>(std::count(text.begin(), text.end(), '\n')) + 1;

    // How to get back to this frame's first line, clearing every line of it.
    std::string reset = "\r\x1B[2K";
    for (int i = 1; i < rows; ++i) reset += "\x1B[1A\x1B[2K";

    std::lock_guard lock(mutex_);
    const auto old_rows = frame_.empty() ? 0 : static_cast<int>(std::count(frame_.begin(), frame_.end(), '\n')) + 1;
    std::string out;
    if (!frame_.empty()) {
        if (rows < old_rows) {
            out += reset_;   // clears every old line
        } else {
            out += '\r';
            for (int i = 1; i < old_rows; ++i) out += "\x1B[1A";
        }
    }
    out += text;
    std::fputs(out.c_str(), stdout);
    std::fflush(stdout);
    frame_ = text;
    reset_ = std::move(reset);
}

void Console::clear_frame()
{
    std::lock_guard lock(mutex_);
    clear_frame_locked();
}

} // namespace npkextpp
