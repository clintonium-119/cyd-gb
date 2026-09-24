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
12. **Panel trim** — this one takes a few minutes and needs no computer. The screen fills with a field of
    small blocks drifting steadily upwards. Somewhere on it you should see a **seam** — a vertical line
    down the screen where the blocks on one side sit a little higher or lower than the blocks on the
    other — and it moves sideways, off one edge and back on at the opposite one.

    **If you cannot see a seam at all, change the picture until you can.** In a run the D-pad sets how
    fast and which way the field scrolls (Up/Down vertical, Left/Right sideways, through zero and out the
    other side), **B** cycles the pattern — `noise`, `check`, `stripe`, `grid` — and **A** gives up on the
    run and puts the numbers back. Nothing you do here is stored; it only changes what is on the glass.
    The page starts on `stripe` scrolling at 5, which is the pair a builder could actually read a seam
    in on the glass and the one the trim is specified at. It replaced `check` at 4, which looked safe on
    paper and was hard to use in the hand — see the two speeds to avoid, below.

    **Do not press Start to escape a run.** Start is the mark, so pressing it to get back to the numbers
    is telling the unit it saw crossings it did not. A run whose marks disagree with each other is thrown
    away rather than averaged — the page says `marks disagreed` and moves nothing — but leave by **A** and
    the question does not arise.

    Press **Start** to begin, then press **Start** again each time the seam goes off one edge and
    reappears at the other. After the fourth press the screen goes back to the numbers, and the unit has
    corrected itself once. Read **Crossing** — that is how long it was taking — and do it again. Each
    round should leave a longer interval than the one before.

    Stop when the crossings are further apart than you are willing to sit and count, when **Last move**
    reads `none left to give`, or **when the seam stops sweeping one way**. That last one is the real
    finish line and it does not look like success the first time you meet it: the seam takes an age to
    appear, crawls part of the way across, and may then turn round and go back out the side it came
    from. Nothing is wrong. Past about six minutes between crossings the two rates differ by under
    fifty parts in a million, and at that distance the panel's crystal and the board's own drift with
    temperature by about as much as is left to correct — so which of them is ahead changes while you
    sit there. There is nothing further to null. Press **A** and write the porch down. Then press **A** to save.

    The page recognises that finish line too. If a run goes about five and a half minutes without a
    crossing, it ends by itself and **Crossing** reads `none in 5 min - settled`: the unit is trimmed,
    nothing moved, and you do not need to wait for another press. If the crossings in a run each came
    later than the one before, **Crossing** reads `slowing - near the null`. That is not a marking
    mistake. The seam is slowing down as you close in, so the run is not averaged. Run once more or
    stop there. Only `marks disagreed` means the presses themselves were off.

    **Watch the Stored row.** Every run ends by moving the porch itself, so what is on screen stops
    being what a power cycle brings back until you save it — the page says `A to save` and turns both
    rows amber while that is true. And note that **A only saves from the numbers screen**: pressed during
    a run it gives up on the run instead, which is easy to do by accident and leaves the store behind.
    If Stored and Porch read the same, you are safe to switch off.
    "Saved" appears for a moment. **B** puts the porch back to the **Default** shown on the page, which is
    the compile-time value and not whatever you last saved — it is the way back to a clean start, and
    after the page has corrected itself the D-pad alone cannot get you there.

    Under about fifteen seconds between crossings is not finished — keep going. Write down the final
    **Porch**, both numbers, and the **Counted on** line beside the crossing: `____`.

    *Which pattern you count on does not bias the number.* The pattern only decides whether you can see
    the seam; the count is you pressing Start at something real. So if the seam shows up better on one of
    the others, count on that and write it in the blank.

    Two speeds to avoid, whichever pattern you are on, and they are different faults.

    A pattern can go **blind**: at a speed where the picture repeats exactly across one frame of motion,
    the two sides of a real seam line up and it vanishes. Only `stripe` can do this within the D-pad's
    range, at 8; `check` needs 16 and `grid` 32, neither of which the knob reaches, and `noise` has no
    repeat and cannot do it at any speed.

    A pattern can also **strobe**, which is not the same thing and is what the old default got wrong. The
    field returns to its own starting position every few frames whenever the speed shares a factor with
    the pattern's repeat, and then it is not scrolling at all — it is flicking between a handful of fixed
    pictures, and a one-frame break is very hard to see inside a flicker. At 4 the stripes and the noise
    field cycle in **two** frames and the checkerboard and grid in four. At 5 nothing repeats inside 8
    frames and all four genuinely move, which is why 5 is the default even though 4 is the tidier number.

    Keep the speed below 8 in either direction. A whole square of motion in one frame loses the
    continuity your eye needs — `noise` re-randomises completely and the periodic patterns realign — and
    there is no picture left to see a break in.

    *If it will not settle:* an interval that gets **shorter** on the round after a correction is normal
    once — the unit does not know which way to go until it has tried one — but twice in a row means you are
    marking something other than the seam wrapping. The exception is the finish line above: once the seam
    has stopped sweeping one way, a shorter interval is you catching it on a wander, not a bad count, and
    the answer is to stop rather than to keep going. That first blind round is once per unit, not once per
    sitting: saving stores which way the unit was going along with the porch, so coming back to a unit you
    trimmed last week picks up where you left off rather than guessing again. An interval that never grows past a few seconds however
    many rounds you do, or a Porch that ends up at `1 + 0/64` or `126 + 63/64`, is this panel's oscillator
    being further out than the trim can reach. That is the fault you came to find; write down what it
    reached: `____`.
13. **System** — Frameskip should read `0`; leave it there unless you were told otherwise. Write down
    Version and Built exactly as shown — that is how this unit gets matched to a build later: `____`.

**Finish:** switch the unit off. Put a game cartridge in and switch it on normally. The game should sit
exactly where the white frame was, and stay there after another power cycle.
