#pragma once

#include <Arduino.h>
#include <FS.h>

#include <vector>

namespace vat::net {

// Two large blocks claimed while the heap is still unfragmented.  They are
// handed back for the duration of a TLS connection or an SSH session (the
// stages never overlap) so mbedTLS and libssh always find contiguous memory.
// The TLS reserve while no TLS connection is up (the boot): block 0 (receive
// buffer size) or 1 (send buffer size), allocated if needed.  Lent to the
// boot screen, which hands it back before the first connection.
void* reserve_block(int index, std::size_t& bytes);
void end_lending();  // the boot screen is done with the reserve
// Frees / reclaims the reserve blocks nobody is using (SSH, recording).
void release_reserve();
void restore_reserve();
// mbedTLS allocations served from the reserve, and large ones that were not.
void reserve_stats(uint32_t& hits, uint32_t& misses);

enum class WifiState { Off, Connecting, Connected, SetupAp };

// Station mode with the configured credentials; falls back to a setup
// access point named VAT-XXXX when no credentials exist or joining fails.
bool connect_station(const String& ssid, const String& password, uint32_t timeout_ms);
void start_setup_ap();
WifiState state();
String ip();
String ssid();
int rssi();
String ap_name();
// The access point the station is on: "bssid=aa:bb:cc:dd:ee:ff ch=2 rssi=-40"
// ("" when not connected).  Several routers or mesh nodes may share the SSID.
String link_text();
String bssid();
int channel();
// Every access point with the station's SSID (a blocking scan, ~2 s):
// "aa:bb:cc:dd:ee:ff ch=2 rssi=-40*" (* = the one in use), space separated.
// The access points carrying this SSID (mesh nodes, a second router):
// scan_nodes() starts an asynchronous scan, nodes_scanned() is true once it is
// done and fills `out` ("<bssid> ch=7 rssi=-21*", * = the one in use).
void scan_nodes();
bool nodes_scanned(String& out);
// Main loop: logs the station's link changes (wifi_link / wifi_lost) noted by
// the Wi-Fi event handler, which runs on a small task and must not write the card.
void report_link_changes();

struct HttpResult {
    int status = 0;
    String body;
    String error;
    bool ok() const { return status >= 200 && status < 300; }
};

// One piece of a streamed request body: `length` bytes from memory, or, when
// `path` is set, `length` bytes of that file starting at `offset` (read in
// small blocks while sending, never loaded whole).  A file that turned out
// shorter is padded with spaces, which JSON ignores between elements.
struct Segment {
    const char* data;
    std::size_t length;
    const char* path = nullptr;
    std::size_t offset = 0;
};
// POST a JSON body made of consecutive segments (flash constants, Strings
// owned by the caller, or file ranges) without assembling it in RAM.
HttpResult post_segments(const String& url, const String& api_key, const std::vector<Segment>& parts,
                         uint32_t timeout_ms, int attempts = 2);

// POST JSON and write the response body to `save_path` instead of memory
// (result.body stays empty); for responses that may be large.
HttpResult post_json_to_file(const String& url, const String& api_key, const String& body,
                             const String& save_path, uint32_t timeout_ms, int attempts = 2);

HttpResult get(const String& url, const String& api_key, uint32_t timeout_ms);

// A request of the caller's own (the http package): http:// or https://, any
// method, the headers as given (a body without Content-Type is sent as JSON
// when it looks like JSON, else as text).  The response body is kept up to
// max_body bytes (error "response too large" past it), or a 2xx one is
// written to save_path.  No retries: the method may not be idempotent.
struct Request {
    String method = "GET";
    String url;
    std::vector<std::pair<String, String>> headers;
    String body;
    String save_path;
    std::size_t max_body = 16384;
    uint32_t timeout_ms = 20000;
};
HttpResult request(const Request& request);

// POST a body produced as: prefix + base64(wav_header + pcm_file) + suffix.
// The PCM never has to be duplicated on storage or in RAM.
HttpResult post_pcm_base64(const String& url, const String& api_key,
                           const String& prefix, const String& pcm_path,
                           const String& suffix, uint32_t timeout_ms, int attempts = 2);

}  // namespace vat::net
