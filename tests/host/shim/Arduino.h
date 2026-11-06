#pragma once

// Host builds only (tests/host): the slice of Arduino's String that the
// portable firmware sources use, over std::string.  Add a method here when a
// newly tested source needs one; keep semantics identical to Arduino's.
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

class String {
public:
    String() = default;
    String(const char* text) : s_(text ? text : "") {}
    String(const std::string& text) : s_(text) {}
    explicit String(char c) : s_(1, c) {}
    String(int v) : s_(std::to_string(v)) {}
    String(unsigned v) : s_(std::to_string(v)) {}
    String(long v) : s_(std::to_string(v)) {}
    String(unsigned long v) : s_(std::to_string(v)) {}
    String(long long v) : s_(std::to_string(v)) {}
    String(unsigned long long v) : s_(std::to_string(v)) {}
    String(double v, unsigned decimals = 2) {
        char buffer[64];
        std::snprintf(buffer, sizeof buffer, "%.*f", static_cast<int>(decimals), v);
        s_ = buffer;
    }
    String(float v, unsigned decimals = 2) : String(static_cast<double>(v), decimals) {}

    std::size_t length() const { return s_.size(); }
    bool isEmpty() const { return s_.empty(); }
    const char* c_str() const { return s_.c_str(); }
    char operator[](std::size_t i) const { return i < s_.size() ? s_[i] : '\0'; }
    void reserve(std::size_t n) { s_.reserve(n); }
    bool concat(const char* text, std::size_t n) {
        s_.append(text, n);
        return true;
    }

    String& operator+=(const String& other) {
        s_ += other.s_;
        return *this;
    }
    String& operator+=(const char* text) {
        if (text) s_ += text;
        return *this;
    }
    String& operator+=(char c) {
        s_ += c;
        return *this;
    }

    int indexOf(char c, std::size_t from = 0) const { return pos(s_.find(c, from)); }
    int indexOf(const String& part, std::size_t from = 0) const { return pos(s_.find(part.s_, from)); }
    String substring(std::size_t from) const { return from >= s_.size() ? String() : String(s_.substr(from)); }
    String substring(std::size_t from, std::size_t to) const {
        if (from > to) std::swap(from, to);
        if (from >= s_.size()) return String();
        return String(s_.substr(from, std::min(to, s_.size()) - from));
    }
    bool startsWith(const String& prefix) const {
        return s_.size() >= prefix.s_.size() && s_.compare(0, prefix.s_.size(), prefix.s_) == 0;
    }
    bool endsWith(const String& suffix) const {
        return s_.size() >= suffix.s_.size() &&
               s_.compare(s_.size() - suffix.s_.size(), suffix.s_.size(), suffix.s_) == 0;
    }
    void remove(std::size_t index) {
        if (index < s_.size()) s_.erase(index);
    }
    void remove(std::size_t index, std::size_t count) {
        if (index < s_.size()) s_.erase(index, count);
    }
    void trim() {
        const auto space = [](unsigned char c) { return std::isspace(c) != 0; };
        while (!s_.empty() && space(s_.back())) s_.pop_back();
        std::size_t start = 0;
        while (start < s_.size() && space(s_[start])) ++start;
        s_.erase(0, start);
    }
    void toLowerCase() {
        for (auto& c : s_) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    long toInt() const { return std::strtol(s_.c_str(), nullptr, 10); }
    double toDouble() const { return std::strtod(s_.c_str(), nullptr); }

    bool operator==(const String& other) const { return s_ == other.s_; }
    bool operator!=(const String& other) const { return s_ != other.s_; }
    bool operator==(const char* text) const { return s_ == (text ? text : ""); }
    bool operator!=(const char* text) const { return !(*this == text); }

private:
    static int pos(std::size_t p) { return p == std::string::npos ? -1 : static_cast<int>(p); }
    std::string s_;
};

inline String operator+(String a, const String& b) { return a += b; }
inline String operator+(String a, const char* b) { return a += b; }
inline String operator+(const char* a, const String& b) { return String(a) += b; }
inline String operator+(String a, char c) { return a += c; }
