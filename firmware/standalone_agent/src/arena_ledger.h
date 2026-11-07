#pragma once

#include <cstddef>
#include <string>
#include <vector>

// The bookkeeping behind arena (arena.h): who holds which large block, and
// how much more may be lent while keeping a floor of free heap for the system
// and small allocations.  No device calls, so it builds on the host.
namespace vat::arena {

struct Lease {
    std::string name;
    std::size_t bytes = 0;
    unsigned long since_ms = 0;
};

class Ledger {
public:
    explicit Ledger(std::size_t floor_bytes) : floor_(floor_bytes) {}

    // What may still be lent given `free_heap`: the heap above the floor.
    // Bytes already lent are no longer in free_heap, so they are not counted twice.
    std::size_t room(std::size_t free_heap) const { return free_heap > floor_ ? free_heap - floor_ : 0; }

    // Records `bytes` for `name` when that much is available (and, for one
    // contiguous block, when `max_block` holds it).  A name holds one lease;
    // asking again replaces it.  false: nothing recorded, `why` says why.
    bool take(const std::string& name, std::size_t bytes, std::size_t free_heap, std::size_t max_block,
              bool contiguous, unsigned long now_ms, std::string& why);
    // Records a lease that already exists (claimed outside take, e.g. at boot).
    void note(const std::string& name, std::size_t bytes, unsigned long now_ms);
    void give_back(const std::string& name);

    std::size_t held(const std::string& name) const;
    std::size_t total() const;
    const std::vector<Lease>& leases() const { return leases_; }
    std::size_t floor() const { return floor_; }

private:
    std::size_t floor_;
    std::vector<Lease> leases_;
};

}  // namespace vat::arena
