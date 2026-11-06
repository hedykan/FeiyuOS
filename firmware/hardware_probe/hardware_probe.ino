#include <M5Cardputer.h>

#include <algorithm>
#include <cstdint>

namespace {

constexpr std::size_t kMicSamples = 256;
constexpr std::uint32_t kMicSampleRate = 16000;
constexpr unsigned long kRenderIntervalMs = 100;

enum class Page {
    Agent,
    Text,
};

Page page = Page::Agent;
String input;
int16_t mic_samples[kMicSamples];
int mic_peak = 0;
unsigned long last_render_ms = 0;

const char* page_label() {
    return page == Page::Agent ? "AGENT" : "TEXT";
}

std::uint16_t page_color() {
    return page == Page::Agent ? 0xA81F : 0x07FF;
}

void update_keyboard() {
    if (!M5Cardputer.Keyboard.isChange() ||
        !M5Cardputer.Keyboard.isPressed()) {
        return;
    }

    const auto state = M5Cardputer.Keyboard.keysState();
    if (state.del && !input.isEmpty()) {
        input.remove(input.length() - 1);
    }
    if (state.enter) {
        Serial.printf("[%s] %s\n", page_label(), input.c_str());
        input = "";
    }
    for (const char c : state.word) {
        if (input.length() < 64) {
            input += c;
        }
    }
}

void update_microphone() {
    if (!M5Cardputer.Mic.isEnabled()) {
        mic_peak = 0;
        return;
    }
    if (!M5Cardputer.Mic.record(mic_samples, kMicSamples, kMicSampleRate)) {
        return;
    }

    int peak = 0;
    for (const int16_t sample : mic_samples) {
        const int value = sample == INT16_MIN ? INT16_MAX : std::abs(sample);
        peak = std::max(peak, value);
    }
    mic_peak = peak;
}

void render() {
    auto& display = M5Cardputer.Display;
    display.fillScreen(TFT_BLACK);

    display.fillRect(0, 0, display.width(), 24, page_color());
    display.setTextColor(TFT_BLACK, page_color());
    display.setTextSize(2);
    display.setCursor(6, 5);
    display.print(page_label());

    display.setTextColor(TFT_WHITE, TFT_BLACK);
    display.setTextSize(1);
    display.setCursor(6, 31);
    display.printf("Mic  %5d", mic_peak);

    const int meter_width = map(std::min(mic_peak, 16000), 0, 16000, 0,
                                display.width() - 12);
    display.drawRect(6, 44, display.width() - 12, 10, TFT_DARKGREY);
    display.fillRect(7, 45, std::max(0, meter_width - 2), 8, TFT_GREEN);

    display.setCursor(6, 62);
    display.printf("Heap %u", ESP.getFreeHeap());
    display.setCursor(6, 76);
    display.print("BtnA: switch page");
    display.setCursor(6, 90);
    display.print("Enter: serial + clear");

    display.drawFastHLine(0, 106, display.width(), TFT_DARKGREY);
    display.setCursor(6, 114);
    display.print("> ");
    display.print(input.substring(std::max(0, static_cast<int>(input.length()) - 34)));
}

}  // namespace

void setup() {
    Serial.begin(115200);

    auto config = M5.config();
    M5Cardputer.begin(config, true);
    M5Cardputer.Display.setRotation(1);

    // The original Cardputer shares audio resources between microphone and
    // speaker. This probe only records, so keep the speaker disabled.
    M5Cardputer.Speaker.end();
    M5Cardputer.Mic.begin();

    Serial.println("Cardputer hardware probe started");
    render();
}

void loop() {
    M5Cardputer.update();

    if (M5Cardputer.BtnA.wasClicked()) {
        page = page == Page::Agent ? Page::Text : Page::Agent;
    }

    update_keyboard();
    update_microphone();

    const unsigned long now = millis();
    if (now - last_render_ms >= kRenderIntervalMs) {
        last_render_ms = now;
        render();
    }
}

