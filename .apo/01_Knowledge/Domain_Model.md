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
updated: '2026-09-23'
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

### `gb_t GB` — gnuboy emulator state

External, owned by the vendored core: one global `gb_t GB`, with cartridge state in a global `gb_cart_t cart`.
Visible only inside `src/emulator_bridge_gnuboy.cpp`, the one firmware file that includes gnuboy's headers.
**Source:** `lib/gnuboy/hw.h:264-265`; `src/emulator_bridge_gnuboy.cpp:17-18` (read 2026-09-23).

`reference/ORIGINAL_ROADMAP.md:494-499` put save-states out of scope because of the old core's struct layout.
That no longer applies: the bridge saves and loads states through gnuboy's `gnuboy_save_state()` /
`gnuboy_load_state()`, exposed as `emu_state_save()` / `emu_state_load()`.
**Source:** `src/emulator_bridge_gnuboy.cpp:1658, 1671`, `include/emulator_bridge.h:82-83` (read 2026-09-23).

### Cartridge RAM — battery-backed save data

gnuboy's `cart.rambanks`, `cart.ramsize * 8192` bytes. Its *meaningful* length is `save_size_from_header()`
— header byte `0x149` through the standard table, `0x200` for MBC2, 0 (autosave off) for an unknown code —
which is what gets written to disk. `emu_set_cart_ram()` clamps a restore to the allocation.
**Source:** `src/emulator_bridge_gnuboy.cpp:1254-1255, 1485-1524` (read 2026-09-23).

### `TouchCalibration` — per-unit touchscreen calibration

```c
struct TouchCalibration { int16_t x_min, x_max, y_min, y_max; bool swapped, invert_x, invert_y; };
```

Persisted to NVS. Slated for deletion with the touch subsystem (`reference/ORIGINAL_ROADMAP.md:556`).
**Source:** `include/touch_input.h:16-19` (read 2026-08-27).

### Button word — a `uint16_t` bitmask

Nine flags, `GB_BTN_RIGHT` `0x01` through `GB_BTN_MENU` `0x100`. **The bit order is not arbitrary:** the low
eight bits match gnuboy's `GB_PAD_*` layout, so `emu_run_frame()` passes the byte straight to
`gnuboy_set_pad()` untouched. `GB_BTN_MENU` at `0x100` sits deliberately above that byte, and `main.cpp:32` masks with
`& 0xFF` before handing it to the emulator.
**Sources:** `include/touch_input.h:6-14`, `src/main.cpp:26-33` (read 2026-08-27);
`lib/gnuboy/gnuboy.h:49-56`, `src/emulator_bridge_gnuboy.cpp:1315-1318` (read 2026-09-23).

These constants live in `touch_input.h` but are consumed by `button_input.cpp` — see the layering hazard in
[[01_Knowledge/Code_Map]] § Module Boundaries.

## Relationships

- **`RomEntry` 1:1 save file.** Derived, not stored: `sd_get_save_path()` strips the directory and the
  extension from the ROM path and rebuilds it as `/saves/<base>.sav`. Two ROMs with the same basename in
  `/roms/gb` and `/roms/gbc` therefore **collide onto one save file**. **Source:**
  `src/sd_manager.cpp:52-58`, `include/sd_manager.h:11-13` (read 2026-08-27).
- **Emulator 1:1 mapped ROM.** One `romdata` partition holds one ROM; `main.cpp` hands the
  `rom_store_mmap()` pointer to the bridge, which keeps a single `rom` / `romlen` pair. One game at a time,
  by construction. **Source:** `partitions.csv:19`, `src/main.cpp:446`,
  `src/emulator_bridge_gnuboy.cpp:91-92` (read 2026-09-23).
- **Palette index → colour table.** `curpal` indexes the gbcore palette module: `PALETTE_COUNT` (14) sets of
  three 4-shade ramps, expanded into a 64-entry `lut[]`; `PALETTE_AUTO` selects the CGB ramps for the cart.
  **Source:** `lib/gbcore/render/palette.h:32-44`, `src/emulator_bridge_gnuboy.cpp:129-135` (read 2026-09-23).

## Invariants

- **`sd_manager` refuses all I/O until `sd_init()` succeeds.** `ready` gates `sd_scan_roms`, `sd_save_state`
  and `sd_load_state`. **Source:** `src/sd_manager.cpp:8, 39, 61, 71`.
- **A save write is reported successful only on a full-length write** (`return w == sz`), and a load only on a
  full-length read (`return r == sz`). **Source:** `src/sd_manager.cpp:68, 81`.
- **Autosave owns cart-RAM dirty state.** gnuboy sets `cart.sram_dirty`; `emu_autosave_tick()` consumes it
  once a frame into the autosave state, which `emu_cart_ram_dirty()`, `emu_get_cart_ram_last_write_ms()` and
  `emu_clear_cart_ram_dirty()` report and clear. **Source:** `src/emulator_bridge_gnuboy.cpp:1526-1559`
  (read 2026-09-23).

## Verification status

All entity and invariant claims are cited to file and line in first-party code. Not verified: the internal
layout of gnuboy's `gb_t` (vendored; read only as far as the fields the bridge touches), and whether
`ui_launcher.cpp` or `bt_scanner.cpp` introduce further shared state — neither was read in full.
