---
note_type: knowledge
template_version: 1
contract_version: 1
knowledge_id: "KNOW-0004"
category: architecture
title: "Integration Map"
status: in_progress
owner: ""
created: '2026-08-27'
updated: '2026-08-27'
reviewed_on: ""
related_notes: ["[[01_Knowledge/System_Overview]]", "[[01_Knowledge/Domain_Model]]"]
tags: [apovault, knowledge, architecture]
---

# Integration Map

This is firmware. "Integrations" are buses, peripherals and storage rather than network services. There are
no HTTP clients, no API keys and no `.env` in the repository.
**Source:** `grep` for `http`/`fetch`/`WiFi` across `src/` returned no client code; no `.env*` present (read
2026-08-27).

## External libraries

| Dependency | Version | Role |
|---|---|---|
| `bodmer/TFT_eSPI` | `^2.5.43` (caret — floats within 2.x) | Display driver; also owns the `tft` global |
| `PaulStoffregen/XPT2046_Touchscreen` | git tag `v1.4` (pinned) | Touch controller. Fetched from GitHub at build time |
| Peanut-GB | **vendored**, no version marker | Emulator core, `include/peanut_gb.h` |
| Arduino-ESP32 core | via `platform = espressif32@6.5.0` | `SD`, `SPIFFS`, `SPI`, `Wire`, `Preferences`, FreeRTOS |

**Sources:** `platformio.ini:7, 73-76`, `include/peanut_gb.h` (read 2026-08-27).

Two supply-chain notes worth carrying into planning:

- **The touch library is fetched from a GitHub URL, not the PlatformIO registry** (`platformio.ini:75`). It is
  tag-pinned, but an offline or GitHub-unavailable build fails. `reference/ORIGINAL_ROADMAP.md:556` deletes the touch subsystem,
  which removes this dependency.
- **Peanut-GB has no pinned version.** `README.md` step 3 tells the user to `curl` it from `master`, while the
  file is in fact committed here. `reference/ORIGINAL_ROADMAP.md:587-588` requires Phase 1 to "fetch `peanut_gb.h` from upstream
  and pin the version" — so the pinning is an open action, not a solved problem.
  **Sources:** `README.md` step 3, `reference/ORIGINAL_ROADMAP.md:587-588` (read 2026-08-27).

## Buses and peripherals (as currently coded)

| Bus | Pins | Devices | Source |
|---|---|---|---|
| HSPI | MOSI 13, MISO 12, SCLK 14, CS 15, DC 2, RST −1, BL 21 | ILI9341 TFT at 40 MHz | `platformio.ini:22-36` |
| VSPI | SCK 18, MISO 19, MOSI 23, CS 5 | SD card at 20 MHz | `src/sd_manager.cpp:7, 11-12`; `include/hw_config.h:16-19` |
| Bit-banged SPI | CS 33, IRQ 36, MOSI 32, MISO 39, CLK 25 | XPT2046 touch | `include/hw_config.h:10-14`; `platformio.ini:58-62` |
| I²C (`Wire`) | SDA 16, SCL 17, 100 kHz | GPIO expander at `0x20` | `src/button_input.cpp:9-12`; `include/hw_config.h:20-22` |

The bit-banged touch driver is deliberate: `README.md` § How It Works states it "avoids bus conflicts with
display and SD card." **Source:** `README.md` (read 2026-08-27).

**The expander is read as a PCF8574, not an MCP23017.** `read_pcf_buttons()` does a bare one-byte
`Wire.requestFrom()` with no register address and inverts the result — the PCF8574 quasi-bidirectional
protocol. **Source:** `src/button_input.cpp:15-28` (read 2026-08-27).

`reference/ORIGINAL_ROADMAP.md:100-118` specifies an **MCP23017** at the same address `0x20`, with `GPPU = 0xFF` for pull-ups and
buttons on GPA0–GPA7, at 400 kHz on pins IO27/IO1. Those are different parts with different protocols: the
MCP23017 requires a register-address write before each read. **The current code will not drive the part the
roadmap specifies.** This is a target-vs-current divergence, not a bug in either document.
**Sources:** `src/button_input.cpp:15-28`, `reference/ORIGINAL_ROADMAP.md:100-118, 379-400` (read 2026-08-27).

## Storage

| Store | Medium | What lives there | Source |
|---|---|---|---|
| SD (FAT32) | external card | `/roms/gb`, `/roms/gbc`, `/saves/<base>.sav` | `include/sd_manager.h:6-9`; `src/sd_manager.cpp:15-18` |
| SPIFFS | `spiffs` partition, 0x1F0000 (~1.98 MB) | `/rom.gb` — single-slot ROM cache | `partitions.csv:5`; `src/emulator_bridge.cpp:120-136` |
| NVS | `nvs` partition, 0x5000 (20 KB) | Touch calibration; palette, frame-skip, brightness, overlay flags | `partitions.csv:2`; `include/touch_input.h:26-30`; `src/touch_input.cpp:5,19` |

Directories are created on first boot if absent (`src/sd_manager.cpp:15-18`). `SPIFFS.begin(true)` formats on
mount failure, and the code degrades to reading the ROM straight off SD when SPIFFS is unavailable
(`src/emulator_bridge.cpp:118-122, 144-146`).

`reference/ORIGINAL_ROADMAP.md:320-329` proposes replacing the SPIFFS cache with a raw flash partition read via
`esp_partition_mmap`, which requires editing `partitions.csv` (currently app0 2 MB / SPIFFS 1.98 MB).

## Data flow — ROM read path

```
gb_rom_read(addr)                       emulator_bridge.cpp:96
  ├─ addr >= romlen        → 0xFF
  ├─ addr <  32 KB         → b0[addr]          (RAM, always resident)
  └─ else → cget(addr)                  emulator_bridge.cpp:32
       ├─ hash hit  → page byte                (~15 cycles per reference/ORIGINAL_ROADMAP.md:311)
       ├─ linear scan hit → page byte, reindex
       └─ miss → LRU evict, romf.seek + read 4 KB   ← SPIFFS or SD
```

`romf` is whichever handle `emu_open_rom()` settled on: the SPIFFS cache if the copy succeeded and its size
matches the SD original, otherwise the SD file directly.
**Sources:** `src/emulator_bridge.cpp:32-46, 96-97, 117-150` (read 2026-08-27).

## Data flow — display path

`lcd_line()` is Peanut-GB's per-scanline callback. It early-returns on skipped frames, maps each of 160 pixels
through `pals[curpal][px & 3]`, and calls `display_push_gb_line()`, which scales 160→240 horizontally by
duplicating every even pixel and calls `tft.pushImage()` once per destination row.
**Sources:** `src/emulator_bridge.cpp:109-115`, `src/display.cpp:29-46` (read 2026-08-27).

`reference/ORIGINAL_ROADMAP.md` targets a materially different path — §2.4 replaces `px[x] & 3` with a 64-entry LUT, §2.3 adds
`avg565` blending, §2.5 replaces per-line `pushImage` with one address window per frame and then
`pushPixelsDMA`. **Source:** `reference/ORIGINAL_ROADMAP.md:206-283` (read 2026-08-27).

## Planned integrations (not yet present in code)

Named in `reference/ORIGINAL_ROADMAP.md`, with no implementation in `src/` as of 2026-08-27:

| Target | Interface | Roadmap section |
|---|---|---|
| PN532 NFC reader | I²C `0x24`, `ntag2xx_ReadPage()`, boot-time read only | §6.2–6.3 |
| MCP23017 expander | I²C `0x20`, polled once per frame | §1.4, §5 |
| MiniGB APU | Compile-time companion to Peanut-GB; `ENABLE_SOUND 1` | §4 |
| Internal DAC audio | IO26, timer-fed, IO4 hardware mute | §1.6, §4 |
| Battery sense | IO34 ADC, divider ratio undocumented | §1.2, §11 item 6 |
| Phone tag-writing web app | GitHub Pages + Web NFC (`NDEFReader`); off-device only | §6.6 |

**Source:** `reference/ORIGINAL_ROADMAP.md:98-118, 347-377, 402-490, 639-652`; absence confirmed by `grep` for `PN532`, `MCP23017`
and `MiniGB` across `src/` and `include/` (read 2026-08-27).

`reference/ORIGINAL_ROADMAP.md:404-415` states the firmware "must be physically incapable of writing tags" — writing is a
build-day activity on a separate phone app, never a device feature.

## Verification status

All claims cited. Hardware pin assignments for the *target* board come from `reference/ORIGINAL_ROADMAP.md` §1.2, which marks
its own rows `proposed` / `confirmed`; §11 items 2 and 4 supersede them if the bench disagrees.
