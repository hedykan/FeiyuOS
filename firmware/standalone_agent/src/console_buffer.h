#pragma once

#include <vector>

#include "common.h"

// The program page's text: what a program printed, as lines.  "\n" ends a
// line, "\r" starts the current one over (a progress line), at most `limit`
// bytes are kept (the oldest lines go first).  Host-tested.
namespace vat::console {

struct Line {
    String text;
    bool error = false;  // stderr / the run's error: drawn in the warning colour
};

class Buffer {
public:
    explicit Buffer(std::size_t limit = 4096) : limit_(limit) {}
    void write(const String& text, bool error = false);
    void clear();
    // Finished lines, then the open one (if it has text).
    const std::vector<Line>& lines() const { return lines_; }
    std::size_t bytes() const { return bytes_; }

private:
    void put(char c, bool error);
    void trim();

    std::vector<Line> lines_;
    bool open_ = false;      // the last line takes more text
    bool restart_ = false;   // "\r" seen: the next character starts the line over
    std::size_t bytes_ = 0;
    std::size_t limit_;
};

constexpr std::size_t kMaxLine = 400;  // bytes of one line (longer: cut)

}  // namespace vat::console
