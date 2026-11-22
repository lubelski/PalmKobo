// palmkobo unit tests - pure front-end logic, no emulator or e-ink needed.
// SPDX-License-Identifier: GPL-3.0-or-later

#include <linux/input.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>

#include "ButtonBar.h"
#include "Canvas.h"
#include "Config.h"
#include "FrameConvert.h"
#include "Geometry.h"
#include "IdlePolicy.h"
#include "Input.h"
#include "Keyboard.h"
#include "Menu.h"
#include "Pgm.h"
#include "RefreshScheduler.h"

using namespace pk;

static int failures = 0, checks = 0;

#define CHECK(cond)                                                          \
    do {                                                                     \
        checks++;                                                            \
        if (!(cond)) {                                                       \
            failures++;                                                      \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);      \
        }                                                                    \
    } while (0)

#define CHECK_EQ(a, b)                                                                  \
    do {                                                                                \
        checks++;                                                                       \
        const long long va = (a), vb = (b);                                             \
        if (va != vb) {                                                                 \
            failures++;                                                                 \
            std::printf("FAIL %s:%d: %s == %s (%lld vs %lld)\n", __FILE__, __LINE__, #a, #b, va, vb); \
        }                                                                               \
    } while (0)

// Clara BW panel: 1072 x 1448 (6", 300 dpi).
static void testLayoutClaraBW() {
    const Layout l = Layout::compute(1072, 1448, 160, 160, 60);
    CHECK_EQ(l.scale, 6);
    CHECK_EQ(l.lcd.w, 960);
    CHECK_EQ(l.lcd.h, 960);
    CHECK_EQ(l.silk.h, 360);
    CHECK_EQ(l.lcd.x, 56);
    CHECK_EQ(l.silk.y, l.lcd.bottom());
    CHECK(l.bar.y >= l.palm.bottom());
    CHECK(l.bar.bottom() <= 1448);
    CHECK(l.bar.h >= Layout::MIN_BAR);
    CHECK_EQ(l.palm.h, 1320);

    // Forced smaller scale stays centered.
    const Layout s = Layout::compute(1072, 1448, 160, 160, 60, 4);
    CHECK_EQ(s.scale, 4);
    CHECK_EQ(s.lcd.x, (1072 - 640) / 2);

    // Forcing a scale larger than fits is capped.
    CHECK_EQ(Layout::compute(1072, 1448, 160, 160, 60, 10).scale, 6);

    // Hopelessly small screen.
    CHECK_EQ(Layout::compute(100, 100, 160, 160, 60).scale, 0);
}

// Sony Clie NR70 series: 320x480, no silkscreen strip (Palm OS draws it in the LCD).
// Too tall for a strip below it at 3x, so the controls go in a rail on the right.
static void testLayoutClie320x480() {
    const Layout l = Layout::compute(1072, 1448, 320, 480, 0);
    CHECK(l.rail);
    CHECK_EQ(l.scale, 3);
    CHECK_EQ(l.lcd.w, 960);
    CHECK_EQ(l.lcd.h, 1440);
    CHECK_EQ(l.lcd.x, 4);
    CHECK_EQ(l.lcd.y, 4);
    CHECK(l.silk.empty());
    CHECK(l.palm == l.lcd);
    // The rail: right of the Palm, same height, wide enough to touch (8 mm at 300 ppi).
    CHECK(l.bar.x >= l.palm.right());
    CHECK(l.bar.right() <= 1072);
    CHECK_EQ(l.bar.y, l.palm.y);
    CHECK_EQ(l.bar.h, l.palm.h);
    CHECK(l.bar.w >= Layout::MIN_BAR);
    CHECK_EQ(l.bar.w, 96);

    // Touch reaches every Palm pixel, including the status bar on the last rows.
    int px = -1, py = -1;
    CHECK(l.toPalm(l.lcd.x, l.lcd.y, px, py, false));
    CHECK_EQ(px, 0);
    CHECK_EQ(py, 0);
    CHECK(l.toPalm(l.lcd.right() - 1, l.lcd.bottom() - 1, px, py, false));
    CHECK_EQ(px, 319);
    CHECK_EQ(py, 479);
    CHECK(l.toPalm(l.lcd.x + 3 * 12 + 1, l.lcd.y + 3 * 466, px, py, false));
    CHECK_EQ(px, 12);
    CHECK_EQ(py, 466);
    CHECK(!l.toPalm(l.bar.x, l.bar.y + 10, px, py, false));  // the rail is not the Palm
    CHECK(l.toPalm(2000, 2000, px, py, true));
    CHECK_EQ(px, 319);
    CHECK_EQ(py, 479);

    // scale= in the ini still wins: at 2x it fits above a strip, which then uses
    // the panel width because the Palm block is narrow.
    const Layout f = Layout::compute(1072, 1448, 320, 480, 0, 2);
    CHECK(!f.rail);
    CHECK_EQ(f.scale, 2);
    CHECK_EQ(f.lcd.x, 216);
    CHECK(f.bar.y >= f.palm.bottom());
    CHECK_EQ(f.bar.x, Layout::GAP);
    CHECK_EQ(f.bar.w, 1072 - 2 * Layout::GAP);

    // Screens that already fit at their best scale above a strip keep the strip:
    // m500 / IIIc (160x160+60), hi-res with silkscreen (320x320+120), 240x320.
    const Layout m = Layout::compute(1072, 1448, 160, 160, 60);
    CHECK(!m.rail);
    CHECK_EQ(m.scale, 6);
    CHECK_EQ(m.bar.x, m.lcd.x);
    CHECK_EQ(m.bar.y, 1336);
    CHECK_EQ(m.bar.w, 960);
    CHECK_EQ(m.bar.h, 104);
    CHECK(!Layout::compute(1072, 1448, 320, 320, 120).rail);
    CHECK(!Layout::compute(1072, 1448, 240, 320, 0).rail);
    // No rail when there is no room for one beside the larger scale.
    CHECK(!Layout::compute(980, 1448, 320, 480, 0).rail);
}

// The Clie's 16-bit colour screen arrives as a 320x480 frame of 4 bytes per pixel.
static void testConvertFrameColor320x480() {
    const Layout l = Layout::compute(1072, 1448, 320, 480, 0);
    Canvas c(1072, 1448, 0x80);

    std::vector<uint8_t> buf(320 * 480 * 4, 0xff);  // white
    auto put = [&](int x, int y, uint8_t r, uint8_t g, uint8_t b) {
        uint8_t* p = &buf[(static_cast<size_t>(y) * 320 + x) * 4];
        p[0] = r, p[1] = g, p[2] = b;
    };
    put(0, 0, 0, 0, 0);
    put(319, 479, 0, 0, 0);
    put(1, 0, 255, 0, 0);
    put(2, 0, 0, 255, 0);

    LcdFrame f;
    f.bpp = 24;
    f.lineWidth = 320;
    f.lines = 480;
    f.bytesPerLine = 320 * 4;
    f.firstDirtyLine = 0;
    f.lastDirtyLine = 479;
    f.buffer = buf.data();

    CHECK(convertFrame(f, c, l) == l.lcd);
    CHECK_EQ(c.at(l.lcd.x, l.lcd.y), 0x00);
    CHECK_EQ(c.at(l.lcd.x + 2, l.lcd.y + 2), 0x00);  // 3x3 block
    CHECK_EQ(c.at(l.lcd.x + 3, l.lcd.y), lumaRGB(255, 0, 0));
    CHECK_EQ(c.at(l.lcd.x + 6, l.lcd.y), lumaRGB(0, 255, 0));
    CHECK(lumaRGB(255, 0, 0) < lumaRGB(0, 255, 0));  // red reads darker than green
    CHECK_EQ(c.at(l.lcd.x + 9, l.lcd.y), 0xff);
    CHECK_EQ(c.at(l.lcd.right() - 1, l.lcd.bottom() - 1), 0x00);
    CHECK_EQ(c.at(l.lcd.right(), l.lcd.bottom() - 1), 0x80);  // nothing outside the LCD
    CHECK_EQ(c.at(l.lcd.x, l.lcd.bottom()), 0x80);

    // A frame of another size (mid mode switch) is skipped.
    f.lines = 320;
    CHECK(convertFrame(f, c, l).empty());
}

static void testToPalm() {
    const Layout l = Layout::compute(1072, 1448, 160, 160, 60);
    int px = -1, py = -1;
    CHECK(l.toPalm(l.lcd.x, l.lcd.y, px, py, false));
    CHECK_EQ(px, 0);
    CHECK_EQ(py, 0);
    CHECK(l.toPalm(l.lcd.right() - 1, l.lcd.bottom() - 1, px, py, false));
    CHECK_EQ(px, 159);
    CHECK_EQ(py, 159);
    // Graffiti area maps below the LCD, into digitizer rows 160..219.
    CHECK(l.toPalm(l.silk.x + 6 * 80, l.silk.y + 6 * 30 + 5, px, py, false));
    CHECK_EQ(px, 80);
    CHECK_EQ(py, 190);
    CHECK(l.toPalm(l.silk.right() - 1, l.silk.bottom() - 1, px, py, false));
    CHECK_EQ(py, 219);
    // Outside the digitizer: rejected unless clamping.
    CHECK(!l.toPalm(5, 5, px, py, false));
    CHECK(l.toPalm(5, 5, px, py, true));
    CHECK_EQ(px, 0);
    CHECK_EQ(py, 0);
    CHECK(l.toPalm(2000, 2000, px, py, true));
    CHECK_EQ(px, 159);
    CHECK_EQ(py, 219);
}

static void testMapTouch() {
    const AxisRange ax{0, 1071}, ay{0, 1447};
    int x, y;
    mapTouch(0, 0, ax, ay, {}, 1072, 1448, x, y);
    CHECK_EQ(x, 0);
    CHECK_EQ(y, 0);
    mapTouch(1071, 1447, ax, ay, {}, 1072, 1448, x, y);
    CHECK_EQ(x, 1071);
    CHECK_EQ(y, 1447);

    // Panel mounted rotated: raw x runs along the long side.
    const AxisRange rx{0, 1447}, ry{0, 1071};
    mapTouch(100, 50, rx, ry, {true, false, false}, 1072, 1448, x, y);
    CHECK_EQ(x, 50);
    CHECK_EQ(y, 100);
    mapTouch(100, 50, rx, ry, {true, true, false}, 1072, 1448, x, y);
    CHECK_EQ(x, 1071 - 50);
    CHECK_EQ(y, 100);
    mapTouch(100, 50, rx, ry, {true, false, true}, 1072, 1448, x, y);
    CHECK_EQ(y, 1447 - 100);

    // Different resolution than the screen is normalized.
    mapTouch(2047, 4095, {0, 4095}, {0, 4095}, {}, 1072, 1448, x, y);
    CHECK_EQ(x, 535);
    CHECK_EQ(y, 1447);

    // Out of range is clamped.
    mapTouch(-5, 99999, ax, ay, {}, 1072, 1448, x, y);
    CHECK_EQ(x, 0);
    CHECK_EQ(y, 1447);
}

static void testUnpack() {
    const uint8_t row1[] = {0b10110000};
    CHECK_EQ(unpackPixel(row1, 1, 0), 1);
    CHECK_EQ(unpackPixel(row1, 1, 1), 0);
    CHECK_EQ(unpackPixel(row1, 1, 2), 1);
    CHECK_EQ(unpackPixel(row1, 1, 3), 1);
    const uint8_t row2[] = {0b11100100};
    CHECK_EQ(unpackPixel(row2, 2, 0), 3);
    CHECK_EQ(unpackPixel(row2, 2, 1), 2);
    CHECK_EQ(unpackPixel(row2, 2, 2), 1);
    CHECK_EQ(unpackPixel(row2, 2, 3), 0);
    const uint8_t row4[] = {0xf0, 0x5a};
    CHECK_EQ(unpackPixel(row4, 4, 0), 15);
    CHECK_EQ(unpackPixel(row4, 4, 1), 0);
    CHECK_EQ(unpackPixel(row4, 4, 2), 5);
    CHECK_EQ(unpackPixel(row4, 4, 3), 10);
    CHECK_EQ(paletteGray16(0), 0xff);
    CHECK_EQ(paletteGray16(15), 0x00);
}

static void testConvertFrame4bpp() {
    const Layout l = Layout::compute(1072, 1448, 160, 160, 60);
    Canvas c(1072, 1448, 0x80);

    // 160x160 at 4bpp, 80 bytes per line; line 10 has pixel 0 black and pixel 159 gray 5.
    std::vector<uint8_t> buf(80 * 160, 0x00);
    buf[10 * 80 + 0] = 0xf0;
    buf[10 * 80 + 79] = 0x05;

    LcdFrame f;
    f.bpp = 4;
    f.lineWidth = 160;
    f.lines = 160;
    f.bytesPerLine = 80;
    f.firstDirtyLine = 10;
    f.lastDirtyLine = 11;
    f.buffer = buf.data();

    const Rect dirty = convertFrame(f, c, l);
    CHECK_EQ(dirty.x, l.lcd.x);
    CHECK_EQ(dirty.y, l.lcd.y + 60);
    CHECK_EQ(dirty.w, 960);
    CHECK_EQ(dirty.h, 12);

    CHECK_EQ(c.at(l.lcd.x, l.lcd.y + 60), 0x00);             // black block, top-left
    CHECK_EQ(c.at(l.lcd.x + 5, l.lcd.y + 65), 0x00);         // ... bottom-right of block
    CHECK_EQ(c.at(l.lcd.x + 6, l.lcd.y + 60), 0xff);         // next pixel white
    CHECK_EQ(c.at(l.lcd.x + 959, l.lcd.y + 60), paletteGray16(5));
    CHECK_EQ(c.at(l.lcd.x, l.lcd.y + 66), 0xff);             // line 11 drawn (white)
    CHECK_EQ(c.at(l.lcd.x, l.lcd.y + 72), 0x80);             // line 12 untouched
    CHECK_EQ(c.at(l.lcd.x, l.lcd.y + 54), 0x80);             // line 9 untouched
}

static void testConvertFrameMarginAnd2bpp() {
    const Layout l = Layout::compute(1072, 1448, 160, 160, 60);
    Canvas c(1072, 1448, 0x80);

    // 2bpp with a 3-pixel margin and 41 bytes per line (164 px).
    std::vector<uint8_t> buf(41 * 160, 0x00);
    // Pixel 0 of line 0 is the 4th 2-bit field of byte 0 (bits 1..0).
    buf[0] = 0b00000011;
    LcdFrame f;
    f.bpp = 2;
    f.lineWidth = 160;
    f.lines = 160;
    f.margin = 3;
    f.bytesPerLine = 41;
    f.firstDirtyLine = 0;
    f.lastDirtyLine = 0;
    f.lcd2bitMapping = 0xf840;  // 0->0, 1->4, 2->8, 3->15
    f.buffer = buf.data();
    convertFrame(f, c, l);
    CHECK_EQ(c.at(l.lcd.x, l.lcd.y), 0x00);
    CHECK_EQ(c.at(l.lcd.x + 6, l.lcd.y), 0xff);

    // 1bpp, and a frame whose geometry doesn't match is ignored.
    LcdFrame bad = f;
    bad.lineWidth = 320;
    CHECK(convertFrame(bad, c, l).empty());
}

static void testScheduler() {
    const Rect screen{0, 0, 1072, 1448};
    RefreshScheduler::Params p;
    p.fullAfterPartials = 2;
    RefreshScheduler s(screen, p);

    // Nothing to do initially.
    CHECK(!s.poll(1000));

    // Change while pen is up: waits for idleDelay, then quality refresh.
    s.addDirty({10, 10, 5, 5}, 1000);
    CHECK(!s.poll(1030));
    auto f = s.poll(1061);
    CHECK(f && f->waveform == Waveform::Quality && f->rect == Rect({10, 10, 5, 5}));

    // Pen down: fast refreshes at penInterval, merged.
    s.setPenDown(true, 2000);
    s.addDirty({0, 0, 10, 10}, 2000);
    f = s.poll(2001);
    CHECK(f && f->waveform == Waveform::Fast);
    s.addDirty({20, 20, 10, 10}, 2010);
    CHECK(!s.poll(2020));  // within penInterval of last flush
    f = s.poll(2045);
    CHECK(f && f->waveform == Waveform::Fast && f->rect == Rect({20, 20, 10, 10}));

    // Pen up: everything that got a fast refresh is redone in quality.
    s.setPenDown(false, 2100);
    CHECK(!s.poll(2120));
    f = s.poll(2161);
    CHECK(f && f->waveform == Waveform::Quality && f->rect == Rect({0, 0, 30, 30}));

    // Two quality refreshes done -> full flashing refresh once idle long enough.
    CHECK(!s.poll(2200));
    f = s.poll(2161 + 1500 + 1);
    CHECK(f && f->waveform == Waveform::Full && f->rect == screen);
    CHECK(!s.poll(5000));

    // Max latency: continuously changing screen still gets refreshed.
    s.addDirty({0, 0, 1, 1}, 6000);
    for (uint64_t t = 6000; t < 6250; t += 20) {
        s.addDirty({0, 0, 1, 1}, t);
        CHECK(!s.poll(t));
    }
    s.addDirty({0, 0, 1, 1}, 6250);
    f = s.poll(6251);
    CHECK(f && f->waveform == Waveform::Quality);

    // Explicit full request.
    s.requestFull();
    CHECK_EQ(s.msUntilNextAction(7000), 0);
    f = s.poll(7000);
    CHECK(f && f->waveform == Waveform::Full);
}

static void testTouchDecoderProtocolB() {
    TouchDecoder d;
    d.configure({0, 1071}, {0, 1447}, {}, 1072, 1448);
    std::vector<TouchEvent> out;

    d.feed(EV_ABS, ABS_MT_SLOT, 0, out);
    d.feed(EV_ABS, ABS_MT_TRACKING_ID, 7, out);
    d.feed(EV_ABS, ABS_MT_POSITION_X, 100, out);
    d.feed(EV_ABS, ABS_MT_POSITION_Y, 200, out);
    d.feed(EV_SYN, SYN_REPORT, 0, out);
    CHECK_EQ(out.size(), 1);
    CHECK(out[0].type == TouchEvent::Type::Down && out[0].x == 100 && out[0].y == 200);

    d.feed(EV_ABS, ABS_MT_POSITION_X, 110, out);
    d.feed(EV_SYN, SYN_REPORT, 0, out);
    CHECK_EQ(out.size(), 2);
    CHECK(out[1].type == TouchEvent::Type::Move && out[1].x == 110 && out[1].y == 200);

    // A second finger in slot 1 is ignored.
    d.feed(EV_ABS, ABS_MT_SLOT, 1, out);
    d.feed(EV_ABS, ABS_MT_TRACKING_ID, 8, out);
    d.feed(EV_ABS, ABS_MT_POSITION_X, 900, out);
    d.feed(EV_SYN, SYN_REPORT, 0, out);
    CHECK_EQ(out.size(), 2);
    d.feed(EV_ABS, ABS_MT_TRACKING_ID, -1, out);
    d.feed(EV_SYN, SYN_REPORT, 0, out);
    CHECK_EQ(out.size(), 2);

    d.feed(EV_ABS, ABS_MT_SLOT, 0, out);
    d.feed(EV_ABS, ABS_MT_TRACKING_ID, -1, out);
    d.feed(EV_SYN, SYN_REPORT, 0, out);
    CHECK_EQ(out.size(), 3);
    CHECK(out[2].type == TouchEvent::Type::Up && out[2].x == 110);
    CHECK(!d.isDown());
}

static void testTouchDecoderBtnTouch() {
    TouchDecoder d;
    d.configure({0, 4095}, {0, 4095}, {true, true, false}, 1072, 1448);
    std::vector<TouchEvent> out;
    d.feed(EV_KEY, BTN_TOUCH, 1, out);
    d.feed(EV_ABS, ABS_X, 0, out);
    d.feed(EV_ABS, ABS_Y, 0, out);
    d.feed(EV_SYN, SYN_REPORT, 0, out);
    CHECK_EQ(out.size(), 1);
    // swap then mirror x: raw y=0 -> x=0 -> mirrored to 1071
    CHECK_EQ(out[0].x, 1071);
    CHECK_EQ(out[0].y, 0);
    d.feed(EV_KEY, BTN_TOUCH, 0, out);
    d.feed(EV_SYN, SYN_REPORT, 0, out);
    CHECK_EQ(out.size(), 2);
    CHECK(out[1].type == TouchEvent::Type::Up);
}

static void testConfig() {
    const char* path = "/tmp/palmkobo_test.ini";
    {
        std::ofstream f(path);
        f << "# comment\n"
             "rom = my.rom\n"
             "scale=4\n"
             "touch_swap_axes = 1\n"
             "touch_mirror_x = auto\n"
             "graffiti_echo = off\n"
             "session = /abs/session.img\n";
    }
    Config c;
    CHECK(!c.load(path));
    CHECK(c.rom == "my.rom");
    CHECK_EQ(c.scale, 4);
    CHECK(c.touchSwapAxes == Tristate::On);
    CHECK(c.touchMirrorX == Tristate::Auto);
    CHECK(!c.graffitiEcho);
    CHECK(c.resolve(c.rom) == "/tmp/my.rom");
    CHECK(c.resolve(c.session) == "/abs/session.img");

    CHECK(c.set("scale", "banana").has_value());
    CHECK(c.set("nonsense", "1").has_value());
    {
        std::ofstream f(path);
        f << "scale\n";
    }
    CHECK(c.load(path).has_value());
    std::remove(path);
}

static void testButtonBar() {
    const Layout l = Layout::compute(1072, 1448, 160, 160, 60);
    ButtonBar b;
    b.layout(l.bar);
    CHECK(!b.vertical());
    CHECK_EQ(b.count(), 7);
    for (int i = 0; i < b.count(); i++) {
        const Rect r = b.at(i).rect;
        CHECK(r.x >= l.bar.x && r.right() <= l.bar.right());
        CHECK_EQ(b.hit(r.x + r.w / 2, r.y + r.h / 2), i);
        CHECK_EQ(b.hit(r.x, r.y), i);
        CHECK_EQ(b.hit(r.right() - 1, r.bottom() - 1), i);
        if (i > 0) CHECK_EQ(r.x, b.at(i - 1).rect.right());  // no dead gaps between cells
        CHECK(r.w >= 96);  // at least 8 mm at 300 ppi
    }
    CHECK_EQ(b.hit(0, 0), -1);
    CHECK_EQ(b.hit(l.bar.x + 10, l.bar.y - 1), -1);
    // Like a Palm: Date Book, Address, scroll up/down, To Do, Memo. Menu at the far right.
    const BarAction order[] = {BarAction::App1, BarAction::App2, BarAction::Up,  BarAction::Down,
                               BarAction::App3, BarAction::App4, BarAction::Menu};
    for (int i = 0; i < 7; i++) CHECK(b.at(i).action == order[i]);
    CHECK_EQ(b.indexOf(BarAction::Menu), 6);
    CHECK_EQ(b.at(6).rect.right(), l.bar.right());

    Canvas c(1072, 1448);
    CHECK(b.draw(c) == l.bar);
    // White on black, and nothing but pure black and white (fast waveform).
    const Rect r0 = b.at(0).rect;
    int dark = 0, light = 0, gray = 0;
    for (int y = l.bar.y; y < l.bar.bottom(); y++)
        for (int x = l.bar.x; x < l.bar.right(); x++) {
            const uint8_t v = c.at(x, y);
            if (v != BLACK && v != WHITE) gray++;
            if (r0.contains(x, y)) (v == BLACK ? dark : light)++;
        }
    CHECK_EQ(gray, 0);
    CHECK(dark > light && light > 500);
    CHECK_EQ(c.at(r0.x + 1, r0.y + 1), BLACK);
    CHECK_EQ(c.at(l.bar.x - 1, l.bar.y), WHITE);  // nothing drawn outside the bar

    // Pressed = inverted: the disc fills with white.
    b.at(0).pressed = true;
    CHECK(b.draw(c, 0) == r0);
    int lightPressed = 0;
    for (int y = r0.y; y < r0.bottom(); y++)
        for (int x = r0.x; x < r0.right(); x++) lightPressed += c.at(x, y) == WHITE;
    CHECK(lightPressed > 2 * light);
    CHECK_EQ(c.at(r0.x + 1, r0.y + 1), BLACK);

    // Rail beside a tall screen: the same buttons stacked, menu at the bottom.
    const Layout t = Layout::compute(1072, 1448, 320, 480, 0);
    ButtonBar v;
    v.layout(t.bar);
    CHECK(v.vertical());
    CHECK_EQ(v.count(), 7);
    const BarAction railOrder[] = {BarAction::App1, BarAction::App2, BarAction::App3, BarAction::App4,
                                   BarAction::Up,   BarAction::Down, BarAction::Menu};
    for (int i = 0; i < 7; i++) {
        const Rect r = v.at(i).rect;
        CHECK(v.at(i).action == railOrder[i]);
        CHECK_EQ(r.x, t.bar.x);
        CHECK_EQ(r.w, t.bar.w);
        CHECK(r.h >= 96);
        CHECK_EQ(v.hit(r.x + r.w / 2, r.y + r.h / 2), i);
        if (i > 0) CHECK_EQ(r.y, v.at(i - 1).rect.bottom());
    }
    CHECK_EQ(v.at(6).rect.bottom(), t.bar.bottom());
    CHECK_EQ(v.hit(t.palm.right() - 1, t.bar.y + 10), -1);
    Canvas cv(1072, 1448);
    CHECK(v.draw(cv) == t.bar);
    CHECK_EQ(cv.at(t.palm.right() - 1, t.palm.y + 10), WHITE);  // the Palm area is untouched

    // An empty bar has no buttons and hits nothing.
    ButtonBar e;
    e.layout({});
    CHECK_EQ(e.count(), 0);
    CHECK_EQ(e.hit(5, 5), -1);
}

static void testMenu() {
    for (const Layout& l : {Layout::compute(1072, 1448, 160, 160, 60), Layout::compute(1072, 1448, 320, 480, 0)}) {
        Menu m;
        m.layout(l.palm);
        const Rect p = m.panel();
        // Inside the Palm area, so closing it only has to repaint the Palm screen.
        CHECK(p == p.intersected(l.palm));
        CHECK(!p.empty());
        CHECK_EQ(m.count(), 4);
        const MenuAction order[] = {MenuAction::Refresh, MenuAction::Power, MenuAction::Exit, MenuAction::Close};
        for (int i = 0; i < m.count(); i++) {
            const Rect r = m.at(i).rect;
            CHECK(m.at(i).action == order[i]);
            CHECK(r.h >= 130);
            CHECK(r == r.intersected(p));
            CHECK_EQ(m.hit(r.x + r.w / 2, r.y + r.h / 2), i);
            CHECK_EQ(m.hit(r.x, r.y), i);
            CHECK_EQ(m.hit(r.right() - 1, r.bottom() - 1), i);
            if (i > 0) CHECK_EQ(r.y, m.at(i - 1).rect.bottom());
        }
        // Border of the panel: on the menu, but no row. Everything else is outside.
        CHECK_EQ(m.hit(p.x, p.y), Menu::NONE);
        CHECK_EQ(m.hit(p.x - 1, p.y + 10), Menu::OUTSIDE);
        CHECK_EQ(m.hit(p.x + 10, p.bottom()), Menu::OUTSIDE);
        CHECK_EQ(m.hit(l.bar.x + l.bar.w / 2, l.bar.y + l.bar.h / 2), Menu::OUTSIDE);
        CHECK_EQ(m.hit(l.palm.x + 2, l.palm.y + 2), Menu::OUTSIDE);

        Canvas c(1072, 1448, 0x80);
        CHECK(m.draw(c) == p);
        CHECK_EQ(c.at(p.x - 1, p.y), 0x80);       // nothing outside the panel
        CHECK_EQ(c.at(p.x, p.bottom()), 0x80);
        CHECK_EQ(c.at(p.x, p.y), BLACK);          // frame
        const Rect r1 = m.at(1).rect;
        CHECK_EQ(c.at(r1.right() - 2, r1.y + 2), WHITE);
        m.draw(c, 1);
        CHECK_EQ(c.at(r1.right() - 2, r1.y + 2), BLACK);  // pressed row inverts
        CHECK_EQ(c.at(m.at(0).rect.right() - 2, m.at(0).rect.y + 2), WHITE);
        int gray = 0;
        for (int y = p.y; y < p.bottom(); y++)
            for (int x = p.x; x < p.right(); x++) gray += c.at(x, y) != BLACK && c.at(x, y) != WHITE;
        CHECK_EQ(gray, 0);
    }
}

static void testPgm() {
    const char* path = "/tmp/palmkobo_test.pgm";
    {
        std::ofstream f(path, std::ios::binary);
        f << "P5\n# made by test\n3 2\n255\n";
        const char px[6] = {0, 1, 2, 3, 4, 5};
        f.write(px, 6);
    }
    GrayImage img = loadPgm(path);
    CHECK(img.valid());
    CHECK_EQ(img.w, 3);
    CHECK_EQ(img.h, 2);
    CHECK_EQ(img.pixels[5], 5);
    std::remove(path);
    CHECK(!loadPgm("/nonexistent.pgm").valid());
}

static void testCanvas() {
    Canvas c(100, 100, WHITE);
    const uint8_t src[4] = {0, 255, 255, 0};  // 2x2 checker
    c.drawResampled(src, 2, 2, {10, 10, 20, 20});
    CHECK_EQ(c.at(10, 10), 0);
    CHECK_EQ(c.at(29, 10), 255);
    CHECK_EQ(c.at(29, 29), 0);
    c.line(0, 50, 99, 50, 3, BLACK, {0, 0, 50, 100});
    CHECK_EQ(c.at(49, 50), BLACK);
    CHECK_EQ(c.at(50, 50), WHITE);  // clipped
    std::vector<uint8_t> out;
    c.extract({10, 10, 2, 2}, out);
    CHECK_EQ(out.size(), 4);
    CHECK_EQ(out[0], 0);
}


static void testKeymap() {
    CHECK_EQ(usKeymap(KEY_A, false, false), 'a');
    CHECK_EQ(usKeymap(KEY_A, true, false), 'A');
    CHECK_EQ(usKeymap(KEY_A, false, true), 'A');
    CHECK_EQ(usKeymap(KEY_A, true, true), 'a');
    CHECK_EQ(usKeymap(KEY_1, false, true), '1');  // caps lock doesn't shift digits
    CHECK_EQ(usKeymap(KEY_1, true, false), '!');
    CHECK_EQ(usKeymap(KEY_SLASH, true, false), '?');
    CHECK_EQ(usKeymap(KEY_APOSTROPHE, true, false), '"');
    CHECK_EQ(usKeymap(KEY_SPACE, false, false), ' ');
    CHECK_EQ(usKeymap(KEY_F1, false, false), 0);
}

static void testKeyDecoder() {
    KeyDecoder d;
    std::vector<KeyAction> out;
    auto tap = [&](unsigned code) {
        d.feed(EV_KEY, code, 1, out);
        d.feed(EV_KEY, code, 0, out);
    };

    tap(KEY_H);
    d.feed(EV_KEY, KEY_LEFTSHIFT, 1, out);
    tap(KEY_I);
    tap(KEY_1);
    d.feed(EV_KEY, KEY_LEFTSHIFT, 0, out);
    tap(KEY_ENTER);
    CHECK_EQ(out.size(), 4);
    CHECK(out[0].kind == KeyAction::Kind::Char && out[0].chr == 'h');
    CHECK_EQ(out[1].chr, 'I');
    CHECK_EQ(out[2].chr, '!');
    CHECK_EQ(out[3].chr, palmchr::LINEFEED);

    // Caps lock toggles on press only.
    out.clear();
    tap(KEY_CAPSLOCK);
    tap(KEY_Q);
    tap(KEY_CAPSLOCK);
    tap(KEY_Q);
    CHECK_EQ(out.size(), 2);
    CHECK_EQ(out[0].chr, 'Q');
    CHECK_EQ(out[1].chr, 'q');

    // Autorepeat repeats characters and backspace.
    out.clear();
    d.feed(EV_KEY, KEY_BACKSPACE, 1, out);
    d.feed(EV_KEY, KEY_BACKSPACE, 2, out);
    d.feed(EV_KEY, KEY_BACKSPACE, 2, out);
    d.feed(EV_KEY, KEY_BACKSPACE, 0, out);
    CHECK_EQ(out.size(), 3);
    CHECK_EQ(out[2].chr, palmchr::BACKSPACE);

    // Ctrl suppresses letters.
    out.clear();
    d.feed(EV_KEY, KEY_LEFTCTRL, 1, out);
    tap(KEY_C);
    d.feed(EV_KEY, KEY_LEFTCTRL, 0, out);
    CHECK_EQ(out.size(), 0);

    // F1 and Page Down are Palm buttons: press + release, no repeat.
    out.clear();
    d.feed(EV_KEY, KEY_F1, 1, out);
    d.feed(EV_KEY, KEY_F1, 2, out);
    d.feed(EV_KEY, KEY_F1, 0, out);
    tap(KEY_PAGEDOWN);
    CHECK_EQ(out.size(), 4);
    CHECK(out[0].kind == KeyAction::Kind::ButtonPress && out[0].button == PalmKey::App1);
    CHECK(out[1].kind == KeyAction::Kind::ButtonRelease && out[1].button == PalmKey::App1);
    CHECK(out[2].kind == KeyAction::Kind::ButtonPress && out[2].button == PalmKey::RockerDown);

    // Esc = Menu icon tap, once per press even when held.
    out.clear();
    d.feed(EV_KEY, KEY_ESC, 1, out);
    d.feed(EV_KEY, KEY_ESC, 2, out);
    d.feed(EV_KEY, KEY_ESC, 0, out);
    tap(KEY_F7);
    CHECK_EQ(out.size(), 2);
    CHECK(out[0].kind == KeyAction::Kind::SilkTap && out[0].icon == SilkIcon::Menu);
    CHECK(out[1].icon == SilkIcon::Find);

    // F9 = full e-ink refresh
    out.clear();
    tap(KEY_F9);
    CHECK_EQ(out.size(), 1);
    CHECK(out[0].kind == KeyAction::Kind::Refresh);

    // Arrows map to Palm arrow characters; non-key events are ignored.
    out.clear();
    tap(KEY_LEFT);
    d.feed(EV_SYN, SYN_REPORT, 0, out);
    d.feed(EV_MSC, MSC_SCAN, 1234, out);
    CHECK_EQ(out.size(), 1);
    CHECK_EQ(out[0].chr, palmchr::LEFT);

    int x, y;
    silkIconPosition(SilkIcon::Menu, x, y);
    CHECK(x < 30 && y >= 190 && y < 220);
    silkIconPosition(SilkIcon::Calc, x, y);
    CHECK(x > 130 && y >= 160 && y < 190);

    // NR70: status bar icons along the bottom of the 320x480 screen; no blind Calc tap.
    CHECK(silkIconPositionNR70(SilkIcon::Home, x, y));
    CHECK(x < 24 && y >= 452 && y < 480);
    const int homeX = x;
    CHECK(silkIconPositionNR70(SilkIcon::Menu, x, y) && x > homeX);
    CHECK(silkIconPositionNR70(SilkIcon::Find, x, y) && x < 80);
    CHECK(!silkIconPositionNR70(SilkIcon::Calc, x, y));
}

static void testClassifyDevice() {
    constexpr size_t LB = sizeof(long) * 8;
    auto set = [](unsigned long* bits, unsigned b) { bits[b / LB] |= 1UL << (b % LB); };

    unsigned long ev[(EV_MAX + LB) / LB], abs[(ABS_MAX + LB) / LB], key[(KEY_MAX + LB) / LB];
    auto reset = [&]() {
        std::memset(ev, 0, sizeof(ev));
        std::memset(abs, 0, sizeof(abs));
        std::memset(key, 0, sizeof(key));
    };

    // Multitouch panel
    reset();
    set(ev, EV_ABS);
    set(abs, 0x35);
    set(abs, 0x36);
    CHECK(classifyDevice(ev, abs, key) == DeviceKind::Touch);

    // Keyboard that also advertises KEY_POWER is still a keyboard
    reset();
    set(ev, EV_KEY);
    for (unsigned k : {KEY_A, KEY_Z, KEY_Q, KEY_SPACE, KEY_ENTER, KEY_POWER}) set(key, k);
    CHECK(classifyDevice(ev, abs, key) == DeviceKind::Keyboard);

    // Power button
    reset();
    set(ev, EV_KEY);
    set(key, KEY_POWER);
    CHECK(classifyDevice(ev, abs, key) == DeviceKind::PowerKey);

    // Page-turn remote (only page keys) drives the rocker
    reset();
    set(ev, EV_KEY);
    set(key, KEY_PAGEUP);
    set(key, KEY_PAGEDOWN);
    CHECK(classifyDevice(ev, abs, key) == DeviceKind::Keyboard);

    // Something with keys but nothing we use
    reset();
    set(ev, EV_KEY);
    set(key, KEY_VOLUMEUP);
    CHECK(classifyDevice(ev, abs, key) == DeviceKind::Ignore);
}

static void testTouchTestExitRect() {
    // Clara BW: a quarter of the width, centred.
    const int W = 1072, H = 1448;
    const Rect r = touchTestExitRect(W, H);
    CHECK_EQ(r.w, 268);
    CHECK(r.contains(W / 2, H / 2));
    CHECK_EQ(r.x, W - r.right());
    CHECK_EQ(r.y, H - r.bottom());

    // Clear of the text above (ends near h/4 + 42p, p = 5) and below (h*3/4 - 12p).
    CHECK(r.y > H / 4 + 42 * 5 + 20);
    CHECK(r.bottom() < H * 3 / 4 - 12 * 5 - 20);

    // A tap at the physical centre lands inside whatever the touch transform is.
    for (int m = 0; m < 8; m++) {
        int x, y;
        mapTouch(2048, 2048, {0, 4095}, {0, 4095}, {(m & 1) != 0, (m & 2) != 0, (m & 4) != 0}, W, H, x, y);
        CHECK(r.contains(x, y));
    }

    // Outside: the corners used for the test itself.
    CHECK(!r.contains(30, 30));
    CHECK(!r.contains(W - 30, H - 30));
    CHECK(!r.contains(r.x - 1, H / 2));
    CHECK(!r.contains(W / 2, r.bottom()));
}

static void testIdlePolicy() {
    using A = IdlePolicy::Action;
    IdlePolicy::Params p;  // autosave 5 min, quiet 10 s, exit 15 min, warn 60 s
    const uint64_t MIN = 60000;
    IdlePolicy ip(p, 0);

    // No input yet: nothing to save, no exit for a while.
    CHECK(ip.poll(1000, false) == A::None);
    CHECK(ip.poll(10 * MIN, false) == A::None);

    // Typing keeps it awake and makes it dirty; no autosave while typing.
    for (uint64_t t = 10 * MIN; t < 16 * MIN; t += 2000) {
        ip.input(t);
        CHECK(ip.poll(t + 1, false) == A::None);
    }
    const uint64_t last = 16 * MIN - 2000;

    // Paused < 10 s: still nothing. After 10 s quiet (and > 5 min since save): autosave.
    CHECK(ip.poll(last + 5000, false) == A::None);
    CHECK(ip.poll(last + 10000, true) == A::None);  // busy (finger down) defers it
    CHECK(ip.poll(last + 10000, false) == A::Autosave);
    ip.saved(last + 10500);
    CHECK(ip.poll(last + 11000, false) == A::None);  // nothing new to save

    // Warning one minute before the idle exit, then exit.
    CHECK(ip.poll(last + 14 * MIN - 1, false) == A::None);
    CHECK(ip.poll(last + 14 * MIN, false) == A::ShowWarning);
    CHECK(ip.warningShown());
    CHECK(ip.poll(last + 14 * MIN + 1000, false) == A::None);
    CHECK(ip.poll(last + 15 * MIN, false) == A::Exit);

    // Input during the warning hides it and restarts the clock.
    IdlePolicy ip2(p, 0);
    CHECK(ip2.poll(14 * MIN, false) == A::ShowWarning);
    ip2.input(14 * MIN + 500);
    CHECK(ip2.poll(14 * MIN + 600, false) == A::HideWarning);
    CHECK(!ip2.warningShown());
    // (the dismissing tap counts as input, so it is autosaved once things are quiet)
    CHECK(ip2.poll(15 * MIN + 600, false) == A::Autosave);
    ip2.saved(15 * MIN + 600);
    CHECK(ip2.poll(15 * MIN + 700, false) == A::None);
    CHECK(ip2.poll(14 * MIN + 500 + 15 * MIN, false) == A::Exit);

    // Both features off.
    IdlePolicy off({0, 10000, 0, 60000}, 0);
    off.input(1);
    CHECK(off.poll(1000 * MIN, false) == A::None);
    CHECK(off.msUntilNext(5) == UINT64_MAX);

    // msUntilNext points at the warning time.
    IdlePolicy ip3(p, 0);
    CHECK(ip3.msUntilNext(0) == 14 * MIN);
}

int main() {
    testLayoutClaraBW();
    testLayoutClie320x480();
    testConvertFrameColor320x480();
    testToPalm();
    testMapTouch();
    testUnpack();
    testConvertFrame4bpp();
    testConvertFrameMarginAnd2bpp();
    testScheduler();
    testTouchDecoderProtocolB();
    testTouchDecoderBtnTouch();
    testConfig();
    testButtonBar();
    testMenu();
    testPgm();
    testCanvas();
    testKeymap();
    testKeyDecoder();
    testClassifyDevice();
    testTouchTestExitRect();
    testIdlePolicy();

    std::printf("%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
