// palmkobo - Palm OS (CloudpilotEmu) front end for Kobo e-readers
// SPDX-License-Identifier: GPL-3.0-or-later
//
// The palmkobo menu: a popover that grows out of the menu button, over the Palm
// screen, with everything that is not a Palm key (refresh, the Palm's power
// button, exit). White on black like the button strip. Layout and hit-testing are
// pure, so they are unit-tested; App decides what the rows do.

#ifndef PALMKOBO_MENU_H
#define PALMKOBO_MENU_H

#include <algorithm>
#include <cstdlib>
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
    static constexpr int ROW_H = 132;   // 11 mm at 300 ppi
    static constexpr int WIDTH = 600;

    // Places the panel inside `within` (the Palm area), against the edge nearest
    // the menu button `anchor`: above the button when the controls are a strip
    // below the Palm (right edges aligned), to its left when they are a rail
    // (bottom edges aligned). A small pointer bridges the gap to the button.
    void layout(const Rect& within, const Rect& anchor, bool rail) {
        rows = {{MenuAction::Refresh, "REFRESH SCREEN", &icons::Refresh, {}},
                {MenuAction::Power, "PALM POWER BUTTON", &icons::Power, {}},
                {MenuAction::Exit, "EXIT TO KOBO", &icons::Exit, {}},
                {MenuAction::Close, "CLOSE MENU", &icons::Close, {}}};
        const int n = static_cast<int>(rows.size());
        const int w = std::min(within.w, WIDTH);
        const int rowH = std::clamp((within.h - 2 * BORDER) / n, 40, ROW_H);
        const int h = std::min(within.h, rowH * n + 2 * BORDER);
        int x, y;
        if (rail) {
            x = within.right() - w;
            y = std::min(anchor.bottom(), within.bottom()) - h;
        } else {
            x = std::min(anchor.right(), within.right()) - w;
            y = within.bottom() - h;
        }
        area = Rect{std::max(x, within.x), std::max(y, within.y), w, h}.intersected(within);
        for (int i = 0; i < n; i++)
            rows[i].rect = Rect{area.x + BORDER, area.y + BORDER + i * rowH, area.w - 2 * BORDER, rowH}
                               .intersected(area);

        // The pointer: a triangle from the panel's edge to just inside the button's cell.
        vertical = rail;
        const int reach = POINTER_INSET + (rail ? anchor.x - area.right() : anchor.y - area.bottom());
        if (anchor.empty() || reach <= 0) {
            pointer = {};
        } else if (rail) {
            const int cy = std::clamp(anchor.y + anchor.h / 2, area.y + 2 * reach, area.bottom() - 2 * reach);
            pointer = {area.right(), cy - reach, reach, 2 * reach};
        } else {
            const int cx = std::clamp(anchor.x + anchor.w / 2, area.x + 2 * reach, area.right() - 2 * reach);
            pointer = {cx - reach, area.bottom(), 2 * reach, reach};
        }
    }

    Rect panel() const { return area; }
    // Everything draw() touches: the panel and its pointer.
    Rect bounds() const { return area.united(pointer); }
    int count() const { return static_cast<int>(rows.size()); }
    const Row& at(int i) const { return rows[i]; }

    // Index of the row under (x, y), NONE on the panel's border, OUTSIDE elsewhere.
    int hit(int x, int y) const {
        if (!area.contains(x, y)) return OUTSIDE;
        for (int i = 0; i < count(); i++)
            if (rows[i].rect.contains(x, y)) return i;
        return NONE;
    }

    // Draws the popover, with one row shown pressed (inverted) if pressed >= 0.
    // Returns bounds().
    Rect draw(Canvas& canvas, int pressed = NONE) const {
        canvas.fill(area, ButtonBar::CHROME);
        canvas.fill({area.x + BORDER, area.y + BORDER, area.w - 2 * BORDER, area.h - 2 * BORDER}, BLACK);
        drawPointer(canvas);
        for (int i = 0; i < count(); i++) {
            const Rect& r = rows[i].rect;
            const uint8_t bg = i == pressed ? WHITE : BLACK, fg = i == pressed ? BLACK : WHITE;
            canvas.fill(r, bg);
            if (i > 0 && i != pressed && i - 1 != pressed) canvas.fill({r.x, r.y, r.w, 1}, ButtonBar::CHROME);
            const int scale = std::max(1, r.h * 3 / 5 / Icon::SIZE);
            const int iconW = Icon::SIZE * scale, pad = (r.h - iconW) / 2;
            drawIcon(canvas, *rows[i].icon, r.x + pad + iconW / 2, r.y + r.h / 2, scale, fg);
            const int textX = r.x + 2 * pad + iconW, textW = r.right() - pad - textX;
            int p = std::max(1, r.h / 4 / 7);
            while (p > 1 && ButtonBar::textWidth(rows[i].label, p) > textW) p--;
            ButtonBar::drawText(canvas, rows[i].label, textX + ButtonBar::textWidth(rows[i].label, p) / 2,
                                r.y + r.h / 2, p, fg);
        }
        return bounds();
    }

   private:
    static constexpr int BORDER = 2;
    static constexpr int POINTER_INSET = 6;  // how far the pointer reaches into the button's cell

    // A black triangle with a border-coloured outline, open towards the panel.
    void drawPointer(Canvas& canvas) const {
        if (pointer.empty()) return;
        const int len = vertical ? pointer.w : pointer.h;
        for (int i = 0; i < len; i++) {
            // i = distance from the panel edge; the triangle narrows towards its tip.
            const int half = len - i;
            if (vertical) {
                const int cy = pointer.y + pointer.h / 2, x = pointer.x + i;
                canvas.fill({x, cy - half, 1, 2 * half}, ButtonBar::CHROME);
                canvas.fill({x, cy - half + BORDER, 1, 2 * (half - BORDER)}, BLACK);
            } else {
                const int cx = pointer.x + pointer.w / 2, y = pointer.y + i;
                canvas.fill({cx - half, y, 2 * half, 1}, ButtonBar::CHROME);
                canvas.fill({cx - half + BORDER, y, 2 * (half - BORDER), 1}, BLACK);
            }
        }
        // Open the panel's border where the pointer joins it.
        if (vertical)
            canvas.fill({area.right() - BORDER, pointer.y + BORDER, BORDER, pointer.h - 2 * BORDER}, BLACK);
        else
            canvas.fill({pointer.x + BORDER, area.bottom() - BORDER, pointer.w - 2 * BORDER, BORDER}, BLACK);
    }

    Rect area, pointer;
    bool vertical{false};
    std::vector<Row> rows;
};

}  // namespace pk

#endif
