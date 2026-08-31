#include <unity.h>

#include "render/scaler.h"

/*
 * Scaler suite. The blend properties asserted here are the ones the design
 * names verbatim for this workstream: avg565(a, a) == a, symmetry, and a
 * result whose per-channel value never leaves the two inputs' range.
 *
 * The geometry cases come in two flavours deliberately. The "literal" tests
 * isolate one axis at a time (uniform source lines isolate the vertical
 * pattern; identical source lines isolate the horizontal one) and assert
 * hand-computed RGB565 constants, so a misunderstanding of the duplication
 * rhythm fails loudly. The "spec" tests then sweep every output pixel of a
 * block against the unit table restated below, which catches indexing and
 * overrun bugs the literal tests cannot reach.
 */

#define CANARY 0xC5C5u
#define GUARD 4

/* One output unit: the source unit copied, and the unit averaged with it
 * (-1 = pure). Restated from the design's duplication rhythms — 1,2 for
 * 24/16 and 1,2,1,2,2,1,2,2 for 26/16 — and applied on both axes. */
typedef struct {
    int src;
    int partner;
} unit_spec_t;

static const unit_spec_t spec_24_16[3] = {
    { 0, -1 }, { 0, 1 }, { 1, -1 },
};

static const unit_spec_t spec_26_16[13] = {
    { 0, -1 },
    { 1, -1 }, { 1, 2 },
    { 2, -1 },
    { 3, -1 }, { 3, 4 },
    { 4, -1 }, { 4, 5 },
    { 5, -1 },
    { 6, -1 }, { 6, 7 },
    { 7, -1 }, { 7, 8 },
};

/* Destination block and scratch row wrapped in canary guards. The block is
 * sized for the largest geometry; the unused tail is checked too, so a
 * per-row overrun at dst_w 240 shows up as a clobbered canary. */
static struct {
    uint16_t pre[GUARD];
    uint16_t block[SCALER_DST_ROWS_MAX * SCALER_DST_W_MAX];
    uint16_t post[GUARD];
} dst;

static struct {
    uint16_t pre[GUARD];
    uint16_t row[SCALER_DST_W_MAX];
    uint16_t post[GUARD];
} scratch;

static uint16_t src[SCALER_SRC_LINES_MAX][SCALER_SRC_W];
static uint16_t lookahead[SCALER_SRC_W];
static const uint16_t* lines[SCALER_SRC_LINES_MAX];

static void reset_dst(void)
{
    size_t i;
    for (i = 0; i < GUARD; i++) {
        dst.pre[i] = dst.post[i] = CANARY;
        scratch.pre[i] = scratch.post[i] = CANARY;
    }
    for (i = 0; i < sizeof(dst.block) / sizeof(dst.block[0]); i++) {
        dst.block[i] = CANARY;
    }
    for (i = 0; i < SCALER_DST_W_MAX; i++) {
        scratch.row[i] = CANARY;
    }
}

void setUp(void)
{
    unsigned l;
    unsigned i;
    for (l = 0; l < SCALER_SRC_LINES_MAX; l++) {
        for (i = 0; i < SCALER_SRC_W; i++) {
            src[l][i] = (uint16_t)((((l * 3u) & 31u) << 11) |
                                   (((i * 7u) & 63u) << 5) |
                                   ((i * 5u) & 31u));
        }
        lines[l] = src[l];
    }
    for (i = 0; i < SCALER_SRC_W; i++) {
        lookahead[i] = (uint16_t)(0xF800u | (((i * 11u) & 63u) << 5) |
                                  ((i * 3u) & 31u));
    }
    reset_dst();
}

void tearDown(void)
{
}

static uint16_t* row_of(unsigned r, unsigned dst_w)
{
    return dst.block + (size_t)r * dst_w;
}

/* Guards intact, and every block pixel past the geometry's own footprint
 * still untouched. */
static void assert_canaries_intact(unsigned used)
{
    size_t i;
    for (i = 0; i < GUARD; i++) {
        TEST_ASSERT_EQUAL_HEX16(CANARY, dst.pre[i]);
        TEST_ASSERT_EQUAL_HEX16(CANARY, dst.post[i]);
        TEST_ASSERT_EQUAL_HEX16(CANARY, scratch.pre[i]);
        TEST_ASSERT_EQUAL_HEX16(CANARY, scratch.post[i]);
    }
    for (i = used; i < sizeof(dst.block) / sizeof(dst.block[0]); i++) {
        TEST_ASSERT_EQUAL_HEX16(CANARY, dst.block[i]);
    }
}

/* Fill every source line, and the lookahead, with one value each. */
static void set_uniform_lines(const uint16_t* values, unsigned count,
                              uint16_t lookahead_value)
{
    unsigned l;
    unsigned i;
    for (l = 0; l < count; l++) {
        for (i = 0; i < SCALER_SRC_W; i++) {
            src[l][i] = values[l];
        }
    }
    for (i = 0; i < SCALER_SRC_W; i++) {
        lookahead[i] = lookahead_value;
    }
}

/* Give every source line, and the lookahead, the same repeating ramp, so
 * vertical blending is the identity and only the horizontal pattern shows. */
static void set_identical_ramp(unsigned period, unsigned step)
{
    unsigned l;
    unsigned i;
    for (i = 0; i < SCALER_SRC_W; i++) {
        uint16_t v = (uint16_t)(((i % period) * step) << 11);
        for (l = 0; l < SCALER_SRC_LINES_MAX; l++) {
            src[l][i] = v;
        }
        lookahead[i] = v;
    }
}

/* ── avg565 properties ────────────────────────────────────────────────── */

static void test_avg565_is_the_identity_for_equal_inputs(void)
{
    uint32_t v;
    for (v = 0; v <= 0xFFFFu; v++) {
        TEST_ASSERT_EQUAL_HEX16((uint16_t)v, scaler_avg565((uint16_t)v, (uint16_t)v));
    }
}

static void assert_symmetric_and_in_range(uint16_t a, uint16_t b)
{
    uint16_t m = scaler_avg565(a, b);
    unsigned fields[3][3];
    unsigned f;

    TEST_ASSERT_EQUAL_HEX16(m, scaler_avg565(b, a));

    fields[0][0] = (a >> 11) & 31u;
    fields[0][1] = (b >> 11) & 31u;
    fields[0][2] = (m >> 11) & 31u;
    fields[1][0] = (a >> 5) & 63u;
    fields[1][1] = (b >> 5) & 63u;
    fields[1][2] = (m >> 5) & 63u;
    fields[2][0] = a & 31u;
    fields[2][1] = b & 31u;
    fields[2][2] = m & 31u;

    for (f = 0; f < 3; f++) {
        unsigned lo = fields[f][0] < fields[f][1] ? fields[f][0] : fields[f][1];
        unsigned hi = fields[f][0] > fields[f][1] ? fields[f][0] : fields[f][1];
        TEST_ASSERT_TRUE_MESSAGE(fields[f][2] >= lo && fields[f][2] <= hi,
            "avg565 left a channel outside the two inputs' range");
    }
}

static void test_avg565_is_symmetric_and_stays_within_channel_ranges(void)
{
    /* Corners, per-channel maxima, single low bits, and the two dither
     * masks, crossed with each other. */
    static const uint16_t corners[] = {
        0x0000u, 0xFFFFu, 0xF800u, 0x07E0u, 0x001Fu,
        0x0800u, 0x0020u, 0x0001u, 0x8410u, 0x7BEFu, 0xF7DEu, 0x0821u,
    };
    unsigned i;
    unsigned j;
    uint32_t r = 0x12345678u;

    for (i = 0; i < sizeof(corners) / sizeof(corners[0]); i++) {
        for (j = 0; j < sizeof(corners) / sizeof(corners[0]); j++) {
            assert_symmetric_and_in_range(corners[i], corners[j]);
        }
    }
    for (i = 0; i < 20000u; i++) {
        uint16_t a;
        uint16_t b;
        r = r * 1103515245u + 12345u;
        a = (uint16_t)(r >> 13);
        r = r * 1103515245u + 12345u;
        b = (uint16_t)(r >> 13);
        assert_symmetric_and_in_range(a, b);
    }
}

/* ── geometry table ──────────────────────────────────────────────────── */

static void test_geom_info_reports_both_geometries(void)
{
    const scaler_geom_info_t* g = scaler_geom_info(SCALER_GEOM_24_16);
    TEST_ASSERT_NOT_NULL(g);
    TEST_ASSERT_EQUAL_UINT(2, g->src_lines_per_block);
    TEST_ASSERT_EQUAL_UINT(3, g->dst_rows_per_block);
    TEST_ASSERT_EQUAL_UINT(240, g->dst_w);

    g = scaler_geom_info(SCALER_GEOM_26_16);
    TEST_ASSERT_NOT_NULL(g);
    TEST_ASSERT_EQUAL_UINT(8, g->src_lines_per_block);
    TEST_ASSERT_EQUAL_UINT(13, g->dst_rows_per_block);
    TEST_ASSERT_EQUAL_UINT(260, g->dst_w);

    /* Both tables sized so one set of caller buffers covers either. */
    TEST_ASSERT_TRUE(g->dst_w <= SCALER_DST_W_MAX);
    TEST_ASSERT_TRUE(g->dst_rows_per_block <= SCALER_DST_ROWS_MAX);
    TEST_ASSERT_TRUE(g->src_lines_per_block <= SCALER_SRC_LINES_MAX);
}

static void test_geom_info_rejects_an_unknown_geometry(void)
{
    TEST_ASSERT_NULL(scaler_geom_info((enum scaler_geom_e)2));
    TEST_ASSERT_NULL(scaler_geom_info((enum scaler_geom_e)99));
}

/* ── 24/16 ───────────────────────────────────────────────────────────── */

static void test_24_16_nearest_keeps_the_placeholder_semantics(void)
{
    unsigned k;
    unsigned x;

    TEST_ASSERT_EQUAL_INT(SCALER_OK,
        scaler_scale_block(SCALER_GEOM_24_16, SCALER_MODE_NEAREST,
                           lines, lookahead, dst.block, scratch.row));

    /* Each source pair (a, b) becomes (a, a, b), and the middle output row
     * duplicates the top one. */
    for (k = 0; k * 2u + 1u < SCALER_SRC_W; k++) {
        uint16_t a0 = src[0][k * 2u];
        uint16_t b0 = src[0][k * 2u + 1u];
        uint16_t a1 = src[1][k * 2u];
        uint16_t b1 = src[1][k * 2u + 1u];
        TEST_ASSERT_EQUAL_HEX16(a0, row_of(0, 240)[k * 3u]);
        TEST_ASSERT_EQUAL_HEX16(a0, row_of(0, 240)[k * 3u + 1u]);
        TEST_ASSERT_EQUAL_HEX16(b0, row_of(0, 240)[k * 3u + 2u]);
        TEST_ASSERT_EQUAL_HEX16(a1, row_of(2, 240)[k * 3u]);
        TEST_ASSERT_EQUAL_HEX16(a1, row_of(2, 240)[k * 3u + 1u]);
        TEST_ASSERT_EQUAL_HEX16(b1, row_of(2, 240)[k * 3u + 2u]);
    }
    for (x = 0; x < 240u; x++) {
        TEST_ASSERT_EQUAL_HEX16(row_of(0, 240)[x], row_of(1, 240)[x]);
    }
    assert_canaries_intact(3u * 240u);
}

static void test_24_16_blend_horizontal_triple_is_a_blended_seam(void)
{
    unsigned x;

    /* One repeating pair per line, identical on every line: R 0 then R 16,
     * so each triple is (0, 8, 16) in the 5-bit red field. */
    set_identical_ramp(2u, 16u);
    reset_dst();
    TEST_ASSERT_EQUAL_INT(SCALER_OK,
        scaler_scale_block(SCALER_GEOM_24_16, SCALER_MODE_BLEND,
                           lines, lookahead, dst.block, scratch.row));

    for (x = 0; x + 2u < 240u; x += 3u) {
        TEST_ASSERT_EQUAL_HEX16(0x0000u, row_of(0, 240)[x]);
        TEST_ASSERT_EQUAL_HEX16(0x4000u, row_of(0, 240)[x + 1u]);
        TEST_ASSERT_EQUAL_HEX16(0x8000u, row_of(0, 240)[x + 2u]);
    }
    assert_canaries_intact(3u * 240u);
}

static void test_24_16_blend_middle_row_is_the_average_of_the_pure_rows(void)
{
    static const uint16_t values[2] = { 0x0000u, 0x8000u }; /* R 0, R 16 */
    unsigned x;

    set_uniform_lines(values, 2u, 0xF800u);
    reset_dst();
    TEST_ASSERT_EQUAL_INT(SCALER_OK,
        scaler_scale_block(SCALER_GEOM_24_16, SCALER_MODE_BLEND,
                           lines, lookahead, dst.block, scratch.row));

    for (x = 0; x < 240u; x++) {
        TEST_ASSERT_EQUAL_HEX16(0x0000u, row_of(0, 240)[x]);
        TEST_ASSERT_EQUAL_HEX16(0x4000u, row_of(1, 240)[x]); /* R 8 */
        TEST_ASSERT_EQUAL_HEX16(0x8000u, row_of(2, 240)[x]);
        TEST_ASSERT_EQUAL_HEX16(
            scaler_avg565(row_of(0, 240)[x], row_of(2, 240)[x]),
            row_of(1, 240)[x]);
    }
    assert_canaries_intact(3u * 240u);
}

/* ── 26/16 ───────────────────────────────────────────────────────────── */

static void test_26_16_vertical_rows_match_the_named_averages(void)
{
    /* Uniform lines at R = 0, 3, 6, 9, 12, 15, 18, 21 and lookahead R = 24,
     * so every output row is uniform and equals one spec entry. Averages
     * floor: (3,6)->4, (9,12)->10, (12,15)->13, (18,21)->19, (21,24)->22. */
    static const uint16_t values[8] = {
        0x0000u, 0x1800u, 0x3000u, 0x4800u,
        0x6000u, 0x7800u, 0x9000u, 0xA800u,
    };
    static const uint16_t expect[13] = {
        0x0000u,            /* (0, pure)  R 0  */
        0x1800u, 0x2000u,   /* (1, pure)  R 3  | (1, blend 2)  R 4  */
        0x3000u,            /* (2, pure)  R 6  */
        0x4800u, 0x5000u,   /* (3, pure)  R 9  | (3, blend 4)  R 10 */
        0x6000u, 0x6800u,   /* (4, pure)  R 12 | (4, blend 5)  R 13 */
        0x7800u,            /* (5, pure)  R 15 */
        0x9000u, 0x9800u,   /* (6, pure)  R 18 | (6, blend 7)  R 19 */
        0xA800u, 0xB000u,   /* (7, pure)  R 21 | (7, blend LA) R 22 */
    };
    unsigned r;
    unsigned x;

    set_uniform_lines(values, 8u, 0xC000u); /* lookahead R 24 */
    reset_dst();
    TEST_ASSERT_EQUAL_INT(SCALER_OK,
        scaler_scale_block(SCALER_GEOM_26_16, SCALER_MODE_BLEND,
                           lines, lookahead, dst.block, scratch.row));

    for (r = 0; r < 13u; r++) {
        for (x = 0; x < 260u; x++) {
            TEST_ASSERT_EQUAL_HEX16(expect[r], row_of(r, 260)[x]);
        }
    }
    assert_canaries_intact(13u * 260u);
}

static void test_26_16_horizontal_pattern_and_right_edge_clamp(void)
{
    /* Identical lines with an 8-pixel ramp R = 0,4,...,28 so the block's
     * final blend reaches the next block's first pixel (R 0) and averages
     * to R 14 — except in the last block, where it clamps to pixel 159. */
    static const uint16_t expect[13] = {
        0x0000u,            /* (0, pure)      R 0  */
        0x2000u, 0x3000u,   /* (1, pure) R 4  | (1, blend 2)  R 6  */
        0x4000u,            /* (2, pure)      R 8  */
        0x6000u, 0x7000u,   /* (3, pure) R 12 | (3, blend 4)  R 14 */
        0x8000u, 0x9000u,   /* (4, pure) R 16 | (4, blend 5)  R 18 */
        0xA000u,            /* (5, pure)      R 20 */
        0xC000u, 0xD000u,   /* (6, pure) R 24 | (6, blend 7)  R 26 */
        0xE000u, 0x7000u,   /* (7, pure) R 28 | (7, blend next block) R 14 */
    };
    unsigned r;
    unsigned b;
    unsigned u;

    set_identical_ramp(8u, 4u);
    reset_dst();
    TEST_ASSERT_EQUAL_INT(SCALER_OK,
        scaler_scale_block(SCALER_GEOM_26_16, SCALER_MODE_BLEND,
                           lines, lookahead, dst.block, scratch.row));

    /* Every row carries the same horizontal pattern, because vertical
     * blending of identical lines is the identity. */
    for (r = 0; r < 13u; r++) {
        for (b = 0; b < 20u; b++) {
            for (u = 0; u < 13u; u++) {
                uint16_t want = expect[u];
                if (b == 19u && u == 12u) {
                    want = 0xE000u; /* clamped to pixel 159: stays pure */
                }
                TEST_ASSERT_EQUAL_HEX16(want, row_of(r, 260)[b * 13u + u]);
            }
        }
    }
    assert_canaries_intact(13u * 260u);
}

/* Sweep every output pixel of a block against the unit table. */
static uint16_t expect_h(const uint16_t* line, unsigned base,
                         const unit_spec_t* u, int blend)
{
    unsigned s = base + (unsigned)u->src;
    unsigned p;
    if (!blend || u->partner < 0) {
        return line[s];
    }
    p = base + (unsigned)u->partner;
    if (p >= SCALER_SRC_W) {
        p = SCALER_SRC_W - 1u;
    }
    return scaler_avg565(line[s], line[p]);
}

static void assert_block_matches_spec(enum scaler_geom_e geom,
                                      enum scaler_mode_e mode,
                                      const unit_spec_t* spec,
                                      const uint16_t* lookahead_line)
{
    const scaler_geom_info_t* gi = scaler_geom_info(geom);
    unsigned units = gi->dst_rows_per_block;
    unsigned span = gi->src_lines_per_block;
    int blend = (mode == SCALER_MODE_BLEND);
    unsigned r;

    reset_dst();
    TEST_ASSERT_EQUAL_INT(SCALER_OK,
        scaler_scale_block(geom, mode, lines, lookahead_line,
                           dst.block, scratch.row));

    for (r = 0; r < units; r++) {
        const uint16_t* top = lines[spec[r].src];
        const uint16_t* bot = top;
        unsigned base;
        unsigned o = 0;

        if (blend && spec[r].partner >= 0) {
            if ((unsigned)spec[r].partner < span) {
                bot = lines[spec[r].partner];
            } else if (lookahead_line != NULL) {
                bot = lookahead_line;
            }
        }
        for (base = 0; base < SCALER_SRC_W; base += span) {
            unsigned u;
            for (u = 0; u < units; u++) {
                uint16_t a = expect_h(top, base, &spec[u], blend);
                uint16_t b = expect_h(bot, base, &spec[u], blend);
                TEST_ASSERT_EQUAL_HEX16(scaler_avg565(a, b),
                                        row_of(r, gi->dst_w)[o]);
                o++;
            }
        }
        TEST_ASSERT_EQUAL_UINT(gi->dst_w, o);
    }
    assert_canaries_intact((unsigned)units * gi->dst_w);
}

static void test_24_16_matches_the_spec_in_both_modes(void)
{
    assert_block_matches_spec(SCALER_GEOM_24_16, SCALER_MODE_NEAREST,
                              spec_24_16, lookahead);
    assert_block_matches_spec(SCALER_GEOM_24_16, SCALER_MODE_BLEND,
                              spec_24_16, lookahead);
}

static void test_26_16_matches_the_spec_in_both_modes(void)
{
    assert_block_matches_spec(SCALER_GEOM_26_16, SCALER_MODE_NEAREST,
                              spec_26_16, lookahead);
    assert_block_matches_spec(SCALER_GEOM_26_16, SCALER_MODE_BLEND,
                              spec_26_16, lookahead);
}

static void test_null_lookahead_clamps_the_trailing_blend_rows(void)
{
    unsigned x;

    /* Frame end: the block's final blend row has no next line, so it
     * clamps to the block's last line and stays pure. */
    assert_block_matches_spec(SCALER_GEOM_26_16, SCALER_MODE_BLEND,
                              spec_26_16, NULL);
    for (x = 0; x < 260u; x++) {
        TEST_ASSERT_EQUAL_HEX16(row_of(11, 260)[x], row_of(12, 260)[x]);
    }
    /* 24/16 blends only within the block, so the lookahead never matters. */
    assert_block_matches_spec(SCALER_GEOM_24_16, SCALER_MODE_BLEND,
                              spec_24_16, NULL);
}

static void test_26_16_lookahead_row_uses_the_next_block(void)
{
    unsigned x;

    reset_dst();
    TEST_ASSERT_EQUAL_INT(SCALER_OK,
        scaler_scale_block(SCALER_GEOM_26_16, SCALER_MODE_BLEND,
                           lines, lookahead, dst.block, scratch.row));

    /* With a real lookahead the final row differs from the pure row above
     * it, and equals the average of that row and the scaled lookahead. */
    TEST_ASSERT_FALSE(row_of(11, 260)[0] == row_of(12, 260)[0]);
    for (x = 0; x < 260u; x++) {
        TEST_ASSERT_EQUAL_HEX16(
            scaler_avg565(row_of(11, 260)[x], scratch.row[x]),
            row_of(12, 260)[x]);
    }
    assert_canaries_intact(13u * 260u);
}

/* ── argument checking ───────────────────────────────────────────────── */

static void test_null_and_unknown_arguments_are_rejected(void)
{
    const uint16_t* holed[SCALER_SRC_LINES_MAX];
    unsigned i;

    for (i = 0; i < SCALER_SRC_LINES_MAX; i++) {
        holed[i] = lines[i];
    }
    holed[1] = NULL;

    TEST_ASSERT_EQUAL_INT(SCALER_ERR_ARGS,
        scaler_scale_block(SCALER_GEOM_24_16, SCALER_MODE_BLEND,
                           NULL, lookahead, dst.block, scratch.row));
    TEST_ASSERT_EQUAL_INT(SCALER_ERR_ARGS,
        scaler_scale_block(SCALER_GEOM_24_16, SCALER_MODE_BLEND,
                           holed, lookahead, dst.block, scratch.row));
    TEST_ASSERT_EQUAL_INT(SCALER_ERR_ARGS,
        scaler_scale_block(SCALER_GEOM_24_16, SCALER_MODE_BLEND,
                           lines, lookahead, NULL, scratch.row));
    TEST_ASSERT_EQUAL_INT(SCALER_ERR_ARGS,
        scaler_scale_block(SCALER_GEOM_26_16, SCALER_MODE_BLEND,
                           lines, lookahead, dst.block, NULL));
    TEST_ASSERT_EQUAL_INT(SCALER_ERR_ARGS,
        scaler_scale_block((enum scaler_geom_e)7, SCALER_MODE_BLEND,
                           lines, lookahead, dst.block, scratch.row));
    TEST_ASSERT_EQUAL_INT(SCALER_ERR_ARGS,
        scaler_scale_block(SCALER_GEOM_24_16, (enum scaler_mode_e)5,
                           lines, lookahead, dst.block, scratch.row));

    /* A rejected call writes nothing. */
    assert_canaries_intact(0u);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_avg565_is_the_identity_for_equal_inputs);
    RUN_TEST(test_avg565_is_symmetric_and_stays_within_channel_ranges);
    RUN_TEST(test_geom_info_reports_both_geometries);
    RUN_TEST(test_geom_info_rejects_an_unknown_geometry);
    RUN_TEST(test_24_16_nearest_keeps_the_placeholder_semantics);
    RUN_TEST(test_24_16_blend_horizontal_triple_is_a_blended_seam);
    RUN_TEST(test_24_16_blend_middle_row_is_the_average_of_the_pure_rows);
    RUN_TEST(test_26_16_vertical_rows_match_the_named_averages);
    RUN_TEST(test_26_16_horizontal_pattern_and_right_edge_clamp);
    RUN_TEST(test_24_16_matches_the_spec_in_both_modes);
    RUN_TEST(test_26_16_matches_the_spec_in_both_modes);
    RUN_TEST(test_null_lookahead_clamps_the_trailing_blend_rows);
    RUN_TEST(test_26_16_lookahead_row_uses_the_next_block);
    RUN_TEST(test_null_and_unknown_arguments_are_rejected);
    return UNITY_END();
}
