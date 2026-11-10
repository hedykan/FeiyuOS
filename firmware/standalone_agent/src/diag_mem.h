#pragma once

#include <Arduino.h>

// Memory report for GET /api/mem: heap totals, every task with its stack
// size and least free stack, the heap's blocks by size class, and each
// block of 512 bytes or more by address and size, so that two reports can
// be compared to find what stayed allocated.  Block contents are never
// read (they may hold keys or tokens).
namespace vat::diag {

String mem_json();

// Heap tracing for finding leaks, only in a build whose IDF libraries have
// CONFIG_HEAP_TRACING_STANDALONE (tools/heaptrace.py; never a release build):
// "start" records every allocation not yet freed (`records` at most, each with
// its call stack), "stop" ends it, "dump" lists what is still allocated.
// Otherwise every action answers that this build has no heap tracing.
String heap_trace(const String& action, std::size_t records);

// Failed allocations: begin() registers a hook that only notes the last one
// (it may run anywhere and must not allocate); report() logs new ones as
// runtime events "alloc_failed" (UI loop).
void begin();
void report();
// Heap-tracing builds only: starts tracing at the top of setup(), so what
// boot leaves allocated can be dumped later ("stop", then "dump").
void trace_from_boot();

}  // namespace vat::diag
