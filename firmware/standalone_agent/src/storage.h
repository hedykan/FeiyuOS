#pragma once

#include <Arduino.h>
#include <FS.h>

#include <vector>

namespace vat::storage {

// Mounts TF first and falls back to on-board LittleFS.  One filesystem is used
// for the whole /vat tree; the two are never mixed.
bool begin();
bool ready();
fs::FS& fs();
String kind();            // "tf" | "flash" | "none"
uint64_t total_bytes();
uint64_t used_bytes();

bool exists(const String& path);
bool ensure_parent(const String& path);
String read_text(const String& path, std::size_t max_bytes = 32768);
// Last max_bytes of a file, starting on a line boundary.
String read_tail(const String& path, std::size_t max_bytes);
bool write_text(const String& path, const String& content);  // atomic
// In place (one open/write/close, no temp + rename): only for disposable
// caches whose reader can detect a torn file.  Not mirrored to flash.
bool overwrite(const String& path, const String& content);
bool append_line(const String& path, const String& line);    // rotates
bool append(const String& path, const String& text);         // no rotation (context, outputs, archive)
// Write-behind for append-only logs: kept in RAM up to 5 s / 2 KB, written in
// one go.  Reads of the same file (or of a directory above it) flush first;
// call flush() before a restart and before reading such files directly.
bool append_buffered(const String& path, const String& text, bool rotate);
void flush();
void tick();  // main loop: writes buffers older than 5 s
std::size_t size(const String& path);                        // 0 when missing
String read_range(const String& path, std::size_t offset, std::size_t max_bytes);
// [offset, offset + length) of a binary file into `out`; false when the file is shorter.
bool read_range_bytes(const String& path, std::size_t offset, std::size_t length, std::vector<uint8_t>& out);
// Whole binary file into `out`; false when missing or larger than max_bytes.
bool read_bytes(const String& path, std::vector<uint8_t>& out, std::size_t max_bytes);
bool rename(const String& from, const String& to);           // replaces `to`
bool remove(const String& path);
// file:///vat/x, /vat/x or x -> /vat/x (no trailing slash).
String vat_path(const String& target);
// Why a tool may not read / write `path` (keys, SSH, system files), "" when it may.
String refused_path(const String& path, bool writing);
// A file was written outside these functions (e.g. a multipart upload).
void changed(const String& path);
// Main loop, only while the worker is idle (remounting under a running job
// would pull the filesystem from under it).  Probes /vat every 60 s, or at
// once after a slow operation; remounts when the card stopped answering.
// Returns "" (fine), "remounted", or "lost" (retried every 30 s).
String check_card();
uint32_t card_hz();  // SPI clock the card is mounted at (0 when not on a card)
// No card: booted from the flash mirror (Wi-Fi, keys, hosts, device key,
// notes).  The long context, outputs, archive and cases are unavailable.
bool degraded();
// Storage calls that took over a second since boot: "count/max ms".
String slow_report();
// Copies the rest of `from` into `to`; false when a write falls short.
bool copy_stream(File& from, File& to);
bool remove_tree(const String& path);
String list_dir(const String& path);  // JSON array of {name,size,dir}

}  // namespace vat::storage
