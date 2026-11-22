// palmkobo - Palm OS (CloudpilotEmu) front end for Kobo e-readers
// SPDX-License-Identifier: GPL-3.0-or-later
//
// The on-screen Palm hardware buttons: white icons on a black strip below the
// Graffiti area, or stacked in a rail beside a tall screen. Plus a menu button for
// everything that is not a Palm key. Text elsewhere uses the tiny built-in 5x7 font
// defined here, so we need no font files or FBInk text API.

#ifndef PALMKOBO_BUTTON_BAR_H
#define PALMKOBO_BUTTON_BAR_H

#include <cstdint>
#include <string>
#include <vector>

#include "Canvas.h"
#include "Geometry.h"

namespace pk {

enum class BarAction { App1, App2, App3, App4, Up, Down, Menu };

class ButtonBar {
   public:
    struct Button {
        BarAction action;
        Rect rect;  // touch cell; the drawn button is centred in it
        bool pressed{false};
    };

    // A bar that is taller than wide is laid out as a rail (buttons stacked).
    void layout(const Rect& bar);
    bool vertical() const { return area.h > area.w; }

    // Index of the button under (x, y), or -1.
    int hit(int x, int y) const;

    // Redraw one button (or all with index -1). Returns the dirty rect.
    Rect draw(Canvas& canvas, int index = -1) const;

    Button& at(int i) { return buttons[i]; }
    const Button& at(int i) const { return buttons[i]; }
    int count() const { return static_cast<int>(buttons.size()); }
    int indexOf(BarAction a) const;

    // Text rendering, exposed for the menu, messages and the touch-test screen.
    static void drawText(Canvas& canvas, const std::string& text, int cx, int cy, int pixel,
                         uint8_t color);
    static int textWidth(const std::string& text, int pixel);

    // Filled rectangle with rounded corners (radius = half the short side gives a
    // disc or a pill), clipped to clip.
    static void fillRound(Canvas& canvas, Rect r, int radius, uint8_t v, Rect clip);

   private:
    Rect area;
    std::vector<Button> buttons;
};

}  // namespace pk

#endif
