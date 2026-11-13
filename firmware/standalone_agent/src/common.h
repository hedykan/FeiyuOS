#pragma once

#include <Arduino.h>
#include <cJSON.h>

#include <cstdint>
#include <vector>

namespace vat {

// UTF-8 safe truncation helpers.  Display and model buffers are bounded, so
// every long string is cut on a code-point boundary.
String utf8_head(const String& text, std::size_t max_bytes);
String utf8_tail(const String& text, std::size_t max_bytes);
// Valid UTF-8 for anything that goes into a JSON request: invalid or cut
// sequences (binary output, a byte cap in the middle of a character) and NUL
// bytes (which end a C string: a binary file read as empty) become '?'.  The API rejects the whole request otherwise (HTTP 400).
String utf8_sanitize(const String& text);

// cJSON convenience accessors (missing or wrong-typed values return fallback).
String json_str(const cJSON* object, const char* key, const String& fallback = "");

// Frees a String's buffer.  Assigning "" or String() does not: the ESP32 core
// keeps a buffer that is large enough and only sets the length to 0.
void release(String& text);
// Zeroes the text, then frees it: for passwords and keys, whose bytes would
// otherwise stay in the heap until something else overwrote them.
void wipe(String& text);
long json_int(const cJSON* object, const char* key, long fallback);
bool json_bool(const cJSON* object, const char* key, bool fallback);
cJSON* json_obj(const cJSON* object, const char* key);  // objects only: nullptr for an array
cJSON* json_arr(const cJSON* object, const char* key);  // arrays only
String json_print(const cJSON* value);

// POSIX single-quote escaping.  This is the only way remote commands are
// assembled, so argv values can never break out of their argument.
// Targets with a query: "memory://out/37?page=2&grep=x".  target_path() is
// the part before '?' without a trailing '/'; target_param() one value
// ("" when absent).  A key listed last may contain '&' (e.g. a url).
String target_path(const String& target);
String target_param(const String& target, const char* key, bool rest_of_query = false);

// %XX escapes in a target path ("ir://%E5%AE%A2%E5%8E%85/power").
String url_decode(const String& text);

String shell_quote(const String& value);
String shell_join(const std::vector<String>& argv);

// Why the chip last reset: poweron, software, panic, int_wdt, task_wdt, brownout...
String reset_reason();

// Append one structured line to /vat/logs/runtime.jsonl.
void runtime_event(const char* event, const String& detail = "");

// The lowest free heap since boot.  ESP-IDF's own low-water mark is reset each
// time the worker starts a per-step monitor, so whoever resets it calls
// heap_low_fold() first, and readers use heap_low_since_boot().
void heap_low_fold();
std::size_t heap_low_since_boot();

}  // namespace vat

namespace vat::cancel {

// Cooperative cancellation for the single worker.  The UI task calls
// request(); network code registers its socket so a blocking read is
// unblocked by shutdown() instead of waiting for a timeout.
void reset();
void request();
bool requested();
void set_socket(int fd);
void clear_socket();

}  // namespace vat::cancel
