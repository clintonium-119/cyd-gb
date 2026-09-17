# Diagnostic screen — one-page check

**What you need:** the unit, a charged battery, one game cartridge, one blank or MENU cartridge. No
computer.

**Get in:** hold **Start + Select** while switching the unit on. Nothing is read from a cartridge on the
way in, and nothing is written at any point.

**Move around:** **Select + Left/Right** changes page. The plain D-pad belongs to the page you are on.

**Get out:** switch the unit off. There is no other way, and that is deliberate.

Work down the list. Write the result in the blank; a blank you cannot fill is the fault you came to find.

1. **Buttons** — press each of the eight in turn. Every one should light its own row, and only its own.
   A row that never lights names the expander pin behind it, which is what to say when reporting it:
   `____`.
2. **SD card** — should read `mounted`. ROMs and Catalog should both show the number of games this
   card was imaged with (that number comes from `games.json`, not from the bench): `____`.
3. **NFC tag** — hold the **game cartridge** against the back of the shell and press **A**. State should
   read `one tag`, Class should read `game`, and Text should be the game's file name: `____`.
4. **NFC tag, again** — swap in the **blank or MENU cartridge** and press **A**. State `one tag`, and
   Class `blank` or `MENU`: `____`.
5. **NFC tag, empty** — take the cartridge away and press **A**. State should read `no tag` within about a
   second: `____`.
6. **Battery** — Cell should sit between 3.5 V and 4.2 V on a charged cell (that range is the cell's
   datasheet, not a measurement of this board). Note all three numbers as shown: `____`.
7. **Audio** — press **A** for the tone. It should be steady, not buzzing. **Down** three times: it should
   get quieter twice, then stop. **Up** three times brings it back. Silence at `Off` is correct — the board
   has no mute, so `Off` parks the output at mid-scale: `____`.
8. **Display, colour bars** — eight bars, left to right: white, yellow, cyan, green, magenta, red, blue,
   black. Two neighbours that look the same mean a dead colour channel: `____`.
9. **Display, border** — press **Down** for the next pattern. A white frame with a cross through the
   middle. All four sides of the frame should be visible through the bezel, with no black gap outside it
   and no edge cut off. If either, do step 11 before going on: `____`.
10. **Display, checkerboard** — **Down** again. A fine grey pattern with no coloured speckle in it. Coloured
    speckle is a real fault; an evenly grey look is correct: `____`.
11. **Nudge** — the D-pad moves the whole picture one pixel at a time. Line the white frame up inside the
    bezel, then press **A** to save. "Saved" appears for a moment. **B** puts it back to the factory
    position if you want to start over. Final values: `____`.
12. **System** — Frameskip should read `1`; leave it there unless you were told otherwise. Write down
    Version and Built exactly as shown — that is how this unit gets matched to a build later: `____`.

**Finish:** switch the unit off. Put a game cartridge in and switch it on normally. The game should sit
exactly where the white frame was, and stay there after another power cycle.
