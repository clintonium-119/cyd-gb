#pragma once
// 12-colour palette tables and the flat LUT that colourizes a Game Boy line.
//
// Peanut-GB is DMG-only, but its 12-colour output packs the palette source
// into each output pixel byte: bits 1-0 are the shade and bits 5-4 identify
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
// Ramp index p follows those pixel bits — 0 = OBJ0, 1 = OBJ1, 2 = BG. Each
// palette's BG ramp is the fork's original four colours verbatim; the two OBJ
// ramps are same-hue-family derivations of it, generated once by
// scripts/gen_palettes.py and committed as literals.
//
// Pure C, no Arduino/ESP-IDF headers, no allocation: the LUT is caller-owned.

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PALETTE_COUNT 20    /* palettes, each a 3-ramp set of 4 shades */
#define PALETTE_LUT_SIZE 64 /* one entry per possible raw pixel byte   */

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
 * the pixel. Peanut-GB does the opposite: its byte already holds the shade the
 * register selected. So a straight reordering of the table would drop BGP,
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

#ifdef __cplusplus
}
#endif
