#include "ui_recorder.h"

#include <M5Cardputer.h>

#include <algorithm>
#include <deque>
#include <math.h>
#include <vector>

#include "common.h"
#include "net.h"
#include "storage.h"
#include "ui_kit.h"
#include "ui_state.h"

namespace vat::ui::recorder {
namespace {

constexpr uint32_t kSampleRate = 16000;
constexpr std::size_t kChunkSamples = 256;
constexpr std::size_t kWarmupChunks = 4;
constexpr std::size_t kFadeSamples = 64;
constexpr unsigned long kMaxRecordMsTf = 30000;
constexpr unsigned long kMaxRecordMsFlash = 20000;
constexpr std::size_t kMinRecordSamples = kSampleRate / 5;

int16_t audio_chunk[kChunkSamples];

bool capture_chunk() {
    if (!M5Cardputer.Mic.record(audio_chunk, kChunkSamples, kSampleRate)) return false;
    const unsigned long deadline = millis() + 250;
    bool started = false;
    while (millis() < deadline) {
        const bool recording = M5Cardputer.Mic.isRecording() != 0;
        started = started || recording;
        if (started && !recording) return true;
        delay(1);
    }
    return false;
}

// G.711 mu-law: 8-bit storage for recordings held in RAM.
uint8_t mulaw_encode(int16_t sample) {
    constexpr int kBias = 0x84;
    constexpr int kClip = 32635;
    int value = sample;
    const int sign = value < 0 ? 0x80 : 0;
    if (sign) value = -value;
    if (value > kClip) value = kClip;
    value += kBias;
    int exponent = 7;
    for (int mask = 0x4000; (value & mask) == 0 && exponent > 0; mask >>= 1) --exponent;
    const int mantissa = (value >> (exponent + 3)) & 0x0F;
    return static_cast<uint8_t>(~(sign | (exponent << 4) | mantissa));
}

int16_t mulaw_decode(uint8_t byte) {
    byte = ~byte;
    const int sign = byte & 0x80;
    const int exponent = (byte >> 4) & 0x07;
    const int mantissa = byte & 0x0F;
    int value = ((mantissa << 3) + 0x84) << exponent;
    value -= 0x84;
    return static_cast<int16_t>(sign ? -value : value);
}

// Recording strip at the bottom of the current screen: red dot, scrolling
// level bars, seconds.  The timeline above stays where it was.
void draw_meter(const std::deque<uint8_t>& levels, float seconds, float limit) {
    auto& d = M5Cardputer.Display;
    const int y = kHeight - kLine - 4;
    d.fillRect(0, y - 2, kWidth, kHeight - y + 2, kBg);
    d.drawFastHLine(0, y - 2, kWidth, kSurface);
    d.fillCircle(kPad + 4, y + 8, 4, kDanger);
    int x = kPad + 14;
    for (const uint8_t level : levels) {
        const int h = 2 + level * 2;
        d.fillRect(x, y + 8 - h / 2, 3, h, kAccent);
        x += 5;
    }
    d.setFont(&fonts::efontCN_12);
    d.setTextColor(kText, kBg);
    const String text = String(seconds, 1) + "s";
    d.drawString(text, kWidth - kPad - d.textWidth(text), y + 2);
    const int bar = static_cast<int>((kWidth - kPad * 2) * std::min(1.0f, seconds / limit));
    d.fillRect(kPad, kHeight - 2, bar, 2, seconds / limit > 0.85f ? kWarn : kMuted);
}

}  // namespace

// The M5Unified microphone dies permanently if the internal flash is written
// while its I2S capture is running (flash writes suspend the cache).  So the
// mic only runs while recording, and with on-board storage the recording is
// kept in RAM (mu-law) and written after the mic stops.  TF cards are written
// through SPI and are safe to stream to.
bool record(bool (*held)()) {
    storage::remove("/vat/cache/ptt.pcm");  // before the mic starts

    const bool to_card = storage::kind() == "tf";
    File output;
    if (to_card) {
        output = storage::fs().open("/vat/cache/ptt.pcm", FILE_WRITE);
        if (!output) {
            set_notice("无法创建录音文件", true);
            return false;
        }
    }
    constexpr std::size_t kBlockBytes = 8192;
    std::vector<uint8_t*> blocks;
    std::size_t ram_budget = 0;
    if (!to_card) {
        net::release_reserve();  // the TLS reserve is idle while recording
        const std::size_t free_heap = ESP.getFreeHeap();
        ram_budget = free_heap > 48000 ? std::min<std::size_t>(free_heap - 48000, 160000) : 0;
    }
    auto release_blocks = [&]() {
        for (auto* block : blocks) free(block);
        blocks.clear();
        if (!to_card) net::restore_reserve();
    };

    M5Cardputer.Mic.begin();
    bool warmed = true;
    for (std::size_t i = 0; i < kWarmupChunks && warmed; ++i) warmed = capture_chunk();
    if (!warmed) {
        M5Cardputer.Mic.end();
        if (output) output.close();
        storage::changed("/vat/cache/ptt.pcm");
        release_blocks();
        set_notice("麦克风预热超时", true);
        runtime_event("ptt_error", "mic warmup timeout");
        return false;
    }

    std::size_t samples = 0;
    std::size_t ram_used = 0;
    bool write_failed = false;
    bool ram_full = false;
    std::deque<uint8_t> levels;
    int peak = 0;
    const unsigned long started = millis();
    const unsigned long max_ms = to_card ? kMaxRecordMsTf : kMaxRecordMsFlash;
    const float limit_s = to_card ? kMaxRecordMsTf / 1000.0f
                                  : std::min(kMaxRecordMsFlash / 1000.0f, ram_budget / 8000.0f);
    while (millis() - started < max_ms) {
        M5Cardputer.update();
        if (!held() && samples > 0) break;
        if (!capture_chunk()) continue;
        if (samples == 0) {
            for (std::size_t i = 0; i < kFadeSamples; ++i) {
                audio_chunk[i] = static_cast<int16_t>(static_cast<int32_t>(audio_chunk[i]) * i / kFadeSamples);
            }
        }
        for (const auto s : audio_chunk) peak = std::max(peak, abs(static_cast<int>(s)));
        if (to_card) {
            if (output.write(reinterpret_cast<const uint8_t*>(audio_chunk), sizeof(audio_chunk)) != sizeof(audio_chunk)) {
                write_failed = true;
                break;
            }
        } else {
            const std::size_t offset = ram_used % kBlockBytes;
            if (offset == 0) {
                uint8_t* block = ram_used + kBlockBytes <= ram_budget
                                     ? static_cast<uint8_t*>(malloc(kBlockBytes)) : nullptr;
                if (!block) {
                    ram_full = true;
                    break;
                }
                blocks.push_back(block);
            }
            uint8_t* target = blocks.back() + offset;
            for (std::size_t i = 0; i < kChunkSamples; ++i) target[i] = mulaw_encode(audio_chunk[i]);
            ram_used += kChunkSamples;
        }
        samples += kChunkSamples;
        if ((samples / kChunkSamples) % 3 == 0) {  // ~48 ms per bar
            const int level = peak < 400 ? 0 : std::min(5, static_cast<int>(log2f(peak / 400.0f)) + 1);
            levels.push_back(level);
            if (levels.size() > 34) levels.pop_front();
            peak = 0;
            draw_meter(levels, static_cast<float>(samples) / kSampleRate, limit_s);
        }
    }
    M5Cardputer.Mic.end();  // from here on flash writes are safe

    if (to_card) {
        output.close();
    } else if (!blocks.empty()) {
        output = storage::fs().open("/vat/cache/ptt.pcm", FILE_WRITE);
        int16_t pcm[256];
        for (std::size_t done = 0; output && done < ram_used && !write_failed;) {
            const std::size_t n = std::min<std::size_t>(256, ram_used - done);
            const uint8_t* source = blocks[done / kBlockBytes] + done % kBlockBytes;
            for (std::size_t i = 0; i < n; ++i) pcm[i] = mulaw_decode(source[i]);
            write_failed = output.write(reinterpret_cast<const uint8_t*>(pcm), n * 2) != n * 2;
            done += n;
        }
        if (output) output.close();
        else write_failed = true;
        storage::changed("/vat/cache/ptt.pcm");
    }
    release_blocks();
    if (write_failed) {
        set_notice("录音写入失败（存储已满？）", true);
        runtime_event("ptt_error", "write failed");
        return false;
    }
    if (samples < kMinRecordSamples) {
        set_notice("录音太短，请按住后再说话", true);
        return false;
    }
    runtime_event("ptt_recorded", String(samples * 1000 / kSampleRate) + "ms" + (to_card ? " tf" : " ram") +
                                      (ram_full ? " ram_full" : ""));
    return true;
}

String mic_test() {
    int ok = 0;
    int peak = 0;
    M5Cardputer.Mic.begin();
    for (int i = 0; i < 20; ++i) {
        if (!capture_chunk()) continue;
        ++ok;
        for (const auto sample : audio_chunk) peak = std::max(peak, abs(static_cast<int>(sample)));
    }
    M5Cardputer.Mic.end();
    return "{\"chunks\":" + String(ok) + ",\"peak\":" + String(peak) + "}";
}

}  // namespace vat::ui::recorder
