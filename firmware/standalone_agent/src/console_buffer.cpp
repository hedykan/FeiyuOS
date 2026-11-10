#include "console_buffer.h"

namespace vat::console {

void Buffer::clear() {
    lines_.clear();
    open_ = restart_ = false;
    bytes_ = 0;
}

void Buffer::put(char c, bool error) {
    if (c == '\n') {
        if (!open_) lines_.push_back(Line{String(), error});
        open_ = restart_ = false;
        return;
    }
    if (c == '\r') {
        restart_ = true;
        return;
    }
    if (!open_) {
        lines_.push_back(Line{String(), error});
        open_ = true;
    }
    Line& line = lines_.back();
    if (restart_) {
        bytes_ -= line.text.length();
        line.text = String();
        restart_ = false;
    }
    if (error) line.error = true;
    if (line.text.length() >= kMaxLine) return;
    line.text += c;
    ++bytes_;
}

void Buffer::trim() {
    std::size_t drop = 0;
    while (bytes_ > limit_ && drop + 1 < lines_.size()) bytes_ -= lines_[drop++].text.length();
    if (drop) lines_.erase(lines_.begin(), lines_.begin() + drop);
}

void Buffer::write(const String& text, bool error) {
    for (std::size_t i = 0; i < text.length(); ++i) put(text[i], error);
    trim();
}

}  // namespace vat::console
