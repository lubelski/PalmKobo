// Renders mock-ups of the palmkobo screen (layout, silkscreen, controls, menu, a
// synthetic LCD frame and a Graffiti stroke) to PGM files, using the real drawing
// code. No ROM needed. Development aid only: build with `make preview`.
// SPDX-License-Identifier: GPL-3.0-or-later

#include <cstdio>
#include <string>
#include <vector>

#include "ButtonBar.h"
#include "Canvas.h"
#include "FrameConvert.h"
#include "Geometry.h"
#include "Menu.h"
#include "Pgm.h"

using namespace pk;

namespace {

void save(const Canvas& c, const std::string& path) {
    if (FILE* fp = std::fopen(path.c_str(), "wb")) {
        std::fprintf(fp, "P5\n%d %d\n255\n", c.w(), c.h());
        for (int y = 0; y < c.h(); y++) std::fwrite(c.row(y), 1, c.w(), fp);
        std::fclose(fp);
    }
}

// One mock-up: a Palm of the given size on a 1072x1448 panel.
void render(const std::string& path, const char* silkPath, int palmW, int palmH, int silkH, int pressed,
            bool menuOpen) {
    Canvas c(1072, 1448, WHITE);
    const Layout l = Layout::compute(c.w(), c.h(), palmW, palmH, silkH);
    c.frame({l.palm.x - 3, l.palm.y - 3, l.palm.w + 6, l.palm.h + 6}, 2, BLACK);

    const GrayImage art = loadPgm(silkPath);
    if (art.valid() && !l.silk.empty()) c.drawResampled(art.pixels.data(), art.w, art.h, l.silk);

    ButtonBar bar;
    bar.layout(l.bar);
    if (pressed >= 0) bar.at(pressed).pressed = true;
    bar.draw(c);

    // Synthetic 4bpp frame: title bar, 16-step gray ramp, a checker.
    const int bytes = palmW / 2;
    std::vector<uint8_t> fb(static_cast<size_t>(bytes) * palmH, 0x00);
    auto put = [&](int x, int y, int v) {
        uint8_t& b = fb[static_cast<size_t>(y) * bytes + x / 2];
        b = (x & 1) ? static_cast<uint8_t>((b & 0xf0) | v) : static_cast<uint8_t>((b & 0x0f) | (v << 4));
    };
    for (int y = 0; y < 14; y++)
        for (int x = 0; x < 64; x++) put(x, y, 15);
    for (int y = 40; y < 70; y++)
        for (int x = 0; x < palmW; x++) put(x, y, x * 16 / palmW);
    for (int y = 90; y < 150; y++)
        for (int x = 20; x < palmW - 20; x++) put(x, y, ((x / 8 + y / 8) & 1) ? 15 : 0);

    LcdFrame f;
    f.bpp = 4;
    f.lineWidth = static_cast<unsigned>(palmW);
    f.lines = static_cast<unsigned>(palmH);
    f.bytesPerLine = static_cast<unsigned>(bytes);
    f.firstDirtyLine = 0;
    f.lastDirtyLine = f.lines - 1;
    f.buffer = fb.data();
    convertFrame(f, c, l);

    if (!l.silk.empty()) {
        // A Graffiti-ish stroke in the letter area.
        const int s = l.scale;
        c.line(l.silk.x + 50 * s, l.silk.y + 45 * s, l.silk.x + 60 * s, l.silk.y + 12 * s, s, BLACK, l.silk);
        c.line(l.silk.x + 60 * s, l.silk.y + 12 * s, l.silk.x + 70 * s, l.silk.y + 45 * s, s, BLACK, l.silk);
    }

    if (menuOpen) {
        Menu menu;
        menu.layout(l.palm);
        menu.draw(c, 1);  // second row shown pressed
    }

    save(c, path);
    std::printf("%s: scale %d, lcd %dx%d at %d,%d, %s %dx%d at %d,%d\n", path.c_str(), l.scale, l.lcd.w,
                l.lcd.h, l.lcd.x, l.lcd.y, l.rail ? "rail" : "strip", l.bar.w, l.bar.h, l.bar.x, l.bar.y);
}

}  // namespace

int main(int argc, char** argv) {
    const char* silkPath = argc > 1 ? argv[1] : "assets/silkscreen.pgm";
    const std::string out = argc > 2 ? argv[2] : "preview.pgm";
    const std::string base = out.size() > 4 ? out.substr(0, out.size() - 4) : out;

    render(out, silkPath, 160, 160, 60, 1, false);               // m500: strip, one key pressed
    render(base + "-menu.pgm", silkPath, 160, 160, 60, -1, true);  // ... with the menu open
    render(base + "-rail.pgm", silkPath, 320, 480, 0, 4, false);   // 320x480 Clie: rail
    render(base + "-rail-menu.pgm", silkPath, 320, 480, 0, -1, true);
    return 0;
}
