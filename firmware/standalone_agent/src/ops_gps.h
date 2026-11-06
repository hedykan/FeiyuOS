#pragma once

#include <Arduino.h>

// GNSS on the Cap LoRa-1262: ATGM336H-6N (GPS/BeiDou/Galileo/GLONASS/QZSS),
// UART 115200 8N1 on G15 (module TX -> device RX) and G13 (device TX).  It
// shares nothing with the SPI bus or I2C.  The cap gives software no way to
// cut its power: "off" only stops reading the UART.
namespace vat::gps {

constexpr int kRx = 15;
constexpr int kTx = 13;

String begin();  // after config; returns "" or why it is off
void loop();     // UI task: feeds pending NMEA bytes to the parser
String status_text();
// Current fix as decimal degrees (WGS-84); false when there is no recent fix.
bool location(double& lat, double& lon);
// Reverse geocoding of the current fix (OpenStreetMap Nominatim): road,
// district, city and the nearest named place.  Sends the coordinates to
// nominatim.openstreetmap.org, so it runs only when asked; cached 60 s.
String address_text();
String enable(bool on);

}  // namespace vat::gps
