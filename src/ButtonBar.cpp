// palmkobo - Palm OS (CloudpilotEmu) front end for Kobo e-readers
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ButtonBar.h"

#include <algorithm>

#include "Glyphs.h"

namespace pk {

namespace {

// 5x7 glyphs, one byte per row, low 5 bits used (bit 4 = leftmost column).
struct Glyph {
    char c;
    uint8_t rows[7];
};

constexpr Glyph FONT[] = {
    {'A', {0x0e, 0x11, 0x11, 0x1f, 0x11, 0x11, 0x11}},
    {'B', {0x1e, 0x11, 0x11, 0x1e, 0x11, 0x11, 0x1e}},
    {'C', {0x0e, 0x11, 0x10, 0x10, 0x10, 0x11, 0x0e}},
    {'D', {0x1e, 0x11, 0x11, 0x11, 0x11, 0x11, 0x1e}},
    {'E', {0x1f, 0x10, 0x10, 0x1e, 0x10, 0x10, 0x1f}},
    {'F', {0x1f, 0x10, 0x10, 0x1e, 0x10, 0x10, 0x10}},
    {'G', {0x0e, 0x11, 0x10, 0x17, 0x11, 0x11, 0x0f}},
    {'H', {0x11, 0x11, 0x11, 0x1f, 0x11, 0x11, 0x11}},
    {'I', {0x0e, 0x04, 0x04, 0x04, 0x04, 0x04, 0x0e}},
    {'J', {0x07, 0x02, 0x02, 0x02, 0x02, 0x12, 0x0c}},
    {'K', {0x11, 0x12, 0x14, 0x18, 0x14, 0x12, 0x11}},
    {'Q', {0x0e, 0x11, 0x11, 0x11, 0x15, 0x12, 0x0d}},
    {'Z', {0x1f, 0x01, 0x02, 0x04, 0x08, 0x10, 0x1f}},
    {'/', {0x01, 0x01, 0x02, 0x04, 0x08, 0x10, 0x10}},
    {'_', {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x1f}},
    {'=', {0x00, 0x00, 0x1f, 0x00, 0x1f, 0x00, 0x00}},
    {'(', {0x02, 0x04, 0x08, 0x08, 0x08, 0x04, 0x02}},
    {')', {0x08, 0x04, 0x02, 0x02, 0x02, 0x04, 0x08}},
    {'\'', {0x04, 0x04, 0x08, 0x00, 0x00, 0x00, 0x00}},
    {'L', {0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x1f}},
    {'M', {0x11, 0x1b, 0x15, 0x15, 0x11, 0x11, 0x11}},
    {'N', {0x11, 0x19, 0x15, 0x13, 0x11, 0x11, 0x11}},
    {'O', {0x0e, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0e}},
    {'P', {0x1e, 0x11, 0x11, 0x1e, 0x10, 0x10, 0x10}},
    {'R', {0x1e, 0x11, 0x11, 0x1e, 0x14, 0x12, 0x11}},
    {'S', {0x0f, 0x10, 0x10, 0x0e, 0x01, 0x01, 0x1e}},
    {'T', {0x1f, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04}},
    {'U', {0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0e}},
    {'V', {0x11, 0x11, 0x11, 0x11, 0x11, 0x0a, 0x04}},
    {'W', {0x11, 0x11, 0x11, 0x15, 0x15, 0x15, 0x0a}},
    {'X', {0x11, 0x11, 0x0a, 0x04, 0x0a, 0x11, 0x11}},
    {'Y', {0x11, 0x11, 0x0a, 0x04, 0x04, 0x04, 0x04}},
    {'0', {0x0e, 0x11, 0x13, 0x15, 0x19, 0x11, 0x0e}},
    {'1', {0x04, 0x0c, 0x04, 0x04, 0x04, 0x04, 0x0e}},
    {'2', {0x0e, 0x11, 0x01, 0x02, 0x04, 0x08, 0x1f}},
    {'3', {0x1f, 0x02, 0x04, 0x02, 0x01, 0x11, 0x0e}},
    {'4', {0x02, 0x06, 0x0a, 0x12, 0x1f, 0x02, 0x02}},
    {'5', {0x1f, 0x10, 0x1e, 0x01, 0x01, 0x11, 0x0e}},
    {'6', {0x06, 0x08, 0x10, 0x1e, 0x11, 0x11, 0x0e}},
    {'7', {0x1f, 0x01, 0x02, 0x04, 0x08, 0x08, 0x08}},
    {'8', {0x0e, 0x11, 0x11, 0x0e, 0x11, 0x11, 0x0e}},
    {'9', {0x0e, 0x11, 0x11, 0x0f, 0x01, 0x02, 0x0c}},
    {'?', {0x0e, 0x11, 0x01, 0x02, 0x04, 0x00, 0x04}},
    {'.', {0x00, 0x00, 0x00, 0x00, 0x00, 0x0c, 0x0c}},
    {',', {0x00, 0x00, 0x00, 0x00, 0x0c, 0x04, 0x08}},
    {':', {0x00, 0x0c, 0x0c, 0x00, 0x0c, 0x0c, 0x00}},
    {'-', {0x00, 0x00, 0x00, 0x1f, 0x00, 0x00, 0x00}},
    {'^', {0x04, 0x0e, 0x15, 0x04, 0x04, 0x04, 0x04}},  // up arrow
    {'v', {0x04, 0x04, 0x04, 0x04, 0x15, 0x0e, 0x04}},  // down arrow
    {' ', {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}},
};

const Glyph* findGlyph(char c) {
    for (const auto& g : FONT)
        if (g.c == c) return &g;
    return nullptr;
}

constexpr int GLYPH_W = 5, GLYPH_H = 7, GLYPH_ADVANCE = 6;
constexpr int RING_LINE = 2;  // outline of a key at rest, in pixels

}  // namespace

int ButtonBar::textWidth(const std::string& text, int pixel) {
    if (text.empty()) return 0;
    return (static_cast<int>(text.size()) * GLYPH_ADVANCE - 1) * pixel;
}

void ButtonBar::drawText(Canvas& canvas, const std::string& text, int cx, int cy, int pixel,
                         uint8_t color) {
    int x = cx - textWidth(text, pixel) / 2;
    const int y = cy - GLYPH_H * pixel / 2;
    for (char c : text) {
        const Glyph* g = findGlyph(c);
        if (g) {
            for (int r = 0; r < GLYPH_H; r++)
                for (int col = 0; col < GLYPH_W; col++)
                    if (g->rows[r] & (0x10 >> col))
                        canvas.fill({x + col * pixel, y + r * pixel, pixel, pixel}, color);
        }
        x += GLYPH_ADVANCE * pixel;
    }
}

void ButtonBar::fillRound(Canvas& canvas, Rect r, int radius, uint8_t v, Rect clip) {
    radius = std::max(0, std::min({radius, r.w / 2, r.h / 2}));
    for (int y = 0; y < r.h; y++) {
        // How far the corner arc eats into this row.
        int inset = 0;
        const int dy = y < radius ? radius - y : (y >= r.h - radius ? y - (r.h - radius - 1) : 0);
        if (dy > 0) {
            int dx = 0;
            while (dx < radius && (2 * (radius - dx) - 1) * (2 * (radius - dx) - 1) + (2 * dy - 1) * (2 * dy - 1) >
                                      4 * radius * radius)
                dx++;
            inset = dx;
        }
        canvas.fill(Rect{r.x + inset, r.y + y, r.w - 2 * inset, 1}.intersected(clip), v);
    }
}

void ButtonBar::layout(const Rect& bar) {
    area = bar;
    buttons.clear();
    if (bar.empty()) return;

    if (vertical()) {
        // Rail: the Palm keys from the top, the menu button at the bottom.
        const BarAction order[] = {BarAction::App1, BarAction::App2, BarAction::App3, BarAction::App4,
                                   BarAction::Up,   BarAction::Down, BarAction::Menu};
        const int n = 7;
        for (int i = 0; i < n; i++) {
            const int y0 = bar.y + bar.h * i / n, y1 = bar.y + bar.h * (i + 1) / n;
            buttons.push_back({order[i], {bar.x, y0, bar.w, y1 - y0}});
        }
        return;
    }

    // Strip, arranged like the front of a Palm: two application keys, the scroll
    // keys in the middle, two more application keys. The menu button sits apart at
    // the right end.
    const BarAction order[] = {BarAction::App1, BarAction::App2, BarAction::Up,
                               BarAction::Down, BarAction::App3, BarAction::App4};
    const int menuW = std::min(bar.w / 7, bar.h + bar.h / 4);
    const int keysW = bar.w - menuW;
    for (int i = 0; i < 6; i++) {
        const int x0 = bar.x + keysW * i / 6, x1 = bar.x + keysW * (i + 1) / 6;
        buttons.push_back({order[i], {x0, bar.y, x1 - x0, bar.h}});
    }
    buttons.push_back({BarAction::Menu, {bar.x + keysW, bar.y, menuW, bar.h}});
}

int ButtonBar::hit(int x, int y) const {
    for (int i = 0; i < count(); i++)
        if (buttons[i].rect.contains(x, y)) return i;
    return -1;
}

int ButtonBar::indexOf(BarAction a) const {
    for (int i = 0; i < count(); i++)
        if (buttons[i].action == a) return i;
    return -1;
}

Rect ButtonBar::draw(Canvas& canvas, int index) const {
    if (index < 0) {
        canvas.fill(area, BLACK);
        for (int i = 0; i < count(); i++) draw(canvas, i);
        return area;
    }

    const Button& b = buttons[index];
    const Rect cell = b.rect;
    canvas.fill(cell, BLACK);

    const uint8_t fg = b.pressed ? BLACK : WHITE;
    const int cx = cell.x + cell.w / 2, cy = cell.y + cell.h / 2;
    const int across = vertical() ? area.w : area.h;  // short side of the bar
    const int d = std::max(8, std::min({across - std::max(8, across / 8), cell.w - 4, cell.h - 4}));
    // A thin gray outline at rest; a pressed key fills white instead.
    const int line = RING_LINE;
    const uint8_t ring = b.pressed ? WHITE : CHROME;
    // Largest whole-number icon scale whose diagonal still fits inside the ring.
    auto iconScale = [](int inner) { return std::max(1, inner * 100 / (Icon::SIZE * 141)); };

    switch (b.action) {
        case BarAction::App1:
        case BarAction::App2:
        case BarAction::App3:
        case BarAction::App4: {
            static const Icon* const art[] = {&icons::DateBook, &icons::Address, &icons::ToDo, &icons::Memo};
            const Rect disc{cx - d / 2, cy - d / 2, d, d};
            if (RING_LINE > 0 || b.pressed) fillRound(canvas, disc, d / 2, ring, cell);
            if (!b.pressed)
                fillRound(canvas, {disc.x + line, disc.y + line, d - 2 * line, d - 2 * line}, d / 2 - line,
                          BLACK, cell);
            drawIcon(canvas, *art[static_cast<int>(b.action)], cx, cy, iconScale(d - 2 * line), fg);
        } break;

        case BarAction::Up:
        case BarAction::Down: {
            // The two scroll keys share one pill; each cell draws its half of it.
            const bool up = b.action == BarAction::Up;
            Rect pill;
            if (vertical()) {
                const int len = std::min(2 * cell.h - 8, 3 * d);
                const int mid = up ? cell.bottom() : cell.y;
                pill = {cx - d / 2, mid - len / 2, d, len};
            } else {
                const int len = std::min(2 * cell.w - 8, 3 * d);
                const int mid = up ? cell.right() : cell.x;
                pill = {mid - len / 2, cy - d / 2, len, d};
            }
            if (RING_LINE > 0 || b.pressed) fillRound(canvas, pill, d / 2, ring, cell);
            if (!b.pressed)
                fillRound(canvas, {pill.x + line, pill.y + line, pill.w - 2 * line, pill.h - 2 * line},
                          d / 2 - line, BLACK, cell);
            // A thin divider between the halves.
            const Rect half = pill.intersected(cell);
            if (vertical())
                canvas.fill({half.x, up ? half.bottom() - 1 : half.y, half.w, 1}, b.pressed ? BLACK : CHROME);
            else
                canvas.fill({up ? half.right() - 1 : half.x, half.y, 1, half.h}, b.pressed ? BLACK : CHROME);
            drawIcon(canvas, up ? icons::Up : icons::Down, half.x + half.w / 2, half.y + half.h / 2,
                     iconScale(d - 2 * line), fg);
        } break;

        case BarAction::Menu: {
            // Square, to set it apart from the round Palm keys.
            const int side = d * 4 / 5;
            const Rect box{cx - side / 2, cy - side / 2, side, side};
            if (RING_LINE > 0 || b.pressed) fillRound(canvas, box, side / 6, ring, cell);
            if (!b.pressed)
                fillRound(canvas, {box.x + line, box.y + line, side - 2 * line, side - 2 * line},
                          side / 6 - line, BLACK, cell);
            drawIcon(canvas, icons::Menu, cx, cy, std::max(1, (side - 12) / Icon::SIZE), fg);
        } break;
    }

    return cell;
}

}  // namespace pk
