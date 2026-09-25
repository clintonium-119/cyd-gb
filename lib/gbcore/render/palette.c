#include "palette.h"

#include <stddef.h>

/*
 * The 14 palettes as 3-ramp sets, native RGB565.
 *
 * Row order matches the raw pixel byte's palette bits: OBJ0, OBJ1, BG.
 *
 * Entries 0 and 1 have a chosen BG row: SameBoy's DMG green, and the fork's
 * Pocket Gray. Their two OBJ rows are derived from it so all three share a hue
 * family; far-apart hues fringe where sprite and background pixels blend at an
 * edge (design §2.4). Chromatic ramps scale saturation by 0.80 for OBJ0 and
 * rotate hue by +12 degrees for OBJ1; ramps whose every colour sits below 0.10
 * saturation, where a hue rotation would be a no-op, instead scale value by
 * 0.88 and 1.14.
 *
 * Entries 2-13 are the Game Boy Color boot ROM's twelve D-pad + button
 * palettes with Nintendo's own three rows, nothing derived. Some use one ramp
 * for all three rows, and Reverse runs dark to light; both are the hardware's
 * choices.
 *
 * scripts/gen_palettes.py holds the generator, the sources and the
 * parameters; regenerate there and paste, do not hand-edit an entry.
 */
static const uint16_t pals[PALETTE_COUNT][3][4] = {
    {  /*  0 DMG Green (chromatic) */
        { 0xCEF3, 0x8D2E, 0x4308, 0x08C2 },  /* OBJ0 */
        { 0xB6F1, 0x752C, 0x3B08, 0x08C2 },  /* OBJ1 */
        { 0xC6F1, 0x852C, 0x3B07, 0x08C2 },  /* BG   */
    },
    {  /*  1 Pocket Gray (achromatic) */
        { 0xDEFB, 0x9CF3, 0x5AEB, 0x0000 },  /* OBJ0 */
        { 0xFFFF, 0xCE59, 0x7BCF, 0x0000 },  /* OBJ1 */
        { 0xFFFF, 0xB596, 0x6B4D, 0x0000 },  /* BG   */
    },
    {  /*  2 Green (CGB Right, combination 1) */
        { 0xFFFF, 0x57E0, 0xFA00, 0x0000 },  /* OBJ0 */
        { 0xFFFF, 0x57E0, 0xFA00, 0x0000 },  /* OBJ1 */
        { 0xFFFF, 0x57E0, 0xFA00, 0x0000 },  /* BG   */
    },
    {  /*  3 Dark Green (CGB Right + A, combination 0) */
        { 0xFFFF, 0xFC30, 0x91C7, 0x0000 },  /* OBJ0 */
        { 0xFFFF, 0xFC30, 0x91C7, 0x0000 },  /* OBJ1 */
        { 0xFFFF, 0x7FE6, 0x0318, 0x0000 },  /* BG   */
    },
    {  /*  4 Reverse (CGB Right + B, combination 6) */
        { 0x0000, 0x0430, 0xFEE0, 0xFFFF },  /* OBJ0 */
        { 0x0000, 0x0430, 0xFEE0, 0xFFFF },  /* OBJ1 */
        { 0x0000, 0x0430, 0xFEE0, 0xFFFF },  /* BG   */
    },
    {  /*  5 Blue (CGB Left, combination 48) */
        { 0xFFFF, 0xFC30, 0x91C7, 0x0000 },  /* OBJ0 */
        { 0xFFFF, 0x7FE6, 0x0420, 0x0000 },  /* OBJ1 */
        { 0xFFFF, 0x653F, 0x001F, 0x0000 },  /* BG   */
    },
    {  /*  6 Dark Blue (CGB Left + A, combination 40) */
        { 0xFFFF, 0xFC30, 0x91C7, 0x0000 },  /* OBJ0 */
        { 0xFFFF, 0xFD6C, 0x8180, 0x0000 },  /* OBJ1 */
        { 0xFFFF, 0x8C7B, 0x5291, 0x0000 },  /* BG   */
    },
    {  /*  7 Grayscale (CGB Left + B, combination 7) */
        { 0xFFFF, 0xA534, 0x528A, 0x0000 },  /* OBJ0 */
        { 0xFFFF, 0xA534, 0x528A, 0x0000 },  /* OBJ1 */
        { 0xFFFF, 0xA534, 0x528A, 0x0000 },  /* BG   */
    },
    {  /*  8 Brown (CGB Up, combination 5) */
        { 0xFFFF, 0xFD6C, 0x8180, 0x0000 },  /* OBJ0 */
        { 0xFFFF, 0xFD6C, 0x8180, 0x0000 },  /* OBJ1 */
        { 0xFFFF, 0xFD6C, 0x8180, 0x0000 },  /* BG   */
    },
    {  /*  9 Red (CGB Up + A, combination 43) */
        { 0xFFFF, 0x7FE6, 0x0420, 0x0000 },  /* OBJ0 */
        { 0xFFFF, 0x653F, 0x001F, 0x0000 },  /* OBJ1 */
        { 0xFFFF, 0xFC30, 0x91C7, 0x0000 },  /* BG   */
    },
    {  /* 10 Dark Brown (CGB Up + B, combination 28) */
        { 0xFFFF, 0xFD6C, 0x8180, 0x0000 },  /* OBJ0 */
        { 0xFFFF, 0xFD6C, 0x8180, 0x0000 },  /* OBJ1 */
        { 0xFF38, 0xCCF0, 0x8345, 0x5981 },  /* BG   */
    },
    {  /* 11 Pastel (CGB Down, combination 8) */
        { 0xFFF4, 0xFCB2, 0x94BF, 0x0000 },  /* OBJ0 */
        { 0xFFF4, 0xFCB2, 0x94BF, 0x0000 },  /* OBJ1 */
        { 0xFFF4, 0xFCB2, 0x94BF, 0x0000 },  /* BG   */
    },
    {  /* 12 Orange (CGB Down + A, combination 3) */
        { 0xFFFF, 0xFFE0, 0xF800, 0x0000 },  /* OBJ0 */
        { 0xFFFF, 0xFFE0, 0xF800, 0x0000 },  /* OBJ1 */
        { 0xFFFF, 0xFFE0, 0xF800, 0x0000 },  /* BG   */
    },
    {  /* 13 Yellow (CGB Down + B, combination 49) */
        { 0xFFFF, 0x653F, 0x001F, 0x0000 },  /* OBJ0 */
        { 0xFFFF, 0x7FE6, 0x0420, 0x0000 },  /* OBJ1 */
        { 0xFFFF, 0xFFE0, 0x7A40, 0x0000 },  /* BG   */
    },
};

static const char* palnames[PALETTE_COUNT] = {
    "DMG Green", "Pocket Gray",
    "Green", "Dark Green", "Reverse",
    "Blue", "Dark Blue", "Grayscale",
    "Brown", "Red", "Dark Brown",
    "Pastel", "Orange", "Yellow",
};

const char* palette_name(uint8_t idx)
{
    if (idx >= PALETTE_COUNT) {
        return "?";
    }
    return palnames[idx];
}

void palette_build_lut_ramps(const uint16_t ramps[3][4],
                             uint16_t lut[PALETTE_LUT_SIZE])
{
    unsigned i;

    if (lut == NULL || ramps == NULL) {
        return;
    }
    for (i = 0; i < PALETTE_LUT_SIZE; i++) {
        unsigned p = i >> 4;
        if (p > 2u) {
            p = 2u; /* no real pixel byte reaches here; fold onto BG */
        }
        lut[i] = ramps[p][i & 3u];
    }
}

void palette_build_lut(uint8_t idx, uint16_t lut[PALETTE_LUT_SIZE])
{
    if (idx >= PALETTE_COUNT) {
        return;
    }
    palette_build_lut_ramps(pals[idx], lut);
}

/* Shade the register selects for raw tile bits c: two bits each, low pair
 * first, exactly as the DMG's BGP/OBP0/OBP1 are laid out. */
static unsigned reg_shade(uint8_t reg, unsigned c)
{
    return (unsigned)((reg >> (2u * c)) & 3u);
}

void palette_build_lut_gnuboy_ramps(const uint16_t ramps[3][4], uint8_t bgp,
                                    uint8_t obp0, uint8_t obp1,
                                    uint16_t lut[PALETTE_LUT_SIZE])
{
    unsigned i;
    unsigned c;

    if (lut == NULL || ramps == NULL) {
        return;
    }
    /* Everything first, folded onto the background: the groups below then
     * overwrite the indices gnuboy actually emits, and no byte is left
     * pointing at an undefined colour. */
    for (i = 0; i < PALETTE_LUT_SIZE; i++) {
        lut[i] = ramps[2][reg_shade(bgp, i & 3u)];
    }
    for (c = 0; c < 4u; c++) {
        lut[0u + c]  = ramps[2][reg_shade(bgp, c)];   /* background */
        lut[4u + c]  = ramps[2][reg_shade(bgp, c)];   /* window     */
        lut[32u + c] = ramps[0][reg_shade(obp0, c)];  /* OBP0       */
        lut[36u + c] = ramps[1][reg_shade(obp1, c)];  /* OBP1       */
    }
}

void palette_build_lut_gnuboy(uint8_t idx, uint8_t bgp, uint8_t obp0,
                              uint8_t obp1, uint16_t lut[PALETTE_LUT_SIZE])
{
    if (idx >= PALETTE_COUNT) {
        return;
    }
    palette_build_lut_gnuboy_ramps(pals[idx], bgp, obp0, obp1, lut);
}

static uint8_t raw_code(uint8_t v)
{
    return (uint8_t)((v & 7u) | ((v >> 2) & 8u));
}

void palette_pack_raw_line(const uint8_t* raw, size_t n, uint8_t* out)
{
    size_t i;

    for (i = 0; i + 1 < n; i += 2) {
        out[i / 2] = (uint8_t)((raw_code(raw[i]) << 4) | raw_code(raw[i + 1]));
    }
    if (i < n) {
        out[i / 2] = (uint8_t)(raw_code(raw[i]) << 4);
    }
}

uint8_t palette_packed_raw(const uint8_t* packed, size_t x)
{
    uint8_t code = packed[x / 2];

    code = (x & 1u) ? (uint8_t)(code & 15u) : (uint8_t)(code >> 4);
    return code < 8u ? code : (uint8_t)(code + 24u);
}
