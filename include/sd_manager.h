// =============================================================================
// sd_manager.h - the only module that touches the SD card
// =============================================================================
// Three SD-facing pieces the cartridge boot flow needs, plus save-state I/O.
//
// There is deliberately no directory listing here. A tag carries a ROM file
// name and the match rule is exact, so the boot flow asks for one path and
// gets one answer; the legacy fuzzy lookup walks the directory an entry at a
// time rather than holding the 132-title library in RAM on a board with no
// PSRAM. Anything that wants to present the library reads /catalog.txt
// through the reader below.
#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#include "cart/catalog.h"
#include "cart/rom_store.h"

#define ROM_PATH_GB     "/roms/gb"
#define SAVE_PATH       "/saves"

// Generated from games.json by the build tooling and never hand-edited or
// written by the firmware.
#define CATALOG_PATH    "/catalog.txt"

bool sd_init();

// Build /roms/gb/<filename> in out. False when the name does not fit out_sz
// or no such file exists — never a truncated path, because a truncation
// would name a different, possibly real, file.
bool sd_rom_path(const char* filename, char* out, size_t out_sz);

// Legacy lookup for tags hand-written from a phone before the device could
// write them: normalises `title`, then walks ROM_PATH_GB and returns the
// first .gb entry the legacy predicate accepts. Exact matching via
// sd_rom_path() is the rule; this is the fallback.
bool sd_rom_find_legacy(const char* title, char* out_path, size_t out_sz);

// Bind a chunk reader over CATALOG_PATH. The file is opened once and stays
// open for the session. False when the catalog is missing, which the boot
// flow treats as "no title available" rather than as a failure.
bool sd_catalog_reader(catalog_reader_t* out);

// Save/Load game state
bool sd_save_state(const char* rom_path, const uint8_t* sram, uint32_t size);
bool sd_load_state(const char* rom_path, uint8_t* sram, uint32_t size);

// Save file path helper
void sd_get_save_path(const char* rom_path, char* save_path, int max_len);
