#pragma once
// 12-colour palette tables and the flat LUT that colourizes a Game Boy line.
//
// palette_build_lut()'s pixel byte packs the palette source alongside the
// shade: bits 1-0 are the shade and bits 5-4 identify
// the palette (OBJ0 0x00, OBJ1 0x10, BG 0x20), so the byte's largest real
// value is 0x23. Three palettes x four shades = 12 simultaneous colours, the
// same mechanism a Game Boy Color uses to colourise DMG cartridges.
//
// A flat 64-entry LUT indexed by that raw byte removes the & 3 from the
// per-pixel inner loop and guarantees the line buffer holds pure RGB565 with
// no palette bits riding along — which is what makes the scaler's blend safe
// (design §2.4).
//
// Values are NATIVE RGB565, never pre-swapped: the blend happens first, and
// the display driver's setSwapBytes(true) handles wire order at push time.
//
// Ramp index p follows those pixel bits — 0 = OBJ0, 1 = OBJ1, 2 = BG. The
// first two palettes have a chosen BG ramp and same-hue-family OBJ ramps
// derived from it; the rest are the Game Boy Color boot ROM's button-combo
// palettes with Nintendo's own three ramps. scripts/gen_palettes.py generates
// them all once, and they are committed as literals.
//
// Pure C, no Arduino/ESP-IDF headers, no allocation: the LUT is caller-owned.

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PALETTE_COUNT 14    /* palettes, each a 3-ramp set of 4 shades */
#define PALETTE_LUT_SIZE 64 /* one entry per possible raw pixel byte   */

/*
 * One index past the table: not a palette here, but the value that means
 * "take the cartridge's own", which is what a fresh unit ships with. It is
 * resolved by the emulator bridge, the only place that has the ROM header,
 * into a ramp set fed to the *_ramps builders below. Nothing in this module
 * knows it: palette_name() and the idx-taking builders still reject it as out
 * of range, which is what keeps the table and the auto choice separate.
 */
#define PALETTE_AUTO PALETTE_COUNT
#define PALETTE_UI_COUNT (PALETTE_COUNT + 1)

/* What Auto resolves to for a cartridge the Game Boy Color's table does not
   know — the muted DMG green, and the first entry of the list, so the
   fallback and the menu's starting point are the same colours. */
#define PALETTE_FALLBACK 0

/* Display name of a palette, or "?" when idx is out of range. */
const char* palette_name(uint8_t idx);

/*
 * Fill all PALETTE_LUT_SIZE entries of lut from palette idx, so no raw pixel
 * byte can index an undefined colour: bits 5-4 pick the ramp, with anything
 * above BG folded onto BG, and bits 1-0 pick the shade.
 *
 * Writes nothing at all if lut is NULL or idx is out of range — a caller that
 * passes a bad index keeps the palette it already had.
 */
void palette_build_lut(uint8_t idx, uint16_t lut[PALETTE_LUT_SIZE]);

/*
 * The same LUT for the gnuboy core, whose pixel byte means something else.
 *
 * gnuboy writes the tile's RAW two bits and identifies the source in the high
 * bits — background 0-3, window 4-7, OBP0 32-35, OBP1 36-39 — and applies the
 * DMG palette registers when it builds its own colour table, not when it draws
 * the pixel. palette_build_lut()'s byte already holds the shade the register
 * selected, so a straight reordering of the table would drop BGP,
 * OBP0 and OBP1 entirely and every fade, flash and inverted screen would stop
 * happening.
 *
 * This composes the two steps into one table: register first, then ramp. Pass
 * the three DMG palette registers as the hardware holds them, two bits per
 * shade, low pair first. Window shares the background's ramp and register,
 * which is what the hardware does.
 *
 * Fills all PALETTE_LUT_SIZE entries as palette_build_lut does, so no raw
 * pixel byte can index an undefined colour; indices gnuboy's DMG path never
 * emits fold onto the background. Writes nothing if lut is NULL or idx is out
 * of range.
 */
void palette_build_lut_gnuboy(uint8_t idx, uint8_t bgp, uint8_t obp0,
                              uint8_t obp1, uint16_t lut[PALETTE_LUT_SIZE]);

/*
 * The same two builders over a ramp set the caller supplies, for colours that
 * are not in this table — the Game Boy Color's per-cartridge palettes, which
 * cart/cgb_palette.h produces in exactly this shape.
 *
 * ramps is [3][4]: row 0 OBJ0, row 1 OBJ1, row 2 BG, four shades light to
 * dark, native RGB565. The idx-taking builders above are these two with
 * ramps = the table's entry, so a supplied set and a table one colourise
 * identically. Writes nothing if either pointer is NULL.
 */
void palette_build_lut_ramps(const uint16_t ramps[3][4],
                             uint16_t lut[PALETTE_LUT_SIZE]);

void palette_build_lut_gnuboy_ramps(const uint16_t ramps[3][4], uint8_t bgp,
                                    uint8_t obp0, uint8_t obp1,
                                    uint16_t lut[PALETTE_LUT_SIZE]);

/*
 * Four bits per pixel for a copy of gnuboy's raw line bytes, so a whole
 * 160x144 frame fits in 11,520 bytes.
 *
 * gnuboy's DMG path emits only 0-7 (background, window) and 32-39 (OBP0,
 * OBP1), the groups palette_build_lut_gnuboy_ramps() fills. Code
 * (v & 7) | ((v >> 2) & 8) maps those sixteen values onto 0-15, and any other
 * byte aliases onto one of them.
 *
 * palette_pack_raw_line() packs n raw bytes into (n + 1) / 2 bytes, the first
 * pixel of each pair in the high nibble; an odd n leaves the last low nibble
 * zero. palette_packed_raw() gives back pixel x's raw byte, ready to index the
 * same LUT as the unpacked line.
 */
void palette_pack_raw_line(const uint8_t* raw, size_t n, uint8_t* out);

uint8_t palette_packed_raw(const uint8_t* packed, size_t x);

#ifdef __cplusplus
}
#endif
