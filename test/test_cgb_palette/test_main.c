#include <unity.h>

#include <string.h>

#include "cart/cgb_palette.h"

/*
 * Game Boy Color per-cartridge palette suite.
 *
 * The expected colours below are NOT read back out of the generated table:
 * they are what a Game Boy Color actually puts on screen for these three
 * cartridges, so a regenerated or mis-parsed table fails here rather than
 * agreeing with itself. Tetris is white/yellow/red/black and Pokemon Blue is
 * blue; those are the observable facts the whole feature exists to reproduce.
 *
 * The collision pair is the other half of the mechanism. GBWARS and
 * KEN GRIFFEY JR have the same title checksum (0xC6) and are told apart only
 * by the fourth character, so an implementation that skipped that check would
 * dress one of them in the other's colours and still pass a single-title test.
 */

#define HDR_SIZE 0x150u
#define HDR_TITLE 0x134u
#define HDR_NEW_LICENSEE 0x144u
#define HDR_OLD_LICENSEE 0x14Bu

/* Ramp index within the result, from the raw pixel byte's bits 5-4. */
#define RAMP_OBJ0 0
#define RAMP_OBJ1 1
#define RAMP_BG 2

static uint8_t rom[HDR_SIZE];

/* A Nintendo-published cartridge carrying this title, with the field zero
 * padded exactly as the header is — the checksum is over all sixteen bytes. */
static void make_cart(const char* title)
{
    memset(rom, 0, sizeof(rom));
    memcpy(rom + HDR_TITLE, title, strlen(title));
    rom[HDR_OLD_LICENSEE] = 0x01u; /* Nintendo, old code */
}

static void assert_ramp(const uint16_t ramp[4], uint16_t a, uint16_t b,
                        uint16_t c, uint16_t d)
{
    TEST_ASSERT_EQUAL_HEX16(a, ramp[0]);
    TEST_ASSERT_EQUAL_HEX16(b, ramp[1]);
    TEST_ASSERT_EQUAL_HEX16(c, ramp[2]);
    TEST_ASSERT_EQUAL_HEX16(d, ramp[3]);
}

static void test_tetris_is_white_yellow_red_black(void)
{
    uint16_t ramps[3][4];

    make_cart("TETRIS");
    TEST_ASSERT_TRUE(cgb_palette_lookup(rom, sizeof(rom), ramps));
    /* Combination 3: one palette for all three ramps. */
    assert_ramp(ramps[RAMP_BG], 0xFFFF, 0xFFE0, 0xF800, 0x0000);
    assert_ramp(ramps[RAMP_OBJ0], 0xFFFF, 0xFFE0, 0xF800, 0x0000);
    assert_ramp(ramps[RAMP_OBJ1], 0xFFFF, 0xFFE0, 0xF800, 0x0000);
}

static void test_pokemon_blue_is_blue(void)
{
    uint16_t ramps[3][4];

    make_cart("POKEMON BLUE");
    TEST_ASSERT_TRUE(cgb_palette_lookup(rom, sizeof(rom), ramps));
    assert_ramp(ramps[RAMP_BG], 0xFFFF, 0x653F, 0x001F, 0x0000);
    /* Sprites are not the background here: the combination names a different
     * palette for OBJ0, which is what makes three ramps worth carrying. */
    assert_ramp(ramps[RAMP_OBJ0], 0xFFFF, 0xFC30, 0x91C7, 0x0000);
}

static void test_colliding_checksums_are_told_apart_by_the_fourth_letter(void)
{
    uint16_t wars[3][4];
    uint16_t griffey[3][4];

    /* Both titles sum to 0xC6. */
    make_cart("GBWARS");
    TEST_ASSERT_TRUE(cgb_palette_lookup(rom, sizeof(rom), wars));
    make_cart("KEN GRIFFEY JR");
    TEST_ASSERT_TRUE(cgb_palette_lookup(rom, sizeof(rom), griffey));
    TEST_ASSERT_NOT_EQUAL(0, memcmp(wars, griffey, sizeof(wars)));
}

static void test_a_colliding_checksum_with_no_matching_letter_is_unknown(void)
{
    uint16_t ramps[3][4];

    /* GBWARS with two letters swapped: the same sixteen bytes in a different
     * order, so the same 0xC6, but a fourth character neither duplicate row
     * for that checksum carries. The scan has to run off the end rather than
     * settle on whichever row it reached first. */
    TEST_ASSERT_EQUAL_UINT8((uint8_t)('G' + 'B' + 'W' + 'A' + 'R' + 'S'),
                            (uint8_t)('G' + 'B' + 'W' + 'R' + 'A' + 'S'));
    make_cart("GBWRAS");
    TEST_ASSERT_FALSE(cgb_palette_lookup(rom, sizeof(rom), ramps));
}

static void test_a_third_party_cartridge_is_never_colourised(void)
{
    uint16_t ramps[3][4];

    make_cart("TETRIS");
    rom[HDR_OLD_LICENSEE] = 0x79u; /* Accolade, say */
    TEST_ASSERT_FALSE(cgb_palette_lookup(rom, sizeof(rom), ramps));

    /* 0x33 defers to the two-character new code, and only "01" is Nintendo. */
    make_cart("TETRIS");
    rom[HDR_OLD_LICENSEE] = 0x33u;
    rom[HDR_NEW_LICENSEE] = '5';
    rom[HDR_NEW_LICENSEE + 1u] = '4';
    TEST_ASSERT_FALSE(cgb_palette_lookup(rom, sizeof(rom), ramps));

    rom[HDR_NEW_LICENSEE] = '0';
    rom[HDR_NEW_LICENSEE + 1u] = '1';
    TEST_ASSERT_TRUE(cgb_palette_lookup(rom, sizeof(rom), ramps));
}

static void test_an_unknown_title_is_left_to_the_caller(void)
{
    uint16_t ramps[3][4];

    make_cart("MY HOMEBREW");
    memset(ramps, 0xAB, sizeof(ramps));
    TEST_ASSERT_FALSE(cgb_palette_lookup(rom, sizeof(rom), ramps));
    /* Untouched, as the header promises, so a caller can fill its fallback
     * either before or after the call. */
    TEST_ASSERT_EQUAL_HEX16(0xABAB, ramps[RAMP_BG][0]);
}

static void test_a_rom_too_short_for_a_header_is_refused(void)
{
    uint16_t ramps[3][4];

    make_cart("TETRIS");
    TEST_ASSERT_FALSE(cgb_palette_lookup(rom, 0x14Bu, ramps));
    TEST_ASSERT_FALSE(cgb_palette_lookup(rom, 0u, ramps));
    TEST_ASSERT_FALSE(cgb_palette_lookup(NULL, sizeof(rom), ramps));
    TEST_ASSERT_FALSE(cgb_palette_lookup(rom, sizeof(rom), NULL));
    /* One byte more is the whole header. */
    TEST_ASSERT_TRUE(cgb_palette_lookup(rom, 0x14Cu, ramps));
}

/*
 * The title field is sixteen bytes and the sum is over all of them, including
 * the manufacturer and CGB bytes that overlap its tail on a later cartridge.
 * The firmware's printable-trimmed display title is NOT what gets hashed, and
 * a lookup that trimmed first would match nothing.
 */
static void test_the_sum_is_over_the_raw_sixteen_bytes(void)
{
    uint16_t trimmed[3][4];
    uint16_t padded[3][4];

    make_cart("TETRIS");
    TEST_ASSERT_TRUE(cgb_palette_lookup(rom, sizeof(rom), trimmed));

    /* A non-zero byte anywhere in the field changes the sum, so this is no
     * longer TETRIS to the table. */
    make_cart("TETRIS");
    rom[HDR_TITLE + 15u] = 0x80u;
    TEST_ASSERT_FALSE(cgb_palette_lookup(rom, sizeof(rom), padded));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_tetris_is_white_yellow_red_black);
    RUN_TEST(test_pokemon_blue_is_blue);
    RUN_TEST(test_colliding_checksums_are_told_apart_by_the_fourth_letter);
    RUN_TEST(test_a_colliding_checksum_with_no_matching_letter_is_unknown);
    RUN_TEST(test_a_third_party_cartridge_is_never_colourised);
    RUN_TEST(test_an_unknown_title_is_left_to_the_caller);
    RUN_TEST(test_a_rom_too_short_for_a_header_is_refused);
    RUN_TEST(test_the_sum_is_over_the_raw_sixteen_bytes);
    return UNITY_END();
}
