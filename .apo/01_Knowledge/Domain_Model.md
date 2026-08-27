---
note_type: knowledge
template_version: 1
contract_version: 1
knowledge_id: "KNOW-0003"
category: architecture
title: "Domain Model"
status: in_progress
owner: ""
created: '2026-08-27'
updated: '2026-08-27'
reviewed_on: ""
related_notes: ["[[01_Knowledge/System_Overview]]", "[[01_Knowledge/Code_Map]]"]
tags: [apovault, knowledge, architecture]
---

# Domain Model

There is no database and no schema layer. The domain is expressed as C structs, fixed-size arrays and
hardware state. Entities below are the ones that cross module boundaries.

## Entities

### `RomEntry` — a ROM file on the SD card

```c
struct RomEntry {
    char     filename[MAX_FILENAME];  // 48
    char     full_path[80];
    uint32_t size;
    bool     is_gbc;                  // true if from /roms/gbc
};
```

Repo-owned. At most `MAX_ROMS` (64) are held, in a single static array in `main.cpp:11`.
**Source:** `include/sd_manager.h:8-19`, `src/main.cpp:11-12` (read 2026-08-27).

### `struct gb_s` — Peanut-GB emulator state

External, owned by the vendored core. Allocated once with `malloc(sizeof(struct gb_s))` and zeroed, then
initialised by `gb_init()` with five callbacks. Visible only inside `emulator_bridge.cpp`.
**Source:** `src/emulator_bridge.cpp:49, 165-172`; `include/peanut_gb.h` (read 2026-08-27).

`reference/ORIGINAL_ROADMAP.md:494-499` records why this matters for saves: `struct gb_s`'s "first ~45 lines are function
pointers that change on every recompile," which is the stated reason save-states are out of scope.

### Cartridge RAM (`cram`) — battery-backed save data

A single `malloc`'d buffer of `MAXRAM` = 32 KB, memset to `0xFF` on init. Read and written by the
`gb_cram_r` / `gb_cram_w` callbacks. Its *meaningful* length is not 32 KB but whatever `gb_get_save_size_s()`
reports for the loaded cartridge, which is what gets written to disk.
**Source:** `src/emulator_bridge.cpp:50-51, 169-170, 99-105, 187` (read 2026-08-27).

### `Pg` — a cached 4 KB page of ROM

```c
struct Pg { uint32_t addr, acc; uint8_t* d; bool v; };
```

`acc` is a monotonically increasing access stamp driving LRU; `v` is the valid flag; `addr` is the page-base
ROM address. 16 of them, indexed by a 32-entry direct-mapped hash `ht`.
**Source:** `src/emulator_bridge.cpp:14-46` (read 2026-08-27).

### `TouchCalibration` — per-unit touchscreen calibration

```c
struct TouchCalibration { int16_t x_min, x_max, y_min, y_max; bool swapped, invert_x, invert_y; };
```

Persisted to NVS. Slated for deletion with the touch subsystem (`reference/ORIGINAL_ROADMAP.md:556`).
**Source:** `include/touch_input.h:16-19` (read 2026-08-27).

### Button word — a `uint16_t` bitmask

Nine flags, `GB_BTN_RIGHT` `0x01` through `GB_BTN_MENU` `0x100`. **The bit order is not arbitrary:** the low
eight bits match the layout `emu_run_frame()` unpacks into `gb->direct.joypad_bits`, so the mask can be passed
through untouched. `GB_BTN_MENU` at `0x100` sits deliberately above that byte, and `main.cpp:32` masks with
`& 0xFF` before handing it to the emulator.
**Sources:** `include/touch_input.h:6-14`, `src/emulator_bridge.cpp:178-181`, `src/main.cpp:26-33` (read
2026-08-27).

These constants live in `touch_input.h` but are consumed by `button_input.cpp` — see the layering hazard in
[[01_Knowledge/Code_Map]] § Module Boundaries.

## Relationships

- **`RomEntry` 1:1 save file.** Derived, not stored: `sd_get_save_path()` strips the directory and the
  extension from the ROM path and rebuilds it as `/saves/<base>.sav`. Two ROMs with the same basename in
  `/roms/gb` and `/roms/gbc` therefore **collide onto one save file**. **Source:**
  `src/sd_manager.cpp:52-58`, `include/sd_manager.h:11-13` (read 2026-08-27).
- **`RomEntry` 1:1 SPIFFS cache slot — and the slot is global.** `emu_open_rom()` uses the single fixed path
  `/rom.gb` for every game, and validates the cache by comparing SPIFFS file size to SD file size. Two
  different ROMs of identical byte length would collide silently. **Source:**
  `src/emulator_bridge.cpp:120-136` (read 2026-08-27).
- **Emulator 1:1 open ROM file.** `romf` is a single module-level `File`; the page cache reads through it.
  One game at a time, by construction. **Source:** `src/emulator_bridge.cpp:26-27, 40-44`.
- **Palette index → colour table.** `curpal` indexes `pals[NUM_PALETTES][4]`, 20 four-entry RGB565 tables
  with a parallel `palnames[]`. `reference/ORIGINAL_ROADMAP.md:230-266` widens this to `[N][3][4]` for 12-colour output.
  **Source:** `src/emulator_bridge.cpp:60-95`, `include/emulator_bridge.h:23-26`.

## Invariants

- **Bank 0 is always resident.** ROM addresses below `B0SZ` (32 KB) are served from `b0[]` and never touch
  the page cache. `gb_rom_read` checks `a < B0SZ` before `cget(a)`. **Source:**
  `src/emulator_bridge.cpp:48, 96-97`.
- **Reads past the end of the ROM return `0xFF`, never garbage.** Enforced at two levels: `gb_rom_read`
  returns `0xFF` for `a >= romlen` (`:97`), and a short page read is padded with `0xFF` (`:43`).
  Cart-RAM reads out of range likewise return `0xFF` (`:100`). **Source:** `src/emulator_bridge.cpp:43, 97,
  100`.
- **The hash index may be stale but never wrong.** `cget` treats `ht` as a hint: a hit is only accepted after
  `pg[i].v && pg[i].addr == pb` is re-checked, and a miss falls through to a linear scan.
  **Source:** `src/emulator_bridge.cpp:32-38`.
- **`sd_manager` refuses all I/O until `sd_init()` succeeds.** `ready` gates `sd_scan_roms`, `sd_save_state`
  and `sd_load_state`. **Source:** `src/sd_manager.cpp:8, 39, 61, 71`.
- **A save write is reported successful only on a full-length write** (`return w == sz`), and a load only on a
  full-length read (`return r == sz`). **Source:** `src/sd_manager.cpp:68, 81`.
- **The ROM-read and cart-RAM callbacks are `IRAM_ATTR`.** They are on the emulator's hot path and must not
  fault to flash. Anything added to them inherits that constraint. **Source:**
  `src/emulator_bridge.cpp:32, 96, 99, 102, 109`.
- **Dirty-tracking is stubbed, not implemented.** `emu_cart_ram_dirty()` returns `false` unconditionally,
  `emu_get_cart_ram_last_write_ms()` returns `0`, and `emu_clear_cart_ram_dirty()` is empty. **Source:**
  `src/emulator_bridge.cpp:189-191`. This matters directly: `reference/ORIGINAL_ROADMAP.md:501-508` specifies automatic saving
  driven by "auto-flush when SRAM is dirty and idle ~10 s", which these three functions exist to support and
  currently cannot.

## Verification status

All entity and invariant claims are cited to file and line in first-party code. Not verified: the internal
layout of `struct gb_s` (vendored; read only as far as the callback signatures), and whether
`ui_launcher.cpp` or `bt_scanner.cpp` introduce further shared state — neither was read in full.
