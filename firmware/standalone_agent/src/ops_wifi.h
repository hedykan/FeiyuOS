#pragma once

#include <Arduino.h>

#include <vector>

// Wi-Fi networks for the settings page, the wifi package and boot.  Joining
// always connects first and saves only on success: a mistyped password never
// replaces a working one (it once did, and the device lost its network).
namespace vat::wifi {

struct Network {
    String ssid;
    int rssi = 0;
    bool secured = true;
};

// Visible networks, strongest first, one entry per name, at most `max`.
// Blocks for the scan (about 2-4 s).
std::vector<Network> scan(std::size_t max = 12);
// The last scan did not run (the radio was busy reconnecting), as opposed to
// finding nothing.  Only 2.4 GHz networks are ever visible.
bool last_scan_failed();
// The same as numbered text: "2. CMCC-mstU  信号中  加密  当前连接".
String scan_text();
String signal_word(int rssi);  // 强 / 中 / 弱

// Whether `ssid` needs a password, from the last scan (scans once if unknown).
bool secured(const String& ssid);
// A saved network by name or SSID (the primary one included).
bool saved(const String& name);

// Connects to `ssid`; on success saves it (primary for the next boot and in
// the saved list), on failure reconnects the previous network and saves
// nothing.  Returns the text shown to the user.
String join(const String& ssid, const String& password, bool& ok);
String join_saved(const String& name);  // a saved network, with its stored password

// Boot: the primary network, then every other saved one.
bool connect_at_boot(uint32_t timeout_ms);

}  // namespace vat::wifi
