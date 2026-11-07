// Host tests for the arena bookkeeping (arena_ledger.cpp).  Included by
// host_test.cpp; uses its CHECK macro.
#include "arena_ledger.h"

namespace {

void arena_ledger() {
    using vat::arena::Ledger;
    Ledger ledger(48000);
    std::string why;
    CHECK(ledger.room(40000) == 0);
    CHECK(ledger.room(100000) == 52000);

    CHECK(ledger.take("recording", 50000, 100000, 60000, false, 10, why));
    CHECK(ledger.held("recording") == 50000 && ledger.total() == 50000);
    // Once the recorder allocates, free heap drops; room follows the heap.
    CHECK(ledger.room(50000) == 2000);

    CHECK(!ledger.take("ir_decode", 4096, 50000, 50000, false, 20, why));  // only 2000 left
    CHECK(why.find("ir_decode") == 0 && ledger.held("ir_decode") == 0);

    // Asking again under the same name replaces the lease (its bytes count as free).
    CHECK(ledger.take("recording", 30000, 50000, 40000, false, 30, why));
    CHECK(ledger.held("recording") == 30000 && ledger.leases().size() == 1);

    // Contiguous: total room is not enough without one block that large.
    CHECK(!ledger.take("tls", 17920, 120000, 16000, true, 40, why));
    CHECK(why.find("contiguous") != std::string::npos);
    CHECK(ledger.take("tls", 17920, 120000, 20000, true, 40, why));

    ledger.note("tls_reserve", 35840, 50);
    CHECK(ledger.total() == 30000 + 17920 + 35840);
    ledger.give_back("recording");
    ledger.give_back("tls");
    ledger.give_back("nothing");  // giving back an unknown name is harmless
    CHECK(ledger.total() == 35840 && ledger.leases().size() == 1 && ledger.leases()[0].since_ms == 50);
}

}  // namespace
