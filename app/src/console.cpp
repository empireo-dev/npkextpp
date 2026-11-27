#include "console.hpp"

#include <ftxui/screen/screen.hpp>
#include <ftxui/screen/terminal.hpp>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <string>
#include <thread>

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
#include <poll.h>
#include <termios.h>
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

// --- Input -----------------------------------------------------------------------

#if defined(_WIN32)

struct Console::Input {
    HANDLE handle = INVALID_HANDLE_VALUE;
    DWORD saved_mode = 0;
};

void Console::start_input()
{
    if (!interactive_ || input_) return;
    const HANDLE handle = ::GetStdHandle(STD_INPUT_HANDLE);
    DWORD mode = 0;
    if (handle == INVALID_HANDLE_VALUE || !::GetConsoleMode(handle, &mode)) return;
    // Mouse events instead of Quick Edit selection; Ctrl+C still raises SIGINT.
    if (!::SetConsoleMode(handle, ENABLE_EXTENDED_FLAGS | ENABLE_MOUSE_INPUT | ENABLE_PROCESSED_INPUT)) return;
    input_ = std::make_unique<Input>(Input{handle, mode});
}

void Console::stop_input()
{
    if (!input_) return;
    ::SetConsoleMode(input_->handle, input_->saved_mode);
    input_.reset();
}

std::vector<Console::Key> Console::read_keys(std::chrono::milliseconds timeout)
{
    std::vector<Key> keys;
    if (!input_) {
        std::this_thread::sleep_for(timeout);
        return keys;
    }
    if (::WaitForSingleObject(input_->handle, static_cast<DWORD>(timeout.count())) != WAIT_OBJECT_0) return keys;
    INPUT_RECORD records[64];
    DWORD count = 0;
    if (!::ReadConsoleInputW(input_->handle, records, 64, &count)) return keys;
    for (DWORD i = 0; i < count; ++i) {
        const INPUT_RECORD& record = records[i];
        if (record.EventType == KEY_EVENT && record.Event.KeyEvent.bKeyDown) {
            switch (record.Event.KeyEvent.wVirtualKeyCode) {
            case VK_UP: keys.push_back(Key::up); break;
            case VK_DOWN: keys.push_back(Key::down); break;
            case VK_PRIOR: keys.push_back(Key::page_up); break;
            case VK_NEXT: keys.push_back(Key::page_down); break;
            case VK_HOME: keys.push_back(Key::home); break;
            case VK_END: keys.push_back(Key::end); break;
            case 'F': keys.push_back(Key::follow); break;
            default: break;
            }
        } else if (record.EventType == MOUSE_EVENT && (record.Event.MouseEvent.dwEventFlags & MOUSE_WHEELED)) {
            const auto delta = static_cast<short>(HIWORD(record.Event.MouseEvent.dwButtonState));
            keys.push_back(delta > 0 ? Key::wheel_up : Key::wheel_down);
        }
    }
    return keys;
}

#else

namespace {
constexpr const char* kMouseOn = "\x1B[?1000h\x1B[?1006h\x1B[?25l";    // report buttons and the wheel (SGR), hide the cursor
constexpr const char* kMouseOff = "\x1B[?1006l\x1B[?1000l\x1B[?25h";
} // namespace

struct Console::Input {
    termios saved{};
    std::string pending;   // bytes of a sequence not complete yet
};

void Console::start_input()
{
    if (!interactive_ || input_ || ::isatty(STDIN_FILENO) == 0) return;
    termios saved{};
    if (::tcgetattr(STDIN_FILENO, &saved) != 0) return;
    termios raw = saved;
    raw.c_lflag &= static_cast<tcflag_t>(~(ICANON | ECHO));   // ISIG stays: Ctrl+C still interrupts
    raw.c_cc[VMIN] = 0;
    raw.c_cc[VTIME] = 0;
    if (::tcsetattr(STDIN_FILENO, TCSANOW, &raw) != 0) return;
    input_ = std::make_unique<Input>(Input{saved, {}});
    std::lock_guard lock(mutex_);
    std::fputs(kMouseOn, stdout);
    std::fflush(stdout);
}

void Console::stop_input()
{
    if (!input_) return;
    ::tcsetattr(STDIN_FILENO, TCSANOW, &input_->saved);
    input_.reset();
    std::lock_guard lock(mutex_);
    std::fputs(kMouseOff, stdout);
    std::fflush(stdout);
}

namespace {

// Takes the keys out of `bytes`, leaving an incomplete sequence at its end.
void parse_keys(std::string& bytes, std::vector<Console::Key>& keys, bool more_may_come)
{
    using Key = Console::Key;
    std::size_t i = 0;
    while (i < bytes.size()) {
        const char c = bytes[i];
        if (c != '\x1B') {
            if (c == 'f' || c == 'F') keys.push_back(Key::follow);
            ++i;
            continue;
        }
        if (i + 1 >= bytes.size()) {
            if (more_may_come) break;   // maybe the start of a sequence
            ++i;                        // a lone Esc
            continue;
        }
        const char kind = bytes[i + 1];
        if (kind != '[' && kind != 'O') {
            i += 1;
            continue;
        }
        // ESC [ parameters final, or ESC O final (application cursor keys)
        std::size_t end = i + 2;
        while (end < bytes.size() && !(bytes[end] >= 0x40 && bytes[end] <= 0x7E)) ++end;
        if (end >= bytes.size()) {
            if (more_may_come) break;
            i = end;
            continue;
        }
        const std::string params = bytes.substr(i + 2, end - i - 2);
        const char final = bytes[end];
        if (final == 'A') keys.push_back(Key::up);
        else if (final == 'B') keys.push_back(Key::down);
        else if (final == 'H') keys.push_back(Key::home);
        else if (final == 'F') keys.push_back(Key::end);
        else if (final == '~') {
            if (params == "5") keys.push_back(Key::page_up);
            else if (params == "6") keys.push_back(Key::page_down);
            else if (params == "1" || params == "7") keys.push_back(Key::home);
            else if (params == "4" || params == "8") keys.push_back(Key::end);
        } else if ((final == 'M' || final == 'm') && !params.empty() && params[0] == '<') {
            const int button = std::atoi(params.c_str() + 1) & ~(4 | 8 | 16);   // without Shift, Alt, Ctrl
            if (button == 64) keys.push_back(Key::wheel_up);
            else if (button == 65) keys.push_back(Key::wheel_down);
        }
        i = end + 1;
    }
    bytes.erase(0, i);
}

} // namespace

std::vector<Console::Key> Console::read_keys(std::chrono::milliseconds timeout)
{
    std::vector<Key> keys;
    if (!input_) {
        std::this_thread::sleep_for(timeout);
        return keys;
    }
    pollfd in{STDIN_FILENO, POLLIN, 0};
    if (::poll(&in, 1, static_cast<int>(timeout.count())) <= 0) {
        parse_keys(input_->pending, keys, false);   // a lone Esc, nothing came after it
        return keys;
    }
    char buffer[256];
    const ssize_t got = ::read(STDIN_FILENO, buffer, sizeof buffer);
    if (got > 0) input_->pending.append(buffer, static_cast<std::size_t>(got));
    parse_keys(input_->pending, keys, true);
    return keys;
}

#endif

Console::Console() : interactive_(is_terminal()) {}

Console::~Console()
{
    stop_input();
}

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

std::optional<std::string> Console::ask(std::string_view question)
{
    {
        std::lock_guard lock(mutex_);
        clear_frame_locked();
        std::fprintf(stdout, "%.*s", static_cast<int>(question.size()), question.data());
        std::fflush(stdout);
    }
    std::string answer;
    if (!std::getline(std::cin, answer)) {
        std::fputs("\n", stdout);
        return std::nullopt;
    }
    while (!answer.empty() && (answer.back() == '\r' || answer.back() == ' ')) answer.pop_back();
    while (!answer.empty() && answer.front() == ' ') answer.erase(answer.begin());
    return answer;
}

bool Console::owns_window()
{
#if defined(_WIN32)
    DWORD processes[2];
    return ::GetConsoleProcessList(processes, 2) == 1;   // nobody else (no cmd.exe, no PowerShell) is attached
#else
    return false;   // a terminal stays open after we exit
#endif
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
