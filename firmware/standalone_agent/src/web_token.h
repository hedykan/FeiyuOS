#pragma once

#include <Arduino.h>

// The token the web API asks for (web_token_core.h has the format).  Made on
// first use from the hardware RNG and kept in /vat/secrets.json ("web.token");
// printed once on the USB serial port at boot, so a machine that can flash
// the device can also read it.  Shown with a login QR code in Settings.
namespace vat::web_token {

String token();       // creates and saves one when there is none
String regenerate();  // a new token; the old one stops working at once
bool accepts(const String& presented);

}  // namespace vat::web_token
