#include "ui_splash.h"

#include <M5Cardputer.h>
#include <esp_random.h>

#include <algorithm>
#include <climits>
#include <cmath>
#include <cstring>
#include <mutex>

#include "net.h"
#include "sprite_sheet.h"
#include "ui_kit.h"
#include "version.h"

namespace vat::ui::splash {
namespace {

constexpr int kScale = 3;
constexpr int kArt = SpriteSheet::kSize * kScale;  // 96
constexpr int kArtX = 10;
constexpr int kArtY = 6;
constexpr int kTextX = kArtX + kArt + 14;
constexpr int kWaterTop = kHeight - 12;   // water band at the bottom
constexpr int kLineY = kWaterTop - kLine - 3;
// The bubbles and the mascot stay left of the title and above the loading
// line: from the right the mascot steps out beside the title, from the
// bottom it climbs out from behind the line.
constexpr int kStageRight = kTextX - 4;
constexpr int kWaterH = kHeight - kWaterTop;
// The water band is drawn in memory and pushed whole: drawn on the display,
// the fish flickered (painted over by water, then drawn again, every frame).
// 8 bits a pixel, at the end of the TLS reserve's larger block.
constexpr std::size_t kWaterBytes = static_cast<std::size_t>(kWidth) * kWaterH;
constexpr uint16_t kWaterBlue = 0x1C9F;
constexpr uint16_t kFishBlue = 0x4B5F;  // deeper than the water, the mascot's blue
constexpr unsigned long kBubblesMinMs = 800;
constexpr unsigned long kEntranceMs = 1000;  // the slide-in without boot.bin
constexpr unsigned long kFrameMs = 40;
constexpr char kBootFile[] = "/vat/assets/boot.bin";
constexpr char kMascotFile[] = "/vat/assets/mascot.bin";

enum class Side : uint8_t { Left, Right, Top, Bottom };

struct Lines {
    const char* key;
    const char* options[3];
};
// One line per step, a few to pick from so every boot reads a little different.
const Lines kLines[] = {
    {"card", {"正在给鱼缸注水…", "正在检查鱼缸有没有漏…", "正在铺鱼缸底的沙子…"}},
    {"config", {"正在翻大肥鱼的小本本…", "正在回想自己叫什么…", "正在找昨天藏的白饭…"}},
    {"packages", {"正在往鱼鳍上绑工具…", "正在清点工具箱…", "正在给每个扩展包挂上名牌…"}},
    {"context", {"正在回忆上次聊到哪了…", "正在翻聊天记录…", "正在把记忆从卡里捞出来…"}},
    {"agent", {"正在叫醒大肥鱼…", "大肥鱼在伸懒腰…", "正在给大脑通电…"}},
    {"radio", {"正在竖起天线…", "正在仰望卫星…", "正在听远方有没有人喊…"}},
    {"worker", {"正在雇佣打工小鱼…", "打工小鱼已就位…", "正在分配今天的活…"}},
    {"wifi", {"正在冲浪找信号…", "正在顺着网线游过去…", "正在敲路由器的门…"}},
    {"time", {"正在跟太阳对表…", "正在问现在几点了…", "正在校准生物钟…"}},
    {"ssh", {"正在把钥匙串挂好…", "正在擦亮远程钥匙…", "正在记住各位主机的门牌号…"}},
    {"web", {"正在挂网页门牌…", "正在给访客准备令牌…", "正在打开小窗口…"}},
    {"ready", {"大肥鱼醒了！", "本小姐来了！", "准备好干活了～"}},
};

// Shared between boot (setup) and the drawing task.  The task draws straight
// to the display, region by region, each one painted over without clearing
// first.  It allocates nothing: memory held through the boot and freed at the
// end leaves holes among the long-lived allocations made meanwhile (the
// largest free block fell from 57 to 38 KB).  Its frames and its stack live
// in the TLS reserve instead, idle until the first TLS connection, which
// comes after the boot.
std::mutex lock;
String line = "";
uint16_t line_color = kMuted;
int progress_index = 0;
int progress_total = 12;
bool card = false;
bool stop = false;
bool stopped = true;

// Owned by the task once card is set (filled before it).
Side side = Side::Left;
SpriteSheet sheet;
const uint8_t* enter_frames = nullptr;  // palette indices, frame after frame (in the TLS reserve)
const uint8_t* wait_frames = nullptr;
StaticTask_t task_block;
int enter_state = -1, wait_state = -1;
bool scripted = false;  // boot.bin: the entrance is played once, each frame where it was drawn
unsigned long entrance_ms = kEntranceMs;
TaskHandle_t task = nullptr;
uint8_t* water_room = nullptr;  // kWaterBytes for the strip, or none
LGFX_Sprite strip;

const char* pick(const char* key) {
    for (const auto& l : kLines) {
        if (std::strcmp(l.key, key) == 0) return l.options[esp_random() % 3];
    }
    return key;
}

unsigned long length_of(int state) {
    unsigned long total = 0;
    for (int f = 0; f < sheet.frames(state); ++f) total += sheet.duration(state, f);
    return total;
}

// Frame of `state` at time `ms`: looping, or held on the last one.
int frame_index(int state, unsigned long ms, bool loop) {
    const int count = sheet.frames(state);
    const unsigned long total = length_of(state);
    if (count == 0 || total == 0) return 0;
    if (!loop && ms >= total) return count - 1;
    unsigned long t = ms % total;
    for (int f = 0; f < count; ++f) {
        const unsigned long d = sheet.duration(state, f);
        if (t < d) return f;
        t -= d;
    }
    return count - 1;
}

const uint8_t* frame_data(const uint8_t* frames, int state, int index) {
    if (!frames || sheet.frames(state) == 0) return nullptr;
    return frames + index * SpriteSheet::kSize * SpriteSheet::kSize;
}

void draw_title(lgfx::LGFXBase& g) {
    g.setFont(&fonts::efontCN_14);
    g.setTextSize(2);
    g.setTextColor(kText, kBg);
    g.drawString("Feiyu", kTextX, 22);
    g.drawString("OS", kTextX, 22 + 30);
    g.setTextSize(1);
    g.setFont(&fonts::efontCN_12);
    g.setTextColor(kMuted, kBg);
    g.drawString(String("肥鱼 OS ") + kVersion, kTextX, 22 + 64);
}

// Bubbles at the edge the mascot will come from, each on a 2 s path: from
// the left or right they drift in from beyond the edge and rise; from the top
// they sink, part to either side and rise again; from the bottom they rise.
// The paths keep clear of the title and the loading line.  `color` kBg
// erases them (the same `ms` gives the same positions).
void draw_bubbles(lgfx::LGFXBase& g, unsigned long ms, uint16_t color) {
    constexpr unsigned long kPeriod = 2000;
    constexpr float kTop = -6;                 // risen out of sight
    constexpr float kBottom = kLineY - 6;
    const auto ease = [](float t) { return t * t * (3 - 2 * t); };
    const auto part = [](float p, float from, float to) { return std::min(std::max((p - from) / (to - from), 0.0f), 1.0f); };
    for (int i = 0; i < 7; ++i) {
        const float p = static_cast<float>((ms + i * kPeriod / 7) % kPeriod) / kPeriod;
        const float wobble = 2.5f * std::sin(ms / 160.0f + i);
        const int lane = (i * 37) % 11;  // 0..10: keeps them apart
        float x, y;
        switch (side) {
            case Side::Left:
            case Side::Right: {
                const float in = ease(part(p, 0, 0.3f)), up = ease(part(p, 0.3f, 1));
                const float depth = 8 + lane * 1.5f;  // how far in from the edge
                const float y0 = kBottom - (i % 3) * 14;
                const float from_edge = -6 + (depth + 6) * in + (up > 0 ? wobble : 0);
                x = side == Side::Left ? from_edge : kStageRight - 1 - from_edge;
                y = y0 + (kTop - y0) * up;
                break;
            }
            case Side::Top: {
                const float cx = kArtX + kArt / 2, dir = i % 2 ? 1 : -1;
                const float low = 34 + lane * 4;  // how deep it sinks
                const float down = ease(part(p, 0, 0.35f)), apart = ease(part(p, 0.35f, 0.6f)), up = ease(part(p, 0.6f, 1));
                x = cx + dir * (lane + (28 + lane * 1.5f) * apart) + (up > 0 ? wobble : 0);
                y = kTop + (low - kTop) * down + (kTop - low) * up;
                break;
            }
            default: {
                x = kArtX + kArt / 2 - 12 + (i % 4) * 8 + wobble;
                y = kBottom + (kTop - kBottom) * part(p, 0, 1);
                break;
            }
        }
        const int r = 1 + (i % 3);
        const int bx = static_cast<int>(x), by = static_cast<int>(y);
        g.drawCircle(bx, by, r, color);
        if (r > 1) g.drawPixel(bx - 1, by - 1, color == kBg ? kBg : kText);
    }
}

// Where the entrance starts, off the side it comes from.
void start_of(int& x, int& y) {
    x = kArtX;
    y = kArtY;
    switch (side) {
        case Side::Left: x = -kArt; break;
        case Side::Right: x = kStageRight; break;
        case Side::Top: y = -kArt; break;
        case Side::Bottom: y = kLineY; break;
    }
}

// The water line filled up to the progress, a small blue fish at its front,
// drawn at `top` of `g` (the strip in memory, or the display without it).
void draw_water(lgfx::LGFXBase& g, int top, unsigned long ms, float& shown) {
    const float target = progress_total > 0 ? static_cast<float>(std::min(progress_index, progress_total)) / progress_total : 0;
    shown += (target - shown) * 0.12f;  // the fish swims, it does not jump
    const int front = static_cast<int>(shown * (kWidth - 16)) + 8;
    const int bottom = top + kWaterH;
    for (int x = 0; x < kWidth; x += 2) {  // every column painted whole
        const int surface = x < front ? top + 3 + static_cast<int>(2 * std::sin((x + ms / 25.0f) / 7.0f)) : bottom;
        g.fillRect(x, top, 2, surface - top, kBg);
        if (x >= front) g.drawPixel(x, top + 4, kSurface);
        else g.fillRect(x, surface, 2, bottom - surface, kWaterBlue);
    }
    const int fy = top + 5 + static_cast<int>(std::sin(ms / 180.0f));
    g.fillEllipse(front, fy, 6, 3, kFishBlue);                                         // body
    g.fillTriangle(front - 5, fy, front - 10, fy - 3, front - 10, fy + 3, kFishBlue);  // tail
    g.drawPixel(front + 3, fy - 1, kBg);                                                // eye
}

void task_main(void*) {
    const unsigned long started = millis();
    unsigned long entrance_from = 0;
    unsigned long last_bubbles = 0;
    bool bubbles_cleared = false;
    int last_x = INT32_MIN, last_y = 0;
    String shown_text;
    uint16_t shown_color = 0;
    float water = 0;
    {
        auto& g = M5Cardputer.Display;
        g.startWrite();
        g.fillScreen(kBg);
        draw_title(g);
        g.endWrite();
    }
    while (true) {
        String text;
        uint16_t text_color;
        bool have_card, stopping;
        {
            std::lock_guard<std::mutex> guard(lock);
            text = line;
            text_color = line_color;
            have_card = card;
            stopping = stop;
        }
        const unsigned long now = millis();
        const bool bubbling = !have_card || now - started < kBubblesMinMs || enter_state < 0 && wait_state < 0;
        if (!bubbling && entrance_from == 0) entrance_from = now;
        const bool entering = entrance_from && now - entrance_from < entrance_ms;
        if (stopping && !entering) break;

        auto& g = M5Cardputer.Display;
        g.startWrite();
        g.setClipRect(0, 0, kStageRight, kLineY - 1);
        if (bubbling) {
            draw_bubbles(g, last_bubbles, kBg);  // last positions out, new ones in
            draw_bubbles(g, now - started, kAccent);
            last_bubbles = now - started;
        } else {
            if (!bubbles_cleared) {
                draw_bubbles(g, last_bubbles, kBg);
                bubbles_cleared = true;
            }
            int x = kArtX, y = kArtY;
            const uint8_t* f = nullptr;
            if (entering && scripted) {  // as drawn: once, each frame at its own offset
                const int i = frame_index(enter_state, now - entrance_from, false);
                int dx, dy;
                sheet.offset(enter_state, i, dx, dy);
                x = kArtX + dx * kScale;
                y = kArtY + dy * kScale;
                f = frame_data(enter_frames, enter_state, i);
            } else if (entering) {
                int x0, y0;
                start_of(x0, y0);
                const float t = static_cast<float>(now - entrance_from) / kEntranceMs;
                const float ease = 1 - (1 - t) * (1 - t);  // slows down as it arrives
                x = x0 + static_cast<int>((kArtX - x0) * ease);
                y = y0 + static_cast<int>((kArtY - y0) * ease);
                f = frame_data(enter_frames, enter_state, frame_index(enter_state, now - entrance_from, true));
            } else {
                f = frame_data(wait_frames, wait_state, frame_index(wait_state, now - entrance_from - entrance_ms, true));
            }
            // The strip it left since the last frame goes back to the background.
            if (last_x != INT32_MIN) {
                if (x > last_x) g.fillRect(last_x, last_y, x - last_x, kArt, kBg);
                if (x < last_x) g.fillRect(x + kArt, last_y, last_x - x, kArt, kBg);
                if (y > last_y) g.fillRect(last_x, last_y, kArt, y - last_y, kBg);
                if (y < last_y) g.fillRect(last_x, y + kArt, kArt, last_y - y, kBg);
            }
            if (f) sheet.draw(g, f, x, y, kScale, kBg);
            last_x = x;
            last_y = y;
        }
        g.clearClipRect();
        if (text != shown_text || text_color != shown_color) {
            g.fillRect(0, kLineY - 1, kWidth, kLine + 2, kBg);
            g.setFont(&fonts::efontCN_12);
            g.setTextColor(text_color, kBg);
            g.drawString(text, kPad, kLineY);
            shown_text = text;
            shown_color = text_color;
        }
        if (water_room) {
            draw_water(strip, 0, now, water);
            strip.pushSprite(&g, 0, kWaterTop);
        } else {
            draw_water(g, kWaterTop, now, water);
        }
        g.endWrite();
        vTaskDelay(pdMS_TO_TICKS(kFrameMs));
    }
    {
        std::lock_guard<std::mutex> guard(lock);
        stopped = true;
    }
    vTaskDelete(nullptr);
}

}  // namespace

void begin() {
    side = static_cast<Side>(esp_random() % 4);
    std::size_t bytes = 0;
    void* stack = net::reserve_block(1, bytes);  // the smaller block: this task's stack
    if (!stack) return;                          // no animation; the boot carries on
    std::size_t room_bytes = 0;
    auto* room = static_cast<uint8_t*>(net::reserve_block(0, room_bytes));
    if (room && room_bytes >= kWaterBytes + 8192) {
        water_room = room + room_bytes - kWaterBytes;  // the frames go before it
        strip.setColorDepth(lgfx::color_depth_t::rgb332_1Byte);
        strip.setBuffer(water_room, kWidth, kWaterH, 8);
        // Never past its room: a deeper colour than asked would run into
        // this task's stack (it did, at boot).  Without it, drawn on screen.
        if (strip.bufferLength() != kWaterBytes) {
            strip.deleteSprite();
            water_room = nullptr;
        }
    }
    stopped = false;
    stop = false;
    task = xTaskCreateStaticPinnedToCore(task_main, "boot-splash", bytes, nullptr, 1,
                                         static_cast<StackType_t*>(stack), &task_block, 1);
}

void card_ready() {
    // Read here (boot context), never from the task: one user of the card at a time.
    std::size_t bytes = 0;
    uint8_t* room = static_cast<uint8_t*>(net::reserve_block(0, bytes));  // the larger block: the frames
    if (water_room) bytes -= kWaterBytes;  // its end holds the water strip
    if (room && sheet.open(kBootFile) && sheet.states() >= 5) {
        enter_state = static_cast<int>(side);  // enter_left, enter_right, enter_top, enter_bottom, wait
        wait_state = 4;
        scripted = true;
        entrance_ms = length_of(enter_state);
    } else if (room && sheet.open(kMascotFile)) {
        enter_state = 0;  // the corner mascot's idle frames slide in
        wait_state = 0;
    }
    const std::size_t frame = SpriteSheet::kSize * SpriteSheet::kSize;
    const std::size_t enter_bytes = enter_state >= 0 ? sheet.frames(enter_state) * frame : 0;
    if (enter_state >= 0 && sheet.read_state(enter_state, room, bytes)) enter_frames = room;
    else enter_state = -1;
    if (wait_state == enter_state && enter_state >= 0) {
        wait_frames = enter_frames;  // same frames (idle): read once
    } else if (wait_state >= 0 && sheet.read_state(wait_state, room + enter_bytes, bytes - enter_bytes)) {
        wait_frames = room + enter_bytes;
    } else {
        wait_state = -1;
    }
    std::lock_guard<std::mutex> guard(lock);
    card = true;
}

void step(const char* key, int index, int total) {
    std::lock_guard<std::mutex> guard(lock);
    line = pick(key);
    line_color = std::strcmp(key, "ready") == 0 ? kAccent : kMuted;
    progress_index = index;
    progress_total = total;
}

void problem(const String& text) {
    std::lock_guard<std::mutex> guard(lock);
    line = text;
    line_color = kWarn;
}

void finish() {
    if (!task) return;
    {
        std::lock_guard<std::mutex> guard(lock);
        stop = true;
    }
    for (int i = 0; i < 300; ++i) {  // the entrance takes about a second
        {
            std::lock_guard<std::mutex> guard(lock);
            if (stopped) break;
        }
        delay(10);
    }
    delay(20);  // the task's last instructions run on its stack: let it go before TLS gets the block
    task = nullptr;
    enter_frames = wait_frames = nullptr;
    if (water_room) strip.deleteSprite();  // the buffer is the reserve's: not freed
    water_room = nullptr;
    scripted = false;
    entrance_ms = kEntranceMs;
}

}  // namespace vat::ui::splash
