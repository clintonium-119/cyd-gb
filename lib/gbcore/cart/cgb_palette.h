#pragma once
// The Game Boy Color's own choice of colours for an original DMG cartridge.
//
// Dropped into a Game Boy Color, a DMG cartridge is not colourised at random:
// the boot ROM sums the sixteen title bytes of the header, looks the sum up in
// a built-in table, and applies the three sub-palettes Nintendo picked for
// that game. Pokemon Red comes out red, Blue blue, Zelda green. Twenty-nine of
// the ninety-four sums collide, and those are told apart by the title's fourth
// character. Only Nintendo-licensed cartridges are looked up at all.
//
// That is the closest thing to a canonical answer to "what colours should this
// cartridge be", and it costs 610 bytes of tables. The data and the mechanism
// come from SameBoy's cgb_boot.asm; scripts/gen_cgb_palettes.py carries the
// source URL, the licence and the derivation, and generates
// cgb_palette_table.h from it.
//
// Three sub-palettes is exactly the shape render/palette.h already uses, so a
// looked-up cartridge drops into the same LUT builder as a menu palette.
//
// Pure C, no Arduino/ESP-IDF headers, no allocation: the ramps are
// caller-owned.

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Fill ramps from the cartridge header at the start of rom.
 *
 * Row order is render/palette.h's ramp index: 0 = OBJ0, 1 = OBJ1, 2 = BG. Each
 * ramp runs light to dark, four shades, native RGB565 — the same convention as
 * the palette table, so either can feed palette_build_lut_gnuboy_ramps().
 *
 * Returns false and leaves ramps untouched when the ROM is too short to hold a
 * header, when the cartridge is not Nintendo-licensed, or when the table does
 * not know this title. The caller picks what an unknown cartridge gets; this
 * does NOT fall back to the boot ROM's own default palette, because the
 * firmware's shipped default is a better answer for a homebrew ROM than a
 * colour scheme Nintendo chose for nothing in particular.
 */
bool cgb_palette_lookup(const uint8_t* rom, uint32_t len, uint16_t ramps[3][4]);

#ifdef __cplusplus
}
#endif
