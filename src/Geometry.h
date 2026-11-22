// palmkobo - Palm OS (CloudpilotEmu) front end for Kobo e-readers
// Copyright (C) 2026  palmkobo contributors
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Pure geometry helpers: rectangles and the screen layout. No device or emulator
// dependencies, so this is unit-tested on the host.

#ifndef PALMKOBO_GEOMETRY_H
#define PALMKOBO_GEOMETRY_H

#include <algorithm>
#include <utility>

namespace pk {

struct Rect {
    int x{0}, y{0}, w{0}, h{0};

    bool empty() const { return w <= 0 || h <= 0; }
    int right() const { return x + w; }   // exclusive
    int bottom() const { return y + h; }  // exclusive

    bool contains(int px, int py) const {
        return !empty() && px >= x && py >= y && px < right() && py < bottom();
    }

    Rect united(const Rect& o) const {
        if (empty()) return o;
        if (o.empty()) return *this;
        const int nx = std::min(x, o.x), ny = std::min(y, o.y);
        return {nx, ny, std::max(right(), o.right()) - nx, std::max(bottom(), o.bottom()) - ny};
    }

    Rect intersected(const Rect& o) const {
        const int nx = std::max(x, o.x), ny = std::max(y, o.y);
        const int nr = std::min(right(), o.right()), nb = std::min(bottom(), o.bottom());
        if (nr <= nx || nb <= ny) return {};
        return {nx, ny, nr - nx, nb - ny};
    }

    bool operator==(const Rect& o) const {
        return x == o.x && y == o.y && w == o.w && h == o.h;
    }
};

// Where everything goes on the e-ink panel.
//
//   +-------------------------+
//   |   Palm LCD (WxH)*scale  |
//   +-------------------------+
//   |  silkscreen (Graffiti)  |
//   +-------------------------+
//   |  hardware button strip  |
//   +-------------------------+
//
// A screen that is tall for the panel instead gets the full height, with the same
// buttons stacked in a rail on its right (see compute()).
//
// The LCD and silkscreen form one contiguous block because the Palm digitizer
// covers both: palm y in [0, lcdH) is the LCD, [lcdH, lcdH + silkH) the silkscreen.
struct Layout {
    int screenW{0}, screenH{0};
    int palmW{0}, palmH{0}, silkH{0};
    int scale{1};

    Rect screen;  // whole panel
    Rect lcd;     // emulated LCD
    Rect silk;    // emulated silkscreen / Graffiti area (may be empty)
    Rect palm;    // lcd + silk (the digitizer)
    Rect bar;     // our controls: a strip below the Palm, or a rail beside it
    bool rail{false};

    static constexpr int GAP = 8;
    static constexpr int MIN_BAR = 80;
    static constexpr int MAX_BAR = 160;
    static constexpr int EDGE = 4;  // margin around the Palm block in the rail layout

    // forcedScale <= 0 picks the largest integer scale that fits.
    // Returns a layout with scale == 0 if nothing fits.
    static Layout compute(int screenW, int screenH, int palmW, int palmH, int silkH,
                          int forcedScale = 0) {
        Layout l;
        l.screenW = screenW;
        l.screenH = screenH;
        l.palmW = palmW;
        l.palmH = palmH;
        l.silkH = silkH;
        l.screen = {0, 0, screenW, screenH};

        const int digH = palmH + silkH;
        if (palmW <= 0 || digH <= 0) {
            l.scale = 0;
            return l;
        }

        const int byW = (screenW - 2 * GAP) / palmW;
        const int byH = (screenH - MIN_BAR - 3 * GAP) / digH;
        int scale = std::min(byW, byH);
        // Alternative for tall screens (a 320x480 Clie): the Palm block uses the whole
        // height and the controls go in a rail to its right. Chosen only when that
        // allows a larger scale than the strip below does.
        int railScale = std::min((screenW - 2 * EDGE - GAP - MIN_BAR) / palmW, (screenH - 2 * EDGE) / digH);
        if (forcedScale > 0) {
            scale = std::min(scale, forcedScale);
            railScale = std::min(railScale, forcedScale);
        }
        if (railScale > scale) {
            l.scale = railScale;
            l.rail = true;
            const int blockW = palmW * railScale, blockH = digH * railScale;
            const int railW = std::min(screenW - 2 * EDGE - GAP - blockW, MAX_BAR);
            const int x0 = (screenW - (blockW + GAP + railW)) / 2;
            const int y0 = (screenH - blockH) / 2;
            l.lcd = {x0, y0, blockW, palmH * railScale};
            l.silk = silkH > 0 ? Rect{x0, y0 + palmH * railScale, blockW, silkH * railScale} : Rect{};
            l.palm = {x0, y0, blockW, blockH};
            l.bar = {x0 + blockW + GAP, y0, railW, blockH};
            return l;
        }
        if (scale < 1) {
            l.scale = 0;
            return l;
        }
        l.scale = scale;

        const int blockW = palmW * scale, blockH = digH * scale;
        const int barH = std::clamp(screenH - blockH - 3 * GAP, MIN_BAR, MAX_BAR);
        const int x0 = (screenW - blockW) / 2;
        const int y0 = std::max(0, (screenH - (blockH + GAP + barH)) / 2);

        l.lcd = {x0, y0, blockW, palmH * scale};
        l.silk = silkH > 0 ? Rect{x0, y0 + palmH * scale, blockW, silkH * scale} : Rect{};
        l.palm = {x0, y0, blockW, blockH};
        // The bar is as wide as the Palm block, unless that is narrow (a 320x480
        // Clie forced to a small scale): then it uses the panel width, so the
        // buttons are not cramped.
        const int barW = blockW < screenW * 3 / 4 ? screenW - 2 * GAP : blockW;
        l.bar = {(screenW - barW) / 2, y0 + blockH + GAP, barW, barH};
        return l;
    }

    // Map a panel coordinate into Palm digitizer coordinates. With clamp=true,
    // points outside the digitizer are pinned to its edge (for drags that leave it).
    bool toPalm(int sx, int sy, int& px, int& py, bool clamp) const {
        if (scale <= 0) return false;
        if (!clamp && !palm.contains(sx, sy)) return false;
        // Floor division that also behaves for negative offsets.
        auto fdiv = [](int a, int b) { return a >= 0 ? a / b : -((-a + b - 1) / b); };
        px = std::clamp(fdiv(sx - palm.x, scale), 0, palmW - 1);
        py = std::clamp(fdiv(sy - palm.y, scale), 0, palmH + silkH - 1);
        return true;
    }

    // Panel rectangle covering a span of Palm pixels.
    Rect fromPalm(int px, int py, int pw, int ph) const {
        return {palm.x + px * scale, palm.y + py * scale, pw * scale, ph * scale};
    }
};

// Raw touch panel coordinates -> panel pixels. Kobo touch controllers are often
// mounted rotated relative to the display, so axes may need swapping and/or
// mirroring. Swap is applied first, mirroring afterwards (same convention as
// FBInk's touch_swap_axes / touch_mirror_x / touch_mirror_y device quirks).
struct TouchTransform {
    bool swapAxes{false};
    bool mirrorX{false};
    bool mirrorY{false};
};

struct AxisRange {
    int min{0};
    int max{0};
};

inline void mapTouch(int rx, int ry, AxisRange ax, AxisRange ay, const TouchTransform& t,
                     int screenW, int screenH, int& sx, int& sy) {
    if (t.swapAxes) {
        std::swap(rx, ry);
        std::swap(ax, ay);
    }
    auto norm = [](int v, AxisRange r, int size) {
        const int span = r.max - r.min;
        if (span <= 0) return std::clamp(v, 0, size - 1);
        const long long scaled = static_cast<long long>(v - r.min) * (size - 1) / span;
        return std::clamp(static_cast<int>(scaled), 0, size - 1);
    };
    sx = norm(rx, ax, screenW);
    sy = norm(ry, ay, screenH);
    if (t.mirrorX) sx = screenW - 1 - sx;
    if (t.mirrorY) sy = screenH - 1 - sy;
}

// The EXIT button of the touch-test screen. It is centred on the panel because
// the centre is the one spot that maps to itself when the touch axes are swapped
// or mirrored, so it stays reachable while touch is still misconfigured.
inline Rect touchTestExitRect(int screenW, int screenH) {
    const int w = screenW / 4, h = screenW / 6;
    return {(screenW - w) / 2, (screenH - h) / 2, w, h};
}

}  // namespace pk

#endif
