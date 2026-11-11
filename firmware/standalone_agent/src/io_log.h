#pragma once

#include <Arduino.h>

// The three streams every program run is appended to: its standard input,
// standard output and standard error.  One set for the whole device, not one
// per program: what ran when, what it read, printed and got wrong line up by
// the run's number.  Read with debug:// (packages/debug_package.cpp).
// Design: docs/V072_PLAN.zh-CN.md, D.
namespace vat::iolog {

struct Run {
    String program;
    String origin;         // 前台 / 程序页 / 触发:… / 程序调用
    bool ok = false;
    uint32_t ms = 0;
    std::size_t peak = 0;  // bytes of Lua memory at most
    String in;             // the lines it read
    String out;            // print, io.write
    String err;            // vat.log, io.stderr, warnings, the error it ended with
};

// Appends the run and returns its number (0 when storage is not ready).  The
// output stream gets a head line for every run, so it is also the record of
// what ran; input and error only when there is something.
long record(const Run& run);

// One stream ("stdin" | "stdout" | "stderr"): its newest `count` runs, oldest
// first, as much as fits one tool result.  `program` and `failed` narrow it.
String read(const String& stream, int count, const String& program, bool failed);

// One run in all three streams.
String read_run(long id);

}  // namespace vat::iolog
