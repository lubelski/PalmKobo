# palmkobo

I built this with Claude AI because I'm good at coming up with dumb ideas but bad at actual programming. All the usual vibe coding / AI slop warnings definitely apply here.

Run Palm OS 4.1 (Palm m500) on a Kobo e-reader. palmkobo is a small front end
around the CloudpilotEmu emulator core. It swaps CloudpilotEmu's SDL window for
the Kobo's e-ink screen (through FBInk) and its touchscreen, and it's launched
from NickelMenu.

```
+--------------------------+
|   Palm LCD  (160x160 x6) |   960 x 960 px on a Clara BW
+--------------------------+
| Graffiti silkscreen (x6) |   tap the icons, write in the box
+--------------------------+
| (D) (A)  ( ^ | v )  (T) (M)   [=] |  Palm keys (white on black) + menu
+--------------------------+
```

The strip has the Palm's keys as icons, arranged as on a Palm: Date Book and
Address, the scroll keys, To Do and Memo. The square button at the right opens
palmkobo's own menu over the Palm screen: refresh, the Palm's power button,
exit. A screen that is tall for the panel (a 320×480 Clie) gets the full
height instead, with the same buttons in a rail down the right-hand side.

## Status

**Version 0.1.0, experimental.** Tested on a Kobo Clara BW with a Palm m500
ROM. m515 support (colour shown in grays) is included but less tested.
Working on the Clara BW:
- boot and session save/restore;
- touch and Graffiti;
- Bluetooth keyboard;
- SD card import and export;
- grayscale apps;
- autosave and idle exit.

Other Kobo models that FBInk and NickelMenu support should work, but haven't
been tried.

The automated tests run on a desktop:
- `make test` (pure front-end logic, with sanitizers);
- `make card-test` (SD card create/import/export, verified with dosfstools fsck).

End users don't need anything in this file. They get `docs/USER-GUIDE.txt` as
the README of the release package.

## What you need

| Thing | Where |
|---|---|
| CloudpilotEmu source | `git clone --recurse-submodules https://github.com/cloudpilot-emu/cloudpilot` |
| FBInk source | `git clone --recursive https://github.com/NiLuJe/FBInk` |
| Kobo cross toolchain | koxtoolchain (`https://github.com/koreader/koxtoolchain`), target `kobo` → `arm-kobo-linux-gnueabihf-*` |
| NickelMenu on the Kobo | `https://pgaskin.net/NickelMenu/` |
| Python 3 + Pillow (build machine) | only to extract the silkscreen artwork |
| **A Palm m500 ROM** | dumped from your own m500 (see below) |

Put the three checkouts side by side, or point the Makefile at them:

```
work/
  cloudpilot/
  FBInk/
  palmkobo/
```

## Building

### 1. Desktop build (fast feedback, no Kobo needed)

```sh
cd palmkobo
make                 # builds CloudpilotEmu's core without SDL, then palmkobo
make test            # unit tests (AddressSanitizer + UBSan)
make card-test       # SD card create/import/export tests, checked with fsck
make preview         # renders build/test/preview.pgm, a mock-up of the screen
```

The host build has no window. It draws into memory and writes a PGM snapshot
on exit. If you have a ROM, you can boot it headless with scripted touches:

```sh
printf '9000 down 500 1200\n9100 up 500 1200\n15000 quit\n' > taps.txt
./build/host/palmkobo rom=/path/to/m500.rom session=/tmp/s.img \
    host_snapshot=/tmp/screen.pgm --script taps.txt
```

Each script line is `<ms> down|move|up X Y` (panel pixels), `<ms> power` or
`<ms> quit`.

### 2. Kobo build

Build the koxtoolchain `kobo` toolchain once (`./gen-tc.sh kobo`; it takes a
while). On recent distributions such as Ubuntu 24.04, its old glibc 2.15
manual crashes the current Texinfo (`texi2any (XS parser): bug: parent hv
not already set`). Work around it with a `makeinfo` shim on your PATH for
the duration of the toolchain build:

```sh
mkdir -p ~/bin-shim
cat > ~/bin-shim/makeinfo <<'SHIM'
#!/bin/sh
case "$1" in --version|-V|--help|-h) exec /usr/bin/makeinfo "$@" ;; esac
TEXINFO_XS=omit /usr/bin/makeinfo --force "$@" >/dev/null 2>&1
out=""; src=""; prev=""
for a in "$@"; do
  case "$prev" in -o|--output) out="$a" ;; esac
  case "$a" in --output=*) out="${a#--output=}" ;; *.texinfo|*.texi|*.txi) src="$a" ;; esac
  prev="$a"
done
[ -z "$out" ] && [ -n "$src" ] && out="$(basename "${src%.*}").info"
[ -n "$out" ] && [ ! -e "$out" ] && [ ! -d "$out" ] && : > "$out"
exit 0
SHIM
chmod +x ~/bin-shim/makeinfo
PATH=$HOME/bin-shim:$PATH ./gen-tc.sh kobo
rm -rf ~/bin-shim
export PATH=$HOME/x-tools/arm-kobo-linux-gnueabihf/bin:$PATH
```

Then:

```sh
# once: build FBInk as a static library for Kobo
make -C ../FBInk staticlib KOBO=true CROSS_TC=arm-kobo-linux-gnueabihf

# palmkobo + quick install zip for your own Kobo
make TARGET=kobo package CROSS_TC=arm-kobo-linux-gnueabihf
# => build/palmkobo-kobo.zip
```

`CLOUDPILOT=`, `FBINK=` and `CROSS_TC=` can be overridden on the command line.
The binary is linked with `-static-libstdc++ -static-libgcc`, so the firmware
only needs to supply glibc. The source already contains the workarounds for
the toolchain's old kernel headers and glibc: missing multitouch constants,
`-ldl`, and `_GNU_SOURCE` for dosfstools.

### 3. Making a release to share

```sh
make TARGET=kobo release   # build/palmkobo-<version>-kobo.zip
make dist                  # build/palmkobo-<version>-src.zip
```

The **release zip** is what end users download. It contains:
- `README.txt`: the user guide, from `docs/USER-GUIDE.txt`;
- `LICENSE.txt`;
- `SOURCES.txt`: the exact CloudpilotEmu and FBInk commits and the compiler
  used;
- `KOBO/.adds/...`: the files to copy onto the Kobo.

The **source zip** is palmkobo's own source. Because the binary includes GPL
code, publish the source zip wherever you publish the release zip. The
upstream commits in `SOURCES.txt` complete the corresponding source. Bump
`PALMKOBO_VERSION` in `src/Version.h` for each release.

## Installing on the Kobo

1. Connect the Kobo over USB. Unzip `palmkobo-kobo.zip` into the root of the
   drive, so you get `.adds/palmkobo/` and `.adds/nm/palmkobo`.
2. Copy your ROM to `.adds/palmkobo/m500.rom`.
3. Optional: put `.prc` / `.pdb` files in `.adds/palmkobo/install/`.
4. Eject, then open NickelMenu → **PalmOS input test** first. Tap near each
   corner. The cross should appear under your finger. If it doesn't, set
   `touch_swap_axes`, `touch_mirror_x` and `touch_mirror_y` in `palmkobo.ini`
   (0/1, instead of `auto`). Tap **EXIT** in the middle of the screen,
   or press the Kobo's power button, to leave the test.
5. NickelMenu → **PalmOS**.

**The menu** (the square button with three bars) covers part of the Palm
screen while it is open. Taps go to the menu only; a tap outside it closes it.

- **Refresh screen** does a full flashing redraw, to clear e‑ink ghosting.
  It's handled by palmkobo; the Palm never sees it.
- **Palm power button** turns the Palm off or on.
- **Exit to Kobo** leaves at once: opening the menu is the confirmation.

**Leaving:** open the menu and tap **Exit to Kobo**. The Kobo's power button
also exits, on Kobos where palmkobo can read it (on some, another process holds
the button; the log then says `grab failed`). Both save the Palm to `session.img` and put the Kobo's screen back as it was. The next
launch resumes exactly where you were. Delete `session.img` to cold-boot again.

Everything palmkobo prints goes to `.adds/palmkobo/palmkobo.log`.

## Getting the ROM

CloudpilotEmu needs an m500 ROM image, and so does palmkobo. Dump it from your
own m500 with a ROM transfer tool. Palm's old Palm OS Emulator (POSE) shipped
one, `ROM Transfer.prc`, and CloudpilotEmu's documentation also covers
obtaining ROMs. Check the file by booting it in CloudpilotEmu's web app before
copying it to the Kobo. If it works there, it should work here: palmkobo uses
the same core and the same device ID (`PalmM500`).

## Keyboards (Bluetooth or USB)

Pair and connect the keyboard in the Kobo's own settings **before** launching
PalmOS. palmkobo finds connected keyboards on startup, and checks again every
2 seconds for keyboards that connect later.

| Key | Palm action |
|---|---|
| letters, digits, punctuation (US layout), Shift, Caps Lock | typed into the current field |
| Enter, Backspace, Tab, arrow keys | the same in Palm OS (held keys repeat) |
| F1 – F4 | Date Book, Address, To Do, Memo buttons |
| Page Up / Page Down | scroll rocker up / down |
| Esc or F5 | Menu (taps the silkscreen Menu icon) |
| F6 or Home | Applications launcher (Home icon) |
| F7 | Find |
| F8 | Calculator |
| F9 | full screen refresh (same as Refresh screen in the menu) |

Page-turn remotes, which only send Page Up/Down, also work and drive the scroll
rocker. Ctrl/Alt shortcuts aren't passed through: Palm OS command strokes need
a modifier flag that CloudpilotEmu's keyboard path doesn't set. Use Esc for the
menu instead.

The **PalmOS input test** menu entry also shows the keys it receives, so you
can check the keyboard is seen before starting Palm OS.

## SD card

The emulated m500 has a virtual SD card stored as `card.img` (64 MB by default,
created and formatted on first launch). Two folders next to it are how you move
files in and out over USB. There's no need to mount the image on your PC.

- **`to-card/` (into the Palm):** anything you copy here is put on the card at
  the next launch, then moved to `to-card/imported/`.
  - `.prc`, `.pdb` and `.pqa` files at the top level go to `/PALM/Launcher`,
    so apps show up in the launcher's card category.
  - Anything else keeps its folder layout, so `to-card/DOCS/a.txt` becomes
    `/DOCS/a.txt`.
- **`card-contents/` (out of the Palm):** after every exit, this folder is
  updated to an exact copy of the card. Copy files from here; don't edit it,
  because it's rewritten each time.

How it stays safe: Palm OS caches the card's file system, so palmkobo never
changes the card while Palm OS is using it. The card is "inserted" just after
launch, after imports. At exit it is "removed", and Palm OS gets 1.5 s to notice
before the session is saved. Exports happen only after that. Only the changed
8 KB blocks of `card.img` are written back, so exits stay quick.

**Getting your own data onto the card:** in the Palm launcher, the **App →
Copy…** menu copies applications between the handheld and the card. To copy
databases, such as a MemoDB or a document, use a Palm file-manager app. Apps
that save plain files to the card, like text editors with VFS support, make
the easiest exports, because the result is readable on a PC as-is.

Settings: `card`, `card_size_mb`, `card_import_dir`, `card_export_dir` in
`palmkobo.ini`. To start over with an empty card, delete `card.img`.

## Autosave and idle exit

- **Autosave** (`autosave_minutes`, default 5): if you've used the Palm since
  the last save, palmkobo saves the session and SD card at most every 5
  minutes. It waits for a 10‑second pause in your typing or tapping, so a save
  never interrupts you. "SAVING..." shows briefly across the Palm screen.
- **Idle exit** (`idle_exit_minutes`, default 15): after 15 minutes without a
  tap or keypress, palmkobo saves everything and returns to the Kobo, which can
  then go to sleep as usual. One minute before, a warning appears across the Palm screen;
  tap the screen or press a key to stay. That tap only dismisses the warning
  and isn't passed to the Palm.

Set either one to 0 to turn it off.

## Sony Clie NR70 series

CloudpilotEmu also emulates the Sony Clie NR70 family (device ID `NR70`), and
palmkobo can run it. It has been tried on the desktop build with an NR70V ROM:
setup, digitizer calibration, the launcher, Graffiti and the card all work.
`kobo/nickelmenu-palmkobo` has a commented entry for it, which starts palmkobo
with `rom=nr70.rom device=NR70 session=session-nr70.img`.

What is different from the m500:

- **Screen:** 320×480 in 16-bit colour, shown in grays. On a Clara BW it runs
  at 3× (960×1440) and fills the panel, with the buttons in a rail on the
  right. `scale = 2` in `palmkobo.ini` gives the smaller picture with the
  strip below it.
- **Silkscreen:** the Clie has no printed Graffiti area. Palm OS draws a soft
  one, and a status bar, at the bottom of its own screen, so palmkobo draws no
  silkscreen art and no Graffiti echo. Palm OS draws the stroke itself.
- **Keyboard:** Esc/F5 (Menu), F6/Home and F7 (Find) tap the icons in the
  status bar. F8 (Calculator) does nothing, because that icon only exists in
  the soft Graffiti area, which apps can collapse.
- **Buttons:** the four app keys and the scroll keys work. The menu's Palm
  power button has no effect on the emulated NR70. The jog
  dial and the Back button are not available: CloudpilotEmu has no input
  events for them.
- **Card:** `card.img` is inserted as a Memory Stick. The same image works for
  the m500, so the two can share it.
- **Speed:** the Clie's 66 MHz CPU needs roughly three times the emulation
  work of the m500. Speed on the Kobo itself has not been measured. On exit,
  `palmkobo.log` gets a line like `emulation: busy 40% of 300 s, 0 catch-ups`.
  Near 100% busy, or many catch-ups, means the Palm is running slower than
  real time.
- No sound, camera, MP3 player or other Sony hardware.

## How it works

- **Emulation:** CloudpilotEmu's core is built straight from its own Makefiles
  (`mk/core.mk` adds a headless library target). Session loading, ROM
  detection, app installing and save states are CloudpilotEmu's code.
  palmkobo adds no patches to the emulator.
- **Screen:** the Palm's 1/2/4‑bpp LCD frame is converted to 8‑bit gray at a
  whole‑number scale (6× on a Clara BW, so 960×960 for the LCD). Only the
  lines the emulator marks dirty are redrawn.
- **E-ink refresh** (`src/RefreshScheduler.h`) is tuned for drawing on an
  e‑reader:
  - While your finger is down, changed areas refresh every 40 ms with the fast
    black-and-white DU waveform.
  - When you lift your finger, everything touched that way is redrawn in
    proper grays with GL16.
  - Every 60 partial refreshes, one flashing GC16 refresh clears ghosting.
  - All of this is configurable in `palmkobo.ini`.
- **Graffiti:** Palm OS 4 doesn't draw your strokes, so palmkobo draws the
  finger trail in the Graffiti box itself. This "echo" is erased when you
  lift your finger.
- **Nickel:** the launcher pauses `nickel`, `sickel` and `hindenburg` with
  SIGSTOP and resumes them afterwards. palmkobo grabs the touch and power
  input devices so Nickel doesn't replay your taps when it wakes up.

## Known limitations and risks

- **The Kobo doesn't sleep while palmkobo runs.** The emulated Palm turns
  itself off, but the e-reader stays awake until palmkobo's idle exit kicks in
  (15 minutes by default).
- **Pausing Nickel is a hack.** It's the same approach other Kobo homebrew
  uses, but if something goes badly wrong, hold the power button to force a
  reboot. Your books are unaffected.
- **No sound, HotSync, IR or network.** CloudpilotEmu supports some of these;
  palmkobo doesn't wire them up yet.
- **Finger, not stylus.** Graffiti works but takes practice. Tapping small
  Palm UI elements at 6× scale is fine.
- **Kobo models other than the Clara BW** should work if FBInk knows them, but
  they're untested.

## Project layout

```
src/
  main.cpp              command line, config loading
  App.cpp/.h            main loop: emulator pacing, input routing, installs, SD card, save/restore
  Geometry.h            layout (scale, placement) and touch coordinate mapping
  FrameConvert.h        Palm LCD frame (1/2/4 bpp gray, 24-bit colour) -> 8-bit gray
  RefreshScheduler.h    when/where/how to refresh the e-ink panel
  IdlePolicy.h          autosave and idle-exit timing
  Canvas.h              off-screen gray canvas, line drawing, resampling
  ButtonBar.cpp/.h      on-screen Palm hardware keys (strip or rail) + tiny 5x7 font
  Glyphs.h              one-bit icons for the keys and the menu
  Menu.h                the overlay menu: refresh, Palm power, exit
  Input.cpp/.h          evdev touchscreen, power key, keyboards (with hotplug rescans)
  Keyboard.cpp/.h       keyboard -> Palm characters, buttons and silkscreen taps
  CardFs.cpp/.h         SD card images: create (mkfs), import, export, dirty-page saving
  Display*.cpp/.h       FBInk backend (Kobo) and in-memory backend (desktop)
  Config.cpp/.h         palmkobo.ini
  Pgm.h                 PGM loader for the silkscreen art
  Version.h             version number
mk/core.mk              headless library targets for CloudpilotEmu's Makefiles
kobo/                   launcher script, default ini, NickelMenu entries, on-device README
docs/USER-GUIDE.txt     end-user guide (README of the release package)
tools/silkscreen2pgm.py extracts CloudpilotEmu's silkscreen PNG
test/                   unit tests, SD card tests, screen mock-up renderer
```

## Next steps

1. Reports from other Kobo models (touch orientation, waveforms).
2. Measure CPU and battery use. If they're high, sleep longer between loop
   iterations while the Palm CPU is halted (`gSession->IsCpuStopped()`).
3. Landscape mode; dithering for photos on colour Palms; non-US keyboard layouts.

## License

GPL-3.0-or-later. palmkobo links CloudpilotEmu (GPL-3.0) and FBInk
(GPL-3.0). See `LICENSE`. Palm OS ROMs are not included and must not
be redistributed.
