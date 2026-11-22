// palmkobo - Palm OS (CloudpilotEmu) front end for Kobo e-readers
// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef PALMKOBO_APP_H
#define PALMKOBO_APP_H

#include <cstdint>
#include <deque>
#include <memory>
#include <string>
#include <vector>

#include "ButtonBar.h"
#include "Menu.h"
#include "Canvas.h"
#include "Config.h"
#include "Display.h"
#include "Geometry.h"
#include "IdlePolicy.h"
#include "Input.h"
#include "Keyboard.h"
#include "RefreshScheduler.h"

class Frame;
class CardImage;

namespace pk {

struct Options {
    std::string configPath;
    bool touchTest{false};
    std::string script;    // scripted input (host testing)
    int64_t maxMs{-1};     // quit after this long (host testing)
};

// One line of a test script: "<ms> down|move|up <x> <y>", "<ms> key <code> <value>",
// "<ms> power" or "<ms> quit".
struct ScriptedEvent {
    uint64_t at;
    enum class Kind { Touch, Key, Power, Quit } kind;
    TouchEvent touch;
    unsigned keyCode{0};
    int keyValue{0};
};

class App {
   public:
    App(Config config, Options options);
    ~App();

    int run();

   private:
    enum class Target { None, Palm, Bar, Menu, Swallow };

    bool startSession(std::string& error);
    void setupScreen();
    void drawSilkscreen();
    void showMessage(const std::vector<std::string>& lines);

    void mainLoop();
    void messageLoop();
    void touchTestLoop();

    void runEmulation(uint64_t now);
    void updateLcd(uint64_t now);
    void handleTouch(const TouchEvent& ev, uint64_t now);
    void sendPen(int sx, int sy, bool clamp);
    void fullRefresh();
    void barDown(int index, uint64_t now);
    void barUp(int index, uint64_t now);
    void echoTo(int sx, int sy, uint64_t now);
    void clearEcho(uint64_t now);

    void setupCard();
    void ejectCard();
    void runFor(uint64_t ms);
    void showStatus(const std::string& text);
    void hideStatus(uint64_t now);
    void openMenu(uint64_t now);
    void closeMenu(uint64_t now);
    void menuAction(MenuAction action, uint64_t now);
    void clearOverlay(uint64_t now);

    void scanInstallDir();
    void processInstalls(uint64_t now);
    bool saveSession();

    void flushDisplay(uint64_t now, bool drainAll);
    int waitTimeout(uint64_t now) const;
    void pollInput(int timeoutMs, InputBatch& batch, bool& quit);
    void handleKey(const KeyAction& key);

    Config config;
    Options options;

    std::unique_ptr<Display> display;
    Input input;
    Canvas canvas;
    Canvas silkBase;  // pristine silkscreen artwork, for erasing Graffiti echo
    Layout layout;
    bool softSilkNR70{false};  // Clie NR70: Palm OS draws the silkscreen inside the LCD
    ButtonBar bar;
    Menu menu;
    // Non-empty while the menu or a status message covers part of the Palm area.
    // The LCD is not redrawn underneath until it is cleared again.
    Rect overlay;
    bool menuOpen{false};
    int menuPressed{Menu::NONE};
    std::unique_ptr<RefreshScheduler> scheduler;
    std::unique_ptr<Frame> frame;

    bool sessionReady{false};
    bool quit{false};

    // Emulation clock (mirrors CloudpilotEmu's native MainLoop)
    uint64_t millisOffset{0};
    double clockEmu{0};
    uint64_t emuBusyUs{0};    // time spent inside the emulator core
    unsigned emuCatchUps{0};  // times the emulated clock fell > 500 ms behind
    uint32_t flushPipelineCounter{0};
    bool lastPowerOn{false};
    bool forceFullFrame{true};

    // Touch state
    Target target{Target::None};
    int targetButton{-1};
    uint64_t lastPenSent{0};
    int pendingPenX{-1}, pendingPenY{-1};
    bool penMovePending{false};
    bool echoActive{false};
    int echoX{0}, echoY{0};
    Rect echoDirty;

    // Virtual SD card
    CardImage* cardImage{nullptr};
    bool cardMounted{false};
    std::string cardFile;

    // App installation
    std::deque<std::string> pendingInstalls;
    bool installNeedsReset{false};
    std::unique_ptr<IdlePolicy> idle;
    void autosave(uint64_t now);

    // Scripted input (host testing)
    std::deque<ScriptedEvent> script;
    KeyDecoder scriptKeys;
    uint64_t startedAt{0};
};

uint64_t nowMs();

}  // namespace pk

#endif
