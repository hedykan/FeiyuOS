#pragma once

#include <Arduino.h>

// Push-to-talk recording into /vat/cache/ptt.pcm (16 kHz mono PCM16) while
// held() is true, with a level meter strip at the bottom of the screen.
namespace vat::ui::recorder {

// True when a usable recording was written; otherwise a notice says why.
bool record(bool (*held)());
// Diagnostics: {"chunks":N,"peak":P} over 20 microphone chunks.
String mic_test();

}  // namespace vat::ui::recorder
