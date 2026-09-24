#include <unity.h>

#include <stdio.h>
#include <string.h>

#include "gnuboy_runner.h"
#include "render/palette.h"

/*
 * gnuboy's host-side contract: the per-line hook, the pixel encoding the LUT
 * resolves, the cartridge RAM allocation, and that dmg-acid2 has settled by
 * the frame the golden pins look at.
 *
 * The fixture is dmg-acid2, a PPU conformance ROM.
 */

#define ROM_PATH "test/roms/dmg-acid2.gb"
#define ROM_MAX (1024 * 1024)

/* The frame the golden pins hash. Whether gnuboy has settled by then is
 * checked below rather than assumed. */
#define SETTLE_FRAMES 60u

/* Any palette would do for the LUT-shape checks; index 1 is the plain DMG
 * ramp. */
#define LUT_PALETTE 1u

#define W 160
#define H 144

static uint8_t rom[ROM_MAX];
static size_t rom_len;

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

/* gnuboy booted and run to the given frame. */
static void run_gnuboy(unsigned frames)
{
    load_rom();
    TEST_ASSERT_EQUAL_INT(GNUBOY_RUNNER_OK, gnuboy_runner_init(rom, rom_len));
    TEST_ASSERT_EQUAL_INT(GNUBOY_RUNNER_OK, gnuboy_runner_run_frames(frames));
}

/* ─── The per-line hook ──────────────────────────────────────────────────── */

static void test_the_hook_fires_once_per_line_in_order(void)
{
    run_gnuboy(SETTLE_FRAMES);
    TEST_ASSERT_EQUAL_UINT(144u, gnuboy_runner_line_calls());
    TEST_ASSERT_EQUAL_UINT(1u, gnuboy_runner_frames());
    TEST_ASSERT_TRUE_MESSAGE(gnuboy_runner_lines_ordered(),
        "the hook reported line numbers out of order, repeated or with gaps");
}

/*
 * A run is not a frame, and the bridge must not assume it is.
 *
 * gnuboy's run loop tests the line counter between CPU steps, so a step that
 * carries the LCD past the last line and around to the top is not noticed and
 * the run keeps going into the next frame. The first run after a reset draws
 * 287 lines — two frames' worth — before it returns.
 *
 * This is pinned because the suite could not catch it before: every other case
 * here looks at the settled frame, by which point a run is one frame and the
 * hazard has gone. A bridge that took the run as the frame boundary committed
 * the second frame's block 0 while the queue was still expecting the first
 * frame's last block, the queue rejected it, and the rejected commit stranded
 * its slot until the producer deadlocked waiting for a free one.
 */
static void test_a_single_run_can_span_more_than_one_lcd_frame(void)
{
    load_rom();
    TEST_ASSERT_EQUAL_INT(GNUBOY_RUNNER_OK, gnuboy_runner_init(rom, rom_len));
    TEST_ASSERT_EQUAL_INT(GNUBOY_RUNNER_OK, gnuboy_runner_run_frames(1u));

    printf("[gnuboy] first run after reset: %u hook calls across %u LCD "
           "frames\n", gnuboy_runner_line_calls(), gnuboy_runner_frames());
    TEST_ASSERT_GREATER_THAN_UINT_MESSAGE(144u, gnuboy_runner_line_calls(),
        "the first run no longer spans more than one frame — if gnuboy's run "
        "loop changed, the bridge's wrap-detected frame boundary should be "
        "re-read, not silently kept");
    TEST_ASSERT_GREATER_THAN_UINT(1u, gnuboy_runner_frames());
    /* Deliberately no ordering assertion here. The reset frame starts at line
     * 1, not 0: gnuboy comes out of a hard reset in hblank, and its state
     * machine increments the line counter before it renders, so line 0 of the
     * very first frame is never drawn. The bridge covers that case already —
     * the blocks before the first drawn line go out carrying the frame
     * buffer's initial blank — and the settled-frame case above is where
     * clean 0..143 ordering is pinned. */
}

/* ─── Pixel encoding ─────────────────────────────────────────────────────── */

static void test_every_gnuboy_pixel_is_inside_the_lut_domain(void)
{
    const uint8_t* g;
    size_t i;

    run_gnuboy(SETTLE_FRAMES);
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
    palette_build_lut_gnuboy(LUT_PALETTE, 0xE4u, 0xE4u, 0xE4u, lut);
    for (c = 0; c < 4u; c++) {
        TEST_ASSERT_EQUAL_HEX16(lut[c], lut[4u + c]);
    }
}

static void test_sprite_indices_resolve_through_their_own_registers(void)
{
    uint16_t a[PALETTE_LUT_SIZE];
    uint16_t b[PALETTE_LUT_SIZE];
    unsigned c;

    palette_build_lut_gnuboy(LUT_PALETTE, 0xE4u, 0xE4u, 0xE4u, a);
    /* Reverse OBP0 alone. If sprites went through BGP the table would not
     * move; if OBP1 shared OBP0's register, 36-39 would move too. */
    palette_build_lut_gnuboy(LUT_PALETTE, 0xE4u, 0x1Bu, 0xE4u, b);
    for (c = 0; c < 4u; c++) {
        TEST_ASSERT_EQUAL_HEX16(a[32u + (3u - c)], b[32u + c]);
        TEST_ASSERT_EQUAL_HEX16(a[36u + c], b[36u + c]);
        TEST_ASSERT_EQUAL_HEX16(a[c], b[c]);
    }
}

/* ─── Cartridge RAM ──────────────────────────────────────────────────────── */

static void test_the_ram_allocation_covers_the_headers_save_length(void)
{
    static const uint32_t sizes[] = {
        0x0u, 0x800u, 0x2000u, 0x8000u, 0x20000u, 0x10000u
    };
    uint8_t code;
    uint32_t declared;
    size_t allocated = 0;

    run_gnuboy(SETTLE_FRAMES);
    code = rom[0x149];
    TEST_ASSERT_LESS_THAN_UINT8((uint8_t)(sizeof(sizes) / sizeof(sizes[0])),
                                code);
    declared = sizes[code];

    /* The save length the bridge hands out is the header's. What has to hold
     * is that gnuboy's allocation covers it — it rounds up to whole 8 KB banks and gives even
     * a cartridge that declares none a bank — or the flat buffer would be
     * read past its end. */
    TEST_ASSERT_NOT_NULL(gnuboy_runner_cart_ram(&allocated));
    TEST_ASSERT_GREATER_OR_EQUAL_UINT32((uint32_t)declared,
                                        (uint32_t)allocated);
    printf("[gnuboy] save length declared=%u, gnuboy allocated=%u\n",
           (unsigned)declared, (unsigned)allocated);
}

/* ─── Settling ─────────────────────────────────────────────────────────── */

static void test_gnuboys_picture_has_settled_by_the_pinned_frame(void)
{
    static uint8_t at_settle[W * H];
    unsigned moved;
    size_t i;

    run_gnuboy(SETTLE_FRAMES);
    memcpy(at_settle, gnuboy_runner_frame(), sizeof(at_settle));
    TEST_ASSERT_EQUAL_INT(GNUBOY_RUNNER_OK, gnuboy_runner_run_frames(30u));

    moved = 0;
    for (i = 0; i < sizeof(at_settle); i++) {
        if (at_settle[i] != gnuboy_runner_frame()[i]) {
            moved++;
        }
    }
    printf("[gnuboy] gnuboy pixels still moving after frame %u: %u\n",
           (unsigned)SETTLE_FRAMES, moved);
    /* If the picture is still changing, the golden pins hash a moment of
     * boot timing rather than a rendered frame. */
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0u, moved,
        "gnuboy's output is still changing at the pinned frame, so the "
        "golden hashes would be measuring boot timing, not rendering");
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_the_hook_fires_once_per_line_in_order);
    RUN_TEST(test_a_single_run_can_span_more_than_one_lcd_frame);
    RUN_TEST(test_every_gnuboy_pixel_is_inside_the_lut_domain);
    RUN_TEST(test_the_gnuboy_lut_maps_background_and_window_alike);
    RUN_TEST(test_sprite_indices_resolve_through_their_own_registers);
    RUN_TEST(test_the_ram_allocation_covers_the_headers_save_length);
    RUN_TEST(test_gnuboys_picture_has_settled_by_the_pinned_frame);
    return UNITY_END();
}
