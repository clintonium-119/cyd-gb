#include "palette.h"

#include <stddef.h>

/*
 * The 20 palettes as 3-ramp sets, native RGB565.
 *
 * Row order matches the raw pixel byte's palette bits: OBJ0, OBJ1, BG. The BG
 * row of every palette is the fork's original four-colour ramp, unchanged —
 * so the background look of each palette is exactly what it was before
 * colourization, and any difference on screen comes from the blend and the
 * sprite ramps, not from re-picked base colours.
 *
 * The two OBJ rows are derived from the BG row so all three share a hue
 * family; far-apart hues fringe where sprite and background pixels blend at an
 * edge (design §2.4). Chromatic ramps scale saturation by 0.80 for OBJ0 and
 * rotate hue by +12 degrees for OBJ1; ramps whose every colour sits below 0.10
 * saturation, where a hue rotation would be a no-op, instead scale value by
 * 0.88 and 1.14. scripts/gen_palettes.py holds the generator and the
 * parameters; regenerate there and paste, do not hand-edit one entry into a
 * different derivation.
 */
static const uint16_t pals[PALETTE_COUNT][3][4] = {
    {  /*  0 Classic Green (chromatic) */
        { 0xAFEA, 0x6F69, 0x3D46, 0x1263 },  /* OBJ0 */
        { 0x77E5, 0x2764, 0x1544, 0x0263 },  /* OBJ1 */
        { 0x9FE5, 0x4F64, 0x2542, 0x0261 },  /* BG   */
    },
    {  /*  1 Original DMG (achromatic) */
        { 0xDEFB, 0x94B2, 0x4A69, 0x0000 },  /* OBJ0 */
        { 0xFFFF, 0xC618, 0x5B0B, 0x0000 },  /* OBJ1 */
        { 0xFFFF, 0xAD55, 0x52AA, 0x0000 },  /* BG   */
    },
    {  /*  2 Pocket Gray (achromatic) */
        { 0xDEFB, 0x9CF3, 0x5AEB, 0x0000 },  /* OBJ0 */
        { 0xFFFF, 0xCE59, 0x7BCF, 0x0000 },  /* OBJ1 */
        { 0xFFFF, 0xB596, 0x6B4D, 0x0000 },  /* BG   */
    },
    {  /*  3 Warm Sepia (chromatic) */
        { 0xFFDF, 0xD691, 0x7AAC, 0x1082 },  /* OBJ0 */
        { 0xFFDF, 0xC6AF, 0x7A4A, 0x1082 },  /* OBJ1 */
        { 0xFFDF, 0xD68F, 0x7A4B, 0x1082 },  /* BG   */
    },
    {  /*  4 Cool Blue (chromatic) */
        { 0xCF7F, 0x8D7F, 0x5C7F, 0x2959 },  /* OBJ0 */
        { 0xBEFF, 0x6BFF, 0x325F, 0x2819 },  /* OBJ1 */
        { 0xBF5F, 0x6CDF, 0x339F, 0x0019 },  /* BG   */
    },
    {  /*  5 Autumn (chromatic) */
        { 0xFFF3, 0xFCC6, 0x88E3, 0x2041 },  /* OBJ0 */
        { 0xE7F0, 0xFDA0, 0x88E0, 0x2040 },  /* OBJ1 */
        { 0xFFF0, 0xFC00, 0x8800, 0x2000 },  /* BG   */
    },
    {  /*  6 Grayscale (achromatic) */
        { 0xCE39, 0x8C51, 0x39E7, 0x0000 },  /* OBJ0 */
        { 0xFFDF, 0xB576, 0x4A89, 0x0000 },  /* OBJ1 */
        { 0xE71C, 0x9CD3, 0x4228, 0x0000 },  /* BG   */
    },
    {  /*  7 Lava (chromatic) */
        { 0xFFFF, 0xFE86, 0xC945, 0x4062 },  /* OBJ0 */
        { 0xFFFF, 0xFFC0, 0xC940, 0x4060 },  /* OBJ1 */
        { 0xFFFF, 0xFE20, 0xC800, 0x4000 },  /* BG   */
    },
    {  /*  8 Ocean (chromatic) */
        { 0xBFFF, 0x7F7F, 0x55BF, 0x2959 },  /* OBJ0 */
        { 0xAF7F, 0x5E5F, 0x2BBF, 0x2819 },  /* OBJ1 */
        { 0xAFFF, 0x5F5F, 0x2D1F, 0x0019 },  /* BG   */
    },
    {  /*  9 Forest (chromatic) */
        { 0xFFF3, 0xBDE5, 0x5AE2, 0x0921 },  /* OBJ0 */
        { 0xE7F0, 0x95E0, 0x4AE0, 0x0121 },  /* OBJ1 */
        { 0xFFF0, 0xBDE0, 0x5AE0, 0x0120 },  /* BG   */
    },
    {  /* 10 Sunset (chromatic) */
        { 0xFFFF, 0xFDA6, 0xAB84, 0x4062 },  /* OBJ0 */
        { 0xFFFF, 0xFEC0, 0xAC20, 0x4060 },  /* OBJ1 */
        { 0xFFFF, 0xFD20, 0xAB00, 0x4000 },  /* BG   */
    },
    {  /* 11 Cherry (chromatic) */
        { 0xFFDF, 0xF73C, 0xAAB3, 0x3868 },  /* OBJ0 */
        { 0xFFDF, 0xF71C, 0xAA10, 0x4007 },  /* OBJ1 */
        { 0xFFDF, 0xF71C, 0xAA13, 0x3808 },  /* BG   */
    },
    {  /* 12 Ice (chromatic) */
        { 0xD7FF, 0x9EBF, 0x6C3F, 0x2959 },  /* OBJ0 */
        { 0xCFBF, 0x85BF, 0x421F, 0x2819 },  /* OBJ1 */
        { 0xCFFF, 0x867F, 0x433F, 0x0019 },  /* BG   */
    },
    {  /* 13 Chocolate (chromatic) */
        { 0xFFD8, 0xD56D, 0x8A8A, 0x3041 },  /* OBJ0 */
        { 0xF7F6, 0xD5EA, 0x8A88, 0x3040 },  /* OBJ1 */
        { 0xFFB6, 0xD52A, 0x8A08, 0x3000 },  /* BG   */
    },
    {  /* 14 Mint (chromatic) */
        { 0xFFFF, 0xCF7F, 0x7F5F, 0x2959 },  /* OBJ0 */
        { 0xFFFF, 0xBEFF, 0x5E1F, 0x2819 },  /* OBJ1 */
        { 0xFFFF, 0xBF5F, 0x5F1F, 0x0019 },  /* BG   */
    },
    {  /* 15 Peach (chromatic) */
        { 0xFFF9, 0xFD66, 0xC9A5, 0x60A2 },  /* OBJ0 */
        { 0xF7F8, 0xFE60, 0xC9C0, 0x60A0 },  /* OBJ1 */
        { 0xFFF8, 0xFCC0, 0xC880, 0x6000 },  /* BG   */
    },
    {  /* 16 Lavender (chromatic) */
        { 0xEF5C, 0x9EBF, 0x5A79, 0x0000 },  /* OBJ0 */
        { 0xEF5C, 0x85BF, 0x6179, 0x0000 },  /* OBJ1 */
        { 0xEF3C, 0x867F, 0x4179, 0x0000 },  /* BG   */
    },
    {  /* 17 Neon (chromatic) */
        { 0xFFFF, 0x37FF, 0x31BF, 0x0000 },  /* OBJ0 */
        { 0xFFFF, 0x065F, 0x301F, 0x0000 },  /* OBJ1 */
        { 0xFFFF, 0x07FF, 0x001F, 0x0000 },  /* BG   */
    },
    {  /* 18 Inverted (achromatic) */
        { 0x0000, 0x39E7, 0x94B2, 0xDEFB },  /* OBJ0 */
        { 0x0000, 0x4A89, 0xC618, 0xFFFF },  /* OBJ1 */
        { 0x0000, 0x4228, 0xAD55, 0xFFFF },  /* BG   */
    },
    {  /* 19 Gold (chromatic) */
        { 0xFEA6, 0xAB84, 0x5082, 0x0000 },  /* OBJ0 */
        { 0xFFE0, 0xAC20, 0x5080, 0x0000 },  /* OBJ1 */
        { 0xFE60, 0xAB00, 0x5000, 0x0000 },  /* BG   */
    },
};

static const char* palnames[PALETTE_COUNT] = {
    "Classic Green", "Original DMG", "Pocket Gray", "Warm Sepia", "Cool Blue",
    "Autumn", "Grayscale", "Lava", "Ocean", "Forest",
    "Sunset", "Cherry", "Ice", "Chocolate", "Mint",
    "Peach", "Lavender", "Neon", "Inverted", "Gold",
};

const char* palette_name(uint8_t idx)
{
    if (idx >= PALETTE_COUNT) {
        return "?";
    }
    return palnames[idx];
}

void palette_build_lut(uint8_t idx, uint16_t lut[PALETTE_LUT_SIZE])
{
    unsigned i;

    if (lut == NULL || idx >= PALETTE_COUNT) {
        return;
    }
    for (i = 0; i < PALETTE_LUT_SIZE; i++) {
        unsigned p = i >> 4;
        if (p > 2u) {
            p = 2u; /* no real pixel byte reaches here; fold onto BG */
        }
        lut[i] = pals[idx][p][i & 3u];
    }
}
