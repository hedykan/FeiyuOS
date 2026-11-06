#pragma once

#include <Arduino.h>

// Phone settings: a WPA2 hotspot with a fresh 8-digit password, shown only on
// the device screen (as a Wi-Fi QR code), plus a DNS server that answers
// every name with the device so phones open the settings page by themselves
// (captive portal).  Closes after 10 minutes without a request.  Clients that
// join this hotspot reach the phone page only, never the debug API.
namespace vat::portal {

bool start();   // keeps the station connection; same channel as it
void stop();
bool active();
void loop();    // main loop: DNS answers and the idle timeout
void touch();   // a phone request: restart the idle timer

String ssid();
String password();
int clients();
String wifi_qr();   // WIFI:T:WPA;S:...;P:...;;
String url();       // http://192.168.4.1/m
bool from_portal(const IPAddress& local);  // request arrived on the hotspot

}  // namespace vat::portal
