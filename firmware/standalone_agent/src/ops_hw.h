#pragma once

#include <Arduino.h>

// On-board hardware of the Cardputer Adv beyond keyboard, screen and audio
// input: battery/charging, the RGB LED (G21), the BMI270 IMU (0x69) and the
// speaker (ES8311, sharing I2S with the microphone).
namespace vat::hw {

void loop();  // UI task: samples the battery voltage for the charging trend

// The board has no charge-status line (battery on an ADC only), so charging
// is inferred from USB host presence and the voltage trend, and says so.
String battery_text();
int battery_mv();
bool usb_connected();

// "red", "green", "#ff8800", "off"... ; returns the text shown to the user.
String set_led(const String& color);
String led_text();

String imu_text();  // orientation, tilt and whether the device is moving

// Speaker: the UI task plays queued beeps between recordings.
String request_beep(int count);

// LED and beep targets, aliases included (shared by the package and policy).
bool is_led_target(String target);
bool is_beep_target(const String& target);
void play_pending_beeps();  // UI task only

}  // namespace vat::hw
