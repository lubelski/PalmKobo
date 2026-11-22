// palmkobo - Palm OS (CloudpilotEmu) front end for Kobo e-readers
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Turns evdev key events from a USB/Bluetooth keyboard into Palm input:
//  * printable keys (US layout) and Enter/Backspace/Tab/arrows -> Palm characters
//  * F1-F4                      -> the four Palm app buttons
//  * Page Up / Page Down        -> the Palm scroll rocker
//  * Esc / F5 / F6 / F7 / F8    -> taps on the Menu / Home / Find / Calc silkscreen icons
//  * F9                         -> full e-ink refresh (palmkobo, not Palm)
//
// Pure logic (no Palm headers), so it is unit-tested on the host.

#ifndef PALMKOBO_KEYBOARD_H
#define PALMKOBO_KEYBOARD_H

#include <cstdint>
#include <vector>

namespace pk {

// Palm OS character codes (from Chars.h) used for non-printing keys.
namespace palmchr {
constexpr uint16_t BACKSPACE = 0x0008;
constexpr uint16_t TAB = 0x0009;
constexpr uint16_t LINEFEED = 0x000A;
constexpr uint16_t LEFT = 0x001C;
constexpr uint16_t RIGHT = 0x001D;
constexpr uint16_t UP = 0x001E;
constexpr uint16_t DOWN = 0x001F;
}  // namespace palmchr

enum class PalmKey { App1, App2, App3, App4, RockerUp, RockerDown };
enum class SilkIcon { Home, Menu, Calc, Find };

struct KeyAction {
    enum class Kind { Char, ButtonPress, ButtonRelease, SilkTap, Refresh } kind;
    uint16_t chr{0};
    PalmKey button{PalmKey::App1};
    SilkIcon icon{SilkIcon::Menu};

    static KeyAction character(uint16_t c) { return {Kind::Char, c, PalmKey::App1, SilkIcon::Menu}; }
    static KeyAction press(PalmKey b) { return {Kind::ButtonPress, 0, b, SilkIcon::Menu}; }
    static KeyAction release(PalmKey b) { return {Kind::ButtonRelease, 0, b, SilkIcon::Menu}; }
    static KeyAction tap(SilkIcon i) { return {Kind::SilkTap, 0, PalmKey::App1, i}; }
    static KeyAction refresh() { return {Kind::Refresh, 0, PalmKey::App1, SilkIcon::Menu}; }
};

class KeyDecoder {
   public:
    // Feed one evdev event. value: 0 = release, 1 = press, 2 = autorepeat.
    void feed(unsigned type, unsigned code, int value, std::vector<KeyAction>& out);

    bool shift() const { return leftShift || rightShift; }
    bool capsLock() const { return caps; }

   private:
    bool leftShift{false}, rightShift{false};
    bool leftCtrl{false}, rightCtrl{false};
    bool leftAlt{false}, rightAlt{false};
    bool caps{false};
};

// Printable character for a key on a US keyboard, or 0.
uint16_t usKeymap(unsigned code, bool shift, bool capsLock);

// Palm digitizer coordinates of the silkscreen icons (160x220 digitizer, as on
// the m500 and other 160x160 devices).
void silkIconPosition(SilkIcon icon, int& x, int& y);

// The same for the Sony Clie NR70 series (320x480, no silkscreen strip): centres of
// the Home, Menu and Find icons in the status bar that Palm OS draws along the bottom
// of the screen. Returns false for Calc, which only exists in the collapsible soft
// Graffiti area, where a blind tap could hit the running app instead.
bool silkIconPositionNR70(SilkIcon icon, int& x, int& y);

}  // namespace pk

#endif
