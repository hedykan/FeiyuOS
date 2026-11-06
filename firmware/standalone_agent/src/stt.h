#pragma once

#include <Arduino.h>

namespace vat::stt {

struct Transcript {
    bool ok = false;
    String text;   // transcript on success, error text otherwise
    uint32_t elapsed_ms = 0;
};

// Transcribe raw PCM16 mono 16 kHz from storage.
Transcript transcribe_pcm(const String& pcm_path);

}  // namespace vat::stt
