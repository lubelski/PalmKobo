// palmkobo - Palm OS (CloudpilotEmu) front end for Kobo e-readers
// SPDX-License-Identifier: GPL-3.0-or-later

#include "Keyboard.h"

#include <linux/input.h>

namespace pk {

namespace {

struct KeyChars {
    unsigned code;
    char normal;
    char shifted;
};

// US layout. Letters are handled separately (caps lock applies to them only).
constexpr KeyChars US_SYMBOLS[] = {
    {KEY_1, '1', '!'},          {KEY_2, '2', '@'},          {KEY_3, '3', '#'},
    {KEY_4, '4', '$'},          {KEY_5, '5', '%'},          {KEY_6, '6', '^'},
    {KEY_7, '7', '&'},          {KEY_8, '8', '*'},          {KEY_9, '9', '('},
    {KEY_0, '0', ')'},          {KEY_MINUS, '-', '_'},      {KEY_EQUAL, '=', '+'},
    {KEY_LEFTBRACE, '[', '{'},  {KEY_RIGHTBRACE, ']', '}'}, {KEY_SEMICOLON, ';', ':'},
    {KEY_APOSTROPHE, '\'', '"'}, {KEY_GRAVE, '`', '~'},     {KEY_BACKSLASH, '\\', '|'},
    {KEY_COMMA, ',', '<'},      {KEY_DOT, '.', '>'},        {KEY_SLASH, '/', '?'},
    {KEY_SPACE, ' ', ' '},
    {KEY_KP0, '0', '0'},        {KEY_KP1, '1', '1'},        {KEY_KP2, '2', '2'},
    {KEY_KP3, '3', '3'},        {KEY_KP4, '4', '4'},        {KEY_KP5, '5', '5'},
    {KEY_KP6, '6', '6'},        {KEY_KP7, '7', '7'},        {KEY_KP8, '8', '8'},
    {KEY_KP9, '9', '9'},        {KEY_KPDOT, '.', '.'},      {KEY_KPPLUS, '+', '+'},
    {KEY_KPMINUS, '-', '-'},    {KEY_KPASTERISK, '*', '*'}, {KEY_KPSLASH, '/', '/'},
};

struct Letter {
    unsigned code;
    char c;
};

constexpr Letter US_LETTERS[] = {
    {KEY_A, 'a'}, {KEY_B, 'b'}, {KEY_C, 'c'}, {KEY_D, 'd'}, {KEY_E, 'e'}, {KEY_F, 'f'},
    {KEY_G, 'g'}, {KEY_H, 'h'}, {KEY_I, 'i'}, {KEY_J, 'j'}, {KEY_K, 'k'}, {KEY_L, 'l'},
    {KEY_M, 'm'}, {KEY_N, 'n'}, {KEY_O, 'o'}, {KEY_P, 'p'}, {KEY_Q, 'q'}, {KEY_R, 'r'},
    {KEY_S, 's'}, {KEY_T, 't'}, {KEY_U, 'u'}, {KEY_V, 'v'}, {KEY_W, 'w'}, {KEY_X, 'x'},
    {KEY_Y, 'y'}, {KEY_Z, 'z'},
};

}  // namespace

uint16_t usKeymap(unsigned code, bool shift, bool capsLock) {
    for (const auto& l : US_LETTERS)
        if (l.code == code) return static_cast<uint16_t>((shift != capsLock) ? l.c - 'a' + 'A' : l.c);
    for (const auto& k : US_SYMBOLS)
        if (k.code == code) return static_cast<uint16_t>(shift ? k.shifted : k.normal);
    return 0;
}

void silkIconPosition(SilkIcon icon, int& x, int& y) {
    // Centres of the four icons in the 160x60 silkscreen strip (below the 160x160 LCD),
    // matching CloudpilotEmu's silkscreen artwork.
    switch (icon) {
        case SilkIcon::Home:
            x = 13, y = 174;
            break;
        case SilkIcon::Menu:
            x = 13, y = 203;
            break;
        case SilkIcon::Calc:
            x = 146, y = 174;
            break;
        case SilkIcon::Find:
            x = 146, y = 203;
            break;
    }
}

bool silkIconPositionNR70(SilkIcon icon, int& x, int& y) {
    y = 466;
    switch (icon) {
        case SilkIcon::Home:
            x = 12;
            return true;
        case SilkIcon::Menu:
            x = 35;
            return true;
        case SilkIcon::Find:
            x = 58;
            return true;
        case SilkIcon::Calc:
            break;
    }
    return false;
}

void KeyDecoder::feed(unsigned type, unsigned code, int value, std::vector<KeyAction>& out) {
    if (type != EV_KEY) return;

    const bool down = value != 0;      // press or repeat
    const bool press = value == 1;     // initial press only
    const bool repeat = value == 2;

    // Modifiers
    switch (code) {
        case KEY_LEFTSHIFT:
            leftShift = down;
            return;
        case KEY_RIGHTSHIFT:
            rightShift = down;
            return;
        case KEY_LEFTCTRL:
            leftCtrl = down;
            return;
        case KEY_RIGHTCTRL:
            rightCtrl = down;
            return;
        case KEY_LEFTALT:
            leftAlt = down;
            return;
        case KEY_RIGHTALT:
            rightAlt = down;
            return;
        case KEY_CAPSLOCK:
            if (press) caps = !caps;
            return;
    }

    // Palm hardware buttons: forward press and release, ignore autorepeat
    // (Palm OS repeats held buttons itself).
    auto button = [&](PalmKey b) {
        if (press) out.push_back(KeyAction::press(b));
        else if (value == 0) out.push_back(KeyAction::release(b));
    };

    switch (code) {
        case KEY_F1:
            return button(PalmKey::App1);
        case KEY_F2:
            return button(PalmKey::App2);
        case KEY_F3:
            return button(PalmKey::App3);
        case KEY_F4:
            return button(PalmKey::App4);
        case KEY_PAGEUP:
            return button(PalmKey::RockerUp);
        case KEY_PAGEDOWN:
            return button(PalmKey::RockerDown);
    }

    if (!down) return;

    // Silkscreen icons: one tap per press.
    if (press) {
        switch (code) {
            case KEY_ESC:
            case KEY_F5:
            case KEY_COMPOSE:  // the "menu" key on PC keyboards
                out.push_back(KeyAction::tap(SilkIcon::Menu));
                return;
            case KEY_F6:
            case KEY_HOME:
                out.push_back(KeyAction::tap(SilkIcon::Home));
                return;
            case KEY_F7:
                out.push_back(KeyAction::tap(SilkIcon::Find));
                return;
            case KEY_F8:
                out.push_back(KeyAction::tap(SilkIcon::Calc));
                return;
            case KEY_F9:
                out.push_back(KeyAction::refresh());
                return;
        }
    } else if (!repeat) {
        return;
    }

    // Editing keys repeat when held.
    switch (code) {
        case KEY_ENTER:
        case KEY_KPENTER:
            out.push_back(KeyAction::character(palmchr::LINEFEED));
            return;
        case KEY_BACKSPACE:
            out.push_back(KeyAction::character(palmchr::BACKSPACE));
            return;
        case KEY_TAB:
            out.push_back(KeyAction::character(palmchr::TAB));
            return;
        case KEY_LEFT:
            out.push_back(KeyAction::character(palmchr::LEFT));
            return;
        case KEY_RIGHT:
            out.push_back(KeyAction::character(palmchr::RIGHT));
            return;
        case KEY_UP:
            out.push_back(KeyAction::character(palmchr::UP));
            return;
        case KEY_DOWN:
            out.push_back(KeyAction::character(palmchr::DOWN));
            return;
    }

    // Ctrl/Alt combinations have no Palm equivalent here; don't type letters for them.
    if (leftCtrl || rightCtrl || leftAlt || rightAlt) return;

    if (const uint16_t c = usKeymap(code, shift(), caps)) out.push_back(KeyAction::character(c));
}

}  // namespace pk
