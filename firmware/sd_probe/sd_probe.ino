#include <M5Cardputer.h>
#include <SD.h>
#include <SPI.h>

namespace {
constexpr int kSck = 40;
constexpr int kMiso = 39;
constexpr int kMosi = 14;
constexpr int kCs = 12;
String probe_result = "booting";

void show(const String& text, std::uint16_t color = TFT_WHITE) {
    Serial.println(text);
    M5Cardputer.Display.setTextColor(color, TFT_BLACK);
    M5Cardputer.Display.println(text);
}
}  // namespace

void setup() {
    Serial.begin(115200);
    auto config = M5.config();
    M5Cardputer.begin(config, true);
    M5Cardputer.Display.setRotation(1);
    M5Cardputer.Display.fillScreen(TFT_BLACK);
    M5Cardputer.Display.setCursor(4, 4);
    show("Official SD probe", TFT_CYAN);

    pinMode(kCs, OUTPUT);
    digitalWrite(kCs, HIGH);
    SPI.begin(kSck, kMiso, kMosi, kCs);
    const bool mounted = SD.begin(kCs, SPI, 25000000);
    probe_result = String("SD.begin=") + (mounted ? "true" : "false");
    show(probe_result,
         mounted ? TFT_GREEN : TFT_RED);
    if (!mounted) return;

    const auto type = SD.cardType();
    show("cardType=" + String(static_cast<int>(type)));
    show("sizeMB=" + String(SD.cardSize() / (1024 * 1024)));
    show("totalMB=" + String(SD.totalBytes() / (1024 * 1024)));
}

void loop() {
    Serial.println(probe_result);
    delay(1000);
}
