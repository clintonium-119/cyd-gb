# CYD-GB — DMG Build Roadmap

**Branch:** `poc-gb`
**Upstream fork:** [artanergin44-collab/cyd-gb](https://github.com/artanergin44-collab/cyd-gb) (MIT)
**Emulator core:** [Peanut-GB](https://github.com/deltabeard/Peanut-GB) by Mahyar Koshkouei (MIT) — fetched by `curl`, not vendored

---

## 0. Read this first

This document is the settled design for a group build of **ten Game Boy DMG-01 units**, each containing an
ESP32-2432S024 board running a Game Boy emulator, with games selected by **physical NFC cartridges**.

Two things constrain nearly every decision below. Any agent planning work on this project should internalise
them before proposing changes:

1. **The cartridge system is the product, not a feature.** The point is that choosing a game is a deliberate
   physical act. Anything that lets a user browse and switch ROMs from the device — a ROM launcher, an
   on-device NFC writer, a "recent games" list — defeats the design and must not be added. The upstream fork's
   ROM browser is being deleted for exactly this reason.
2. **Ten units, built by kids, assembled once.** Favour solder-free connections, per-unit adjustability stored
   in NVS, and diagnostics that let a builder self-diagnose. Avoid anything that needs a rework station or
   per-unit firmware variation.

Several numbers below are **unverified** and flagged as such. They come from vendor datasheets of mixed
quality or from measurements taken off product photos. Section 11 lists every open item with the bench test
that resolves it. **Do not treat flagged values as settled.**

---

## 1. Hardware

### 1.1 Board

**ESP32-2432S024 / E32R24P** — 2.4" CYD variant. *Not* the more common 2.8" ESP32-2432S028R; pinouts differ.

| | |
|---|---|
| MCU | ESP32-D0WD-V3 (WROOM-32E), 240 MHz dual-core |
| RAM | 520 KB SRAM, **no PSRAM** |
| Flash | 4 MB QSPI |
| Panel | ST7789, 240×320, TFT |
| Active area | 36.20 (W) × 49.00 (H) mm |
| Pixel pitch | ~0.153 mm (see note) |
| PCB | 74.33 × 42.89 mm, 5.44 mm thick, 4.63 mm max SMD height |
| Mounting | 4 × Ø3.2 on 34.89 × 66.33 mm centres |

> **Datasheet inconsistency:** the vendor lists pixel size as 0.15 × 0.15 mm, which implies an active area of
> 36.00 × 48.00 mm, not the stated 36.20 × 49.00. Calculations here use the stated active area (0.1508 mm/px
> vertical, 0.1531 mm/px horizontal). Difference is ~2%; confirm by filling the screen white and measuring the
> lit area with calipers.

> **Active area is not centred on the PCB.** It sits 4.00 mm from one edge and ~21 mm from the other. Account
> for this in the mounting bracket or the image will sit off-axis behind the bezel.

### 1.2 Pin map

| Signal | Pin | Source | Status |
|---|---|---|---|
| I²C SDA | IO27 | SPI header pin 1 | proposed |
| I²C SCL | IO1 | UART header (TX0) | proposed — see §1.3 |
| 3.3 V / GND | — | EXP header | confirmed (silkscreen) |
| Amp enable | IO4 | onboard | **HIGH = on** |
| Audio DAC | IO26 | onboard | to amp |
| Battery sense | IO34 | onboard ADC | divider ratio undocumented |
| SD CS | IO5 | onboard | |
| SD SPI | IO18 / IO19 / IO23 | onboard | now exclusive to SD |
| LCD | IO15 / IO2 / IO14 / IO13 | CS / DC / SCK / MOSI | |
| Backlight | IO21 | PWM | |
| TFT_RST | −1 | panel reset ties to EN | **must be −1** |
| Unused | IO35 | EXP header, input-only | |
| Unknown | 4th EXP pad | unlabelled | **meter it** |

Confirmed from the board photo silkscreen: SPI header carries **IO27 / IO18 / IO19 / IO23**. EXP header reads
**GND / IO35 / 3.3V** plus one unlabelled pad.

**`LED_R_PIN` in the fork's `hw_config.h` is set to 4.** On this board IO4 is the audio amplifier enable. If
anything drives it as a status LED the amp will mute and unmute at random. **Remap it as part of Phase 1.**

### 1.3 The I²C pin decision

I²C needs two bidirectional pins. Only IO27 is cleanly available on a connector, so the second comes from the
UART header.

**SCL must be IO1, not SDA.** IO1 is TX0: the ROM bootloader prints on it at every power-up. With SCL there,
that traffic is only clock transitions with no START condition, so every I²C device ignores it. Reversed, you
would generate a burst of spurious STARTs and STOPs.

**Never put I²C on IO3.** IO3 is RX0, driven push-pull by the USB-serial bridge whenever USB is powered —
*including while charging*. Open-drain I²C cannot pull against it.

Run bus recovery before `Wire.begin(27, 1)`: nine manual SCL pulses, then a STOP. 400 kHz is fine.

> **Contingency:** if the 4th EXP pad meters out as **IO22**, use it for SCL instead. IO1 stays free, the boot
> noise question disappears entirely, and no bus recovery is needed. Check this first — it is the better
> outcome and costs 30 seconds.

### 1.4 Peripherals

| Device | Bus | Address | Notes |
|---|---|---|---|
| MCP23017 | I²C | **0x20** (A0–A2 → GND) | 16 GPIO; `RESET` → 3.3 V, never float |
| PN532 | I²C | **0x24** | DIP switches to I²C mode |

0x20 and 0x24 do not collide. Keep the expander off 0x24 if the address is ever changed. Fit 4.7 kΩ SDA/SCL
pull-ups if neither breakout has them.

**Button PCB** — existing 8-output + common-ground board, wired to GPA0–GPA7:

| | | | |
|---|---|---|---|
| GPA0 | Up | GPA4 | A |
| GPA1 | Down | GPA5 | B |
| GPA2 | Left | GPA6 | Start |
| GPA3 | Right | GPA7 | Select |

Enable internal pull-ups (`GPPU = 0xFF`), active LOW. **INT is not used** — polling once per frame is one
byte, ~60 µs at 400 kHz. GPB0–GPB7 are spare.

### 1.5 Power

```
LiPo (+) ──> SPST SWITCH ──> CYD BAT +
LiPo (−) ───────────────────> CYD BAT −
```

3.7 V pouch cell, ~2000 mAh, max ~60 × 35 × 7 mm (fits the AA bay), **with an integrated protection PCB** —
the CYD's charger has no cell protection of its own.

Charging uses the CYD's onboard charger via a short USB-C male-to-female pigtail routed into the battery
compartment. The DMG battery door pops off without tools, so no shell machining is needed.

**Charge with the switch ON.** The switch cuts the cell off from the entire board, charger included. Switch
off means no charging. Note that plugging USB in powers the board on regardless of switch position.

> **Meter cell polarity against the BAT silkscreen before first connection.** JST is not a polarity standard;
> reversed, you can destroy the board, the cell, or both. Add this to the assembly checklist.

### 1.6 Audio hardware

Onboard amp on IO26 (8-bit internal DAC) with **hardware enable on IO4**.

The Game Boy's own audio hardware is 4-bit, so an 8-bit DAC has more resolution than the source. The DAC is
not the limiting factor; the amplifier is.

Escalation path, in order:

1. **Test first.** `tone()` on IO26 with the stock speaker. Square waves and noise are far more forgiving than
   music. There is a real chance nothing is needed.
2. **Resistor mod.** The known-bad 2.8" ST7789 CYD has fixed gain of ~×14.5 and an amp input impedance of
   ~4.7 kΩ that loads the DAC buffer enough to clip half the waveform. Gain ≈ `2 × (Rf / Ri)`; raising Ri
   fixes both problems at once. Designators on the 2.8" board are R7/R8/R9 — **these will not map to the 2.4"
   board.** Trace the resistor pair between IO26 and the amp input, plus the feedback resistor. Passives
   measure as **0603** (2.9 mm pad-to-pad, 1.6 mm pitch); an 0603 assortment is on hand.
3. **MAX98357A I²S amp.** Last resort. Costs three GPIO (IO22 / IO16 / IO17, all RGB-LED pins requiring
   soldering) *and* the IO4 hardware mute. See `assets/DMG-CYD-audio-mod.pdf`. Prefer the resistor mod.

### 1.7 Mechanical

DMG-01 shell, screen cutout measured with calipers at **47 × 42.5 mm**.

The panel's short axis (36.2 mm) is smaller than the cutout height, so a border is unavoidable — roughly
3.15 mm per edge minimum regardless of scale. The 3D-printed bezel masks it.

The PCB is 42.89 mm on its short axis against a 42.5 mm cutout — about 0.2 mm margin per edge. **Design the
bezel with a rear flange overlapping the cutout from behind**, 1–1.5 mm, so misalignment is absorbed by the
print rather than showing as a bright sliver.

The board is shifted **~4 mm toward the USB-C side** so a slot in the shell can reach the connector. This is
compensated in software via `GAME_X` (see §2.2), not by moving the image physically.

---

## 2. Rendering

### 2.1 Orientation and scale

**Landscape.** The panel is 320 px (49.0 mm) horizontal × 240 px (36.2 mm) vertical.

Because 160 and 144 are both divisible by 16, any scale of the form **k/16** produces integer output on both
axes. Vertical caps it: 9k ≤ 240 → k ≤ 26.

| Scale | Output | Image (mm) | Rows used | SPI/frame | Bezel border |
|---|---|---|---|---|---|
| **24/16 (3/2)** | **240 × 216** | 36.75 × 32.58 | 90% | 103,680 B | 5.1 / 5.0 mm |
| 26/16 (13/8) | 260 × 234 | 39.81 × 35.29 | 97.5% | 121,680 B | 3.6 mm all round |

**Build 24/16 first**, then flip the constant and evaluate 26/16 once the core split is working. Both are
aspect-correct. 24/16 has a more regular duplication rhythm (`1,2,1,2…` vs `1,2,1,2,2,1,2,2`); 26/16 is 17%
more SPI, which is free if DMA overlap is working and costly if it isn't.

**Do not stretch to fill 240 rows.** 240×240 would mean 1.5× horizontal and 1.667× vertical — an 11% vertical
stretch. Game Boy art assumes square pixels.

### 2.2 Positioning

`GAME_X` and `GAME_Y` place the image within the panel's 320 × 240.

- Centred: `GAME_X 40`, `GAME_Y 12`
- With the 4 mm board shift: `GAME_X 14` (40 − 26 px)

**These must be adjustable from the diagnostic screen and stored in NVS.** Ten hand-built units will each land
slightly differently; a single hardcoded offset that suits the prototype will be wrong on the others. Provide a
border test pattern and two-button nudging.

### 2.3 Scaler

2 source pixels → 3 destination, **both axes**, with the interpolated pixel **blended, not duplicated**:

```
a, (a+b)/2, b
```

The pure pixels stay sharp; only the genuine seam softens. On a 4-shade palette this yields 7 shades and reads
as natural anti-aliasing.

```c
static inline uint16_t avg565(uint16_t a, uint16_t b) {
    return (((a ^ b) & 0xF7DEu) >> 1) + (a & b);
}
```

**Order matters:** `avg565` assumes native RGB565 bit layout. The upstream fork pre-swaps palette bytes. Blend
*before* swapping, or keep the palette native and let `setSwapBytes(true)` handle it at push time. Blending
byte-swapped values averages across misaligned fields and produces colour fringing.

Cost: ~29k blends/frame, roughly 0.5 ms at 240 MHz. This is the single best quality-per-millisecond change
available and matters more than the choice between 24/16 and 26/16.

### 2.4 Colour

Peanut-GB is **DMG-only** — it will not emulate Game Boy Color hardware. But `PEANUT_GB_12_COLOUR` (on by
default) packs the palette source into each output pixel: bits 1–0 are the shade, bits 5–4 identify the
palette (BG `0x20`, OBJ0 `0x00`, OBJ1 `0x10`). Three palettes × four shades = **12 simultaneous colours**, the
same mechanism a real Game Boy Color uses to colourise DMG cartridges.

`gb_colour_hash()` returns the same cartridge hash the CGB boot ROM uses, so palettes can be auto-selected
per title.

**The fork currently discards all of this** — `lcd_line()` does `p[px[x] & 3]` and `pals[][4]` has only four
entries.

Replace with a **flat 64-entry LUT** indexed by the raw pixel byte (max value 0x23):

```c
static uint16_t lut[64];              // rebuilt on palette change

void emu_set_palette(uint8_t i) {
    curpal = i;
    for (int p = 0; p < 3; p++)
        for (int c = 0; c < 4; c++)
            lut[(p << 4) | c] = pals[i][p][c];
}

// inner loop
for (int x = 0; x < 160; x++) lbuf[x] = lut[px[x]];
```

This removes the `& 3` from the inner loop (23,040 ANDs/frame) and **guarantees `lbuf` contains pure RGB565
with no palette bits riding along**, which is what makes the blend safe. Colorization therefore costs slightly
*less* than the current code, not more.

Design palettes whose three sub-ramps share a hue family. Cross-palette blending at sprite edges is real
anti-aliasing and generally desirable, but far-apart hues will fringe. **Do not add a per-pixel branch to skip
cross-palette blends** — the branch costs more than the blend.

### 2.5 Push strategy

The fork calls `tft.pushImage()` per destination line — ~216 address-window setups per frame. Replace with a
single address window per frame:

```c
tft.startWrite();
tft.setAddrWindow(GAME_X, GAME_Y, GAME_W, GAME_H);
// per 2 source lines: tft.pushPixels(block, 3 * GAME_W);
tft.endWrite();
```

Safe because Peanut-GB emits lines in order, and SD is on VSPI while the LCD is on HSPI — no bus contention.

Then `pushPixelsDMA` with two alternating row buffers so the transfer overlaps emulation.

---

## 3. Performance

### 3.1 Budget

Frame budget at 59.7 fps is **16.75 ms**.

| | ms |
|---|---|
| Peanut-GB CPU + PPU | ~8–11 *(estimated, not measured)* |
| MiniGB APU | ~1 |
| Scale + blend | ~1.7 |
| **Core 1 total** | **~11–14** |
| SPI push @ 80 MHz (core 0, overlapped) | 10.4 |

The emulation figure is reasoned from instruction counts, **not measured**, and could be off by 50% either
way. Measure before optimising.

### 3.2 The ROM cache is the biggest risk

The fork reads ROM through a paged SPIFFS cache:

```c
#define PG_SZ 4096
#define PG_N  16          // 64 KB of pages
#define B0SZ  (32*1024)   // first 32 KB always resident
...
romf.seek(pb); romf.read(pg[lru].d, ...);
```

Hit path is fast (hash lookup, ~15 cycles). **Miss path is a 4 KB SPIFFS read — call it 1.5–4 ms.** In a
16.75 ms frame, one miss is a visible hitch and a bank-switch storm is a stutter. This is why the project has a
frameskip setting.

A 32 KB or 64 KB game never misses. A 256 KB game misses occasionally. Large ROMs with aggressive bank
switching will thrash.

**Fix: memory-map the ROM.** Store it in a raw flash partition instead of SPIFFS and `esp_partition_mmap` it.
`gb_rom_read` collapses to a pointer dereference through the hardware flash cache — no page management, no
stalls, and ~96 KB of DRAM handed back for audio and row buffers.

Cost is writing the ROM to a partition on cart insert (a few seconds, ~100k erase cycles per sector — a
non-issue). Requires repartitioning; current table is app0 2 MB / SPIFFS 1.98 MB.

> Reference reading: `ROM_PARTITION_FIX_COMPLETE.md` in `GodSpoon/cyd-gb-v2`. That repo is Espeon-based and
> not a useful code base, but that document covers the pitfalls of this exact change.

### 3.3 Core split

Peanut-GB on core 1, display push on core 0 behind a queue. Serial at 80 MHz is ~12 + 10.4 ≈ 22 ms → frameskip
1, ~30 fps. Overlapped it's max(12, 10.4) ≈ 12 ms → full 60 fps.

Also try raising `SPI_FREQUENCY` to 80000000. Short traces usually take it, and it's free.

### 3.4 Reference

Retro-Go runs Game Boy at full speed with audio on the same silicon, so 60 fps DMG is settled as achievable.
**Steal its techniques — core split, DMA, one framebuffer push — not its architecture.** Retro-Go is a ROM
launcher, which is precisely what this project must not become, and adapting it would mean suppressing its
central feature and writing a new target definition.

---

## 4. Audio

Peanut-GB has **no sound emulation**. `ENABLE_SOUND` is `0` in the fork, and there is no `audio_read` /
`audio_write` implementation anywhere in the repo — setting it to `1` produces link errors. Verified.

Integrate **MiniGB APU** (companion to Peanut-GB), set `ENABLE_SOUND 1`, wire the two callbacks, and feed the
DAC on a timer. Output is 32768 Hz stereo.

**Mono:** sum in software, `(l + r) >> 1`. Do not discard a channel — some games pan effects hard left or
right and you would lose sounds entirely.

**Volume — high / med / low / off:**

```c
static const uint16_t vol_lut[3] = {256, 176, 128};   // high, med, low
static uint8_t vol = 0;                                // 0-2, 3 = off
```

Med at 0.69 rather than 0.75 spaces the steps more evenly by ear (perceived loudness ≈ cube root of
amplitude). Nothing drops below 7 bits, so quantisation grit never appears. **Off uses IO4 hardware mute**,
not scaling to zero.

Scale in 16-bit *before* truncating to 8, never on the 8-bit value.

**Dither:** add ±1 LSB of noise before truncating. Converts quantisation grit into faint hiss, which is far
less objectionable. Three lines; the best low-volume quality improvement available.

**Auto-mute:** pull IO4 low after ~200 ms of APU silence, restore on the next sample. Removes idle hiss —
often more noticeable than the audio itself — and saves current. Independent of the volume setting.

---

## 5. Input

Poll the MCP23017 once per frame — one I²C byte, ~60 µs. Debounce ~8 ms in software; the expander has no
hardware filter.

| Combo | Action |
|---|---|
| Start + Select | Menu |
| Select + Up / Down | Volume (clamped, no wrap) |
| Select + Left / Right | Brightness (IO21 PWM) |

**Mask Up/Down/Left/Right out of the button word before it reaches Peanut-GB while Select is held**, or the
character walks around during adjustment.

Edge-trigger, don't level-trigger — a held press should step once. Optional repeat: ~200 ms after a ~400 ms
initial delay. Persist volume and brightness to NVS.

Do not bind anything to a bare Start or Select press; games use them constantly.

The Start+Select menu combo is **already implemented** in the fork's `main.cpp`.

---

## 6. Cartridges and NFC

### 6.1 Design intent

Each kid gets **5 game cartridges (write-locked) + 1 wildcard (rewritable)**. Cartridges are NTAG215 25 mm
discs inside empty DMG cartridge shells, with printed game-name stickers.

**The device never writes tags.** This is deliberate, not an omission. An on-device writer would let a kid use
diagnostic mode as a ROM launcher, which is the exact interaction the cartridge system exists to prevent.
Hiding it behind a boot combo is not sufficient — in a group of ten, that becomes common knowledge within a
week. **The firmware must be physically incapable of writing tags.**

Locking is likewise done off-device, before build day, because it is irreversible. Order of operations: write
NDEF → read back and verify → **test in an actual unit** → apply sticker → lock.

### 6.2 Reading

PN532 in I²C mode at 0x24, read **once at boot / cart insert only**. Never polled during emulation —
`InListPassiveTarget` blocks for tens of milliseconds.

No re-read combo is needed: the DMG's mechanical interlock makes it impossible to remove a cartridge with the
power on, so every cart change is followed by a boot.

**Remove the DMG's internal RF shielding** or the read will fail.

### 6.3 NDEF parsing

Tags carry an NDEF Text record containing the ROM filename. The payload is **not** just the string:

```
03 <len> D1 01 <plen> 54 02 65 6E <filename...> FE
│   │    │  │   │     │  │  └"en"┘
│   │    │  │   │     │  └─ status byte
│   │    │  │   │     └──── type 'T'
│   │    │  │   └────────── payload length
│   │    └──┴────────────── NDEF record header
└───┴──────────────────── TLV
```

**Parse the language-code length from the status byte — do not assume 2:**

```c
uint8_t status  = payload[0];
uint8_t langLen = status & 0x3F;
const char *name = (const char *)&payload[1 + langLen];
```

Phones set their own language code by locale. Assuming 2 yields `nTetris.gb` on a French device.

Read raw pages with `ntag2xx_ReadPage()` and parse these few bytes directly. A full NDEF library is more
dependency than this warrants.

### 6.4 Filename matching

Tags are written by kids using a phone, so filenames arrive imperfect. Normalise before matching:

1. lowercase
2. trim whitespace
3. append `.gb` if no extension
4. substring search across the ROM directory as a last resort

The fallback turns `tetris` into a hit even when the file is `Tetris (World).gb`. Cheap to write, and it
converts a dead cartridge into a working one.

**On failure, display the string that was actually read** — "Not found: tetrsi.gb" is fixable; a black screen
isn't.

### 6.5 SD card

Every card is **identical** — one canonical library cloned ten times. This is required, because a traded
cartridge must work in any unit. Every card therefore needs every game, including ones its owner didn't pick.

Keep the game list in a JSON file in the repo, read by both the card-imaging script and the phone web app, so
the two can never drift.

### 6.6 Writing tags (off-device)

- **Android:** QR sticker on the wildcard shell → GitHub Pages web app using Web NFC (`NDEFReader`). Chrome
  only, HTTPS, user gesture, top level. Cannot lock tags, which is correct here.
- **iOS:** Web NFC is unavailable and will not be. Use NFC Tools. The web app should detect the lack of
  `NDEFReader` and offer a **clipboard fallback** — copy the exact filename for pasting into NFC Tools.
  Call `navigator.clipboard.writeText()` **synchronously inside the tap handler**; iOS drops the gesture
  context if anything is awaited first.
- Copy only the filename (`tetris.gb`), never NDEF bytes — NFC Tools builds the record wrapper itself.
- The page should **read before writing** (so a locked game cart is identified rather than failing
  confusingly) and **read back after writing** to confirm.
- Build day uses a single shared Android phone as the writing station.

---

## 7. Saves

**Battery saves only.** Peanut-GB has no state-serialization API; `gb_get_save_size_s()` is cartridge SRAM
sizing, not snapshots. A save-state implementation would require serializing `struct gb_s`, whose first ~45
lines are function pointers that change on every recompile — out of scope.

**Make saving automatic.** Manual Save/Load is a footgun with kids: one wrong menu press wipes an hour of
progress with no undo. Real cartridges just save.

- auto-load on boot
- auto-flush when SRAM is dirty and idle ~10 s
- flush on menu open
- flush on the low-battery threshold

Both Save and Load then disappear from the menu. `save_ram()` in the fork becomes the flush function; keep the
"SAVED!" toast as brief confirmation.

---

## 8. Menu and diagnostics

### 8.1 In-game menu

The fork already has `launcher_ingame_menu()` returning `0=resume 1=save 2=load 3=quit 4=calibrate 5=settings`,
plus a settings submenu covering palette, frameskip, brightness and overlays.

**Remove:**
- **Quit** — returns to the ROM launcher. This is a full game browser one menu press away and dismantles the
  cartridge system. Repurpose as **Reset Game** (`gb_reset()`) or drop.
- **Calibrate** — touchscreen calibration, gone with the touch code.
- **Save / Load** — replaced by automatic saving (§7).

**Target menu:** Resume / Volume / Brightness / Palette / Cart Info / Reset.

**Add Cart Info** — read-only display of tag UID and decoded filename. It cannot change which game is on a
cart, so it doesn't undermine anything, and it's how a kid identifies an unlabelled cart after a trade and how
you debug a failed write.

The menu is currently drawn with touch buttons (`mbtn()`). Budget time to convert it to D-pad navigation
rather than assuming it drops straight onto physical buttons.

Keep frameskip as a diagnostic even after the core split.

### 8.2 Diagnostic mode

Entered by holding Start+Select at power-on. Essential across ten hand-assembled units — it turns "it doesn't
work" into "GPA3 never goes low."

- Every button, lighting up as pressed
- SD card detected, ROM count
- PN532 responding, live UID readout
- Battery voltage from IO34
- Audio test tone
- Colour / scaling test pattern
- **`GAME_X` / `GAME_Y` nudge**, saved to NVS

**No tag writing. Ever.** See §6.1.

---

## 9. What to delete from the fork

| File | Lines | Action |
|---|---|---|
| `src/bt_scanner.cpp` | 1033 | **delete** — drags in the whole BT stack, ~1 MB flash |
| `src/touch_input.cpp` | 422 | **delete** — physical buttons |
| `src/ui_launcher.cpp` | 287 | replace — ROM browser must go (§6.1) |
| `src/display.cpp` | 52 | replace entirely |
| `src/emulator_bridge.cpp` | 196 | keep callbacks, rewrite palette path |
| `src/main.cpp` | 209 | keep, restructure |
| `src/sd_manager.cpp` | 82 | **keep as-is** |
| `include/hw_config.h` | 67 | rewrite for this board |

Delete Bluetooth first — it reclaims real partition space, which matters for the ROM partition (§3.2).

**Config changes:**
- `platformio.ini` has `-DILI9341_2_DRIVER`. This panel is **ST7789**.
- `TFT_RST` must be `-1`.
- Try `SPI_FREQUENCY` 80000000.
- Remap `LED_R_PIN` off IO4.
- `ENABLE_SOUND` 0 → 1 (only after MiniGB APU is integrated).

**Stale comments:** `display.cpp` comments describe "2x horiz, ~1.33x vert" and 192 px, but `GAME_H` is
currently 256. Trust the code, not the comments; all of it is being replaced anyway.

---

## 10. Phased workstreams

Phases 1–7 need nothing but the board on a desk. Phases 8+ need the ordered parts.

### Phase 0 — Bench checks *(blocking; do first)*
See §11. Three tests, ~30 minutes, each capable of changing the design.

### Phase 1 — Strip and boot
Delete Bluetooth and touch. Fix the driver define, `TFT_RST`, `LED_R_PIN`. Fetch `peanut_gb.h` from upstream
and pin the version. **Exit: black screen, board boots, no crashes.**

### Phase 2 — Scaler and baseline
Replace `display.cpp`. `GAME_W`/`GAME_H`/`GAME_X`/`GAME_Y` as constants, 24/16, nearest-neighbour, one address
window per frame. Add an FPS counter and a frame-time breakdown.
**Exit: a game renders, and you have a measured baseline.** This number decides everything downstream.

### Phase 3 — Colour
64-entry LUT, `PEANUT_GB_12_COLOUR`, widen the palette table to `[N][3][4]`. Re-measure.
**Exit: 12-colour output, frame time unchanged or slightly better.**

### Phase 4 — Blend
`avg565` on both axes. Verify byte-order handling (§2.3). Re-measure.
**Exit: visibly smoother scaling, ~0.5 ms cost.**

### Phase 5 — Performance
Core split (emulation core 1, push core 0), `pushPixelsDMA` with double row buffers, `SPI_FREQUENCY` 80 MHz.
Then mmap the ROM from a raw partition; repartition as needed. Re-measure after each.
**Exit: 60 fps with no frameskip, no stalls on a large ROM.**
Then flip to 26/16 and re-measure; decide 24/16 vs 26/16 on evidence.

### Phase 6 — Audio
MiniGB APU, `ENABLE_SOUND 1`, timer-fed DAC, mono sum, dither, IO4 mute, auto-mute on silence, 4-state volume.
Re-measure.
**Exit: audio at 60 fps, no idle hiss.**

### Phase 7 — Input
MCP23017 over I²C, bus recovery, per-frame poll, debounce, combos, D-pad masking, NVS persistence.
**Exit: full control from physical buttons; touch code fully gone.**

### Phase 8 — NFC
PN532 I²C, boot-time read, NDEF parse with language-length handling, filename normalisation, `carts.txt`
fallback, clear failure display.
**Exit: tapping a cart at boot loads the right ROM.**

### Phase 9 — Menu and saves
Rebuild the menu for D-pad. Remove Quit/Calibrate/Save/Load. Add Cart Info. Automatic save flush including the
low-battery path.
**Exit: no path from the UI to browsing or switching ROMs.**

### Phase 10 — Diagnostics
Boot-combo diagnostic screen per §8.2, including `GAME_X`/`GAME_Y` nudge to NVS.
**Exit: a builder can self-verify a unit without a computer.**

### Phase 11 — Mechanical and build day
Bezel print and fit, USB-C slot, battery harness, assembly checklist (including the polarity check), SD card
imaging script, phone web app, pre-assembly flashing station (ESP Web Tools over USB-C — flash *before* the
board goes in the shell).

---

## 11. Open items — resolve on the bench

| # | Question | Test | Impact if it goes badly |
|---|---|---|---|
| 1 | Does the board run from a 3.7 V cell? | Bench supply, sweep 4.2 → 3.3 V, watch the display | Needs a boost module; changes the whole power section |
| 2 | What is the 4th EXP-header pad? | Toggle IO22/16/17, meter the pad | If IO22: move SCL there, delete the IO1 boot handling |
| 3 | Is the onboard amp usable? | `tone()` on IO26, stock speaker, at full volume | Resistor mod, then MAX98357A |
| 4 | Does BAT actually power the system? | Cell connected, USB unplugged — does it boot? | Charge-only would need a boost + USB VBUS feed |
| 5 | Actual pixel pitch | Fill screen white, caliper the lit area | Recompute all bezel and `GAME_X`/`GAME_Y` numbers |
| 6 | IO34 divider ratio | Compare ADC reading to a meter across the cell range | Low-battery cutoff thresholds are wrong |
| 7 | Max reliable `SPI_FREQUENCY` | Sweep 40 / 55 / 80 MHz, look for artifacts | Directly caps frame rate |
| 8 | Real emulation frame time | Phase 2 FPS counter | May need Retro-Go's gnuboy core instead of Peanut-GB |

---

## 12. Assets

| File | Contents |
|---|---|
| `assets/DMG-CYD-wiring.pdf` | Wiring diagram over the actual board photo, BOM, pin map, boot-order and power notes |
| `assets/DMG-CYD-audio-mod.pdf` | MAX98357A fallback — solder points, blink-test identification procedure, wiring |

Both are A3 landscape. Pin assignments in them are proposals derived from the vendor pin table and board
photos; §11 items 2 and 4 supersede them if the bench says otherwise.

---

## 13. Things not to do

Collected because each was considered and rejected for a reason that isn't obvious from the code.

- **Don't add a ROM browser or on-device tag writer.** §6.1.
- **Don't switch to Retro-Go.** It's a launcher; adapting it means suppressing its central feature and writing
  a new target definition. Steal techniques, not architecture. §3.4.
- **Don't put I²C on IO3.** USB-serial drives it whenever USB is powered, including while charging. §1.3.
- **Don't reuse IO4** for anything but the amp enable. §1.2.
- **Don't stretch the image to 240 rows.** 11% vertical distortion. §2.1.
- **Don't blend byte-swapped pixels**, and don't blend before the palette LUT. §2.3.
- **Don't branch per-pixel to skip cross-palette blends.** Costs more than the blend. §2.4.
- **Don't add manual Save/Load.** §7.
- **Don't lock tags from any device the kids control.** Irreversible. §6.1.
- **Don't use `pushImage` per line.** §2.5.
