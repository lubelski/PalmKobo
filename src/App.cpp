// palmkobo - Palm OS (CloudpilotEmu) front end for Kobo e-readers
// SPDX-License-Identifier: GPL-3.0-or-later

#include "App.h"

#include <dirent.h>
#include <linux/input.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>

// CloudpilotEmu core
#include "ButtonEvent.h"
#include "DbInstaller.h"
#include "EmHAL.h"
#include "EmSession.h"
#include "EmSystemState.h"
#include "ExternalStorage.h"
#include "CardImage.h"
#include "KeyboardEvent.h"
#include "FileUtil.h"
#include "Frame.h"
#include "PenEvent.h"
#include "ScreenDimensions.h"
#include "SessionImage.h"
#include "util.h"

#include "CardFs.h"
#include "FrameConvert.h"
#include "Pgm.h"

namespace pk {

namespace {

volatile std::sig_atomic_t gSignalQuit = 0;
void onSignal(int) { gSignalQuit = 1; }

constexpr uint32_t FLUSH_PIPELINE_FRAMES = 3;  // as in CloudpilotEmu's MainLoop
constexpr uint64_t EXIT_CONFIRM_MS = 3000;
constexpr const char* CARD_KEY = "palmkobo-card";

bool fileExists(const std::string& path) {
    struct stat st;
    return stat(path.c_str(), &st) == 0 && S_ISREG(st.st_mode);
}

std::string upper(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
    return s;
}

bool isInstallable(const std::string& name) {
    const size_t dot = name.find_last_of('.');
    if (dot == std::string::npos) return false;
    const std::string ext = upper(name.substr(dot + 1));
    return ext == "PRC" || ext == "PDB" || ext == "PQA";
}

ButtonEvent::Button palmButton(BarAction a) {
    switch (a) {
        case BarAction::App1:
            return ButtonEvent::Button::app1;
        case BarAction::App2:
            return ButtonEvent::Button::app2;
        case BarAction::App3:
            return ButtonEvent::Button::app3;
        case BarAction::App4:
            return ButtonEvent::Button::app4;
        case BarAction::Up:
            return ButtonEvent::Button::rockerUp;
        case BarAction::Down:
            return ButtonEvent::Button::rockerDown;
        case BarAction::Power:
            return ButtonEvent::Button::power;
        default:
            return ButtonEvent::Button::invalid;
    }
}

std::deque<ScriptedEvent> loadScript(const std::string& path) {
    std::deque<ScriptedEvent> out;
    std::ifstream in(path);
    std::string line;
    while (std::getline(in, line)) {
        if (line.empty() || line[0] == '#') continue;
        std::istringstream ss(line);
        uint64_t at;
        std::string what;
        if (!(ss >> at >> what)) continue;
        ScriptedEvent ev{at, ScriptedEvent::Kind::Touch, {TouchEvent::Type::Down, 0, 0}};
        if (what == "power") {
            ev.kind = ScriptedEvent::Kind::Power;
        } else if (what == "quit") {
            ev.kind = ScriptedEvent::Kind::Quit;
        } else if (what == "key") {
            // "<ms> key CODE VALUE": raw evdev key code (linux/input.h), 1 press, 0 release
            ev.kind = ScriptedEvent::Kind::Key;
            ss >> ev.keyCode >> ev.keyValue;
        } else {
            int x = 0, y = 0;
            ss >> x >> y;
            ev.touch.x = x;
            ev.touch.y = y;
            if (what == "down") ev.touch.type = TouchEvent::Type::Down;
            else if (what == "move") ev.touch.type = TouchEvent::Type::Move;
            else if (what == "up") ev.touch.type = TouchEvent::Type::Up;
            else continue;
        }
        out.push_back(ev);
    }
    return out;
}

}  // namespace

uint64_t nowMs() {
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                     std::chrono::steady_clock::now().time_since_epoch())
                                     .count());
}

// Prints the input log lines and pushes them to disk, so that they are still in
// palmkobo.log if the Kobo has to be hard-rebooted afterwards.
static void printLog(const InputBatch& batch) {
    if (batch.log.empty()) return;
    for (const auto& line : batch.log) std::cout << line;
    std::cout << std::flush;
    fsync(STDOUT_FILENO);
}

App::App(Config c, Options o) : config(std::move(c)), options(std::move(o)) {}

App::~App() {
    input.close();
    if (display) display->close();
}

// ---------------------------------------------------------------------------
// Startup

int App::run() {
    std::signal(SIGINT, onSignal);
    std::signal(SIGTERM, onSignal);
    startedAt = nowMs();

    if (!options.script.empty()) script = loadScript(options.script);

    display = makeDisplay(config);
    if (std::string err = display->open(); !err.empty()) {
        std::cerr << "palmkobo: " << err << std::endl;
        return 1;
    }
    std::cout << display->describe() << std::endl;

    TouchTransform t = display->touchDefaults();
    if (config.touchSwapAxes != Tristate::Auto) t.swapAxes = config.touchSwapAxes == Tristate::On;
    if (config.touchMirrorX != Tristate::Auto) t.mirrorX = config.touchMirrorX == Tristate::On;
    if (config.touchMirrorY != Tristate::Auto) t.mirrorY = config.touchMirrorY == Tristate::On;
    std::cout << input.open(t, display->width(), display->height(), config.grabInput)
              << "touch transform: swap=" << t.swapAxes << " mirror_x=" << t.mirrorX
              << " mirror_y=" << t.mirrorY << std::endl;

    canvas.resize(display->width(), display->height(), WHITE);
    scheduler = std::make_unique<RefreshScheduler>(
        canvas.bounds(),
        RefreshScheduler::Params{static_cast<uint64_t>(config.penIntervalMs),
                                 static_cast<uint64_t>(config.idleDelayMs),
                                 static_cast<uint64_t>(config.maxLatencyMs),
                                 config.fullRefreshEvery, 1500});

    if (options.touchTest) {
        touchTestLoop();
        return 0;
    }

    std::string error;
    if (!startSession(error)) {
        std::cerr << "palmkobo: " << error << std::endl;
        showMessage({"PALMKOBO CANNOT START", "", error, "",
                     "PRESS THE POWER BUTTON", "OR TAP ANYWHERE TO EXIT"});
        messageLoop();
        return 1;
    }

    setupScreen();
    setupCard();
    scanInstallDir();
    const uint64_t loopStarted = nowMs();
    mainLoop();
    // How hard the emulator works on this device: near 100% busy, or many catch-ups,
    // means the Palm runs slower than real time.
    const uint64_t loopMs = std::max<uint64_t>(1, nowMs() - loopStarted);
    std::cout << "emulation: busy " << emuBusyUs / 10 / loopMs << "% of " << loopMs / 1000
              << " s, " << emuCatchUps << " catch-ups" << std::endl;

    ejectCard();
    if (!saveSession()) std::cerr << "palmkobo: WARNING: failed to save session" << std::endl;
    return 0;
}

bool App::startSession(std::string& error) {
    const std::string sessionPath = config.resolve(config.session);
    const std::string romPath = config.resolve(config.rom);
    const std::optional<std::string> device =
        config.device.empty() ? std::nullopt : std::optional<std::string>(config.device);

    bool ok = false;
    if (fileExists(sessionPath)) {
        std::cout << "restoring " << sessionPath << std::endl;
        ok = util::initializeSession(sessionPath, device);
        if (!ok) {
            // Keep the broken image for inspection, then start fresh from the ROM.
            std::rename(sessionPath.c_str(), (sessionPath + ".bad").c_str());
            std::cerr << "palmkobo: session restore failed, booting ROM instead" << std::endl;
        }
    }

    if (!ok) {
        if (!fileExists(romPath)) {
            error = "ROM NOT FOUND: " + upper(config.rom);
            return false;
        }
        std::cout << "booting " << romPath << std::endl;
        if (!util::initializeSession(romPath, device)) {
            error = "ROM NOT SUPPORTED" + (device ? " FOR " + upper(*device) : std::string());
            return false;
        }
    }

    if (!config.hotsyncName.empty()) gSession->SetHotsyncUserName(config.hotsyncName);

    frame = std::make_unique<Frame>(320 * 480 * 4);  // same size CloudpilotEmu uses
    millisOffset = nowMs();
    clockEmu = 0;
    sessionReady = true;
    return true;
}

void App::setupScreen() {
    ScreenDimensions dims(gSession->GetDevice().GetScreenDimensions());
    layout = Layout::compute(canvas.w(), canvas.h(), static_cast<int>(dims.Width()),
                             static_cast<int>(dims.Height()), static_cast<int>(dims.SilkscreenHeight()),
                             config.scale);
    softSilkNR70 = gSession->GetDevice().GetIDString() == "NR70";
    std::cout << "layout: palm " << layout.palmW << "x" << layout.palmH << "+" << layout.silkH
              << " at scale " << layout.scale << ", lcd @" << layout.lcd.x << "," << layout.lcd.y
              << ", bar " << layout.bar.w << "x" << layout.bar.h << std::endl;

    canvas.fill(canvas.bounds(), WHITE);
    canvas.frame({layout.palm.x - 3, layout.palm.y - 3, layout.palm.w + 6, layout.palm.h + 6}, 2, BLACK);
    drawSilkscreen();
    bar.layout(layout.bar);
    bar.draw(canvas);

    forceFullFrame = true;
    scheduler->requestFull();
}

void App::drawSilkscreen() {
    if (layout.silk.empty()) return;

    const GrayImage art = loadPgm(config.resolve(config.silkscreen));
    if (art.valid()) {
        canvas.drawResampled(art.pixels.data(), art.w, art.h, layout.silk);
    } else {
        // No artwork: plain Graffiti box so the area is at least recognizable.
        std::cerr << "palmkobo: silkscreen artwork missing, drawing a plain one" << std::endl;
        canvas.fill(layout.silk, WHITE);
        const int s = layout.scale;
        const Rect graffiti{layout.silk.x + layout.silk.w / 5, layout.silk.y + 2 * s,
                            layout.silk.w * 3 / 5, layout.silk.h - 4 * s};
        canvas.frame(graffiti, std::max(2, s / 2), BLACK);
        canvas.fill({graffiti.x + graffiti.w * 5 / 8, graffiti.y, std::max(2, s / 2), graffiti.h}, BLACK);
    }

    silkBase.resize(layout.silk.w, layout.silk.h);
    for (int y = 0; y < layout.silk.h; y++)
        std::copy(canvas.row(layout.silk.y + y) + layout.silk.x,
                  canvas.row(layout.silk.y + y) + layout.silk.right(), silkBase.row(y));
}

void App::showMessage(const std::vector<std::string>& lines) {
    canvas.fill(canvas.bounds(), WHITE);
    const int pixel = std::max(2, canvas.w() / 200);
    const int lineH = 11 * pixel;
    int y = canvas.h() / 2 - static_cast<int>(lines.size()) * lineH / 2;
    for (const auto& l : lines) {
        // Shrink long lines to fit the width.
        int p = pixel;
        while (p > 1 && ButtonBar::textWidth(l, p) > canvas.w() - 40) p--;
        ButtonBar::drawText(canvas, l, canvas.w() / 2, y, p, BLACK);
        y += lineH;
    }
    display->present(canvas, canvas.bounds(), Waveform::Full);
}

// ---------------------------------------------------------------------------
// Loops

void App::pollInput(int timeoutMs, InputBatch& batch, bool& quitNow) {
    if (options.maxMs >= 0 && nowMs() - startedAt >= static_cast<uint64_t>(options.maxMs)) quitNow = true;
    if (gSignalQuit) quitNow = true;

    if (!options.script.empty()) {
        // Scripted (host) mode: sleep, then release events that are due.
        if (timeoutMs > 0) {
            const uint64_t elapsed = nowMs() - startedAt;
            if (!script.empty() && script.front().at > elapsed)
                timeoutMs = static_cast<int>(std::min<uint64_t>(timeoutMs, script.front().at - elapsed));
            struct timespec ts{timeoutMs / 1000, (timeoutMs % 1000) * 1000000L};
            nanosleep(&ts, nullptr);
        }
        const uint64_t elapsed = nowMs() - startedAt;
        while (!script.empty() && script.front().at <= elapsed) {
            const ScriptedEvent ev = script.front();
            script.pop_front();
            switch (ev.kind) {
                case ScriptedEvent::Kind::Touch:
                    batch.touches.push_back(ev.touch);
                    break;
                case ScriptedEvent::Kind::Key:
                    scriptKeys.feed(EV_KEY, ev.keyCode, ev.keyValue, batch.keys);
                    break;
                case ScriptedEvent::Kind::Power:
                    batch.power = true;
                    break;
                case ScriptedEvent::Kind::Quit:
                    quitNow = true;
                    break;
            }
        }
        return;
    }

    input.wait(timeoutMs, batch);
}

int App::waitTimeout(uint64_t now) const {
    uint64_t t = 10;  // keep the emulated clock ticking smoothly
    t = std::min(t, scheduler->msUntilNextAction(now));
    if (penMovePending) {
        const uint64_t since = now - lastPenSent;
        t = std::min<uint64_t>(t, since >= static_cast<uint64_t>(config.penThrottleMs)
                                       ? 0
                                       : config.penThrottleMs - since);
    }
    return static_cast<int>(t);
}

void App::flushDisplay(uint64_t now, bool drainAll) {
    // Keep issuing flushes the scheduler wants right now (cleanup, then full).
    for (int i = 0; i < 3; i++) {
        auto f = scheduler->poll(now);
        if (!f) return;
        display->present(canvas, f->rect, f->waveform);
        if (!drainAll) return;
    }
}

void App::mainLoop() {
    InputBatch batch;
    idle = std::make_unique<IdlePolicy>(
        IdlePolicy::Params{static_cast<uint64_t>(config.autosaveMinutes) * 60000, 10000,
                           static_cast<uint64_t>(config.idleExitMinutes) * 60000, 60000},
        nowMs());

    while (!quit) {
        const uint64_t now = nowMs();

        runEmulation(now);
        processInstalls(now);
        updateLcd(now);

        // Deliver a throttled pen move, if one is waiting.
        if (penMovePending && now - lastPenSent >= static_cast<uint64_t>(config.penThrottleMs)) {
            gSession->QueuePenEvent(PenEvent::down(pendingPenX, pendingPenY));
            lastPenSent = now;
            penMovePending = false;
        }

        if (exitArmed && now - exitArmedAt > EXIT_CONFIRM_MS) {
            exitArmed = false;
            const int i = bar.indexOf(BarAction::Exit);
            bar.at(i).label = "EXIT";
            scheduler->addDirty(bar.draw(canvas, i), now);
        }

        switch (idle->poll(now, target != Target::None)) {
            case IdlePolicy::Action::Autosave:
                autosave(now);
                break;
            case IdlePolicy::Action::ShowWarning:
                std::cout << "idle: warning shown" << std::endl;
                showStatus("NO ACTIVITY: EXITING IN 1 MINUTE. TAP TO STAY.");
                break;
            case IdlePolicy::Action::HideWarning:
                scheduler->addDirty(bar.draw(canvas), now);
                break;
            case IdlePolicy::Action::Exit:
                std::cout << "idle: no input for " << config.idleExitMinutes
                          << " minutes, saving and exiting" << std::endl;
                quit = true;
                break;
            case IdlePolicy::Action::None:
                break;
        }

        flushDisplay(now, false);

        batch.clear();
        pollInput(waitTimeout(nowMs()), batch, quit);
        printLog(batch);
        if (!batch.touches.empty() || !batch.keys.empty()) {
            // A tap that dismisses the idle warning is not passed on to the Palm.
            if (idle->warningShown() && !batch.touches.empty() &&
                batch.touches.front().type == TouchEvent::Type::Down)
                target = Target::Swallow;
            idle->input(nowMs());
        }
        for (const auto& ev : batch.touches) {
            if (target == Target::Swallow && ev.type != TouchEvent::Type::Up) continue;
            handleTouch(ev, nowMs());
        }
        for (const auto& k : batch.keys) handleKey(k);
        if (batch.power) {
            std::cout << "power button: saving and exiting" << std::endl;
            quit = true;
        }
    }

    // Leave the screen in a clean state before handing back to Nickel.
    if (target == Target::Palm) gSession->QueuePenEvent(PenEvent::up());
}

void App::messageLoop() {
    InputBatch batch;
    bool done = false;
    while (!done) {
        batch.clear();
        pollInput(100, batch, done);
        printLog(batch);
        for (const auto& ev : batch.touches)
            if (ev.type == TouchEvent::Type::Up) done = true;
        if (batch.power) done = true;
    }
}

void App::touchTestLoop() {
    // Shows where touches land (to check the touch_swap_axes / mirror settings)
    // and echoes keyboard input (to check a Bluetooth keyboard is seen).
    std::string touchInfo, keyInfo;
    int crossX = -1, crossY = -1;
    const Rect exitRect = touchTestExitRect(canvas.w(), canvas.h());

    auto redraw = [&]() {
        canvas.fill(canvas.bounds(), WHITE);
        const int p = std::max(2, canvas.w() / 200);
        const int small = std::max(1, p - 1);
        ButtonBar::drawText(canvas, "TOUCH AND KEYBOARD TEST", canvas.w() / 2, canvas.h() / 4, p, BLACK);
        ButtonBar::drawText(canvas, "TAP NEAR EACH CORNER. THE CROSS", canvas.w() / 2,
                            canvas.h() / 4 + 12 * p, small, BLACK);
        ButtonBar::drawText(canvas, "MUST APPEAR UNDER YOUR FINGER.", canvas.w() / 2,
                            canvas.h() / 4 + 22 * p, small, BLACK);
        ButtonBar::drawText(canvas, "TYPE ON YOUR KEYBOARD TO TEST IT.", canvas.w() / 2,
                            canvas.h() / 4 + 32 * p, small, BLACK);
        ButtonBar::drawText(canvas, "TAP EXIT OR PRESS POWER TO LEAVE.", canvas.w() / 2,
                            canvas.h() / 4 + 42 * p, small, BLACK);
        canvas.frame(exitRect, 2 * p, BLACK);
        ButtonBar::drawText(canvas, "EXIT", exitRect.x + exitRect.w / 2, exitRect.y + exitRect.h / 2,
                            2 * p, BLACK);
        const int m = 30;
        for (const auto& c : {std::pair{m, m}, std::pair{canvas.w() - m, m},
                              std::pair{m, canvas.h() - m}, std::pair{canvas.w() - m, canvas.h() - m}})
            canvas.frame({c.first - 10, c.second - 10, 20, 20}, 3, BLACK);
        if (crossX >= 0) {
            canvas.fill({crossX - 40, crossY - 2, 80, 5}, BLACK);
            canvas.fill({crossX - 2, crossY - 40, 5, 80}, BLACK);
        }
        ButtonBar::drawText(canvas, touchInfo, canvas.w() / 2, canvas.h() * 3 / 4 - 12 * p, p, BLACK);
        const std::string kb = input.keyboardCount() > 0 || !options.script.empty()
                                   ? "KEYBOARD: " + (keyInfo.empty() ? std::string("CONNECTED") : keyInfo)
                                   : std::string("NO KEYBOARD CONNECTED");
        ButtonBar::drawText(canvas, kb, canvas.w() / 2, canvas.h() * 3 / 4, p, BLACK);
        display->present(canvas, canvas.bounds(), Waveform::Quality);
    };

    auto describeKey = [](const KeyAction& k) -> std::string {
        switch (k.kind) {
            case KeyAction::Kind::Char:
                switch (k.chr) {
                    case palmchr::LINEFEED: return "ENTER";
                    case palmchr::BACKSPACE: return "BACKSPACE";
                    case palmchr::TAB: return "TAB";
                    case palmchr::LEFT: return "LEFT";
                    case palmchr::RIGHT: return "RIGHT";
                    case palmchr::UP: return "UP";
                    case palmchr::DOWN: return "DOWN";
                    case ' ': return "SPACE";
                }
                return std::string("'") + static_cast<char>(std::toupper(k.chr & 0xff)) + "'" +
                       (std::isupper(k.chr & 0xff) ? " (SHIFT)" : "");
            case KeyAction::Kind::ButtonPress:
                return "PALM BUTTON " + std::to_string(static_cast<int>(k.button) + 1);
            case KeyAction::Kind::Refresh:
                return "REFRESH";
            case KeyAction::Kind::SilkTap: {
                static const char* names[] = {"HOME", "MENU", "CALC", "FIND"};
                return std::string(names[static_cast<int>(k.icon)]) + " ICON";
            }
            default:
                return "";
        }
    };

    redraw();
    InputBatch batch;
    bool done = false;
    bool exitArmed = false;  // the current touch went down on the EXIT button
    while (!done) {
        batch.clear();
        pollInput(200, batch, done);
        printLog(batch);
        if (batch.power) done = true;
        bool changed = !batch.log.empty();
        for (const auto& ev : batch.touches)
            if (ev.type == TouchEvent::Type::Up) {
                if (exitArmed && exitRect.contains(ev.x, ev.y)) done = true;
                exitArmed = false;
            } else if (ev.type == TouchEvent::Type::Down) {
                exitArmed = exitRect.contains(ev.x, ev.y);
                if (exitArmed) continue;  // a tap on EXIT does not move the cross
                crossX = ev.x;
                crossY = ev.y;
                touchInfo = "X=" + std::to_string(ev.x) + " Y=" + std::to_string(ev.y);
                changed = true;
            }
        for (const auto& k : batch.keys) {
            const std::string d = describeKey(k);
            if (!d.empty()) {
                keyInfo = d;
                changed = true;
            }
        }
        if (changed) redraw();
    }
}

// ---------------------------------------------------------------------------
// Emulation and screen

void App::runEmulation(uint64_t now) {
    // Same pacing as CloudpilotEmu's native MainLoop::Cycle: run the number of
    // cycles that corresponds to elapsed wall time, never catching up > 500 ms.
    const uint32_t clocksPerSecond = gSession->GetClocksPerSecond();
    const double elapsed = static_cast<double>(now - millisOffset);

    if (elapsed - clockEmu > 500) {
        clockEmu = elapsed - 10;
        emuCatchUps++;
    }

    const long cycles = static_cast<long>((elapsed - clockEmu) * clocksPerSecond / 1000.);
    if (cycles <= 0) return;

    const auto started = std::chrono::steady_clock::now();
    long passed = 0;
    while (passed < cycles) passed += gSession->RunEmulation(static_cast<uint32>(cycles - passed));
    emuBusyUs += static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(
                                           std::chrono::steady_clock::now() - started)
                                           .count());
    clockEmu += static_cast<double>(passed) / (static_cast<double>(clocksPerSecond) / 1000.);
}

void App::updateLcd(uint64_t now) {
    const bool powerOn = gSession->IsPowerOn();

    if (!powerOn) {
        if (lastPowerOn) {
            // Palm went to sleep: blank the LCD like a real one.
            canvas.fill(layout.lcd, WHITE);
            scheduler->addDirty(layout.lcd, now);
        }
        lastPowerOn = false;
        return;
    }

    if (!lastPowerOn) forceFullFrame = true;
    lastPowerOn = true;

    const bool dirty = gSystemState.IsScreenDirty() || forceFullFrame;
    if (!dirty && flushPipelineCounter >= FLUSH_PIPELINE_FRAMES) return;
    flushPipelineCounter = dirty ? 0 : flushPipelineCounter + 1;

    if (EmHAL::CopyLCDFrame(*frame, forceFullFrame) && frame->hasChanges) {
        LcdFrame f;
        f.bpp = frame->bpp;
        f.lineWidth = frame->lineWidth;
        f.lines = frame->lines;
        f.margin = frame->margin;
        f.bytesPerLine = frame->bytesPerLine;
        f.firstDirtyLine = frame->firstDirtyLine;
        f.lastDirtyLine = frame->lastDirtyLine;
        f.scaleX = frame->scaleX;
        f.scaleY = frame->scaleY;
        f.dirtyRegionIsVertical = frame->dirtyRegionIsVertical;
        f.lcd2bitMapping = frame->bpp == 2 ? EmHAL::GetLCD2bitMapping() : 0;
        f.buffer = frame->GetBuffer();

        if (forceFullFrame) {
            f.firstDirtyLine = 0;
            f.lastDirtyLine = f.lines ? f.lines - 1 : 0;
        }

        scheduler->addDirty(convertFrame(f, canvas, layout), now);
    }

    forceFullFrame = false;
    gSystemState.MarkScreenClean();
}

// ---------------------------------------------------------------------------
// Touch

void App::sendPen(int sx, int sy, bool clamp) {
    int px, py;
    if (!layout.toPalm(sx, sy, px, py, clamp)) return;
    pendingPenX = px;
    pendingPenY = py;
    penMovePending = true;
}

void App::echoTo(int sx, int sy, uint64_t now) {
    if (!config.graffitiEcho || layout.silk.empty()) return;
    const int thickness = std::max(3, layout.scale);
    const int x = std::clamp(sx, layout.silk.x, layout.silk.right() - 1);
    const int y = std::clamp(sy, layout.silk.y, layout.silk.bottom() - 1);
    if (!echoActive) {
        echoActive = true;
        echoX = x;
        echoY = y;
    }
    canvas.line(echoX, echoY, x, y, thickness, BLACK, layout.silk);
    const Rect seg = Rect{std::min(echoX, x) - thickness, std::min(echoY, y) - thickness,
                          std::abs(x - echoX) + 2 * thickness + 1, std::abs(y - echoY) + 2 * thickness + 1}
                         .intersected(layout.silk);
    echoDirty = echoDirty.united(seg);
    scheduler->addDirty(seg, now);
    echoX = x;
    echoY = y;
}

void App::clearEcho(uint64_t now) {
    if (!echoActive) return;
    echoActive = false;
    const Rect r = echoDirty.intersected(layout.silk);
    for (int y = r.y; y < r.bottom(); y++) {
        const uint8_t* src = silkBase.row(y - layout.silk.y) + (r.x - layout.silk.x);
        std::copy(src, src + r.w, canvas.row(y) + r.x);
    }
    scheduler->addDirty(r, now);
    echoDirty = {};
}

void App::handleTouch(const TouchEvent& ev, uint64_t now) {
    switch (ev.type) {
        case TouchEvent::Type::Down:
            if (layout.palm.contains(ev.x, ev.y)) {
                if (!gSession->IsPowerOn()) {
                    // Tapping a sleeping Palm wakes it, like pressing its power key.
                    gSession->QueueButtonEvent(ButtonEvent(ButtonEvent::Button::power, ButtonEvent::Type::press));
                    gSession->QueueButtonEvent(ButtonEvent(ButtonEvent::Button::power, ButtonEvent::Type::release));
                    target = Target::Swallow;
                    return;
                }
                target = Target::Palm;
                scheduler->setPenDown(true, now);
                int px = 0, py = 0;
                layout.toPalm(ev.x, ev.y, px, py, true);
                gSession->QueuePenEvent(PenEvent::down(px, py));
                lastPenSent = now;
                penMovePending = false;
                if (layout.silk.contains(ev.x, ev.y)) echoTo(ev.x, ev.y, now);
            } else if (int i = bar.hit(ev.x, ev.y); i >= 0) {
                target = Target::Bar;
                targetButton = i;
                barDown(i, now);
            } else {
                target = Target::Swallow;
            }
            break;

        case TouchEvent::Type::Move:
            if (target == Target::Palm) {
                sendPen(ev.x, ev.y, true);
                if (echoActive || layout.silk.contains(ev.x, ev.y)) echoTo(ev.x, ev.y, now);
            }
            break;

        case TouchEvent::Type::Up:
            if (target == Target::Palm) {
                if (penMovePending) {
                    gSession->QueuePenEvent(PenEvent::down(pendingPenX, pendingPenY));
                    penMovePending = false;
                }
                gSession->QueuePenEvent(PenEvent::up());
                clearEcho(now);
                scheduler->setPenDown(false, now);
            } else if (target == Target::Bar) {
                barUp(targetButton, now);
            }
            target = Target::None;
            targetButton = -1;
            break;
    }
}

void App::fullRefresh() {
    // Re-read the whole Palm screen and redraw the panel with a flashing
    // refresh, clearing any ghosting.
    forceFullFrame = true;
    scheduler->requestFull();
}

void App::barDown(int index, uint64_t now) {
    auto& b = bar.at(index);
    b.pressed = true;
    scheduler->addDirty(bar.draw(canvas, index), now);

    const ButtonEvent::Button pb = palmButton(b.action);
    if (b.action != BarAction::Exit && b.action != BarAction::Power && pb != ButtonEvent::Button::invalid)
        gSession->QueueButtonEvent(ButtonEvent(pb, ButtonEvent::Type::press));
}

void App::barUp(int index, uint64_t now) {
    auto& b = bar.at(index);
    b.pressed = false;

    switch (b.action) {
        case BarAction::Exit:
            if (exitArmed) {
                quit = true;
            } else {
                exitArmed = true;
                exitArmedAt = now;
                b.label = "SURE?";
            }
            break;
        case BarAction::Refresh:
            fullRefresh();
            break;
        case BarAction::Power:
            gSession->QueueButtonEvent(ButtonEvent(ButtonEvent::Button::power, ButtonEvent::Type::press));
            gSession->QueueButtonEvent(ButtonEvent(ButtonEvent::Button::power, ButtonEvent::Type::release));
            break;
        default:
            gSession->QueueButtonEvent(ButtonEvent(palmButton(b.action), ButtonEvent::Type::release));
            break;
    }

    scheduler->addDirty(bar.draw(canvas, index), now);
}

void App::handleKey(const KeyAction& k) {
    if (k.kind == KeyAction::Kind::Refresh) {
        fullRefresh();
        return;
    }

    // Palm OS drops key and pen events while it is off; a key wakes it like
    // pressing its power button (the key itself is not typed).
    if (!gSession->IsPowerOn()) {
        if (k.kind == KeyAction::Kind::ButtonRelease) return;
        gSession->QueueButtonEvent(ButtonEvent(ButtonEvent::Button::power, ButtonEvent::Type::press));
        gSession->QueueButtonEvent(ButtonEvent(ButtonEvent::Button::power, ButtonEvent::Type::release));
        return;
    }

    switch (k.kind) {
        case KeyAction::Kind::Char:
            gSession->QueueKeyboardEvent(KeyboardEvent(k.chr));
            break;

        case KeyAction::Kind::ButtonPress:
        case KeyAction::Kind::ButtonRelease: {
            static const ButtonEvent::Button map[] = {
                ButtonEvent::Button::app1,     ButtonEvent::Button::app2,
                ButtonEvent::Button::app3,     ButtonEvent::Button::app4,
                ButtonEvent::Button::rockerUp, ButtonEvent::Button::rockerDown};
            gSession->QueueButtonEvent(ButtonEvent(
                map[static_cast<int>(k.button)], k.kind == KeyAction::Kind::ButtonPress
                                                     ? ButtonEvent::Type::press
                                                     : ButtonEvent::Type::release));
        } break;

        case KeyAction::Kind::Refresh:
            break;

        case KeyAction::Kind::SilkTap: {
            if (target == Target::Palm) break;  // don't interfere with a finger on the screen
            int x = 0, y = 0;
            if (layout.silkH > 0) silkIconPosition(k.icon, x, y);
            else if (!softSilkNR70 || !silkIconPositionNR70(k.icon, x, y)) break;
            gSession->QueuePenEvent(PenEvent::down(x, y));
            gSession->QueuePenEvent(PenEvent::up());
        } break;
    }
}

// ---------------------------------------------------------------------------
// Virtual SD card
//
// The card is "inserted" after launch and "removed" (with Palm OS given time to
// notice) before the session is saved. That way Palm OS always sees a fresh card
// at launch, so it is safe to change the card's contents in between: files are
// imported from card_import_dir before insertion, and the card is mirrored to
// card_export_dir after removal.

void App::showStatus(const std::string& text) {
    if (layout.bar.empty()) return;
    canvas.fill(layout.bar, WHITE);
    canvas.frame(layout.bar, 2, BLACK);
    int p = std::max(2, layout.bar.h / 14);
    while (p > 1 && ButtonBar::textWidth(text, p) > layout.bar.w - 24) p--;
    ButtonBar::drawText(canvas, text, layout.bar.x + layout.bar.w / 2, layout.bar.y + layout.bar.h / 2, p, BLACK);
    display->present(canvas, layout.bar, Waveform::Quality);
}

void App::runFor(uint64_t ms) {
    const uint64_t until = nowMs() + ms;
    while (nowMs() < until) {
        runEmulation(nowMs());
        struct timespec ts{0, 5 * 1000000L};
        nanosleep(&ts, nullptr);
    }
}

void App::autosave(uint64_t now) {
    const uint64_t started = nowMs();
    showStatus("SAVING...");
    if (cardImage) {
        std::string err;
        if (!card::saveDirty(*cardImage, cardFile, err)) std::cerr << "card: " << err << std::endl;
    }
    const bool ok = saveSession();
    idle->saved(nowMs());
    scheduler->addDirty(bar.draw(canvas), nowMs());
    std::cout << "autosave " << (ok ? "done" : "FAILED") << " in " << (nowMs() - started) << " ms" << std::endl;
    (void)now;
}

void App::setupCard() {
    if (config.card.empty()) return;
    cardFile = config.resolve(config.card);

    std::unique_ptr<uint8_t[]> data;
    size_t size = 0;
    std::string err;
    bool created = false;

    if (fileExists(cardFile)) {
        if (!card::load(cardFile, data, size, err)) {
            std::cerr << "card: " << err << " -- running without a card" << std::endl;
            return;
        }
    } else {
        std::cout << "card: creating " << config.cardSizeMb << " MB card " << cardFile << std::endl;
        if (!card::create(static_cast<unsigned>(config.cardSizeMb), data, size, err)) {
            std::cerr << "card: " << err << std::endl;
            return;
        }
        created = true;
    }

    if (!gExternalStorage.AddImage(CARD_KEY, data.get(), size)) {
        std::cerr << "card: cannot register image (bad size?)" << std::endl;
        return;
    }
    data.release();  // now owned by the emulator
    cardImage = gExternalStorage.GetImage(CARD_KEY);

    if (created && !card::saveFull(*cardImage, cardFile, err)) std::cerr << "card: " << err << std::endl;

    if (!card::hasFileSystem(*cardImage))
        std::cerr << "card: WARNING: no FAT file system found on " << cardFile
                  << " (Palm OS will offer to format it)" << std::endl;

    // Import files dropped into the import folder.
    const std::string importDir = config.resolve(config.cardImportDir);
    std::error_code ec;
    std::filesystem::create_directories(importDir, ec);
    const card::Report r = card::importDir(*cardImage, importDir);
    for (const auto& l : r.lines) std::cout << "card: " << l << std::endl;
    if (r.files > 0) {
        if (!card::saveDirty(*cardImage, cardFile, err)) std::cerr << "card: " << err << std::endl;
        std::cout << "card: imported " << r.files << " file(s)" << std::endl;
    }

    if (!gExternalStorage.Mount(CARD_KEY)) {
        std::cerr << "card: the emulated device has no SD slot or does not accept this card size"
                  << std::endl;
        return;
    }
    cardMounted = true;
    std::cout << "card: inserted " << cardFile << " (" << size / (1024 * 1024) << " MB)" << std::endl;
}

void App::ejectCard() {
    if (!cardImage) return;

    if (cardMounted) {
        showStatus("SAVING CARD...");

        // Palm OS only notices a removed card while it is awake.
        if (!gSession->IsPowerOn()) {
            gSession->QueueButtonEvent(ButtonEvent(ButtonEvent::Button::power, ButtonEvent::Type::press));
            gSession->QueueButtonEvent(ButtonEvent(ButtonEvent::Button::power, ButtonEvent::Type::release));
            runFor(1000);
        }
        gExternalStorage.Unmount(CARD_KEY);
        runFor(1500);  // let Palm OS unmount the volume before we save the session
        cardMounted = false;
    }

    std::string err;
    size_t pages = 0;
    if (card::saveDirty(*cardImage, cardFile, err, &pages))
        std::cout << "card: saved (" << (pages == SIZE_MAX ? std::string("full image")
                                                           : std::to_string(pages) + " changed blocks")
                  << ")" << std::endl;
    else
        std::cerr << "card: " << err << std::endl;

    if (!config.cardExportDir.empty()) {
        showStatus("COPYING CARD FILES...");
        const card::Report r = card::exportMirror(*cardImage, config.resolve(config.cardExportDir));
        for (const auto& l : r.lines) std::cout << "card: " << l << std::endl;
        std::cout << "card: exported " << r.files << " file(s), " << r.skipped << " unchanged, "
                  << r.deleted << " removed" << std::endl;
    }
}

// ---------------------------------------------------------------------------
// App installation and persistence

void App::scanInstallDir() {
    const std::string dir = config.resolve(config.installDir);
    DIR* d = opendir(dir.c_str());
    if (!d) return;
    std::vector<std::string> names;
    while (dirent* e = readdir(d))
        // Skip hidden files, e.g. the "._name.prc" files macOS Finder leaves on FAT drives.
        if (e->d_name[0] != '.' && isInstallable(e->d_name)) names.push_back(e->d_name);
    closedir(d);
    std::sort(names.begin(), names.end());
    for (const auto& n : names) pendingInstalls.push_back(dir + "/" + n);
    if (!names.empty()) std::cout << names.size() << " file(s) queued for installation" << std::endl;
}

void App::processInstalls(uint64_t now) {
    if (pendingInstalls.empty()) {
        if (installNeedsReset) {
            installNeedsReset = false;
            std::cout << "soft reset after installation" << std::endl;
            gSession->Reset(EmSession::ResetType::soft);
        }
        return;
    }
    if (now - millisOffset < static_cast<uint64_t>(config.installDelayMs) || !gSession->IsPowerOn())
        return;

    // One file per loop iteration keeps the UI responsive.
    const std::string path = pendingInstalls.front();
    pendingInstalls.pop_front();

    std::unique_ptr<uint8[]> buffer;
    size_t size = 0;
    if (!util::ReadFile(path, buffer, size)) {
        std::cerr << "install: cannot read " << path << std::endl;
        return;
    }

    const DbInstaller::Result result = DbInstaller(size, buffer.get()).Install();
    const bool ok = result == DbInstaller::Result::success || result == DbInstaller::Result::needsReboot;
    std::cout << "install " << path << ": " << (ok ? "ok" : "FAILED (") << (ok ? "" : std::to_string(static_cast<int>(result)) + ")") << std::endl;
    if (result == DbInstaller::Result::needsReboot) installNeedsReset = true;

    // Move it out of the way so it isn't installed again next launch.
    const size_t slash = path.find_last_of('/');
    const std::string doneDir = path.substr(0, slash) + (ok ? "/installed" : "/failed");
    mkdir(doneDir.c_str(), 0755);
    std::rename(path.c_str(), (doneDir + path.substr(slash)).c_str());
}

bool App::saveSession() {
    if (!sessionReady) return false;

    SessionImage image;
    if (!gSession->SaveImage(image)) return false;

    const std::string path = config.resolve(config.session);
    const std::string tmp = path + ".tmp";
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        if (!out) return false;
        out.write(static_cast<const char*>(image.GetSerializedImage()),
                  static_cast<std::streamsize>(image.GetSerializedImageSize()));
        if (!out) return false;
    }
    if (std::rename(tmp.c_str(), path.c_str()) != 0) return false;
    std::cout << "session saved to " << path << " (" << image.GetSerializedImageSize() << " bytes)" << std::endl;
    return true;
}

}  // namespace pk
