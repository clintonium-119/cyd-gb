---
note_type: knowledge
template_version: 1
contract_version: 1
knowledge_id: "KNOW-0002"
category: architecture
title: "Code Map"
status: in_progress
owner: ""
created: '2026-08-27'
updated: '2026-08-27'
reviewed_on: ""
related_notes: ["[[01_Knowledge/System_Overview]]", "[[01_Knowledge/Coding_Standards]]"]
tags: [apovault, knowledge, architecture]
---

# Code Map

## Subsystems

`apo mine subsystems --json` returned an empty list: it walks top-level directories under `src/`, and `src/`
here is flat. With 17 first-party source files (8 `.cpp` + 9 `.h`, one of which is vendored), this is a
small codebase and takes the single-subsystem fallback.

| slug | path | purpose | file count | status |
|---|---|---|---|---|
| `cyd-gb` | `src/`, `include/` | Whole firmware — emulator bridge, display, input, storage, UI | 16 first-party (+1 vendored) | active |

**Source:** `apo mine subsystems --json`, `ls -F src/ include/` (read 2026-08-27).

## Directory Structure

```
cyd-gb/
├── include/       9 headers — one per src/ module, plus hw_config.h and vendored peanut_gb.h
├── src/           8 .cpp modules, flat, no subdirectories
├── scripts/       post_build_timestamp.py — PlatformIO post-build hook
├── reference/     DMG-CYD-wiring.pdf, DMG-CYD-audio-mod.pdf, ORIGINAL_ROADMAP.md (design doc)
├── ROADMAP.md     work breakdown — apo workstreams WS-01..11, dependency order, exits
├── platformio.ini single [env:cyd] environment; all pin config lives in build_flags
├── partitions.csv nvs / otadata / app0 (2 MB) / spiffs (1.98 MB)
├── README.md      documents the inherited fork, not the target
└── LICENSE        MIT
```

**Source:** `ls -F`, `ls -F include/ src/ scripts/ reference/` (read 2026-08-27).

Note `reference/ORIGINAL_ROADMAP.md` §12 refers to these PDFs as `assets/DMG-CYD-wiring.pdf` and `assets/DMG-CYD-audio-mod.pdf`.
They actually live in `reference/`. **Source:** `reference/ORIGINAL_ROADMAP.md:656-661` vs `ls -F reference/` (read 2026-08-27).

## Key Files

| File | Lines | What it is |
|---|---|---|
| `src/main.cpp` | 209 | Entry point. `setup()`/`loop()`, `run_emu()`, `input_task`, `save_ram()`/`load_ram()` |
| `src/bt_scanner.cpp` | 1033 | Largest first-party file. BLE beacon scanner. Marked **delete** by `reference/ORIGINAL_ROADMAP.md` §9 |
| `src/touch_input.cpp` | 422 | Bit-bang XPT2046 driver + calibration + NVS settings. Marked **delete** |
| `src/ui_launcher.cpp` | 287 | ROM browser, in-game menu, settings. Marked **replace** |
| `src/emulator_bridge.cpp` | 196 | Peanut-GB glue. Marked keep-callbacks/rewrite-palette-path |
| `src/sd_manager.cpp` | 82 | Marked **keep as-is** — the only file `reference/ORIGINAL_ROADMAP.md` §9 leaves untouched |
| `src/display.cpp` | 52 | Marked **replace entirely** |
| `src/button_input.cpp` | 37 | I²C expander read. Not listed in §9 |
| `include/hw_config.h` | 67 | All pin and geometry constants. Marked **rewrite for this board** |
| `include/peanut_gb.h` | 4044 | Vendored emulator core. Do not edit; it is upstream |
| `platformio.ini` | 76 | Board, build flags, `lib_deps`. §9 lists five config changes required |

**Sources:** `wc -l src/*.cpp include/*.h`, `reference/ORIGINAL_ROADMAP.md:554-577` (read 2026-08-27).

## Module Boundaries — `cyd-gb`

**Subsystem path:** `src/`, `include/` — scanned 2026-08-27.

Boundaries are enforced by convention only; there is no build-level enforcement. Observed rules:

- **Each `src/<name>.cpp` has exactly one `include/<name>.h`.** The only header without a `.cpp` is
  `hw_config.h` (constants) and the vendored `peanut_gb.h`. **Source:** `ls -F src/ include/`.
- **Module state is file-scope `static`, never exposed.** Every module keeps its state private and publishes
  only functions. Examples: `static Pg pg[PG_N]` and `static struct gb_s* gb` in `emulator_bridge.cpp:22,49`;
  `static volatile uint16_t cur_btns` in `button_input.cpp:5`; `static SPIClass sdSPI(VSPI)` and
  `static bool ready` in `sd_manager.cpp:7-8`.
- **One documented exception:** `display.h:3` declares `extern TFT_eSPI tft`, defined at `display.cpp:5`. The
  TFT object is global, and `ui_launcher.cpp` draws through it directly.
- **`peanut_gb.h` is included in exactly one translation unit** — `emulator_bridge.cpp:12` — with its feature
  macros defined immediately above the include (`emulator_bridge.cpp:9-11`). No other module sees `struct
  gb_s`. This is what makes the emulator swappable.
- **`hw_config.h` is the single source of pin numbers within C++ code**, but it is *not* the only source
  overall: `platformio.ini:19-71` defines an overlapping set of pin macros as `-D` build flags for TFT_eSPI's
  benefit. `SD_PIN_CS` (`hw_config.h:16`) and `-DSD_CS=5` (`platformio.ini:66`) are the same pin declared
  twice by two names. **Treat this duplication as a known hazard** when changing boards.
- **Button-code layering is inverted.** `button_input.cpp:2` includes `touch_input.h` solely to get the
  `GB_BTN_*` bit constants, which are defined at `touch_input.h:6-14`. The physical-button module therefore
  depends on the touchscreen module it is meant to replace. `reference/ORIGINAL_ROADMAP.md` §9 deletes `touch_input.cpp`; the
  constants must move first or `button_input.cpp` stops compiling.

**Sources:** files and lines as cited above (read 2026-08-27).

The rules above describe the Arduino layer (`src/` + `include/`). Since the host-test workstream
(scanned 2026-08-31) the tree has a second, pure-C layer they are scoped to sit above:

- **Pure logic lives in `lib/gbcore/`** — Arduino-free C modules (`render/scaler`, `cart/ndef`,
  `cart/match`, `input/combo`, `audio/mix`), each a `<dir>/<name>.c` + `<name>.h` pair with
  `<module>_<verb>` free functions, built by PlatformIO's LDF for both `env:cyd` and `env:native`.
  `src/<name>.cpp` + `include/<name>.h` pairing remains the rule for the Arduino wrappers only.
  **Source:** `lib/gbcore/`, `platformio.ini` (`[env:native]`).
- **`lib/gb_runner/` is test-only** — a headless Peanut-GB runner for the native suite; nothing under
  `src/` may reference it, so it never links into firmware (checked by the unchanged `env:cyd` flash
  size). **Source:** `lib/gb_runner/gb_runner.h` header comment.
- **`peanut_gb.h` in one FIRMWARE translation unit still holds** — `emulator_bridge.cpp` remains the
  only include under `src/`; the host-side includes (`lib/gb_runner/gb_runner.c`,
  `test/test_toolchain/test_main.c`) never compile into `env:cyd`, so the emulator stays swappable.
  **Source:** `grep -rn "peanut_gb.h" src/ lib/ test/` (read 2026-08-31).

## Boot and control flow

`setup()` initialises in this order: Serial at 115200 (`main.cpp:136`), LED pins driven HIGH
(`main.cpp:138-146`), `display_init()` (`:145`), `touch_init()` (`:146`), `button_init()` (`:147`),
`sd_init()` (`:149`) — which halts forever on an "SD Card Error!" screen if the card is missing
(`main.cpp:149-155`) — then a splash, then `touch_load_settings()` restores palette / frame-skip /
brightness from NVS (`main.cpp:164-172`). **Source:** `src/main.cpp:135-175` (read 2026-08-27).

**`main.cpp:138` and `:141` drive `LED_R_PIN` as an output and set it HIGH at every boot.** `LED_R_PIN` is
`4` (`include/hw_config.h:23`). On the target ESP32-2432S024 board, `reference/ORIGINAL_ROADMAP.md:52` records IO4 as the audio
amplifier enable, HIGH = on — and `reference/ORIGINAL_ROADMAP.md:76-78` calls out this exact line as a defect to fix in Phase 1.
On the current board it is a status LED; on the target board it is the amp. **Sources:**
`src/main.cpp:138,141`, `include/hw_config.h:23`, `reference/ORIGINAL_ROADMAP.md:52,76-78` (read 2026-08-27).

`loop()` is the per-game cycle, and it runs the ROM browser every iteration: `sd_scan_roms()` fills a fixed
`RomEntry roms[64]` array (`main.cpp:178`, `sd_manager.h:8`), `launcher_show()` returns an index or the
sentinel `LAUNCHER_SEL_BT_SCANNER` (`main.cpp:179-183`, `ui_launcher.h:4`), `emu_open_rom()` copies the ROM to
SPIFFS if it fits and opens it (`main.cpp:196`), `emu_init()` allocates the page cache and bank-0 buffer and
calls `gb_init()` (`main.cpp:199`), then `run_emu()` loops on `emu_run_frame()` until the Start+Select combo
opens the in-game menu (`main.cpp:206`, `:83-118`, `:24-30`).
**Sources:** `src/main.cpp:177-209`, `src/emulator_bridge.cpp:117-176` (read 2026-08-27).

**This `loop()` structure is itself the thing `reference/ORIGINAL_ROADMAP.md` §6.1 forbids** — quitting a game returns to a full
ROM browser. `reference/ORIGINAL_ROADMAP.md:417-425` replaces it with a single boot-time NFC read, and §8.1 removes the menu's
Quit entry for the same reason. **Source:** `reference/ORIGINAL_ROADMAP.md:404-415, 513-524` (read 2026-08-27).

## Verification status

All structural claims cited to file and line. Not verified: whether `bt_scanner.cpp` and `ui_launcher.cpp`
hold further cross-module dependencies beyond those listed — both were skimmed for structure rather than read
in full, since `reference/ORIGINAL_ROADMAP.md` §9 marks both for deletion or replacement.
