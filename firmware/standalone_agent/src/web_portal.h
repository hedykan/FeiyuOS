#pragma once

#include <WebServer.h>

// Phone settings page (/m) and its JSON API, served on the phone-settings
// hotspot (see ops_portal) and on the LAN.  Keys are write-only: the page sees
// the last four characters, never a whole key.
//   GET  /m                    the page
//   GET  /m/api/state          keys (hints), hosts, device public key, LoRa, font
//   POST /m/api/key            provider, key
//   POST /m/api/host_add       id, host, user, port
//   POST /m/api/host_delete    id
//   POST /m/api/lora           key (enabled|frequency|power|antenna), value
//   POST /m/api/font           size (10|12|14)
namespace vat::web_portal {

void register_routes(WebServer& server);

}  // namespace vat::web_portal
