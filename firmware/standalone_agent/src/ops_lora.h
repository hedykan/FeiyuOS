#pragma once

#include <Arduino.h>

// Cap LoRa-1262 (SX1262 on the TF card's SCK/MOSI/MISO, RF switch behind a
// PI4IOE5V6408 at I2C 0x43).  See docs/TECHNICAL_RESEARCH.zh-CN.md.
//
// Off by default: the SX1262 is held in reset (all its pins high-Z) so it
// cannot disturb the shared SPI bus.  Receiving needs `enabled` and a
// frequency; transmitting also needs the antenna confirmed by the user.
namespace vat::lora {

// Pins on the Cardputer Adv (official Cap LoRa-1262 mapping).
constexpr int kNss = 5;
constexpr int kReset = 3;
constexpr int kIrq = 4;   // DIO1
constexpr int kBusy = 6;

// Before the TF card is mounted: chip select high, radio held in reset.
void hold_off_bus();

// After storage and config: start receiving when enabled, else stay off.
// Returns "" or why the radio is off.
String begin();
void loop();  // UI task: collects received packets

bool cap_present();
String status_text();
String messages_text();
String send(const String& text);                       // from the worker, after confirmation
String set(const String& key, const String& value);    // enabled|frequency|power|sf|bandwidth|cr|antenna
String enable(bool on);

}  // namespace vat::lora
