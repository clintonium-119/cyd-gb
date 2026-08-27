---
note_type: knowledge
template_version: 1
contract_version: 1
knowledge_id: "KNOW-0001"
category: architecture
title: "System Overview"
status: in_progress
owner: ""
created: '2026-08-27'
updated: '2026-08-27'
reviewed_on: ""
related_notes: ["[[01_Knowledge/Code_Map]]", "[[01_Knowledge/Domain_Model]]", "[[01_Knowledge/Integration_Map]]"]
tags: [apovault, knowledge, architecture]
---

# System Overview

## Purpose

CYD-GB is a Game Boy emulator for the ESP32 "Cheap Yellow Display" board. The repository as it stands is a
fork of `artanergin44-collab/cyd-gb`, whose stated goal is "the first GB emulator running on ESP32-2432S028R
(CYD) without PSRAM and without any physical buttons" — touchscreen-controlled, with an SD-card ROM browser.
**Source:** `README.md` (read 2026-08-27).

The `poc-gb` branch is repurposing that fork toward a different product: ten hand-built Game Boy DMG-01 units
on a *different* board, driven by physical buttons, with games selected by NFC cartridge rather than a
browser. **Source:** `reference/ORIGINAL_ROADMAP.md:9-23` (read 2026-08-27).

**These two descriptions conflict on purpose.** `README.md` documents the inherited fork; `reference/ORIGINAL_ROADMAP.md`
documents the settled target design and calls itself "the settled design for a group build."
`reference/ORIGINAL_ROADMAP.md` §9 lists the fork files to delete or replace. Treat `reference/ORIGINAL_ROADMAP.md` as authoritative for intent and
the current `src/` tree as the starting point being replaced. **Source:** `reference/ORIGINAL_ROADMAP.md:11, 552-577` (read
2026-08-27).

## Two governing constraints

Recorded verbatim in scope because `reference/ORIGINAL_ROADMAP.md:14-23` instructs any agent planning work to internalise them
first:

1. **The cartridge system is the product, not a feature.** Choosing a game must be a deliberate physical act.
   A ROM launcher, an on-device NFC writer, or a "recent games" list defeats the design and must not be added.
   The fork's ROM browser is being deleted for exactly this reason.
2. **Ten units, built by kids, assembled once.** Favour solder-free connections, per-unit adjustability stored
   in NVS, and diagnostics that let a builder self-diagnose. Avoid anything needing a rework station or
   per-unit firmware variation.

**Source:** `reference/ORIGINAL_ROADMAP.md:14-23` (read 2026-08-27).

## High-Level Architecture

Single-binary Arduino-framework firmware for ESP32, built by PlatformIO. One env, `[env:cyd]`.
**Source:** `platformio.ini:6-9` (read 2026-08-27).

Structure is a flat set of C++ modules under `src/`, each paired with a header under `include/`, coordinated
by `src/main.cpp`. There is no class hierarchy: each module exposes prefixed free functions and keeps its
state in file-scope `static` variables. **Source:** `include/display.h`, `include/sd_manager.h`,
`include/emulator_bridge.h`, `include/button_input.h` (read 2026-08-27).

Concurrency in the current code is one pinned FreeRTOS task: `input_task` on core 0 at priority 2, polling
buttons every 12 ms, with the emulation loop running on the main Arduino task.
**Source:** `src/main.cpp:19-43` (read 2026-08-27).

`reference/ORIGINAL_ROADMAP.md` §3.3 specifies a different target split — Peanut-GB on core 1, display push on core 0 behind a
queue — which is not yet implemented. **Source:** `reference/ORIGINAL_ROADMAP.md:331-336` (read 2026-08-27).

## Key Components

| Path | Role |
|---|---|
| `src/main.cpp` | Boot, ROM selection, emulation loop, save/load orchestration, input task |
| `src/emulator_bridge.cpp` | Peanut-GB callbacks, ROM page cache, palette table, frame pacing |
| `src/display.cpp` | TFT_eSPI init, backlight PWM, per-scanline scale and push |
| `src/sd_manager.cpp` | SD mount, ROM directory scan, `.sav` battery-save read/write |
| `src/ui_launcher.cpp` | ROM browser, in-game menu, settings submenu |
| `src/touch_input.cpp` | XPT2046 bit-bang touch driver, calibration, NVS settings persistence |
| `src/button_input.cpp` | I²C GPIO-expander button read |
| `src/bt_scanner.cpp` | Bluetooth LE beacon scanner |
| `include/hw_config.h` | Pin map, screen geometry, touch-zone constants |
| `include/peanut_gb.h` | Vendored Peanut-GB emulator core (4044 lines) |

**Source:** `ls -F src/ include/`, `wc -l src/*.cpp include/*.h` (read 2026-08-27).

## Technology Stack

| Layer | Choice | Source |
|---|---|---|
| Platform | `espressif32@6.5.0`, board `esp32dev`, framework `arduino` | `platformio.ini:7-9` |
| Language | C++ (Arduino), plus one Python build script | `src/*.cpp`, `scripts/post_build_timestamp.py` |
| Emulator core | Peanut-GB, vendored at `include/peanut_gb.h`; MIT, by Mahyar Koshkouei | `README.md` Credits, `include/peanut_gb.h` |
| Display driver | `bodmer/TFT_eSPI@^2.5.43` | `platformio.ini:74` |
| Touch driver | `PaulStoffregen/XPT2046_Touchscreen` pinned at tag `v1.4` | `platformio.ini:75` |
| Build tool | PlatformIO, with a post-build timestamp script | `platformio.ini:10-11`, `scripts/post_build_timestamp.py` |
| Persistence | Arduino `Preferences` (NVS) for settings; SD FAT32 for ROMs and saves; SPIFFS as ROM cache | `src/touch_input.cpp:5,19`, `src/sd_manager.cpp`, `src/emulator_bridge.cpp:117-150` |
| Test framework | **None present.** No test files, no `test/` directory, no CI workflow. | `find` for `*test*`/`*spec*` returned nothing; no `.github/` (read 2026-08-27) |

Note the README claims Peanut-GB "is **not included** in this repo — you must download it," but
`include/peanut_gb.h` is committed and tracked (last touched 2026-08-02). The README instruction is stale for
this fork. **Source:** `README.md` step 3, `git log -1 -- include/peanut_gb.h` (read 2026-08-27).

## Memory and performance shape

The board has no PSRAM. The fork's answer is a paged ROM cache in SPIFFS: 16 pages of 4096 bytes with a
32-entry hash index and LRU eviction, plus bank 0 (first 32 KB) permanently resident in a `malloc`'d buffer.
**Source:** `src/emulator_bridge.cpp:14-46, 152-163` (read 2026-08-27).

`reference/ORIGINAL_ROADMAP.md` §3.2 names this cache "the biggest risk": the hit path is ~15 cycles but a miss is a 4 KB SPIFFS
read estimated at 1.5–4 ms against a 16.75 ms frame budget, so a bank-switch storm stutters. The proposed fix
is to store the ROM in a raw flash partition and `esp_partition_mmap` it, collapsing `gb_rom_read` to a
pointer dereference. **Source:** `reference/ORIGINAL_ROADMAP.md:302-329` (read 2026-08-27).

The frame budget and its breakdown in `reference/ORIGINAL_ROADMAP.md:287-300` are explicitly flagged as *estimated, not
measured* — the emulation figure "could be off by 50% either way." Do not plan against those numbers as if
they were data. **Source:** `reference/ORIGINAL_ROADMAP.md:293-300` (read 2026-08-27).

## Open bench items

`reference/ORIGINAL_ROADMAP.md` §11 lists eight unresolved questions, each with a bench test and a stated consequence if it goes
badly. They gate design decisions in the rendering, power, audio and performance sections. Summarised:
board runs from a 3.7 V cell; identity of the 4th EXP-header pad; onboard amp usability; whether BAT actually
powers the system; actual pixel pitch; IO34 divider ratio; max reliable `SPI_FREQUENCY`; real emulation frame
time. **Source:** `reference/ORIGINAL_ROADMAP.md:639-652` (read 2026-08-27).

`reference/ORIGINAL_ROADMAP.md:26-29` states plainly: "Do not treat flagged values as settled."

## Verification status

Every claim above is cited. The one section that is not directly verifiable from this repository is the
hardware pin map in `reference/ORIGINAL_ROADMAP.md` §1.2, which the document itself marks per-row as `proposed` versus
`confirmed`. Carry that distinction forward rather than flattening it.
