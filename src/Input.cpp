// palmkobo - Palm OS (CloudpilotEmu) front end for Kobo e-readers
// SPDX-License-Identifier: GPL-3.0-or-later

#include "Input.h"

#include <fcntl.h>
#include <linux/input.h>

// The Kobo toolchain ships old (2.6.35-era) kernel headers; the running kernel
// supports these, so define any constants the headers predate.
#ifndef ABS_MT_SLOT
#define ABS_MT_SLOT 0x2f
#endif
#ifndef ABS_MT_POSITION_X
#define ABS_MT_POSITION_X 0x35
#endif
#ifndef ABS_MT_POSITION_Y
#define ABS_MT_POSITION_Y 0x36
#endif
#ifndef ABS_MT_TRACKING_ID
#define ABS_MT_TRACKING_ID 0x39
#endif

#include <poll.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstring>

namespace pk {

namespace {

constexpr size_t LONG_BITS = sizeof(long) * 8;
constexpr size_t nlongs(size_t bits) { return (bits + LONG_BITS - 1) / LONG_BITS; }
constexpr uint64_t RESCAN_MS = 2000;

bool testBit(const unsigned long* bits, unsigned bit) {
    return (bits[bit / LONG_BITS] >> (bit % LONG_BITS)) & 1UL;
}

uint64_t monotonicMs() {
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                     std::chrono::steady_clock::now().time_since_epoch())
                                     .count());
}

}  // namespace

// ---------------------------------------------------------------------------
// TouchDecoder

void TouchDecoder::configure(AxisRange x, AxisRange y, TouchTransform t, int w, int h) {
    ax = x;
    ay = y;
    transform = t;
    screenW = w;
    screenH = h;
}

void TouchDecoder::feed(unsigned type, unsigned code, int value, std::vector<TouchEvent>& out) {
    switch (type) {
        case EV_ABS:
            switch (code) {
                case ABS_MT_SLOT:
                    slot = value;
                    break;
                case ABS_MT_TRACKING_ID:
                    if (slot == 0) contact = value >= 0;
                    break;
                case ABS_MT_POSITION_X:
                    if (slot == 0) {
                        rawX = value;
                        moved = true;
                    }
                    break;
                case ABS_MT_POSITION_Y:
                    if (slot == 0) {
                        rawY = value;
                        moved = true;
                    }
                    break;
                case ABS_X:
                    rawX = value;
                    moved = true;
                    break;
                case ABS_Y:
                    rawY = value;
                    moved = true;
                    break;
            }
            break;

        case EV_KEY:
            if (code == BTN_TOUCH) contact = value != 0;
            break;

        case EV_SYN:
            if (code != SYN_REPORT) break;
            {
                int x, y;
                mapTouch(rawX, rawY, ax, ay, transform, screenW, screenH, x, y);
                if (contact && !contactPrev) {
                    out.push_back({TouchEvent::Type::Down, x, y});
                } else if (contact && contactPrev && moved && (x != lastX || y != lastY)) {
                    out.push_back({TouchEvent::Type::Move, x, y});
                } else if (!contact && contactPrev) {
                    out.push_back({TouchEvent::Type::Up, lastX, lastY});
                }
                if (contact) {
                    lastX = x;
                    lastY = y;
                }
                contactPrev = contact;
                moved = false;
            }
            break;
    }
}

// ---------------------------------------------------------------------------
// Device classification

DeviceKind classifyDevice(const unsigned long* evBits, const unsigned long* absBits,
                          const unsigned long* keyBits) {
    const bool hasAbs = testBit(evBits, EV_ABS);
    const bool hasKey = testBit(evBits, EV_KEY);

    const bool mt = hasAbs && testBit(absBits, ABS_MT_POSITION_X) && testBit(absBits, ABS_MT_POSITION_Y);
    const bool st = hasAbs && hasKey && testBit(absBits, ABS_X) && testBit(absBits, ABS_Y) &&
                    testBit(keyBits, BTN_TOUCH);
    if (mt || st) return DeviceKind::Touch;

    if (!hasKey) return DeviceKind::Ignore;

    const bool letters = testBit(keyBits, KEY_A) && testBit(keyBits, KEY_Z) &&
                         testBit(keyBits, KEY_Q) && testBit(keyBits, KEY_SPACE) &&
                         testBit(keyBits, KEY_ENTER);
    if (letters) return DeviceKind::Keyboard;

    // Page-turn remotes: their Page Up/Down drive the Palm scroll rocker.
    if (testBit(keyBits, KEY_PAGEUP) && testBit(keyBits, KEY_PAGEDOWN)) return DeviceKind::Keyboard;

    if (testBit(keyBits, KEY_POWER)) return DeviceKind::PowerKey;
    return DeviceKind::Ignore;
}

// ---------------------------------------------------------------------------
// Input

Input::~Input() { close(); }

std::string Input::open(TouchTransform t, int w, int h, bool g) {
    transform = t;
    screenW = w;
    screenH = h;
    grab = g;
    std::string report = scan(true);
    if (touchFd < 0) report += "no touchscreen found\n";
    if (keyboards.empty()) report += "no keyboard connected (will keep looking)\n";
    return report;
}

bool Input::isOpen(const std::string& path) const {
    if (path == touchPath) return true;
    for (const auto& d : powerKeys)
        if (d.path == path) return true;
    for (const auto& d : keyboards)
        if (d.path == path) return true;
    return false;
}

std::string Input::scan(bool initial) {
    std::string report;
    char path[64];
    lastScan = monotonicMs();

    for (int i = 0; i < 32; i++) {
        std::snprintf(path, sizeof(path), "/dev/input/event%d", i);
        if (isOpen(path)) continue;

        const int fd = ::open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
        if (fd < 0) continue;

        char name[128] = "?";
        ioctl(fd, EVIOCGNAME(sizeof(name)), name);

        unsigned long evBits[nlongs(EV_MAX + 1)] = {};
        unsigned long absBits[nlongs(ABS_MAX + 1)] = {};
        unsigned long keyBits[nlongs(KEY_MAX + 1)] = {};
        ioctl(fd, EVIOCGBIT(0, sizeof(evBits)), evBits);
        if (testBit(evBits, EV_ABS)) ioctl(fd, EVIOCGBIT(EV_ABS, sizeof(absBits)), absBits);
        if (testBit(evBits, EV_KEY)) ioctl(fd, EVIOCGBIT(EV_KEY, sizeof(keyBits)), keyBits);

        DeviceKind kind = classifyDevice(evBits, absBits, keyBits);
        // Only the initial scan picks the touchscreen and power key; later scans
        // only look for keyboards that have (re)connected.
        if (!initial && kind != DeviceKind::Keyboard) kind = DeviceKind::Ignore;
        if (kind == DeviceKind::Touch && touchFd >= 0) kind = DeviceKind::Ignore;

        switch (kind) {
            case DeviceKind::Touch: {
                const bool mt = testBit(absBits, ABS_MT_POSITION_X);
                input_absinfo ix{}, iy{};
                ioctl(fd, EVIOCGABS(mt ? ABS_MT_POSITION_X : ABS_X), &ix);
                ioctl(fd, EVIOCGABS(mt ? ABS_MT_POSITION_Y : ABS_Y), &iy);
                touchDecoder.configure({ix.minimum, ix.maximum}, {iy.minimum, iy.maximum},
                                       transform, screenW, screenH);
                touchFd = fd;
                touchPath = path;
                report += std::string("touch: ") + path + " \"" + name + "\" x " +
                          std::to_string(ix.minimum) + ".." + std::to_string(ix.maximum) + " y " +
                          std::to_string(iy.minimum) + ".." + std::to_string(iy.maximum) +
                          (mt ? " (multitouch)" : " (single touch)") + "\n";
            } break;
            case DeviceKind::Keyboard:
                keyboards.push_back({fd, path, name});
                report += std::string("keyboard connected: ") + path + " \"" + name + "\"\n";
                break;
            case DeviceKind::PowerKey:
                powerKeys.push_back({fd, path, name});
                report += std::string("power key: ") + path + " \"" + name + "\"\n";
                break;
            case DeviceKind::Ignore:
                // Logged once at startup, to diagnose a power button that is not seen.
                if (initial)
                    report += std::string("ignored: ") + path + " \"" + name + "\" KEY_POWER=" +
                              (testBit(keyBits, KEY_POWER) ? "yes" : "no") + "\n";
                ::close(fd);
                continue;
        }

        // Grab so that Nickel (paused, but still holding the devices open) does not
        // receive a backlog of our taps and keystrokes when it is resumed.
        if (grab && ioctl(fd, EVIOCGRAB, 1) < 0)
            report += std::string("grab failed: ") + path + " errno=" + std::to_string(errno) + "\n";
    }

    return report;
}

void Input::close() {
    auto release = [this](int fd) {
        if (fd < 0) return;
        if (grab) ioctl(fd, EVIOCGRAB, 0);
        ::close(fd);
    };
    release(touchFd);
    touchFd = -1;
    touchPath.clear();
    for (auto& d : powerKeys) release(d.fd);
    powerKeys.clear();
    for (auto& d : keyboards) release(d.fd);
    keyboards.clear();
}

void Input::wait(int timeoutMs, InputBatch& batch) {
    if (monotonicMs() - lastScan >= RESCAN_MS) {
        const std::string found = scan(false);
        if (!found.empty()) batch.log.push_back(found);
    }

    std::vector<pollfd> fds;
    if (touchFd >= 0) fds.push_back({touchFd, POLLIN, 0});
    for (const auto& d : powerKeys) fds.push_back({d.fd, POLLIN, 0});
    for (const auto& d : keyboards) fds.push_back({d.fd, POLLIN, 0});

    if (fds.empty()) {
        if (timeoutMs > 0) usleep(static_cast<useconds_t>(timeoutMs) * 1000);
        return;
    }

    if (poll(fds.data(), fds.size(), timeoutMs) <= 0) return;

    std::vector<int> ready;
    for (const auto& p : fds)
        if (p.revents & (POLLIN | POLLERR | POLLHUP | POLLNVAL)) ready.push_back(p.fd);

    for (int fd : ready) {
        if (fd == touchFd) {
            drainTouch(batch);
            continue;
        }
        bool handled = false;
        for (const auto& d : powerKeys)
            if (d.fd == fd) {
                drainPower(fd, batch);
                handled = true;
            }
        if (handled) continue;
        for (auto it = keyboards.begin(); it != keyboards.end(); ++it) {
            if (it->fd != fd) continue;
            if (!drainKeyboard(*it, batch)) {
                // Bluetooth keyboard went to sleep or disconnected.
                batch.log.push_back("keyboard disconnected: " + it->path + " \"" + it->name +
                                    "\"\n");
                ::close(it->fd);
                keyboards.erase(it);
            }
            break;
        }
    }
}

void Input::drainTouch(InputBatch& batch) {
    input_event ev[64];
    for (;;) {
        const ssize_t n = read(touchFd, ev, sizeof(ev));
        if (n <= 0) return;
        for (size_t i = 0; i < static_cast<size_t>(n) / sizeof(input_event); i++)
            touchDecoder.feed(ev[i].type, ev[i].code, ev[i].value, batch.touches);
    }
}

bool Input::drainKeyboard(const Device& d, InputBatch& batch) {
    input_event ev[64];
    for (;;) {
        const ssize_t n = read(d.fd, ev, sizeof(ev));
        if (n < 0) return errno == EAGAIN || errno == EINTR;  // ENODEV etc. = gone
        if (n == 0) return false;
        for (size_t i = 0; i < static_cast<size_t>(n) / sizeof(input_event); i++)
            keyDecoder.feed(ev[i].type, ev[i].code, ev[i].value, batch.keys);
    }
}

void Input::drainPower(int fd, InputBatch& batch) {
    input_event ev[64];
    for (;;) {
        const ssize_t n = read(fd, ev, sizeof(ev));
        if (n <= 0) {
            // Logged once: a dead device keeps poll() returning, which would flood the log.
            if (n < 0 && errno != EAGAIN && errno != EINTR && !powerReadErrorLogged) {
                powerReadErrorLogged = true;
                batch.log.push_back("power key read error: errno=" + std::to_string(errno) + "\n");
            }
            return;
        }
        for (size_t i = 0; i < static_cast<size_t>(n) / sizeof(input_event); i++) {
            // Diagnostics: show what the power button actually sends.
            if (ev[i].type != EV_SYN)
                batch.log.push_back("power key event: type=" + std::to_string(ev[i].type) +
                                    " code=" + std::to_string(ev[i].code) +
                                    " value=" + std::to_string(ev[i].value) + "\n");
            if (ev[i].type == EV_KEY && ev[i].code == KEY_POWER && ev[i].value == 0) batch.power = true;
        }
    }
}

}  // namespace pk
