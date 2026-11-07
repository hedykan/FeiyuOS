#include "arena.h"

#include <esp_heap_caps.h>

#include <mutex>

#include "arena_ledger.h"
#include "common.h"

namespace vat::arena {
namespace {

std::mutex lock;
Ledger ledger(kFloor);

std::size_t free_heap() { return heap_caps_get_free_size(MALLOC_CAP_8BIT | MALLOC_CAP_INTERNAL); }
std::size_t max_block() { return heap_caps_get_largest_free_block(MALLOC_CAP_8BIT | MALLOC_CAP_INTERNAL); }

}  // namespace

bool take(const char* name, std::size_t bytes, bool contiguous) {
    std::string why;
    bool ok;
    {
        std::lock_guard<std::mutex> guard(lock);
        ok = ledger.take(name, bytes, free_heap(), max_block(), contiguous, millis(), why);
    }
    if (!ok) runtime_event("arena_deny", why.c_str());
    return ok;
}

void note(const char* name, std::size_t bytes) {
    std::lock_guard<std::mutex> guard(lock);
    ledger.note(name, bytes, millis());
}

void give_back(const char* name) {
    std::lock_guard<std::mutex> guard(lock);
    ledger.give_back(name);
}

std::size_t room() {
    std::lock_guard<std::mutex> guard(lock);
    return ledger.room(free_heap());
}

String status_json() {
    std::lock_guard<std::mutex> guard(lock);
    String out = "{\"floor\":" + String(static_cast<unsigned>(ledger.floor())) +
                 ",\"room\":" + String(static_cast<unsigned>(ledger.room(free_heap()))) +
                 ",\"held\":" + String(static_cast<unsigned>(ledger.total())) + ",\"leases\":[";
    const unsigned long now = millis();
    bool first = true;
    for (const auto& l : ledger.leases()) {
        out += String(first ? "" : ",") + "{\"name\":\"" + l.name.c_str() + "\",\"bytes\":" +
               String(static_cast<unsigned>(l.bytes)) + ",\"ms\":" + String(now - l.since_ms) + "}";
        first = false;
    }
    return out + "]}";
}

}  // namespace vat::arena
