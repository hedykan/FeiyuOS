#pragma once

#include <cstdint>
#include <vector>

#include "common.h"

// Air conditioners over IR, the device-independent part: the state the
// remote holds, the words that change it, the brand index of the packed code
// database (tools/pack_irdb.py) and encoding a state into a pulse train with
// the IRext decoder (third_party/irext).  An AC remote always sends the whole
// state, so the device keeps it and every command sends all of it.  Builds on
// the host (tests/host); reading the card and transmitting are in ops_ac.
namespace vat::ac {

enum Mode : uint8_t { kCool = 0, kHeat, kAuto, kFan, kDry, kModeCount };   // IRext t_ac_mode
enum Fan : uint8_t { kFanAuto = 0, kFanLow, kFanMid, kFanHigh, kFanCount };  // IRext t_ac_wind_speed
constexpr int kTempMin = 16;
constexpr int kTempMax = 30;

struct State {
    bool on = true;
    uint8_t mode = kCool;
    int temp = 26;
    uint8_t fan = kFanAuto;
};

const char* mode_label(uint8_t mode);  // 制冷 …
const char* fan_label(uint8_t fan);    // 自动风 …
String describe(const State& state);   // "开，制冷 26°C，自动风"

// Applies `words` ("开 制冷 26", "temp=+1", "关", "奥克斯 off" …) to `state`.
// A word that is none of power / mode / temperature / fan is taken as the
// brand (only one is allowed).  Changing mode, temperature or fan without a
// power word also turns the unit on.  false: `error` says which word failed.
bool apply(const String& words, State& state, String& brand, String& error);

// The remote key that goes with a change (some units put it in the frame).
uint8_t key_for(const State& before, const State& after);

void to_json(const State& state, cJSON* object);
State from_json(const cJSON* object);  // missing fields keep their defaults

// One line of the packed index: "<brand>|<brand_en>|<id>@<offset>+<length>,…".
struct Remote {
    String id;
    uint32_t offset = 0;
    uint32_t length = 0;
};
struct Brand {
    String name;
    String name_en;
    std::vector<Remote> remotes;
};
bool parse_brand(const String& line, Brand& out);
// The person's words name this brand: either contains the other (Chinese),
// or the English name, case and spaces ignored.
bool brand_matches(const Brand& brand, const String& words);
// "IRAC1 <index bytes>": the index length, -1 for anything else.
long index_length(const String& header_line);

// Encodes `state` with one remote's code file.  The pulse train (µs, mark
// first) goes to `out`.  The mode must be one the remote has; a temperature
// outside its range for that mode is clamped (state.temp is updated); a fan
// speed it lacks falls back to auto.  false: `error` says why.
bool encode(const std::vector<uint8_t>& code, State& state, uint8_t key, std::vector<uint16_t>& out, String& error);

}  // namespace vat::ac
