# CYD-GB — DMG Build Roadmap

**Branch:** `poc-gb`
**Upstream fork:** [artanergin44-collab/cyd-gb](https://github.com/artanergin44-collab/cyd-gb) (MIT)
**Emulator core:** [Peanut-GB](https://github.com/deltabeard/Peanut-GB) by Mahyar Koshkouei (MIT) — currently
vendored at `include/peanut_gb.h` (4044 lines, **no version marker**). The README's "fetch with `curl`" step is
stale. Pin to a specific upstream commit and record the SHA in the file header (Phase 1).
**Work breakdown:** `../ROADMAP.md` (repo root) maps §10 onto apo workstreams.

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

3. **No hardware is on hand yet (as of 2026-08-27); everything is on order.** The software is to be taken as
   far as possible before parts arrive, so every phase has two exits: **code-complete** (builds, passes
   host-side tests, reviewed) and **bench-verified** (the §10 Exit line, run on real hardware). Anything the
   bench could overturn — I²C pin choice, expander part, scale factor, SPI clock, volume LUT — must be a
   single named constant, not a design baked into the code. A host-side test harness (native PlatformIO
   env) substitutes for the board wherever logic can be exercised without one.

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
| I²C SDA | IO22 | CN1 | **verified (rev C)** |
| I²C SCL | IO27 | CN1 | **verified (rev C)** — see §1.3 |
| 3.3 V / GND | — | CN1 | **verified (rev C)** |
| Unused | IO4 | onboard | **not an amp enable** — see §1.6; leave it alone |
| Audio DAC | IO26 | onboard | to amp (always live; no enable pin exists) |
| Battery sense | IO34 | onboard ADC | divider ratio undocumented |
| SD CS | IO5 | onboard | |
| SD SPI | IO18 / IO19 / IO23 | onboard | now exclusive to SD |
| LCD | IO15 / IO2 / IO14 / IO13 | CS / DC / SCK / MOSI | |
| Backlight | IO21 | PWM | |
| TFT_RST | −1 | panel reset ties to EN | **must be −1** |
| Spare | IO35 | P3 header, input-only | verified (rev C) |

Metered on the bench (wiring PDF rev C): **CN1 is a 4-pin 1.25 mm connector carrying GND / IO22 / IO27 /
3.3 V** — the whole I²C bus, power included, on one plug. **P3 carries GND / IO35 / IO22 / IO21** and is
spare (IO21 is the backlight — leave it alone; IO35 is input-only). The vendor pin table got the header
pinout wrong; the silkscreen readings that previously stood here are superseded. No peripheral wiring is
soldered to the CYD anywhere in this build — the SW1 bridge (§1.5) is the one solder joint.

**`LED_R_PIN` in the fork's `hw_config.h` is set to 4.** The vendor datasheet calls IO4 the audio amplifier
enable, but the bench says otherwise (§1.6) — its real function is unknown. Remap `LED_R_PIN` off it (done
in Phase 1) and leave IO4 unused.

### 1.3 The I²C pin decision

**Resolved on the bench (wiring PDF rev C): I²C lives entirely on CN1 — SDA on IO22, SCL on IO27, with
3.3 V and GND on the same 4-pin 1.25 mm connector.** Both the MCP23017 and the PN532 hang off that one plug.
`Wire.begin(22, 27)` at 400 kHz, and no bus recovery is needed.

The earlier proposal put SDA on IO27 (SPI header) and SCL on IO1 (TX0), which dragged in bootloader-noise
reasoning, a nine-pulse bus-recovery routine, and a never-use-IO3 warning. All of that is gone: neither UART
pin carries I²C now, so the ROM bootloader's power-up output and the USB-serial bridge are irrelevant to the
bus — and serial logging no longer conflicts with the buttons.

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
| GPA7 | Up | GPA3 | Start |
| GPA6 | Down | GPA2 | Select |
| GPA5 | Left | GPA1 | A |
| GPA4 | Right | GPA0 | B |

The bottom 8 header pins on the button PCB run in order to GPA7 → GPA0, so the harness is a **straight
ribbon, not a crossed one** (rev C — this fixes the map above). The PCB's X/Y/L/R pads are unused — a DMG
shell has no buttons for them — and its common ground is bridged separately from the header to module GND.

Enable internal pull-ups (`GPPU = 0xFF`), active LOW. **INT is not used** — polling once per frame is one
byte, ~60 µs at 400 kHz. GPB0–GPB7 are spare.

> **The fork's `src/button_input.cpp` is not this.** It drives a **PCF8574** at 0x20 on **IO16/IO17** at
> 100 kHz with no debounce. IO16/IO17 are RGB-LED pins needing solder, and the PCF8574 has no per-pin
> pull-up register. Confirmed 2026-08-27: the button PCB uses an **MCP23017**. Rewrite the driver; keep only
> the `button_init / button_update / button_get_buttons` interface (§9). Put the bus pins, address and
> expander type behind constants in `hw_config.h` so a bench surprise is a one-line change.

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

**Bridge SW1's two pads.** SW1 — the CYD's onboard power button — is a momentary that latches the battery
rail on: one press after the battery is connected, and pressing it again does nothing. It has no off path,
so without a bridge a sealed unit will not start. Before making the bridge permanent, hold SW1 while
connecting the battery; if the unit boots, a permanent bridge is safe (§11 item 9). The DMG's original
slide switch stays the real power switch, in series with the cell.

> **Meter cell polarity against the BAT silkscreen before first connection.** JST is not a polarity standard;
> reversed, you can destroy the board, the cell, or both. Add this to the assembly checklist.

### 1.6 Audio hardware

Onboard amp fed by IO26 (8-bit internal DAC). **Resolved on the bench (rev C): the onboard amp is good
enough** — verified with real Game Boy music from SD, on battery. No resistor mod, no MAX98357A.

**IO4 is not an amp enable.** The vendor datasheet says it is; on the bench the mute command printed and the
audio kept playing. There is no hardware mute at all, so volume is software-only and "off" must hold the DAC
at 128 (mid-scale, not 0) — see §4. Auto-mute-on-silence is likewise impossible.

The Game Boy's own audio hardware is 4-bit, so an 8-bit DAC has more resolution than the source. The DAC is
not the limiting factor; the amplifier is — and it passed.

The rejected escalation path (resistor mod, then a MAX98357A on IO22/IO16/IO17) is preserved in
`reference/DMG-CYD-audio-mod.pdf` for the record only; IO22 now carries I²C SDA in any case.

### 1.7 Mechanical

DMG-01 shell, screen cutout measured with calipers at **47 × 42.5 mm**.

The panel's short axis (36.2 mm) is smaller than the cutout height, so a border is unavoidable — roughly
3.15 mm per edge minimum regardless of scale. The 3D-printed bezel masks it.

The PCB is 42.89 mm on its short axis against a 42.5 mm cutout — about 0.2 mm margin per edge. **Design the
bezel with a rear flange overlapping the cutout from behind**, 1–1.5 mm, so misalignment is absorbed by the
print rather than showing as a bright sliver.

The board is shifted **~4 mm toward the USB-C side** so a slot in the shell can reach the connector. This is
compensated in software via `GAME_X` (see §2.2), not by moving the image physically.

**Everything drawn full-screen — the in-game menu, the cart writer, diagnostics — must render inside
`GAME_X`/`GAME_Y`/`GAME_W`×`GAME_H`.** The bezel masks the rest of the 320×240 panel on every unit (§8,
§11 item 14).

**Cartridge tags.** Single **anti-metal** NTAG215 25 mm discs — not plain tags plus ferrite stickers, which
detune a tag that was not tuned for them. A printed cup glued into a 1.125" bore in the cart's label face,
well depth 1.4 mm, three tabs at ~0.30 mm overhang to retain the disc (tab size verified in §11 item 13),
Ø5 push-out hole in the floor. The two-sided menu/wildcard puck was considered and dropped.

**PN532.** Antenna spiral toward the cart, ferrite sheet on the component side between module and CYD,
mounted behind the cart slot as far from the CYD as the shell allows.

**Disc storage.** A printed tray in the DMG battery bay, four discs per layer — **gated on the battery
relocating below the button PCB; measure before designing.**

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
amplitude). Nothing drops below 7 bits, so quantisation grit never appears. **Off holds the DAC at 128**
(mid-scale, not 0 — rev C), because no hardware mute exists (§1.6).

Scale in 16-bit *before* truncating to 8, never on the 8-bit value.

**Dither:** add ±1 LSB of noise before truncating. Converts quantisation grit into faint hiss, which is far
less objectionable. Three lines; the best low-volume quality improvement available.

**Auto-mute is gone.** It needed the IO4 hardware mute, which does not exist (rev C). The amp is always
live; whatever idle hiss remains with the DAC parked at mid-scale is a bench observation for §11, not
something the firmware can remove.

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

Each kid gets a **MENU cart, one wildcard, and a set of game carts**, all NTAG215 25 mm **anti-metal** discs
inside empty DMG cartridge shells with printed game-name stickers. Choosing a game is still a deliberate
physical act: you pick up a cart, put it in the slot, and power on.

**The device writes tags itself, on-device.** The 2026-09-02 amendments (`reference/NFC_AMENDMENTS.md`)
replaced the earlier "physically incapable of writing" rule with four properties that preserve the same
intent through a different mechanism:

1. **The writer is reachable only by booting with a MENU cart**, or through the one-shot first-boot
   provisioning wizard (§6.6), which is re-armed only by clearing NVS at the flashing station over USB. No
   button combo, no settings entry, no hidden path. The cart is the key; the wizard is an adult-with-a-
   computer gate of the same class the phone station used to be.
2. **Selecting a game in the writer never launches it.** From a MENU cart it records a *pending write* and
   tells the kid to power off and swap carts; in the wizard it writes the blank cart in the slot and halts.
   Playing the chosen game always costs a power cycle and a cart swap.
3. **A running game has no path back to selection.** Unchanged, and still what the menu's exit criteria
   protect (§8.1).
4. **The device never writes content to a tag that carried a game at boot.** Writes land on the *next* cart
   presented after a power cycle, or on a blank cart during setup.

Property 2 does the real work. The writer is a browser in the literal sense, but it cannot be a launcher:
a selection costs exactly the friction the cartridge system was built to impose.

**Protection is by password, never by lock bits.** NTAG21x lock bits are irreversible; across ~60 tags
written by kids, mistakes are certain, and an irreversible mistake is landfill. Password-protected tags are
rewritable by any unit through the MENU cart (§6.6).

**Tags are self-describing and the device stores no UIDs.** Carts stay tradeable — any kid's cart, wildcard
or menu cart works in any unit — a reflash orphans nothing, and there is no per-tag table to corrupt. The
ROM **filename is the permanent key** and is frozen once carts are written; the current 132-title library
tops out at 30 characters, and every buffer follows `ROM_STORE_NAME_MAX` (64).

| Payload | Meaning |
|---|---|
| `MENU` | Menu cart. Boots the writer. Never a write target. |
| `WILD:<rom>` | Wildcard. Loads `<rom>`; flagged as expected-to-change so the UI can name it. |
| `<rom>` | Ordinary game cart. Loads `<rom>`. |
| *(blank)* | Factory tag — an empty NDEF message `03 00 FE` or all zeros both count. |

The `WILD:` prefix is informational; protection is uniform across every tag the build writes.

### 6.2 Reading

PN532 in I²C mode at 0x24, read **once at boot / cart insert only**. Never polled during emulation —
`InListPassiveTarget` blocks for tens of milliseconds.

**Call `InListPassiveTarget` with `MaxTg = 2`.** If two targets answer, halt with "Shielding fault" rather than
pick one. This catches a real assembly-error class — a stray disc left in the shell, a tag stuck to the
reader — for free, and it stays even though the two-sided disc it was first designed for was dropped.

**Read before initialising the display and SD**, while the RF environment is quietest. Retry once after the
display is up only so an error can be shown.

No re-read combo is needed: the DMG's mechanical interlock makes it impossible to remove a cartridge with the
power on, so every cart change is followed by a boot.

**Remove the DMG's internal RF shielding** or the read will fail. Anti-metal discs have roughly **half the read
range** of plain tags; the budget is verified at final geometry on the bench (§11 item 10).

### 6.3 NDEF parsing and composition

Tags carry an NDEF Text record containing the payload from §6.1. The payload is **not** just the string:

```
03 <len> D1 01 <plen> 54 02 65 6E <payload...> FE
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

Phones set their own language code by locale; a legacy hand-written tag from a French device would otherwise
read as `nTetris.gb`. The device itself always composes with `en` (2 bytes).

**Compose** is the same layout in reverse: TLV, record header, status byte `0x02`, `en`, payload, terminator
`FE`, padded to a 4-byte page boundary. **Classify blank** explicitly: an empty NDEF message (`03 00 FE`) and
an all-zero user area are both "blank", and both are valid wizard targets.

Read raw pages with `ReadPage` and parse these few bytes directly. A full NDEF library is more dependency than
this warrants.

### 6.4 Filename matching

Tags are written by the device from the catalog (§6.5), so **exact match against the ROM directory is the
rule.** The normaliser and substring fallback from the original design stay as a legacy path for tags that
were hand-written from a phone:

1. lowercase
2. trim whitespace
3. append `.gb` if the name does not **end in `.gb`** — test the suffix, not "contains a dot":
   `Snow Bros. Jr..gb`, `Dr. Mario.gb` and `Super R.C. Pro-Am.gb` are all real library entries
4. substring search across the ROM directory as a last resort

**On failure, display the string that was actually read** — "Not found: tetrsi.gb" is fixable; a black screen
isn't.

### 6.5 SD card

Every card is **identical** — one canonical library cloned ten times. This is required, because a traded
cartridge must work in any unit. Every card therefore needs every game, including ones its owner didn't pick.

```
/roms/gb/<name>.gb       132 ROMs, ~30 MB
/art/<stem>.565          96×96 raw RGB565, little-endian, ~18 KB each, ~2.4 MB total
/saves/<name>.sav
/catalog.txt             generated — see below
```

Cards are 128 MB; the library uses under a quarter of that.

**Art is pre-converted, not decoded on the device.** PNG decoding on the ESP32 is slow and heap-hungry. The
imaging tool flattens RGBA onto a solid background and emits raw RGB565 with `-pix_fmt rgb565le` — the same
byte order the frame path pushes with `setSwapBytes(true)` (§2.3), so one push configuration serves both.
Loading is `fread` into a static buffer and one `pushImage`. A missing art file draws a placeholder, never
fails.

**`games.json` is the single source of truth**, kept in the repo and validated in CI. Fields: `filename`,
`title`, `description`, `art`, `starter`, `developer`, `publisher`, `year`, `genre`, `players`. It is seeded
once from an ES-DE `gamelist.xml` through an alias map (the scraper's names and paths do not match the
library's filenames), then hand-curated — scraped descriptions run 130–1050 characters and are not written
for kids.

**The firmware never parses `games.json`.** `tools/image_sd.py` generates `/catalog.txt` from it: TSV, one
line per entry — `filename`, `title`, `flags` (`starter`), `description` — plain ASCII, description ≤ 200
bytes, line length capped by the tool. It is never hand-edited, so the two cannot drift. The firmware reads it
with a small pure-C reader in `lib/gbcore/cart/` that builds a static index of `{offset, filename, title}`
and reads a line's description on demand. Reasons:

- no ArduinoJson: no extra flash, no document heap;
- bounded static memory on a board with no PSRAM;
- `games.json` can grow fields freely without touching firmware;
- a malformed library fails the CI validator on your machine, not a unit on build day.

### 6.6 Writing tags (on-device)

**Boot decision flow.** Classify the payload *first*; only then consider pending writes:

```
power on
  └─ read tag (InListPassiveTarget, MaxTg = 2, ~1 s)
       ├─ no target ................. "No cartridge"            (halt)
       ├─ two targets ............... "Shielding fault"         (halt)
       ├─ unreadable / not Text ..... "Unreadable tag"          (halt)
       │
       ├─ setup not finished ........ provisioning wizard (below)
       │
       ├─ MENU ...................... open writer — never a write target
       ├─ pending write set?
       │    ├─ tag matches target ... write → verify → protect → clear pending → load
       │    └─ otherwise ............ "Insert your wildcard"    (halt)
       ├─ blank ..................... "Blank cart. Use your MENU cart."  (halt)
       ├─ WILD:<rom> / <rom> ........ resolve, load
       └─ resolve failure ........... "Not found: <string read>" (halt)
```

**Halt means halt.** No retry loop, no fallback browser; power-cycle is the retry (§6.2). While a pending
write is set, every boot first shows *"Pending: <TITLE> — insert your wildcard"* for a second or two so a kid
who forgot what they were doing does not have to guess.

**Pending record** — one NVS entry, overwritten by a later selection:

```c
struct pending_t {
    char    rom[ROM_STORE_NAME_MAX];  // filename as it appears in the catalog
    uint8_t target;                   // WILDCARD | NEW_CART | REWRITE
};
```

| Target | Matches | Set from |
|---|---|---|
| `WILDCARD` (default) | any `WILD:` tag | selecting a game in the writer |
| `NEW_CART` | a blank tag | "New cart" in the writer |
| `REWRITE` | any tag that authenticates with the build password and is not `MENU` | "Rewrite a game cart" — heavier confirmation wording |

The sequence write → read-back verify → protect → clear pending → load is **idempotent**: power loss before
"clear pending" simply repeats it on the next boot.

**First-boot provisioning wizard.** A fresh flash leaves three NVS flags unset: `menu_done`, `wild_done`,
`setup_done`. Until `setup_done` is set, a boot with a blank tag enters the wizard. One write per boot — the
DMG interlock forces a power cycle per cart anyway — so each line below is a boot:

1. *"Make your MENU cart."* Write `MENU`, protect, "Power off."
2. *"Pick your wildcard's first game."* Starter picker (§8.3). Write `WILD:<rom>`, protect, "Power off." The
   wildcard is never empty.
3. *"Make a game cart."* Starter picker, carts already made this setup marked. Write `<rom>`, protect,
   "Power off." Repeat.
4. **Finish setup** — an entry in the picker, hold to confirm. Sets `setup_done`; the wizard is unreachable
   from then on.

The wizard accepts a blank tag *or* an existing `MENU` / `WILD:` tag that authenticates with the build
password, so re-running it after an NVS clear re-adopts the kid's existing carts rather than demanding new
ones. **Run it after the shell is assembled** — it doubles as the per-unit NFC read test through the shell.
A lost menu cart is not a brick: any unit's menu cart works, and failing that an adult clears NVS and the
wizard runs again.

**Protection.** After a successful write and read-back verification:

- `AUTH0` → `0x04` (first user page: all user memory and the config pages are covered)
- `ACCESS` `PROT` → 0 — **write-protect only**; reads stay open so a phone or the diagnostic inspector can
  still see the tag
- `AUTHLIM` → 0 — no lockout counter; a lockout would be as bad as a lock
- `PWD` / `PACK` → build-wide constants in `hw_config.h`

> **The password is not a secret.** It is shared across all ten units — it has to be, or carts would not be
> tradeable — and it may live in the repo. Its only job is stopping a stray phone from clobbering a cart.
> Nobody should later mistake it for a security boundary.

Any valid tag found unprotected at boot is protected after the game loads (a config-only write, so property
4 holds). That heals a tag that lost power between write and protect and needs no separate tooling.

**Page whitelist, enforced in code and unit-tested.** Writable: user pages and the four config pages. Never
page 2 (static lock bytes), page 3 (capability container — one-time programmable), or `0x82` (dynamic lock
bytes). **Never set `CFGLCK`**, which freezes the configuration permanently. This whitelist is the mechanical
replacement for the old "never write" rule.

**Verification gotchas.** `PWD` and `PACK` read back as zeros — verify protection by issuing `PWD_AUTH` and
comparing the returned `PACK`, not by reading the page. `PWD_AUTH` goes through the PN532's raw data-exchange
command, not the page-read helper, which is one reason to write a minimal I²C-only driver rather than pull in
the Adafruit library's SPI/HSU paths.

**Part.** NTAG215 throughout. Config page addresses differ between NTAG213/215/216: hard-code the NTAG215 map,
verified against the NXP datasheet for the exact part ordered — not a tutorial — and run `GET_VERSION` on the
first tag out of the bag (§11 item 12).

**Guard tests** replace the old "no write symbol" test:

- write symbols (`nfc_write_*`, `PWD_AUTH`, config-page writes) are referenced only from `cart_provision.cpp`;
- exactly one call site for `writer_open()`, inside the MENU / wizard branch of the boot state machine;
- writer translation units reference no write, `emu_*` or `rom_store_*` symbols — the writer UI (§8.3)
  returns a selection and never writes; the boot state machine decides write-now versus pending;
- the page whitelist rejects pages 2, 3 and `0x82` and any config write that sets `CFGLCK`.

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

**The dirty-tracking hooks are stubs.** `emu_cart_ram_dirty()` returns `false`, `emu_get_cart_ram_last_write_ms()`
returns `0`, and `gb_cram_w()` never sets anything. Auto-flush needs `gb_cram_w` to set a dirty flag and a
last-write timestamp; only bytes inside the cartridge's real save size (`gb_get_save_size_s`) count.

Saves stay on the **unit's** SD card (`/saves/<rom>.sav`, as the fork already does), not on the cartridge —
an NTAG215 holds 504 bytes. A traded cart therefore does not carry its owner's progress. Accepted.

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

**Add Cart Info** — read-only display of tag UID, decoded payload, its classification (`MENU` / `WILD:` /
plain), and protection state. It cannot change which game is on a cart, so it doesn't undermine anything, and
it's how a kid identifies an unlabelled cart after a trade and how you debug a failed write.

**The menu is not a route to the writer.** The writer (§8.3) is reachable only through the boot flow in §6.6.

**The menu renders inside `GAME_X`/`GAME_Y`/`GAME_W`×`GAME_H`.** The bezel masks everything outside that
window (§1.7); a menu drawn to the full panel is partly hidden behind plastic on every unit.

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
- **Read-only tag inspector** — UID, `GET_VERSION`, protection state, raw NDEF hex, decoded payload

**No write path here.** The §6.6 guard tests cover this module: write symbols live only in
`cart_provision.cpp`. The diagnostic screen renders inside the game window like everything else (§1.7).

### 8.3 Cart writer

Full-screen UI reached only through the boot flow of §6.6 — from a MENU cart (pending mode) or from the
first-boot wizard (setup mode). It renders inside `GAME_X`/`GAME_Y`/`GAME_W`×`GAME_H`.

- Scrolling list of titles from `/catalog.txt`, D-pad navigated with key repeat; Left/Right jump a page. The
  library is 132 titles, so paging matters.
- Setup mode lists **starters only** (`starter` flag); pending mode lists the whole catalog.
- The highlighted entry shows its 96×96 art and description. **Debounce art loading** (~150 ms of stillness)
  so holding Down doesn't trigger an SD read per row; a missing art file draws a placeholder.
- **A** → confirmation screen naming the exact target and filename → **hold to confirm**. **B** backs out.
- Pending mode also offers **Cancel pending write** (when one is set), **New cart** and **Rewrite a game
  cart** (§6.6 targets). Setup mode offers **Finish setup**.
- **The writer returns a selection and never writes.** The boot state machine decides write-now (setup) or
  record-pending (menu cart). It references no `emu_*` or `rom_store_*` symbol — enforced by the §6.6 guard
  tests.
- It never coexists with the emulator: every exit is a halt or a power-off prompt. Art and list buffers are
  therefore static and generous; heap contention is not a constraint here.
- Reuses the in-game menu's D-pad list state machine rather than growing a second one.

---

## 9. What to delete from the fork

| File | Lines | Action |
|---|---|---|
| `src/bt_scanner.cpp` | 1033 | **delete** — drags in the whole BT stack, ~1 MB flash |
| `src/touch_input.cpp` | 422 | **delete** — physical buttons; but it owns `touch_load_settings()` (NVS) — move that to a `settings.cpp` first |
| `src/button_input.cpp` | 37 | rewrite — PCF8574 on IO16/17 → MCP23017 on I²C per §1.4; keep the 3-function interface |
| `src/ui_launcher.cpp` | 287 | replace — ROM browser must go (§6.1); `launcher_ingame_menu()` / `launcher_settings_menu()` are the seed of the new D-pad menu |
| `include/peanut_gb.h` | 4044 | keep — vendored, **pin to an upstream SHA** and note it in the header |
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

Phases 1–7 need nothing but the board on a desk. Phases 8+ need the ordered parts. **Nothing is on hand yet**,
so each phase below is worked to code-complete now and its Exit line is re-run in a final bench pass. The
apo breakdown, dependency order and per-phase deferred checks live in the root `ROADMAP.md`.

### Phase 0 — Bench checks *(blocking for hardware decisions, not for code)*
See §11. Three tests, ~30 minutes, each capable of changing the design. Until they run, every affected value
is a named constant with the roadmap's default.

### Phase 0.5 — Host test harness
Native PlatformIO env (`[env:native]`) that compiles Peanut-GB and the pure-logic modules (scaler, LUT, blend,
NDEF parser/composer, tag page whitelist, catalog reader, filename normaliser, input combo/debounce state
machine) with a unit-test runner, plus a GitHub
Actions job running `pio run` for both envs. **Exit: `pio test -e native` passes; firmware builds in CI.**
This is what lets Phases 2–9 be verified without a board.

### Phase 1 — Strip and boot
Delete Bluetooth and touch. Fix the driver define, `TFT_RST`, `LED_R_PIN`. Pin the vendored `peanut_gb.h` to an
upstream SHA. Rewrite `hw_config.h` for the 2.4" board. Move NVS settings out of `touch_input.cpp`. Stub the
launcher so the build links. **Exit: builds warning-free with no BT/touch symbols; binary size recorded.**
*Bench:* black screen, board boots, no crashes.

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
MiniGB APU, `ENABLE_SOUND 1`, timer-fed DAC, mono sum, dither, 4-state volume (off = DAC held at 128 —
no hardware mute exists, rev C).
Re-measure.
**Exit: audio at 60 fps; idle hiss with the amp always live measured and judged acceptable.**

### Phase 7 — Input
MCP23017 over I²C, per-frame poll, debounce, combos, D-pad masking, NVS persistence.
**Exit: full control from physical buttons; touch code fully gone.**

### Phase 8 — NFC and tag protocol
Minimal PN532 I²C driver, boot-time read with `MaxTg = 2`, NDEF parse *and compose*, page write, `PWD_AUTH`,
config-page protection with the page whitelist, read-back verification, the §6.6 boot state machine, the
pending-write record, the wizard's write steps, the catalog reader, clear failure display. Guard tests per
§6.6. The writer UI is stubbed (a "Menu cart" halt screen) until Phase 8.5.
**Exit: booting with a cart loads the right ROM; a pending write lands on the wildcard and nowhere else.**

### Phase 9 — Menu and saves
Rebuild the menu for D-pad, inside the game window. Remove Quit/Calibrate/Save/Load. Add Cart Info with
classification and protection state. Automatic save flush including the low-battery path.
**Exit: no path from the UI to browsing or switching ROMs.**

### Phase 8.5 — Cart writer *(after Phase 9, whose list state machine it reuses)*
The §8.3 UI: catalog list, art and description, confirmation, pending / new-cart / rewrite / cancel entries,
wizard steps end-to-end.
**Exit: the wizard completes end-to-end on an assembled unit; the writer is fully visible inside the bezel
window.**

### Phase 10 — Diagnostics
Boot-combo diagnostic screen per §8.2, including `GAME_X`/`GAME_Y` nudge to NVS.
**Exit: a builder can self-verify a unit without a computer.**

### Phase 11 — Mechanical and build day
Bezel print and fit, USB-C slot, battery harness, tag cups, assembly checklist (including the polarity check),
SD card imaging script (ROMs, art conversion, catalog generation), pre-assembly flashing station (ESP Web
Tools over USB-C — flash *before* the board goes in the shell — with a **factory-reset** action that clears NVS
to re-arm the wizard). **The kids' first step after closing the shell is the provisioning wizard** (§6.6):
menu cart, wildcard, starter carts. There is no phone writing station and no QR sticker.

---

## 11. Open items — resolve on the bench

| # | Question | Test | Impact if it goes badly | Bench status (rev C, 2026-09-01) |
|---|---|---|---|---|
| 1 | Does the board run from a 3.7 V cell? | Bench supply, sweep 4.2 → 3.3 V, watch the display | Needs a boost module; changes the whole power section | Runs and plays audio on the cell; the brownout sweep is still open |
| 2 | What is the 4th EXP-header pad? | Toggle IO22/16/17, meter the pad | If IO22: move SCL there, delete the IO1 boot handling | **Moot** — I²C moved to CN1 (SDA IO22 / SCL IO27); P3 metered as GND / IO35 / IO22 / IO21 |
| 3 | Is the onboard amp usable? | `tone()` on IO26, stock speaker, at full volume | Resistor mod, then MAX98357A | **Yes** — real GB music from SD, on battery; no mod needed |
| 4 | Does BAT actually power the system? | Cell connected, USB unplugged — does it boot? | Charge-only would need a boost + USB VBUS feed | **Yes** — the amp test ran on battery |
| 5 | Actual pixel pitch | Fill screen white, caliper the lit area | Recompute all bezel and `GAME_X`/`GAME_Y` numbers | open |
| 6 | IO34 divider ratio | Compare ADC reading to a meter across the cell range | Low-battery cutoff thresholds are wrong | open |
| 7 | Max reliable `SPI_FREQUENCY` | Sweep 40 / 55 / 80 MHz, look for artifacts | Directly caps frame rate | open |
| 8 | Real emulation frame time | Phase 2 FPS counter | May need Retro-Go's gnuboy core instead of Peanut-GB | open |
| 9 | Does SW1 bridge cleanly? | Hold SW1 while connecting the battery — does it boot? | A sealed unit cannot be started; rework the power path (§1.5) | open — new in rev C |
| 10 | Anti-metal read range | Read reliability at final geometry, through the shell, backlight at full | Move the PN532, change disc, or thin the shell behind the slot | open — new 2026-09-02 |
| 11 | Protect-then-rewrite cycle | On one sacrificial tag: write, protect, confirm a phone cannot write, confirm the device can | Password scheme is wrong; carts are not recoverable | open — new 2026-09-02 |
| 12 | NTAG215 config page addresses | Verify against the NXP datasheet for the exact part; `GET_VERSION` confirms the chip | Protection writes land on the wrong page — possibly a lock byte | open — new 2026-09-02 |
| 13 | Cup tab retention | Print tabs at 0.20 / 0.30 / 0.40 mm overhang; pick the one that clicks without force | Discs fall out or cannot be swapped | open — new 2026-09-02 |
| 14 | Bezel window | Writer and menu render fully inside the visible aperture on an assembled unit | UI hidden behind plastic; shrink the layouts | open — new 2026-09-02 |
| 15 | Wizard end-to-end | Fresh unit, assembled: menu → wildcard → starter carts → finish; two tags in the field → shielding fault | Build-day first step fails in front of the kids | open — new 2026-09-02 |

The vendor datasheet has now been wrong twice — the header pinout and IO4. Meter anything sourced from it
before building on it.

---

## 12. Assets

| File | Contents |
|---|---|
| `reference/DMG-CYD-wiring.pdf` | Rev C wiring diagram over the actual board photo, BOM, pin map, power notes — **bench-verified** |
| `reference/DMG-CYD-audio-mod.pdf` | MAX98357A fallback — historical; the escalation path was rejected on the bench (rev C, §1.6) |
| `reference/NFC_AMENDMENTS.md` | 2026-09-02 brief that moved cart writing on-device (MENU cart, wizard, password protection) — folded into §6, §8, §10, §11, §13 |

Both are A3 landscape. Rev C of the wiring PDF records bench-verified assignments and now supersedes the
vendor documentation, which has been wrong twice (header pinout, IO4).

---

## 13. Things not to do

Collected because each was considered and rejected for a reason that isn't obvious from the code.

- **Don't add a path from a running game to ROM selection, a recent-games list, or any writer entry other
  than the MENU cart / first-boot wizard.** §6.1.
- **Don't store tag UIDs.** Tags are self-describing. §6.1.
- **Don't parse JSON on the device.** Read the generated catalog. §6.5.
- **Don't draw full-screen UI outside `GAME_X`/`GAME_Y`/`GAME_W`×`GAME_H`.** The bezel hides it. §8.
- **Don't switch to Retro-Go.** It's a launcher; adapting it means suppressing its central feature and writing
  a new target definition. Steal techniques, not architecture. §3.4.
- **Don't drive IO4.** The vendor datasheet calls it the amp enable; the bench proved it is not, and its
  real function is unknown. §1.6. (The old warning against I²C on IO3 is moot — I²C lives on CN1, §1.3.)
- **Don't build on the vendor datasheet without metering.** It has been wrong twice — the header pinout and
  IO4. §11.
- **Don't stretch the image to 240 rows.** 11% vertical distortion. §2.1.
- **Don't blend byte-swapped pixels**, and don't blend before the palette LUT. §2.3.
- **Don't branch per-pixel to skip cross-palette blends.** Costs more than the blend. §2.4.
- **Don't add manual Save/Load.** §7.
- **Don't set lock bits or `CFGLCK` on any tag.** Password protection only — it is reversible. §6.6.
- **Don't use `pushImage` per line.** §2.5.
