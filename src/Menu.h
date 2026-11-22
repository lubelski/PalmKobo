// palmkobo - Palm OS (CloudpilotEmu) front end for Kobo e-readers
// SPDX-License-Identifier: GPL-3.0-or-later
//
// The palmkobo menu: a panel drawn over the Palm screen with everything that is
// not a Palm key (refresh, the Palm's power button, exit). Layout and hit-testing
// are pure, so they are unit-tested; App decides what the rows do.

#ifndef PALMKOBO_MENU_H
#define PALMKOBO_MENU_H

#include <algorithm>
#include <vector>

#include "ButtonBar.h"
#include "Canvas.h"
#include "Geometry.h"
#include "Glyphs.h"

namespace pk {

enum class MenuAction { Refresh, Power, Exit, Close };

class Menu {
   public:
    struct Row {
        MenuAction action;
        const char* label;
        const Icon* icon;
        Rect rect;
    };

    static constexpr int OUTSIDE = -2;  // hit(): not on the panel at all
    static constexpr int NONE = -1;     // hit(): on the panel, between rows

    // Centres the panel inside `within` (the Palm area), so that closing the menu
    // only ever has to repaint the Palm screen.
    void layout(const Rect& within) {
        rows = {{MenuAction::Refresh, "REFRESH SCREEN", &icons::Refresh, {}},
                {MenuAction::Power, "PALM POWER BUTTON", &icons::Power, {}},
                {MenuAction::Exit, "EXIT TO KOBO", &icons::Exit, {}},
                {MenuAction::Close, "CLOSE MENU", &icons::Close, {}}};
        const int n = static_cast<int>(rows.size());
        border = std::max(4, within.w / 160);
        const int w = std::min(within.w - 4 * border, 760);
        const int rowH = std::clamp((within.h - 4 * border) / n, 40, 150);
        const int h = rowH * n + 2 * border;
        area = Rect{within.x + (within.w - w) / 2, within.y + (within.h - h) / 2, w, h}.intersected(within);
        for (int i = 0; i < n; i++)
            rows[i].rect = Rect{area.x + border, area.y + border + i * rowH, area.w - 2 * border, rowH}
                               .intersected(area);
    }

    Rect panel() const { return area; }
    int count() const { return static_cast<int>(rows.size()); }
    const Row& at(int i) const { return rows[i]; }

    // Index of the row under (x, y), NONE on the panel's border, OUTSIDE elsewhere.
    int hit(int x, int y) const {
        if (!area.contains(x, y)) return OUTSIDE;
        for (int i = 0; i < count(); i++)
            if (rows[i].rect.contains(x, y)) return i;
        return NONE;
    }

    // Draws the panel, with one row shown pressed (inverted) if pressed >= 0.
    // Pure black and white. Returns the panel rectangle.
    Rect draw(Canvas& canvas, int pressed = NONE) const {
        canvas.fill(area, BLACK);
        for (int i = 0; i < count(); i++) {
            const Rect& r = rows[i].rect;
            const uint8_t bg = i == pressed ? BLACK : WHITE, fg = i == pressed ? WHITE : BLACK;
            // Leave a black rule between rows.
            canvas.fill({r.x, r.y, r.w, r.h - (i + 1 < count() ? std::max(2, border / 2) : 0)}, bg);
            const int scale = std::max(1, r.h * 3 / 5 / Icon::SIZE);
            const int iconW = Icon::SIZE * scale, pad = (r.h - iconW) / 2;
            drawIcon(canvas, *rows[i].icon, r.x + pad + iconW / 2, r.y + r.h / 2, scale, fg);
            const int textX = r.x + 2 * pad + iconW, textW = r.right() - pad - textX;
            int p = std::max(1, r.h / 4 / 7);
            while (p > 1 && ButtonBar::textWidth(rows[i].label, p) > textW) p--;
            ButtonBar::drawText(canvas, rows[i].label, textX + ButtonBar::textWidth(rows[i].label, p) / 2,
                                r.y + r.h / 2, p, fg);
        }
        return area;
    }

   private:
    Rect area;
    int border{6};
    std::vector<Row> rows;
};

}  // namespace pk

#endif
