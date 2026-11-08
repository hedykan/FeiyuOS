#pragma once

#include <Arduino.h>

// Bluetooth LE through NimBLE-Arduino (the IDF libraries carry only the
// controller, see docs/V061_PLAN.zh-CN.md §六).  All four roles:
//   observer     scan for advertisements, a fixed table of the devices heard
//   broadcaster  advertise a name, manufacturer data or an iBeacon
//   central      connect to a device, read / write its characteristics,
//                subscribe to notifications (taken as streams)
//   peripheral   a BLE keyboard (HID) that types into a paired computer
//
// Off unless turned on in the settings, and only at boot: the stack takes
// its memory (controller, host task, buffers) before anything else, while the
// heap is still in one piece, and is released by restarting.  Calls that take
// time (scan, connect, GATT) block the calling task (the worker); the UI task
// only drives the scan and the advertising in loop().
namespace vat::ble {

String begin();    // boot, after config; "" when running, else why it is off
void loop();       // UI task
bool running();
bool enabled();    // the setting (takes effect after a restart)
String set_enabled(bool on);
String status_text();

// observer
String scan(uint32_t seconds);  // listens `seconds`, then lists what was heard
String devices_text();          // the table, strongest first, without scanning
String device_text(const String& address);
String scan_log(uint32_t seconds);  // every advertisement into /vat/ble/scan-*.jsonl; 0 stops
// Streams (UI task, no waiting): the strongest signal and how many devices
// were heard in the last 10 s; one device's latest RSSI.
void want_scan(bool on);  // a stream reader keeps the radio listening
bool nearby(int& count, int& strongest);
bool rssi(const String& address, int& dbm);

// broadcaster: JSON {"name":…, "data":"hex", "ibeacon":{"uuid","major","minor"}, "seconds":N}
String advertise(const String& spec);
String advertise_stop();

// central
String connect(const String& address);
String disconnect(const String& address);
String services(const String& address);
String gatt_read(const String& address, const String& service, const String& characteristic);
// `value`: text, or "hex:0102…" for bytes
String gatt_write(const String& address, const String& service, const String& characteristic, const String& value);
String subscribe(const String& address, const String& service, const String& characteristic, bool on);
// The latest notification of a subscribed characteristic ("address/service/characteristic").
String notify_text(const String& key);
bool notified(const String& key, uint8_t* bytes, size_t capacity, size_t& length);

// peripheral (keyboard)
String hid_pair(uint32_t seconds);  // accept one new computer for `seconds`
String hid_type(const String& text);
String hid_key(const String& combo);  // "enter", "tab", "cmd+space", "ctrl+c"…
String hid_forget();                  // drop every paired computer
String hid_status();

}  // namespace vat::ble
