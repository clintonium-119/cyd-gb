#include "cgb_palette.h"

#include "cgb_palette_table.h"

#include <stddef.h>

/* Cartridge header fields, at their absolute addresses in the ROM image. */
#define HDR_TITLE         0x134u  /* sixteen bytes                          */
#define HDR_NEW_LICENSEE  0x144u  /* two ASCII digits, when old is 0x33     */
#define HDR_OLD_LICENSEE  0x14Bu
#define HDR_END           0x14Cu  /* one past the last byte read here       */

#define TITLE_LEN         16u
#define FOURTH_LETTER     3u      /* the disambiguating character           */

/*
 * Nintendo-published or nothing. The boot ROM refuses to colourise a
 * third-party cartridge, and so does this: without the gate a homebrew whose
 * title happens to sum to one of the ninety-four would be dressed in some
 * unrelated game's colours, which is worse than the default.
 */
static bool is_nintendo(const uint8_t* rom)
{
    if (rom[HDR_OLD_LICENSEE] == 0x01u) {
        return true;
    }
    if (rom[HDR_OLD_LICENSEE] != 0x33u) {
        return false;
    }
    /* 0x33 defers to the two-character new code, where Nintendo is "01". */
    return rom[HDR_NEW_LICENSEE] == '0' && rom[HDR_NEW_LICENSEE + 1u] == '1';
}

/*
 * The combination id for this title, or -1 when the table does not know it.
 *
 * The raw header bytes, not a printable-trimmed name: the sum is over all
 * sixteen including the padding and, on a cartridge that fills the tail with
 * the manufacturer and CGB fields, over those too. Trimming the title first
 * would change the sum and match nothing.
 */
static int combo_for_title(const uint8_t* rom)
{
    const uint8_t* title = rom + HDR_TITLE;
    unsigned sum = 0;
    unsigned i;

    for (i = 0; i < TITLE_LEN; i++) {
        sum += title[i];
    }
    sum &= 0xFFu;

    for (i = 0; i < CGB_PAL_CHECKSUM_COUNT; i++) {
        if (cgb_pal_checksums[i] != (uint8_t)sum) {
            continue;
        }
        /* The tail of the table collides: several games share a sum, and the
         * fourth character is what tells them apart. A sum that matches there
         * but whose letter does not is not a match at all — keep scanning,
         * because the same sum appears again further down. */
        if (i >= CGB_PAL_FIRST_DUPLICATE
            && title[FOURTH_LETTER]
                   != (uint8_t)cgb_pal_fourth[i - CGB_PAL_FIRST_DUPLICATE]) {
            continue;
        }
        return (int)cgb_pal_combo_of[i];
    }
    return -1;
}

bool cgb_palette_lookup(const uint8_t* rom, uint32_t len, uint16_t ramps[3][4])
{
    int combo;
    unsigned ramp;
    unsigned shade;

    if (rom == NULL || ramps == NULL || len < HDR_END) {
        return false;
    }
    if (!is_nintendo(rom)) {
        return false;
    }
    combo = combo_for_title(rom);
    if (combo < 0 || combo >= CGB_PAL_COMBO_COUNT) {
        return false;
    }

    for (ramp = 0; ramp < 3u; ramp++) {
        /* A combination entry is a BYTE offset into the colour array, not a
         * palette number, and three of the fifty-one are deliberately odd
         * multiples: they read a four-colour window that straddles two
         * palettes. Halving to a colour index keeps that; dividing by eight to
         * a palette index would quietly lose it. */
        unsigned first = cgb_pal_combos[combo][ramp] / 2u;

        for (shade = 0; shade < 4u; shade++) {
            ramps[ramp][shade] = cgb_pal_colours[first + shade];
        }
    }
    return true;
}
