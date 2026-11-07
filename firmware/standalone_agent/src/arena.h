#pragma once

#include <Arduino.h>

#include <cstddef>

// Large blocks of RAM (≥ 4 KB) are taken through here so one place knows who
// holds what and how much is left: the TLS reserve, the recording buffer, the
// IR decode buffer.  Small allocations use malloc as usual.  The bookkeeping
// is arena_ledger (host-tested); this adds the heap readings, a lock and the
// status report.  No memory is allocated here: callers allocate what they
// were granted and give it back when done.
namespace vat::arena {

// Free heap kept for the system and small allocations whatever is lent
// (what the recorder used to keep on its own).
constexpr std::size_t kFloor = 48000;

// Grants `bytes` to `name` (one lease per name) when they fit above the
// floor; `contiguous` also requires one free block that large.  false: not
// granted, an arena_deny event says why.
bool take(const char* name, std::size_t bytes, bool contiguous = false);
// Records memory claimed elsewhere (e.g. the TLS reserve at boot).
void note(const char* name, std::size_t bytes);
void give_back(const char* name);
std::size_t room();  // more that could be granted now
String status_json();  // {"floor","room","held","leases":[{"name","bytes","ms"}]}

// A lease for one scope.
class Hold {
public:
    Hold(const char* name, std::size_t bytes, bool contiguous = false) : name_(name), ok_(take(name, bytes, contiguous)) {}
    ~Hold() {
        if (ok_) give_back(name_);
    }
    Hold(const Hold&) = delete;
    Hold& operator=(const Hold&) = delete;
    bool ok() const { return ok_; }

private:
    const char* name_;
    bool ok_;
};

}  // namespace vat::arena
