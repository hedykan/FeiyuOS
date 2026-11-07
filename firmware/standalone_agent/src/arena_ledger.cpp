#include "arena_ledger.h"

#include <algorithm>

namespace vat::arena {

bool Ledger::take(const std::string& name, std::size_t bytes, std::size_t free_heap, std::size_t max_block,
                  bool contiguous, unsigned long now_ms, std::string& why) {
    // A lease being replaced gives its bytes back first.
    const std::size_t returning = held(name);
    const std::size_t available = room(free_heap + returning);
    if (bytes > available) {
        why = name + ": want " + std::to_string(bytes) + ", room " + std::to_string(available);
        return false;
    }
    if (contiguous && bytes > max_block + returning) {
        why = name + ": want " + std::to_string(bytes) + " contiguous, largest block " + std::to_string(max_block);
        return false;
    }
    note(name, bytes, now_ms);
    return true;
}

void Ledger::note(const std::string& name, std::size_t bytes, unsigned long now_ms) {
    give_back(name);
    leases_.push_back({name, bytes, now_ms});
}

void Ledger::give_back(const std::string& name) {
    leases_.erase(std::remove_if(leases_.begin(), leases_.end(), [&](const Lease& l) { return l.name == name; }),
                  leases_.end());
}

std::size_t Ledger::held(const std::string& name) const {
    for (const auto& l : leases_) {
        if (l.name == name) return l.bytes;
    }
    return 0;
}

std::size_t Ledger::total() const {
    std::size_t sum = 0;
    for (const auto& l : leases_) sum += l.bytes;
    return sum;
}

}  // namespace vat::arena
