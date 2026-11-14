// Host tests for the screen compositor's plan (ui_compose_plan.cpp): the
// three rules that keep the screen from flickering, checked on a fake screen
// that counts every pixel written.  Included by host_test.cpp; uses its CHECK.
#include <cstring>

#include "ui_compose_plan.h"

namespace {

using vat::ui::compose::kBandHeight;
using vat::ui::compose::kScreenHeight;
using vat::ui::compose::kScreenWidth;
using vat::ui::compose::Rect;

struct Shape {
    Rect rect;
    uint16_t color;
};
using Scene = std::vector<Shape>;

// A screen and the strip: painters fill rects into the strip (clipped to the
// band being composed); push copies a rect of the strip onto the screen.
class FakeScreen : public vat::ui::compose::Target {
public:
    uint16_t screen[kScreenHeight][kScreenWidth] = {};
    uint8_t writes[kScreenHeight][kScreenWidth] = {};  // this update
    int pushes = 0;
    bool wrote_outside = false;  // a push outside the update's area
    Rect allowed = vat::ui::compose::kScreen;

    void start(const Rect& area) {
        std::memset(writes, 0, sizeof(writes));
        pushes = 0;
        wrote_outside = false;
        allowed = area;
    }
    void fill(const Rect& r, uint16_t color) {
        const Rect part = r.intersect(clip_);
        for (int y = part.y; y < part.y + part.h; ++y) {
            for (int x = part.x; x < part.x + part.w; ++x) strip_[y - band_ * kBandHeight][x] = color;
        }
    }
    void paint(const Scene& scene) {
        for (const auto& s : scene) fill(s.rect, s.color);
    }
    int max_writes() const {
        int most = 0;
        for (const auto& row : writes) {
            for (const uint8_t n : row) most = std::max<int>(most, n);
        }
        return most;
    }

    void begin(int band, const Rect& clip, uint16_t bg) override {
        band_ = band;
        clip_ = clip;
        fill(clip, bg);
    }
    uint32_t content_hash(int band) override {
        const int rows = std::min(kBandHeight, kScreenHeight - band * kBandHeight);
        return vat::ui::compose::hash(&strip_[0][0], rows * kScreenWidth);
    }
    void push(int band, const Rect& rect) override {
        ++pushes;
        if (!(rect.intersect(allowed) == rect)) wrote_outside = true;
        for (int y = rect.y; y < rect.y + rect.h; ++y) {
            for (int x = rect.x; x < rect.x + rect.w; ++x) {
                screen[y][x] = strip_[y - band * kBandHeight][x];
                ++writes[y][x];
            }
        }
    }

private:
    uint16_t strip_[kBandHeight][kScreenWidth] = {};
    int band_ = 0;
    Rect clip_;
};

constexpr uint16_t kBg = 0x0882;

// What the screen must show for `scene` (inside `area`; `before` elsewhere).
bool shows(const FakeScreen& s, const Scene& scene) {
    for (int y = 0; y < kScreenHeight; ++y) {
        for (int x = 0; x < kScreenWidth; ++x) {
            uint16_t want = kBg;
            for (const auto& shape : scene) {
                const Rect& r = shape.rect;
                if (x >= r.x && x < r.x + r.w && y >= r.y && y < r.y + r.h) want = shape.color;
            }
            if (s.screen[y][x] != want) return false;
        }
    }
    return true;
}

struct Rig {
    FakeScreen screen;
    vat::ui::compose::Ledger ledger;
    vat::ui::compose::Stats stats;
    void update(const Scene& scene, const Rect& area = vat::ui::compose::kScreen) {
        screen.start(area);
        vat::ui::compose::run(screen, ledger, stats, area, kBg, [&] { screen.paint(scene); });
    }
};

void ui_compose_slices() {
    using vat::ui::compose::slices;
    const auto all = slices(vat::ui::compose::kScreen);
    CHECK(all.size() == 9);
    CHECK(all.back().band == 8 && all.back().rect.y == 128 && all.back().rect.h == 7);  // 135 = 8 × 16 + 7
    const auto corner = slices({208, 103, 32, 32});  // the mascot: bands 6, 7, 8
    CHECK(corner.size() == 3 && corner[0].band == 6 && corner[0].rect == (Rect{208, 103, 32, 9}));
    CHECK(corner[2].rect == (Rect{208, 128, 32, 7}));
    CHECK(slices({0, 200, 10, 10}).empty());      // off the screen
    CHECK(slices({-10, -5, 20, 10}).size() == 1);  // clipped to it
    CHECK(slices({0, 16, 240, 16}).size() == 1);   // exactly one band
}

void ui_compose_rules() {
    Rig rig;
    const Scene page = {{{0, 0, 240, 15}, 0x18E4},  // top bar
                        {{4, 2, 30, 12}, 0xE75E},   // its title
                        {{0, 40, 240, 14}, 0x18E4},  // a selected row
                        {{4, 41, 100, 12}, 0x3DD5}};

    // First frame: every band pushed once, every pixel written exactly once.
    rig.update(page);
    CHECK(shows(rig.screen, page));
    CHECK(rig.screen.pushes == 9 && rig.screen.max_writes() == 1);

    // The same frame again: nothing pushed.
    rig.update(page);
    CHECK(rig.screen.pushes == 0 && rig.screen.max_writes() == 0 && rig.stats.skipped == 9);
    CHECK(rig.stats.frame_pushed == 9 && rig.stats.pushed == 9);

    // The clock in the top bar changes: only band 0.
    Scene clock = page;
    clock.push_back({{200, 2, 30, 12}, 0xE75E});
    rig.update(clock);
    CHECK(shows(rig.screen, clock));
    CHECK(rig.screen.pushes == 1 && rig.screen.writes[5][210] == 1 && rig.screen.writes[60][10] == 0);

    // Another page: whatever differs is pushed once, never cleared first.
    const Scene other = {{{0, 0, 240, 15}, 0x18E4}, {{0, 15, 240, 120}, 0x0000}, {{20, 60, 200, 40}, 0xE289}};
    rig.update(other);
    CHECK(shows(rig.screen, other));
    CHECK(rig.screen.max_writes() == 1);
    rig.update(page);
    CHECK(shows(rig.screen, page) && rig.screen.max_writes() == 1);

    // A part of the screen (the mascot's corner): nothing written outside it.
    const Rect corner{208, 103, 32, 32};
    const Scene mascot = {{corner, 0xED07}};
    rig.update(mascot, corner);
    CHECK(!rig.screen.wrote_outside && rig.screen.pushes == 3 && rig.screen.max_writes() == 1);
    CHECK(rig.screen.screen[110][220] == 0xED07 && rig.screen.screen[45][10] == 0x3DD5);  // the page around it stays
    // ...and the next whole frame repaints those bands even though the page did not change.
    rig.update(page);
    CHECK(shows(rig.screen, page) && rig.screen.pushes == 3);

    // An animation: a square moving across the page; each frame writes each pixel once
    // and pushes only the bands the square left or entered.
    int most_pushes = 0;
    bool once = true;
    bool right = true;
    for (int t = 0; t < 60; ++t) {
        Scene frame = page;
        frame.push_back({{t * 3, 70 + (t % 20), 20, 20}, 0x3DD5});
        rig.update(frame);
        right = right && shows(rig.screen, frame);
        once = once && rig.screen.max_writes() <= 1;
        most_pushes = std::max(most_pushes, rig.screen.pushes);
    }
    CHECK(right && once);
    CHECK(most_pushes <= 3);  // a 20-row square spans at most 3 bands; the bands it left are among them
}

void ui_compose() {
    ui_compose_slices();
    ui_compose_rules();
}

}  // namespace
