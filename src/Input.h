// palmkobo - Palm OS (CloudpilotEmu) front end for Kobo e-readers
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Reads the touchscreen, the power button and any keyboards (USB or Bluetooth)
// straight from evdev (/dev/input/event*). Touch handles multitouch protocol B
// (tracking ids), BTN_TOUCH devices and single-touch ABS_X/ABS_Y panels; only the
// first finger is used. Keyboards that connect later (Bluetooth keyboards waking
// up) are picked up by a periodic rescan.

#ifndef PALMKOBO_INPUT_H
#define PALMKOBO_INPUT_H

#include <cstdint>
#include <string>
#include <vector>

#include "Geometry.h"
#include "Keyboard.h"

namespace pk {

struct TouchEvent {
    enum class Type { Down, Move, Up } type;
    int x, y;  // panel pixels
};

struct InputBatch {
    std::vector<TouchEvent> touches;
    std::vector<KeyAction> keys;
    bool power{false};         // device power button released
    std::vector<std::string> log;  // lines for the log file, each ending in '\n'

    void clear() {
        touches.clear();
        keys.clear();
        power = false;
        log.clear();
    }
};

// Pure evdev touch state machine, separated out so it can be unit-tested.
class TouchDecoder {
   public:
    void configure(AxisRange ax, AxisRange ay, TouchTransform t, int screenW, int screenH);

    // Feed one evdev event (type, code, value). Emits events on SYN_REPORT.
    void feed(unsigned type, unsigned code, int value, std::vector<TouchEvent>& out);

    bool isDown() const { return contactPrev; }

   private:
    AxisRange ax, ay;
    TouchTransform transform;
    int screenW{1}, screenH{1};

    int slot{0};
    int rawX{0}, rawY{0};
    bool contact{false}, contactPrev{false};
    bool moved{false};
    int lastX{0}, lastY{0};
};

enum class DeviceKind { Ignore, Touch, Keyboard, PowerKey };

// Classify an input device from its capability bitmaps (EVIOCGBIT results).
// Touch wins over keyboard, keyboard over power key (many keyboards also
// advertise KEY_POWER, which must not quit palmkobo).
DeviceKind classifyDevice(const unsigned long* evBits, const unsigned long* absBits,
                          const unsigned long* keyBits);

class Input {
   public:
    ~Input();

    // Scans /dev/input. Returns a description of what was found.
    std::string open(TouchTransform transform, int screenW, int screenH, bool grab);
    void close();

    bool hasTouch() const { return touchFd >= 0; }
    int keyboardCount() const { return static_cast<int>(keyboards.size()); }

    // Wait up to timeoutMs for input, then drain it. Also rescans for new
    // keyboards every couple of seconds.
    void wait(int timeoutMs, InputBatch& batch);

   private:
    struct Device {
        int fd;
        std::string path;
        std::string name;
    };

    std::string scan(bool initial);
    bool isOpen(const std::string& path) const;
    void drainTouch(InputBatch& batch);
    bool drainKeyboard(const Device& d, InputBatch& batch);  // false = device gone
    void drainPower(int fd, InputBatch& batch);

    TouchTransform transform;
    int screenW{1}, screenH{1};
    bool grab{false};

    int touchFd{-1};
    std::string touchPath;
    std::vector<Device> powerKeys;
    bool powerReadErrorLogged{false};
    std::vector<Device> keyboards;
    uint64_t lastScan{0};

    TouchDecoder touchDecoder;
    KeyDecoder keyDecoder;
};

}  // namespace pk

#endif
