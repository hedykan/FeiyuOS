#pragma once

#include <Arduino.h>

// Memory report for GET /api/mem: heap totals, every task with its stack
// size and least free stack, the heap's blocks by size class, and each
// block of 512 bytes or more by address and size, so that two reports can
// be compared to find what stayed allocated.  Block contents are never
// read (they may hold keys or tokens).
namespace vat::diag {

String mem_json();

}  // namespace vat::diag
