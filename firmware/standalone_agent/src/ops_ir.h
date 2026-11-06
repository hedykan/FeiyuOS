#pragma once

#include <Arduino.h>
#include <cJSON.h>

// IR emitter on G44 (transmit only, no learning).  Codes come from a small
// built-in table of common TV protocols; whether one worked is only known to
// the person in front of the appliance, so every send asks and the answer is
// recorded.  Appliance profiles live in /vat/things/ir/<name>.json:
// brand, the code set that works, last action and the verification history.
namespace vat::ir {

constexpr int kPin = 44;

// Sends `action` (power, volume_up, volume_down, mute) to appliance `name`.
// `brand` (samsung, lg, sony, philips) is needed the first time; without it
// the brands are tried one by one, each waiting for a yes/no.
String send(const String& name, const String& action, const String& brand);
// The person's answer to the last send: yes keeps the code, no tries the next.
String confirm(const String& name, bool worked);
// Raw: "NEC 0x04 0x08", "SAMSUNG 0x07 0x02", "SONY 0x01 0x15", "RC5 0x00 0x0C".
String send_raw(const String& spec);
String list_text();
String brands_text();

// Shared with ops_ac (air conditioners): one emitter, one profile directory.
void transmit_pulses(const uint16_t* pulses, std::size_t count, uint8_t khz);  // µs, mark first
String safe_name(String name);
cJSON* load_profile(const String& name);  // an object with "name" when there is none yet
bool save_profile(const String& name, cJSON* profile);
String now_text();

}  // namespace vat::ir
