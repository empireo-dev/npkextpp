#pragma once

#include <ftxui/dom/elements.hpp>

#include <chrono>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

namespace npkextpp {

// The terminal. Permanent lines go to stdout as they come; below them, a live
// frame (FTXUI elements) can be redrawn in place many times per second. A line
// printed while a frame is shown goes above it. Thread-safe.
//
// Not a terminal (redirected to a file, TERM=dumb): no live frame, and
// everything is printed without colours.
class Console {
public:
    // What the user pressed (or turned) while reading input.
    enum class Key { up, down, page_up, page_down, home, end, wheel_up, wheel_down, follow };

    Console();
    ~Console();
    Console(const Console&) = delete;
    Console& operator=(const Console&) = delete;

    void line(std::string_view text);           // a permanent line
    void print(const ftxui::Element& element);  // a permanent block, as wide as it needs (at most the terminal)
    void error(std::string_view text);          // "error: ..." on stderr

    // Replaces the live frame (cut to the terminal's height). Does nothing when
    // not a terminal.
    void draw(const ftxui::Element& frame);
    // Removes the live frame (the next line or print goes where it was).
    void clear_frame();

    // Keys and the mouse wheel, while the live frame is shown (a terminal only):
    // keys are no longer echoed, the wheel is reported instead of scrolling the
    // terminal (Shift + drag still selects text), Ctrl+C still interrupts.
    void start_input();
    void stop_input();   // as it was before; also done on destruction
    // Waits up to `timeout` for input and returns what came (sleeps when not reading).
    [[nodiscard]] std::vector<Key> read_keys(std::chrono::milliseconds timeout);

    [[nodiscard]] bool interactive() const noexcept { return interactive_; }
    [[nodiscard]] int width() const;    // of the terminal (100 when not a terminal)
    [[nodiscard]] int height() const;   // of the terminal (0 when not a terminal)

private:
    void clear_frame_locked();
    [[nodiscard]] std::string render(const ftxui::Element& element, int width, int height) const;

    struct Input;
    std::unique_ptr<Input> input_;
    std::mutex mutex_;
    bool interactive_ = false;
    std::string frame_;   // the live frame on screen, as printed
    std::string reset_;   // moves back to its first line, clearing it
};

[[nodiscard]] std::string format_bytes(std::uint64_t bytes);
[[nodiscard]] std::string format_count(std::uint64_t count);   // 1,234,567
[[nodiscard]] std::string format_duration(double seconds);     // 4.2 s, 3 min 05 s, 1 h 02 min

} // namespace npkextpp
