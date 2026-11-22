palmkobo - Palm OS on your Kobo
================================

1. Copy your Palm m500 ROM into this folder as  m500.rom
2. (Optional) Drop .prc / .pdb apps into the install folder.
3. Eject the Kobo, open the NickelMenu and choose "PalmOS".

Buttons along the bottom: calendar, telephone, checklist and notepad are the
Palm's four app keys, the arrows are its scroll buttons. The square button with
three bars opens a menu: REFRESH SCREEN (clears ghosting), PALM POWER BUTTON,
and EXIT TO KOBO, which saves the Palm and returns to the Kobo. On some Kobos
the Kobo's own power button also saves and exits.

Keyboard: connect it in the Kobo's Bluetooth settings first, then launch.
F1-F4 = app buttons, PgUp/PgDn = scroll, Esc = Menu, F6 = Home,
F7 = Find, F8 = Calc, F9 = refresh screen. "PalmOS input test" also shows keys as you type.

SD card: the Palm has a virtual 64 MB SD card (card.img).
- To put files ON the card: copy them into the to-card folder, then launch.
  Apps (.prc/.pdb) appear in the Palm launcher's card category.
- To get files OFF the card: after you exit, everything on the card is in
  the card-contents folder. Copy from there (don't edit it).

Your Palm is saved to session.img on exit, and autosaved every few minutes
while you use it. After 15 minutes without input palmkobo saves and returns
to the Kobo (tap or press a key when the warning appears to stay). Delete it to start fresh.
Problems? Look in palmkobo.log in this folder.
If taps land in the wrong place, run "PalmOS input test" from the menu and
adjust the touch_* settings in palmkobo.ini.
