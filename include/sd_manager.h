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
// through the reader below. A count is not a listing: sd_rom_count() is the
// one walk here that returns a number rather than a name, and it holds no
// entry while it does it.
#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#include "cart/catalog.h"
#include "cart/rom_store.h"
#include "ui/manual.h"

#define ROM_PATH_GB     "/roms/gb"
#define SAVE_PATH       "/saves"

// Appended to a save path to name the file a save is written to before it is
// renamed into place. Nothing but sd_save_state() ever creates or reads one,
// and a leftover is stale by definition — the rename is the last step, so a
// temp file that outlives its save means that save never completed.
#define SAVE_TMP_SUFFIX ".tmp"

// Generated from games.json by the build tooling and never hand-edited or
// written by the firmware.
#define CATALOG_PATH    "/catalog.txt"

// ─── Art ────────────────────────────────────────────────────────────────────
// Two images per game, both raw RGB565, 96x96, little-endian, no header —
// exactly 18,432 bytes each (design §6 "Art"). The box art identifies the
// game and the gameplay snapshot shows what playing it looks like; the
// writer's detail page reads both.
//
// Produced by the imaging tool from games.json and NEVER written by the
// firmware, which is why sd_init() does not create either directory: a card
// with no /shot at all simply draws placeholders. Either file missing is an
// ordinary case, not a failure.
#define ART_PATH        "/art"
#define SHOT_PATH       "/shot"
#define ART_SUFFIX      ".565"

// The longest media path: "/shot" (5) + a 63-character name less its ".gb"
// (60) + ".565" (4) + the NUL is 70, rounded up.
#define ART_PATH_MAX    80

// Build <dir>/<stem>.565 in out, where <stem> is `rom_filename` without a
// trailing ".gb". The match is case-sensitive because the filename is the
// frozen key the cartridge carries. False when the path does not fit out_sz
// or no such file exists — never a truncated path, because a truncation would
// name a different, possibly real, file.
bool sd_media_path(const char* dir, const char* rom_filename, char* out,
                   size_t out_sz);

// Read one media file whole into `out`, which must hold px_count pixels. The
// file has to be exactly px_count * 2 bytes: a file of any other size is a
// file from a different imaging run, and padding it would paint garbage.
// False on a missing, mis-sized or short-reading file, logged once.
bool sd_media_read(const char* dir, const char* rom_filename, uint16_t* out,
                   size_t px_count);

// One band of a media file, handed to `fn` as it arrives.
typedef void (*sd_media_band_fn)(void* ctx, const uint16_t* px, size_t row0,
                                 size_t rows);

// Read one media file a band at a time, calling `fn` with each band as it
// lands in `buf`. Same file contract as sd_media_read(): the file must be
// exactly row_w * total_rows * 2 bytes, so a band can never be cut from a
// file belonging to a different imaging run.
//
// This exists because the whole image does not fit. At game time the heap is
// 100 KB free but its largest contiguous block is about 15 KB, so an 18 KB
// buffer for a 96x96 cover is refused outright; a few rows at a time is
// served comfortably. `buf` holds row_w * band_rows pixels and is the
// caller's — nothing here allocates.
//
// False on a missing, mis-sized or short-reading file, and on unusable
// arguments; `fn` is not called at all in that case.
bool sd_media_stream(const char* dir, const char* rom_filename, uint16_t* buf,
                     size_t row_w, size_t total_rows, size_t band_rows,
                     sd_media_band_fn fn, void* ctx);

// ─── Manuals ────────────────────────────────────────────────────────────────
// One scanned manual per game that has one, 1 bpp pages behind a page table;
// the format is docs/CATALOG_FORMAT.md § Manuals, and ui/manual.h parses it.
// Like the art, written only by the imaging tool: a game with no manual has
// no file, and that is an ordinary case.
//
// "/manual" (7) + a 60-character stem + ".1bp" (4) + the NUL is 72, so a
// manual path fits ART_PATH_MAX too.
#define MANUAL_PATH     "/manual"
#define MANUAL_SUFFIX   ".1bp"

// Build /manual/<stem>.1bp in out, by sd_media_path()'s rule: false when the
// path does not fit out_sz or no such file exists, never a truncated path.
bool sd_manual_path(const char* rom_filename, char* out, size_t out_sz);

// Open the running game's manual and bind a chunk reader over it, with the
// file's size in *size for the exact-size check. The file stays open until
// sd_manual_close() — the reader seeks into it for every band it draws — and
// opening another closes the previous one first. False when there is no
// manual or it will not open.
bool sd_manual_reader(const char* rom_filename, manual_reader_t* out,
                      uint32_t* size);

// Close the manual sd_manual_reader() opened. Safe to call when none is open.
void sd_manual_close();

bool sd_init();

// Build /roms/gb/<filename> in out. False when the name does not fit out_sz
// or no such file exists — never a truncated path, because a truncation
// would name a different, possibly real, file.
bool sd_rom_path(const char* filename, char* out, size_t out_sz);

// How many .gb files ROM_PATH_GB holds. The same one-entry-at-a-time walk
// sd_rom_find_legacy() uses, counting instead of matching, so nothing is held
// in RAM but the running total. 0 when the card is not mounted or the
// directory is missing — the diagnostic page reports "not mounted" from
// sd_init()'s own result, so a 0 here is never mistaken for a verdict.
uint16_t sd_rom_count();

// Card capacity and how much of it is in use, in whole megabytes. False with
// both untouched when the card is not mounted. Either pointer may be NULL.
bool sd_card_stats(uint32_t* total_mb, uint32_t* used_mb);

// Legacy lookup for tags hand-written from a phone before the device could
// write them: normalises `title`, then walks ROM_PATH_GB and returns the
// first .gb entry the legacy predicate accepts. Exact matching via
// sd_rom_path() is the rule; this is the fallback.
bool sd_rom_find_legacy(const char* title, char* out_path, size_t out_sz);

// Bind a chunk reader over CATALOG_PATH. The file is opened once and stays
// open for the session. False when the catalog is missing, which the boot
// flow treats as "no title available" rather than as a failure.
bool sd_catalog_reader(catalog_reader_t* out);

// Save/Load game state.
//
// sd_save_state() writes to <name>.sav.tmp and renames it over <name>.sav, so
// an interrupted save leaves the previous one intact rather than leaving the
// card with no save at all. It returns false without touching <name>.sav when
// the write comes up short, which leaves the caller's RAM dirty for a retry.
//
// sd_load_state() reads <name>.sav and nothing else; a temp file left behind
// by an interrupted save is not a save and is never loaded.
bool sd_save_state(const char* rom_path, const uint8_t* sram, uint32_t size);
bool sd_load_state(const char* rom_path, uint8_t* sram, uint32_t size);

// Save file path helper
void sd_get_save_path(const char* rom_path, char* save_path, int max_len);
