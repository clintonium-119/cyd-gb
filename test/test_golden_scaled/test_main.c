#include <unity.h>

#include <stdio.h>

#include "gb_runner.h"
#include "render/palette.h"
#include "render/scaler.h"

/*
 * Scaled golden-frame regression pins.
 *
 * The index-buffer suite next door pins Peanut-GB's own output. These four
 * pins cover everything downstream of it: the palette LUT, the scaler's
 * pattern tables for both geometries, and the blend. Each hash is FNV-1a 64
 * over the whole scaled RGB565 frame after running dmg-acid2 for
 * GOLDEN_FRAME_COUNT frames and colourizing it through palette 0.
 *
 * The four constants are independent measurements, not derived from each
 * other. What legitimately changes them:
 *
 *   - a deliberate change to the scaler's pattern tables or blend
 *   - regenerating the palette OBJ ramps (scripts/gen_palettes.py) — palette
 *     0's BG ramp is pinned verbatim by the palette suite, but dmg-acid2 draws
 *     sprites, so OBJ values reach these hashes too
 *   - a change in Peanut-GB's output, which the index-buffer pin catches first
 *
 * In every case: update the constant in the SAME commit as the change, with
 * the reason in the commit body. Otherwise a failure here is a regression.
 *
 * The hash is taken over the frame's bytes, so it assumes a little-endian
 * host; test_toolchain pins that assumption separately.
 */
#define GOLDEN_FRAME_COUNT 60u

#define GOLDEN_24_16_NEAREST 0xC14E4D5E7D8F3294ULL
#define GOLDEN_24_16_BLEND   0x7879A9E86DB38D3FULL
#define GOLDEN_26_16_NEAREST 0x8C129E3C797D7FA8ULL
#define GOLDEN_26_16_BLEND   0x57BB983C883E96FFULL

#define GOLDEN_PALETTE 0 /* "Classic Green" */

#define ROM_PATH "test/roms/dmg-acid2.gb"
#define ROM_MAX (1024 * 1024)

static uint8_t rom[ROM_MAX];
static uint16_t lines[GB_RUNNER_H][SCALER_SRC_W];
static const uint16_t* line_ptrs[GB_RUNNER_H];
/* Widest and tallest either geometry can produce: 26/16 has both the most
 * rows per block and the widest rows, so its full frame is the bound. */
#define FRAME_MAX_PX (SCALER_DST_ROWS_MAX \
                      * (GB_RUNNER_H / SCALER_SRC_LINES_MAX) \
                      * SCALER_DST_W_MAX)

static uint16_t frame[FRAME_MAX_PX];
static uint16_t scratch[SCALER_DST_W_MAX];
static uint16_t lut[PALETTE_LUT_SIZE];

void setUp(void)
{
}

void tearDown(void)
{
}

static uint64_t fnv1a64(const uint8_t* buf, size_t len)
{
    uint64_t h = 0xcbf29ce484222325ULL;
    size_t i;
    for (i = 0; i < len; i++) {
        h ^= buf[i];
        h *= 0x100000001b3ULL;
    }
    return h;
}

/* Boot dmg-acid2, run to the pinned frame, and colourize every line into
 * RGB565 through the real palette LUT — LUT first, exactly as the firmware
 * does, because the scaler blends colours and not index bytes. */
static void render_source_lines(void)
{
    FILE* f;
    size_t rom_len;
    const uint8_t* px;
    unsigned y;
    unsigned x;

    f = fopen(ROM_PATH, "rb");
    TEST_ASSERT_NOT_NULL_MESSAGE(f, "could not open " ROM_PATH
        " — is the test running from the project root?");
    rom_len = fread(rom, 1, sizeof(rom), f);
    fclose(f);

    TEST_ASSERT_EQUAL_INT(GB_RUNNER_OK, gb_runner_init(rom, rom_len));
    TEST_ASSERT_EQUAL_INT(GB_RUNNER_OK,
        gb_runner_run_frames(GOLDEN_FRAME_COUNT));
    TEST_ASSERT_EQUAL_UINT(0, gb_runner_error_count());

    px = gb_runner_frame();
    TEST_ASSERT_NOT_NULL(px);

    palette_build_lut(GOLDEN_PALETTE, lut);
    for (y = 0; y < GB_RUNNER_H; y++) {
        for (x = 0; x < SCALER_SRC_W; x++) {
            lines[y][x] = lut[px[y * GB_RUNNER_W + x]];
        }
        line_ptrs[y] = lines[y];
    }
}

/* Scale the whole frame block by block into one contiguous buffer, then hash
 * it. Blocks are written straight into the frame: the scaler's destination is
 * row-major at dst_w, which is the frame's stride too. */
static uint64_t scale_frame_and_hash(enum scaler_geom_e geom,
                                     enum scaler_mode_e mode,
                                     unsigned expected_h)
{
    const scaler_geom_info_t* gi = scaler_geom_info(geom);
    unsigned blocks;
    unsigned dst_h;
    unsigned b;

    TEST_ASSERT_NOT_NULL(gi);
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0, GB_RUNNER_H % gi->src_lines_per_block,
        "the frame height must divide into whole blocks");
    blocks = GB_RUNNER_H / gi->src_lines_per_block;
    dst_h = blocks * gi->dst_rows_per_block;

    /* Frame dimensions come from the geometry table, and must land on the
     * design's tabulated output size for this scale. */
    TEST_ASSERT_EQUAL_UINT(expected_h, dst_h);
    TEST_ASSERT_TRUE((size_t)dst_h * gi->dst_w
                     <= sizeof(frame) / sizeof(frame[0]));

    for (b = 0; b < blocks; b++) {
        unsigned first = b * gi->src_lines_per_block;
        /* The last block has no next line, so its trailing blend rows clamp
         * to the frame's bottom edge — dmg-acid2 exercises that branch on
         * every frame, so these hashes cover the clamp. */
        const uint16_t* lookahead =
            (b + 1u < blocks) ? line_ptrs[first + gi->src_lines_per_block]
                              : NULL;
        TEST_ASSERT_EQUAL_INT(SCALER_OK,
            scaler_scale_block(geom, mode, &line_ptrs[first], lookahead,
                               frame + (size_t)b * gi->dst_rows_per_block
                                       * gi->dst_w,
                               scratch));
    }

    return fnv1a64((const uint8_t*)frame,
                   (size_t)dst_h * gi->dst_w * sizeof(frame[0]));
}

#define GOLDEN_MESSAGE \
    "the scaled output of dmg-acid2 changed. If the rendering change is " \
    "intentional, update this constant in the same commit and say why in the " \
    "commit body; otherwise this is a regression. Check the index-buffer pin " \
    "first — if that failed too, the change is upstream of the scaler."

static void test_golden_24_16_nearest(void)
{
    render_source_lines();
    TEST_ASSERT_EQUAL_HEX64_MESSAGE(GOLDEN_24_16_NEAREST,
        scale_frame_and_hash(SCALER_GEOM_24_16, SCALER_MODE_NEAREST, 216u),
        GOLDEN_MESSAGE);
}

static void test_golden_24_16_blend(void)
{
    render_source_lines();
    TEST_ASSERT_EQUAL_HEX64_MESSAGE(GOLDEN_24_16_BLEND,
        scale_frame_and_hash(SCALER_GEOM_24_16, SCALER_MODE_BLEND, 216u),
        GOLDEN_MESSAGE);
}

static void test_golden_26_16_nearest(void)
{
    render_source_lines();
    TEST_ASSERT_EQUAL_HEX64_MESSAGE(GOLDEN_26_16_NEAREST,
        scale_frame_and_hash(SCALER_GEOM_26_16, SCALER_MODE_NEAREST, 234u),
        GOLDEN_MESSAGE);
}

static void test_golden_26_16_blend(void)
{
    render_source_lines();
    TEST_ASSERT_EQUAL_HEX64_MESSAGE(GOLDEN_26_16_BLEND,
        scale_frame_and_hash(SCALER_GEOM_26_16, SCALER_MODE_BLEND, 234u),
        GOLDEN_MESSAGE);
}

/* The blend must actually change the picture: a mode that silently fell back
 * to nearest-neighbour would otherwise pass three of the four pins above by
 * matching a constant measured from the same broken code. */
static void test_blend_differs_from_nearest(void)
{
    uint64_t nearest;
    uint64_t blend;

    render_source_lines();
    nearest = scale_frame_and_hash(SCALER_GEOM_24_16, SCALER_MODE_NEAREST, 216u);
    blend = scale_frame_and_hash(SCALER_GEOM_24_16, SCALER_MODE_BLEND, 216u);
    TEST_ASSERT_NOT_EQUAL(nearest, blend);

    nearest = scale_frame_and_hash(SCALER_GEOM_26_16, SCALER_MODE_NEAREST, 234u);
    blend = scale_frame_and_hash(SCALER_GEOM_26_16, SCALER_MODE_BLEND, 234u);
    TEST_ASSERT_NOT_EQUAL(nearest, blend);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_golden_24_16_nearest);
    RUN_TEST(test_golden_24_16_blend);
    RUN_TEST(test_golden_26_16_nearest);
    RUN_TEST(test_golden_26_16_blend);
    RUN_TEST(test_blend_differs_from_nearest);
    return UNITY_END();
}
