#include <unity.h>

#include <stdio.h>

#include "gb_runner.h"
#include "render/palette.h"
#include "render/scaler.h"

/*
 * Scaled golden-frame regression pins.
 *
 * The index-buffer suite next door pins Peanut-GB's own output. These two
 * pins cover everything downstream of it: the palette LUT, the scaler's
 * pattern table, and the blend. Each hash is FNV-1a 64 over the whole scaled
 * RGB565 frame after running dmg-acid2 for GOLDEN_FRAME_COUNT frames and
 * colourizing it through palette 0.
 *
 * The two constants are independent measurements, not derived from each
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

#define GOLDEN_5_3_NEAREST   0x83CB60D3C84CEC38ULL
#define GOLDEN_5_3_BLEND     0x5E331E79E1250913ULL

#define GOLDEN_PALETTE 0 /* "Classic Green" */

#define ROM_PATH "test/roms/dmg-acid2.gb"
#define ROM_MAX (1024 * 1024)

static uint8_t rom[ROM_MAX];
static uint16_t lines[GB_RUNNER_H][SCALER_SRC_W];
static const uint16_t* line_ptrs[GB_RUNNER_H];
/* The panel is 240 rows and 5/3's output row is SCALER_DST_W_MAX wide, so
 * this is the frame exactly. */
#define FRAME_MAX_PX (SCALER_DST_W_MAX * 240)

static uint16_t frame[FRAME_MAX_PX];
static uint16_t scratch[SCALER_DST_W_MAX];
static uint16_t lut[PALETTE_LUT_SIZE];

/* The same source frame as columns, which is the shape the column walk reads
 * and the shape the producer will hand it on target. */
static uint16_t cols[SCALER_SRC_W][GB_RUNNER_H];
static const uint16_t* col_ptrs[SCALER_SRC_W];
static uint16_t col_frame[FRAME_MAX_PX];

/* The same frame packed: two pixels in three bytes, with a tail of canary
 * bytes so a walk that wrote a pixel count where bytes were meant runs into
 * them rather than off the end. */
#define PACKED_FRAME_BYTES (FRAME_MAX_PX * 3 / 2)
#define PACKED_CANARY 0xC5u
static uint8_t packed_frame[PACKED_FRAME_BYTES + 16];
static uint16_t scratch444[SCALER_SCRATCH_444_MAX];

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
            cols[x][y] = lines[y][x];
        }
        line_ptrs[y] = lines[y];
    }
    for (x = 0; x < SCALER_SRC_W; x++) {
        col_ptrs[x] = cols[x];
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

/* Which output unit of the group is an interpolated one, restated from the
 * design's 2,1,2 duplication rhythm and applied on both axes. */
static const uint8_t blend_unit_5_3[5] = { 0, 1, 0, 0, 1 };

/* Scale the whole frame as columns, the counterpart of scale_frame_and_hash().
 * Blocks are written straight into col_frame, which is column-major at dst_h:
 * one output column is dst_h contiguous pixels. The one leftover source
 * column past the last whole block is the tail, and belongs to no block. */
static unsigned scale_frame_col(enum scaler_geom_e geom,
                                enum scaler_mode_e mode)
{
    const scaler_geom_info_t* gi = scaler_geom_info(geom);
    unsigned src_units;
    unsigned dst_units;
    unsigned dst_h;
    unsigned blocks;
    unsigned b;

    TEST_ASSERT_NOT_NULL(gi);
    src_units = gi->src_lines_per_block;
    dst_units = gi->dst_rows_per_block;
    dst_h = GB_RUNNER_H / src_units * dst_units;
    blocks = SCALER_SRC_W / src_units;
    TEST_ASSERT_TRUE((size_t)dst_h * gi->dst_w
                     <= sizeof(col_frame) / sizeof(col_frame[0]));

    for (b = 0; b < blocks; b++) {
        unsigned first = b * src_units;
        /* At the frame's right edge there is no next column, so the trailing
         * blend columns clamp — the same branch the row walk takes at the
         * bottom edge. That edge is the tail column below rather than a
         * block, so in practice every block here has a real lookahead. */
        const uint16_t* lookahead = (first + src_units < SCALER_SRC_W)
            ? col_ptrs[first + src_units] : NULL;
        TEST_ASSERT_EQUAL_INT(SCALER_OK,
            scaler_scale_col_block(geom, mode, &col_ptrs[first], lookahead,
                                   col_frame + (size_t)b * dst_units * dst_h,
                                   scratch, SCALER_COLS_ASCENDING));
    }
    TEST_ASSERT_EQUAL_INT(SCALER_OK,
        scaler_scale_col_tail(geom, mode, &col_ptrs[blocks * src_units],
                              col_frame + (size_t)blocks * dst_units * dst_h));
    return dst_h;
}

/* The 565 frame's two walks again, packed. Same block placement, same tail,
 * same lookahead rule; only the destination's units are bytes. */
static void scale_frame_packed(enum scaler_geom_e geom)
{
    const scaler_geom_info_t* gi = scaler_geom_info(geom);
    unsigned blocks = GB_RUNNER_H / gi->src_lines_per_block;
    unsigned b;

    for (b = 0; b < sizeof(packed_frame); b++) {
        packed_frame[b] = PACKED_CANARY;
    }
    for (b = 0; b < blocks; b++) {
        unsigned first = b * gi->src_lines_per_block;
        const uint16_t* lookahead =
            (b + 1u < blocks) ? line_ptrs[first + gi->src_lines_per_block]
                              : NULL;
        TEST_ASSERT_EQUAL_INT(SCALER_OK,
            scaler_scale_block_444(geom, SCALER_MODE_BLEND,
                                   &line_ptrs[first], lookahead,
                                   packed_frame
                                   + SCALER_PACKED_BYTES((size_t)b
                                       * gi->dst_rows_per_block * gi->dst_w),
                                   scratch444));
    }
}

static unsigned scale_frame_col_packed(enum scaler_geom_e geom)
{
    const scaler_geom_info_t* gi = scaler_geom_info(geom);
    unsigned src_units = gi->src_lines_per_block;
    unsigned dst_units = gi->dst_rows_per_block;
    unsigned dst_h = GB_RUNNER_H / src_units * dst_units;
    unsigned blocks = SCALER_SRC_W / src_units;
    unsigned b;

    for (b = 0; b < sizeof(packed_frame); b++) {
        packed_frame[b] = PACKED_CANARY;
    }
    for (b = 0; b < blocks; b++) {
        unsigned first = b * src_units;
        const uint16_t* lookahead = (first + src_units < SCALER_SRC_W)
            ? col_ptrs[first + src_units] : NULL;
        TEST_ASSERT_EQUAL_INT(SCALER_OK,
            scaler_scale_col_block_444(geom, SCALER_MODE_BLEND,
                                       &col_ptrs[first], lookahead,
                                       packed_frame
                                       + SCALER_PACKED_BYTES((size_t)b
                                           * dst_units * dst_h),
                                       scratch444, SCALER_COLS_ASCENDING));
    }
    TEST_ASSERT_EQUAL_INT(SCALER_OK,
        scaler_scale_col_tail_444(geom, SCALER_MODE_BLEND,
                                  &col_ptrs[blocks * src_units],
                                  packed_frame
                                  + SCALER_PACKED_BYTES((size_t)blocks
                                      * dst_units * dst_h),
                                  scratch444));
    return dst_h;
}

/* The truncation, restated from the 565 field layout rather than from the
 * packer's masks. */
static unsigned quant444(uint16_t c)
{
    unsigned r = (unsigned)((c >> 11) & 0x1Fu);
    unsigned g = (unsigned)((c >> 5) & 0x3Fu);
    unsigned b = (unsigned)(c & 0x1Fu);

    return ((r >> 1) << 8) | ((g >> 2) << 4) | (b >> 1);
}

/* Pixel i out of the packed stream: byte 0 = R0 G0, byte 1 = B0 R1,
 * byte 2 = G1 B1. */
static unsigned packed_px(size_t i)
{
    size_t b = (i >> 1) * 3u;

    if (i & 1u) {
        return (unsigned)(((packed_frame[b + 1] & 0x0Fu) << 8)
                          | packed_frame[b + 2]);
    }
    return (unsigned)((packed_frame[b] << 4) | (packed_frame[b + 1] >> 4));
}

static void assert_packed_frame_matches(const uint16_t* src565, unsigned n)
{
    size_t used = SCALER_PACKED_BYTES(n);
    size_t i;

    for (i = 0; i < n; i++) {
        char msg[80];

        if (quant444(src565[i]) == packed_px(i)) {
            continue;
        }
        sprintf(msg, "packed pixel %u is %03X, not %03X", (unsigned)i,
                packed_px(i), quant444(src565[i]));
        TEST_ASSERT_EQUAL_HEX16_MESSAGE(quant444(src565[i]), packed_px(i),
                                        msg);
    }
    for (i = used; i < sizeof(packed_frame); i++) {
        TEST_ASSERT_EQUAL_HEX8_MESSAGE(PACKED_CANARY, packed_frame[i],
            "the packed frame is not width * height * 3 / 2 bytes");
    }
}

/*
 * A whole dmg-acid2 frame through both packed walks, against the same frame
 * in 565. This is the round trip the phase rests on: the picture the panel
 * gets is the 565 picture with four bits a channel dropped, and nothing else
 * — no channel shifted, no pair swapped, no column landing a nibble out.
 *
 * The frame is the fixture rather than a gradient because a DMG title's
 * colours ARE its colourised ramps, so this is where a quantisation that
 * collapses two neighbouring shades into one would show.
 *
 * It also pins the packed walks' agreement with each other, transitively:
 * the column walk transposes the row walk to one channel step above, and
 * quantisation cannot widen that — floor of a bounded difference is bounded
 * by it.
 */
static void test_packed_frame_is_the_565_frame_quantised(void)
{
    const scaler_geom_info_t* gi = scaler_geom_info(SCALER_GEOM_5_3);
    unsigned dst_h;

    render_source_lines();

    (void)scale_frame_and_hash(SCALER_GEOM_5_3, SCALER_MODE_BLEND, 240u);
    scale_frame_packed(SCALER_GEOM_5_3);
    assert_packed_frame_matches(frame, 240u * gi->dst_w);

    dst_h = scale_frame_col(SCALER_GEOM_5_3, SCALER_MODE_BLEND);
    TEST_ASSERT_EQUAL_UINT(240u, dst_h);
    TEST_ASSERT_EQUAL_UINT(dst_h, scale_frame_col_packed(SCALER_GEOM_5_3));
    assert_packed_frame_matches(col_frame, dst_h * gi->dst_w);
}

/* The largest per-channel gap between two RGB565 pixels. */
static unsigned channel_gap(uint16_t a, uint16_t b)
{
    static const unsigned shift[3] = { 11u, 5u, 0u };
    static const unsigned mask[3] = { 31u, 63u, 31u };
    unsigned worst = 0;
    unsigned f;

    for (f = 0; f < 3; f++) {
        unsigned ca = (a >> shift[f]) & mask[f];
        unsigned cb = (b >> shift[f]) & mask[f];
        unsigned gap = ca > cb ? ca - cb : cb - ca;
        if (gap > worst) {
            worst = gap;
        }
    }
    return worst;
}

#define GOLDEN_MESSAGE \
    "the scaled output of dmg-acid2 changed. If the rendering change is " \
    "intentional, update this constant in the same commit and say why in the " \
    "commit body; otherwise this is a regression. Check the index-buffer pin " \
    "first — if that failed too, the change is upstream of the scaler."

static void test_golden_5_3_nearest(void)
{
    render_source_lines();
    TEST_ASSERT_EQUAL_HEX64_MESSAGE(GOLDEN_5_3_NEAREST,
        scale_frame_and_hash(SCALER_GEOM_5_3, SCALER_MODE_NEAREST, 240u),
        GOLDEN_MESSAGE);
}

static void test_golden_5_3_blend(void)
{
    render_source_lines();
    TEST_ASSERT_EQUAL_HEX64_MESSAGE(GOLDEN_5_3_BLEND,
        scale_frame_and_hash(SCALER_GEOM_5_3, SCALER_MODE_BLEND, 240u),
        GOLDEN_MESSAGE);
}

/* The blend must actually change the picture: a mode that silently fell back
 * to nearest-neighbour would otherwise pass one of the pins above by matching
 * a constant measured from the same broken code. */
static void test_blend_differs_from_nearest(void)
{
    uint64_t nearest;
    uint64_t blend;

    render_source_lines();
    nearest = scale_frame_and_hash(SCALER_GEOM_5_3, SCALER_MODE_NEAREST, 240u);
    blend = scale_frame_and_hash(SCALER_GEOM_5_3, SCALER_MODE_BLEND, 240u);
    TEST_ASSERT_NOT_EQUAL(nearest, blend);
}

/*
 * The column walk equals the row walk transposed. One assertion covers the
 * whole geometry: the tail, the clamping at the frame's right and bottom
 * edges, and the cross-block lookahead rule, over the same dmg-acid2 frame the
 * hashes above pin — which exercises every one of those branches.
 *
 * Exact is the rule, with one bounded exception. Where an output pixel is both
 * a horizontal and a vertical seam, the two walks average the same four
 * sources in a different order — the row walk averages two horizontally scaled
 * rows, the column walk two vertically scaled columns — and scaler_avg565() is
 * a per-channel floor((a + b) / 2), which is not associative across that
 * regrouping. There the two may differ by 1 in a channel and no more.
 * Everywhere else, and everywhere at all in NEAREST mode, they agree pixel for
 * pixel.
 */
static void assert_column_walk_transposes_the_row_walk(enum scaler_geom_e geom,
                                                       enum scaler_mode_e mode,
                                                       unsigned expected_h)
{
    const scaler_geom_info_t* gi = scaler_geom_info(geom);
    const uint8_t* blend = blend_unit_5_3;
    unsigned dst_units = gi->dst_rows_per_block;
    unsigned dst_w = gi->dst_w;
    unsigned whole_w = SCALER_SRC_W / gi->src_lines_per_block * dst_units;
    unsigned dst_h;
    unsigned x;
    unsigned y;
    unsigned tolerated = 0;

    (void)scale_frame_and_hash(geom, mode, expected_h);
    dst_h = scale_frame_col(geom, mode);
    TEST_ASSERT_EQUAL_UINT(expected_h, dst_h);

    for (x = 0; x < dst_w; x++) {
        /* A column past the last whole group is the tail: pure, never a seam
         * across the walk. Vertically there is no tail — three divides 144 —
         * so every row's unit index is its position in the group. */
        int seam_x = (x < whole_w) && blend[x % dst_units];

        for (y = 0; y < dst_h; y++) {
            int seam_y = blend[y % dst_units] != 0;
            uint16_t r = frame[(size_t)y * dst_w + x];
            uint16_t c = col_frame[(size_t)x * dst_h + y];
            char msg[96];

            if (r == c) {
                continue;
            }
            if (mode == SCALER_MODE_BLEND && seam_x && seam_y) {
                sprintf(msg, "both-seam pixel (%u, %u) is %u channel steps "
                             "apart, not 1", x, y, channel_gap(r, c));
                TEST_ASSERT_EQUAL_UINT_MESSAGE(1u, channel_gap(r, c), msg);
                tolerated++;
                continue;
            }
            sprintf(msg, "pixel (%u, %u): row walk %04X, column walk %04X",
                    x, y, r, c);
            TEST_ASSERT_EQUAL_HEX16_MESSAGE(r, c, msg);
        }
    }

    /* NEAREST copies and never averages, so nothing is tolerated there. */
    if (mode == SCALER_MODE_NEAREST) {
        TEST_ASSERT_EQUAL_UINT(0u, tolerated);
    }
}

static void test_column_walk_transposes_the_row_walk(void)
{
    render_source_lines();
    assert_column_walk_transposes_the_row_walk(SCALER_GEOM_5_3,
                                               SCALER_MODE_NEAREST, 240u);
    assert_column_walk_transposes_the_row_walk(SCALER_GEOM_5_3,
                                               SCALER_MODE_BLEND, 240u);
}

/*
 * The 5/3 tail is a whole output column rather than a pixel per row, and it is
 * the one column no block emits. Pinned on its own because an off-by-one in
 * the block count would leave it as whatever the buffer held, and because a
 * pure column must match the row walk exactly: only one axis interpolates
 * there, so the tolerance above cannot hide a wrong column.
 */
static void test_5_3_tail_column_is_the_last_source_column(void)
{
    unsigned dst_h;
    unsigned y;

    render_source_lines();
    (void)scale_frame_and_hash(SCALER_GEOM_5_3, SCALER_MODE_BLEND, 240u);
    dst_h = scale_frame_col(SCALER_GEOM_5_3, SCALER_MODE_BLEND);

    for (y = 0; y < dst_h; y++) {
        TEST_ASSERT_EQUAL_HEX16_MESSAGE(frame[(size_t)y * 266u + 265u],
            col_frame[(size_t)265u * dst_h + y],
            "the 5/3 tail column is not the row walk's last column");
    }
    /* And it is the scale of source column 159 alone, not a blend reaching
     * toward a column that does not exist: its pure rows are source pixels. */
    TEST_ASSERT_EQUAL_HEX16(cols[159][0], col_frame[(size_t)265u * dst_h]);
    TEST_ASSERT_EQUAL_HEX16(cols[159][GB_RUNNER_H - 1],
                            col_frame[(size_t)265u * dst_h + dst_h - 1u]);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_golden_5_3_nearest);
    RUN_TEST(test_golden_5_3_blend);
    RUN_TEST(test_blend_differs_from_nearest);
    RUN_TEST(test_column_walk_transposes_the_row_walk);
    RUN_TEST(test_5_3_tail_column_is_the_last_source_column);
    RUN_TEST(test_packed_frame_is_the_565_frame_quantised);
    return UNITY_END();
}
