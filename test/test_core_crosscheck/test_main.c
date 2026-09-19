#include <unity.h>

#include <stdio.h>
#include <string.h>

#include "gb_runner.h"
#include "gnuboy_runner.h"
#include "render/palette.h"

/*
 * Cross-check: the two emulator cores on one fixture.
 *
 * This suite REPORTS divergence; it does not assert the cores agree. Neither
 * core is the reference for the other, and gnuboy's day-one output is not
 * captured as a golden here — the point is to see where and how much they
 * differ so a human can judge which is closer to a real Game Boy. The only
 * failures are the ones that mean something is miswired rather than merely
 * different.
 *
 * The fixture is dmg-acid2, a PPU conformance ROM. That is what makes it the
 * right fixture for a renderer swap, rather than merely the one in the tree:
 * a picture-quality verdict wants a fixture whose output can show the
 * artefact under test.
 *
 * COMPARISON IS ON COLOUR, NOT ON RAW INDICES. The cores' pixel bytes mean
 * different things — Peanut-GB packs a palette id into bits 5-4 and the shade
 * the palette register selected into bits 1-0, while gnuboy identifies the
 * source in the high bits and carries the tile's RAW two bits, applying the
 * registers when it builds its own colour table. Diffing raw bytes would call
 * every pixel divergent and measure nothing. Each core's buffer is resolved
 * through its own LUT, from the same palette, and the resulting colours are
 * compared.
 */

#define ROM_PATH "test/roms/dmg-acid2.gb"
#define ROM_MAX (1024 * 1024)

/* dmg-acid2 settles at frame 10 under Peanut-GB and is constant through 180;
 * the golden pin uses 60 and this uses the same count so the two suites are
 * looking at the same moment. Whether gnuboy has settled by then is checked
 * below rather than assumed. */
#define SETTLE_FRAMES 60u

/* The palette the comparison resolves through. Any would do — the question is
 * whether the two cores put the same colour in the same place, not which
 * colour — and index 1 is the plain DMG ramp, which makes a printed report
 * readable. */
#define CROSSCHECK_PALETTE 1u

#define W 160
#define H 144

static uint8_t rom[ROM_MAX];
static size_t rom_len;

static uint16_t lut_peanut[PALETTE_LUT_SIZE];
static uint16_t lut_gnuboy[PALETTE_LUT_SIZE];

void setUp(void)
{
}

void tearDown(void)
{
}

static void load_rom(void)
{
    FILE* f = fopen(ROM_PATH, "rb");
    TEST_ASSERT_NOT_NULL_MESSAGE(f, "could not open " ROM_PATH
        " — is the test running from the project root?");
    rom_len = fread(rom, 1, sizeof(rom), f);
    fclose(f);
    TEST_ASSERT_GREATER_THAN_UINT32(0x150u, (uint32_t)rom_len);
}

/* Both cores booted and run to the same point, with the LUTs built from the
 * palette registers as they stand at that moment. */
static void run_both(unsigned frames)
{
    load_rom();
    TEST_ASSERT_EQUAL_INT(GB_RUNNER_OK, gb_runner_init(rom, rom_len));
    TEST_ASSERT_EQUAL_INT(GNUBOY_RUNNER_OK, gnuboy_runner_init(rom, rom_len));
    TEST_ASSERT_EQUAL_INT(GB_RUNNER_OK, gb_runner_run_frames(frames));
    TEST_ASSERT_EQUAL_INT(GNUBOY_RUNNER_OK, gnuboy_runner_run_frames(frames));

    palette_build_lut(CROSSCHECK_PALETTE, lut_peanut);
    palette_build_lut_gnuboy(CROSSCHECK_PALETTE, gnuboy_runner_bgp(),
                             gnuboy_runner_obp0(), gnuboy_runner_obp1(),
                             lut_gnuboy);
}

/*
 * Differing pixels after both buffers are resolved to colour, and the first
 * coordinate at which they differ. Either output pointer may be NULL.
 */
static unsigned diff_colours(int* first_x, int* first_y)
{
    const uint8_t* p = gb_runner_frame();
    const uint8_t* g = gnuboy_runner_frame();
    unsigned n = 0;
    int fx = -1;
    int fy = -1;
    int y;
    int x;

    for (y = 0; y < H; y++) {
        for (x = 0; x < W; x++) {
            uint16_t cp = lut_peanut[p[(size_t)y * W + x]];
            uint16_t cg = lut_gnuboy[g[(size_t)y * W + x]];
            if (cp != cg) {
                if (fx < 0) {
                    fx = x;
                    fy = y;
                }
                n++;
            }
        }
    }
    if (first_x) {
        *first_x = fx;
    }
    if (first_y) {
        *first_y = fy;
    }
    return n;
}

/* ─── Boot ───────────────────────────────────────────────────────────────── */

static void test_both_cores_boot_the_fixture_without_error(void)
{
    run_both(SETTLE_FRAMES);
    TEST_ASSERT_EQUAL_UINT(0u, gb_runner_error_count());
    TEST_ASSERT_EQUAL_UINT(0u, gnuboy_runner_error_count());
    TEST_ASSERT_NOT_NULL(gb_runner_frame());
    TEST_ASSERT_NOT_NULL(gnuboy_runner_frame());
}

/* ─── The per-line hook ──────────────────────────────────────────────────── */

static void test_the_hook_fires_once_per_line_in_order(void)
{
    run_both(SETTLE_FRAMES);
    TEST_ASSERT_EQUAL_UINT(144u, gnuboy_runner_line_calls());
    TEST_ASSERT_TRUE_MESSAGE(gnuboy_runner_lines_ordered(),
        "the hook reported line numbers out of order, repeated or with gaps");
}

/* ─── Pixel encoding ─────────────────────────────────────────────────────── */

static void test_every_gnuboy_pixel_is_inside_the_lut_domain(void)
{
    const uint8_t* g;
    size_t i;

    run_both(SETTLE_FRAMES);
    g = gnuboy_runner_frame();
    /* The LUT covers all 64 values, which is what lets the consumer index it
     * with no mask; this pins that no pixel escapes even so. */
    for (i = 0; i < (size_t)W * H; i++) {
        TEST_ASSERT_LESS_THAN_UINT(PALETTE_LUT_SIZE, g[i]);
    }
}

static void test_the_gnuboy_lut_maps_background_and_window_alike(void)
{
    uint16_t lut[PALETTE_LUT_SIZE];
    unsigned c;

    /* 0xE4 is the identity palette register: shade n for raw bits n. */
    palette_build_lut_gnuboy(CROSSCHECK_PALETTE, 0xE4u, 0xE4u, 0xE4u, lut);
    for (c = 0; c < 4u; c++) {
        TEST_ASSERT_EQUAL_HEX16(lut[c], lut[4u + c]);
    }
}

static void test_sprite_indices_resolve_through_their_own_registers(void)
{
    uint16_t a[PALETTE_LUT_SIZE];
    uint16_t b[PALETTE_LUT_SIZE];
    unsigned c;

    palette_build_lut_gnuboy(CROSSCHECK_PALETTE, 0xE4u, 0xE4u, 0xE4u, a);
    /* Reverse OBP0 alone. If sprites went through BGP the table would not
     * move; if OBP1 shared OBP0's register, 36-39 would move too. */
    palette_build_lut_gnuboy(CROSSCHECK_PALETTE, 0xE4u, 0x1Bu, 0xE4u, b);
    for (c = 0; c < 4u; c++) {
        TEST_ASSERT_EQUAL_HEX16(a[32u + (3u - c)], b[32u + c]);
        TEST_ASSERT_EQUAL_HEX16(a[36u + c], b[36u + c]);
        TEST_ASSERT_EQUAL_HEX16(a[c], b[c]);
    }
}

/* ─── Audio ──────────────────────────────────────────────────────────────── */

static void test_the_two_cores_produce_comparable_sample_counts(void)
{
    unsigned peanut;
    unsigned gnuboy;

    run_both(SETTLE_FRAMES);
    peanut = gb_runner_audio_samples();
    gnuboy = gnuboy_runner_audio_samples();
    printf("[crosscheck] samples/frame: peanut=%u gnuboy=%u\n", peanut, gnuboy);
    /* Not equal, and that is the finding rather than a fault: MiniGB APU
     * emits a fixed frame, gnuboy's count follows the frame's real emulated
     * length and alternates by one. Anything further apart than that is a
     * wiring fault — a wrong sample rate would be off by a factor. */
    TEST_ASSERT_UINT_WITHIN_MESSAGE(1u, peanut, gnuboy,
        "gnuboy's samples per frame are further than one from the other "
        "core's — check the rate it was initialised with");
}

/* ─── Cartridge RAM ──────────────────────────────────────────────────────── */

static void test_both_cores_agree_on_the_cartridges_save_length(void)
{
    static const uint32_t sizes[] = {
        0x0u, 0x800u, 0x2000u, 0x8000u, 0x20000u, 0x10000u
    };
    uint8_t code;
    uint32_t declared;
    size_t allocated = 0;

    run_both(SETTLE_FRAMES);
    code = rom[0x149];
    TEST_ASSERT_LESS_THAN_UINT8((uint8_t)(sizeof(sizes) / sizeof(sizes[0])),
                                code);
    declared = sizes[code];

    /* The save length both bridges hand out is the header's, so it is the
     * same number by construction. What has to hold is that gnuboy's
     * allocation covers it — it rounds up to whole 8 KB banks and gives even
     * a cartridge that declares none a bank — or the flat buffer would be
     * read past its end. */
    TEST_ASSERT_NOT_NULL(gnuboy_runner_cart_ram(&allocated));
    TEST_ASSERT_GREATER_OR_EQUAL_UINT32((uint32_t)declared,
                                        (uint32_t)allocated);
    printf("[crosscheck] save length declared=%u, gnuboy allocated=%u\n",
           (unsigned)declared, (unsigned)allocated);
}

/* ─── Divergence ─────────────────────────────────────────────────────────── */

static void test_gnuboys_picture_has_settled_by_the_compared_frame(void)
{
    static uint8_t at_settle[W * H];
    unsigned moved;
    size_t i;

    run_both(SETTLE_FRAMES);
    memcpy(at_settle, gnuboy_runner_frame(), sizeof(at_settle));
    TEST_ASSERT_EQUAL_INT(GNUBOY_RUNNER_OK, gnuboy_runner_run_frames(30u));

    moved = 0;
    for (i = 0; i < sizeof(at_settle); i++) {
        if (at_settle[i] != gnuboy_runner_frame()[i]) {
            moved++;
        }
    }
    printf("[crosscheck] gnuboy pixels still moving after frame %u: %u\n",
           (unsigned)SETTLE_FRAMES, moved);
    /* If the picture is still changing, any divergence figure below is partly
     * a timing artefact rather than a renderer disagreement. */
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0u, moved,
        "gnuboy's output is still changing at the compared frame, so the "
        "divergence figure would be measuring boot timing, not rendering");
}

static void test_divergence_is_reported_and_bounded(void)
{
    unsigned differing;
    int fx = -1;
    int fy = -1;

    run_both(SETTLE_FRAMES);
    differing = diff_colours(&fx, &fy);

    printf("[crosscheck] fixture=%s frames=%u\n", ROM_PATH,
           (unsigned)SETTLE_FRAMES);
    printf("[crosscheck] differing pixels: %u of %u (%.2f%%)\n",
           differing, (unsigned)(W * H),
           100.0 * (double)differing / (double)(W * H));
    if (differing) {
        printf("[crosscheck] first difference at x=%d y=%d: "
               "peanut idx 0x%02X -> 0x%04X, gnuboy idx 0x%02X -> 0x%04X\n",
               fx, fy,
               gb_runner_frame()[(size_t)fy * W + fx],
               lut_peanut[gb_runner_frame()[(size_t)fy * W + fx]],
               gnuboy_runner_frame()[(size_t)fy * W + fx],
               lut_gnuboy[gnuboy_runner_frame()[(size_t)fy * W + fx]]);
    } else {
        printf("[crosscheck] the two cores agree on every pixel\n");
    }

    /* The only assertion: more than half the picture differing is not two
     * renderers disagreeing, it is one of them not drawing the fixture —
     * a wrong palette layout, a buffer that never filled, a core that never
     * booted. Below that, the number is a result to read, not a verdict. */
    TEST_ASSERT_LESS_THAN_UINT_MESSAGE((unsigned)(W * H) / 2u, differing,
        "over half the pixels differ, which is a wiring fault rather than a "
        "renderer disagreement");
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_both_cores_boot_the_fixture_without_error);
    RUN_TEST(test_the_hook_fires_once_per_line_in_order);
    RUN_TEST(test_every_gnuboy_pixel_is_inside_the_lut_domain);
    RUN_TEST(test_the_gnuboy_lut_maps_background_and_window_alike);
    RUN_TEST(test_sprite_indices_resolve_through_their_own_registers);
    RUN_TEST(test_the_two_cores_produce_comparable_sample_counts);
    RUN_TEST(test_both_cores_agree_on_the_cartridges_save_length);
    RUN_TEST(test_gnuboys_picture_has_settled_by_the_compared_frame);
    RUN_TEST(test_divergence_is_reported_and_bounded);
    return UNITY_END();
}
