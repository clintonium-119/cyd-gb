#include <unity.h>

#include <stdio.h>

#include "render/scaler.h"

/*
 * Scaler suite. The blend properties asserted here are the ones the design
 * names verbatim for this workstream: avg565(a, a) == a, symmetry, and a
 * result whose per-channel value never leaves the two inputs' range.
 *
 * The geometry cases come in two flavours deliberately. The "literal" tests
 * name hand-computed RGB565 constants at the places the arithmetic is easiest
 * to get wrong — the horizontal tail, the cross-block lookahead row, and a
 * source that alternates every pixel on both axes — so a misunderstanding of
 * the duplication rhythm fails loudly. The "spec" tests then sweep every
 * output pixel of a block against the unit table restated below, which
 * catches indexing and overrun bugs the literal tests cannot reach.
 */

#define CANARY 0xC5C5u
#define GUARD 4

/* One output unit: the source unit copied, and the unit averaged with it
 * (-1 = pure). Restated from the design's 2,1,2 duplication rhythm, and
 * applied on both axes. */
typedef struct {
    int src;
    int partner;
} unit_spec_t;

/* 5/3: three source units to five output units, blending on half-steps.
 * Unit 4's partner is unit 3 — the next group's first, so horizontally that
 * is the next three pixels' first and vertically the next block's first
 * line. */
static const unit_spec_t spec_5_3[5] = {
    { 0, -1 }, { 0, 1 }, { 1, -1 }, { 2, -1 }, { 2, 3 },
};

/* Destination block and scratch row wrapped in canary guards. The block is
 * the geometry table's own maxima, dst_rows_per_block by dst_w, which at one
 * geometry is a block exactly — so a row that overruns its width lands in
 * the trailing canaries rather than in the next row, and the last row's
 * overrun lands in dst.post. */
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

static void test_geom_info_reports_the_geometry(void)
{
    const scaler_geom_info_t* g = scaler_geom_info(SCALER_GEOM_5_3);
    unsigned i;

    TEST_ASSERT_NOT_NULL(g);
    TEST_ASSERT_EQUAL_UINT(3, g->src_lines_per_block);
    TEST_ASSERT_EQUAL_UINT(5, g->dst_rows_per_block);
    TEST_ASSERT_EQUAL_UINT(266, g->dst_w);
    TEST_ASSERT_EQUAL_UINT(1, g->uses_lookahead);

    /* The table's maxima are what a caller sizes its buffers from, so no row
     * of it may exceed them. */
    for (i = 0; i < 1u; i++) {
        g = scaler_geom_info((enum scaler_geom_e)i);
        TEST_ASSERT_NOT_NULL(g);
        TEST_ASSERT_TRUE(g->dst_w <= SCALER_DST_W_MAX);
        TEST_ASSERT_TRUE(g->dst_rows_per_block <= SCALER_DST_ROWS_MAX);
        TEST_ASSERT_TRUE(g->src_lines_per_block <= SCALER_SRC_LINES_MAX);
    }
}

/*
 * dst_w is whole groups plus the leftover source pixels, each emitted once.
 * This is what lets the tail length stay derived from the table rather than
 * stored beside it: no field can disagree with the arithmetic.
 */
static void test_every_geometry_dst_w_matches_its_group_arithmetic(void)
{
    unsigned i;

    for (i = 0; i < 1u; i++) {
        const scaler_geom_info_t* g = scaler_geom_info((enum scaler_geom_e)i);
        unsigned span;
        TEST_ASSERT_NOT_NULL(g);
        span = g->src_lines_per_block;
        TEST_ASSERT_EQUAL_UINT((SCALER_SRC_W / span) * g->dst_rows_per_block +
                                   SCALER_SRC_W % span,
                               g->dst_w);
    }
}

static void test_geom_info_rejects_an_unknown_geometry(void)
{
    TEST_ASSERT_NULL(scaler_geom_info((enum scaler_geom_e)1));
    TEST_ASSERT_NULL(scaler_geom_info((enum scaler_geom_e)99));
}

/* Sweep every output pixel of a block against the unit table. */
static uint16_t expect_along(const uint16_t* line, unsigned base,
                            const unit_spec_t* u, int blend, unsigned src_len)
{
    unsigned s = base + (unsigned)u->src;
    unsigned p;
    if (!blend || u->partner < 0) {
        return line[s];
    }
    p = base + (unsigned)u->partner;
    if (p >= src_len) {
        p = src_len - 1u;
    }
    return scaler_avg565(line[s], line[p]);
}

static uint16_t expect_h(const uint16_t* line, unsigned base,
                         const unit_spec_t* u, int blend)
{
    return expect_along(line, base, u, blend, SCALER_SRC_W);
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
        for (base = 0; base + span <= SCALER_SRC_W; base += span) {
            unsigned u;
            for (u = 0; u < units; u++) {
                uint16_t a = expect_h(top, base, &spec[u], blend);
                uint16_t b = expect_h(bot, base, &spec[u], blend);
                TEST_ASSERT_EQUAL_HEX16(scaler_avg565(a, b),
                                        row_of(r, gi->dst_w)[o]);
                o++;
            }
        }
        /* Leftover source pixels emit themselves once, so the only blend
         * they can carry is the vertical one. Zero-length unless the group
         * fails to divide 160. */
        for (; base < SCALER_SRC_W; base++) {
            TEST_ASSERT_EQUAL_HEX16(scaler_avg565(top[base], bot[base]),
                                    row_of(r, gi->dst_w)[o]);
            o++;
        }
        /* Both walks end on the table's dst_w, which is what catches a tail
         * implemented on only one side. */
        TEST_ASSERT_EQUAL_UINT(gi->dst_w, o);
    }
    assert_canaries_intact((unsigned)units * gi->dst_w);
}

/*
 * The same sweep with the axes swapped: source COLUMNS in, output columns out,
 * against the same unit table, because a geometry applies identically on both
 * axes. What differs is the length walked — 144, which every group divides, so
 * there is no tail here — and the stride of an output unit, which is dst_h.
 * The canaries are the point of doing this at block level: a column written at
 * the wrong stride lands outside its footprint and says so.
 */
static void assert_col_block_matches_spec(enum scaler_geom_e geom,
                                          enum scaler_mode_e mode,
                                          const unit_spec_t* spec,
                                          const uint16_t* lookahead_col)
{
    const scaler_geom_info_t* gi = scaler_geom_info(geom);
    unsigned units = gi->dst_rows_per_block;
    unsigned span = gi->src_lines_per_block;
    unsigned dst_h = SCALER_SRC_H / span * units;
    int blend = (mode == SCALER_MODE_BLEND);
    unsigned c;

    reset_dst();
    TEST_ASSERT_EQUAL_INT(SCALER_OK,
        scaler_scale_col_block(geom, mode, lines, lookahead_col,
                               dst.block, scratch.row,
                               SCALER_COLS_ASCENDING));

    for (c = 0; c < units; c++) {
        const uint16_t* left = lines[spec[c].src];
        const uint16_t* right = left;
        unsigned base;
        unsigned o = 0;

        if (blend && spec[c].partner >= 0) {
            if ((unsigned)spec[c].partner < span) {
                right = lines[spec[c].partner];
            } else if (lookahead_col != NULL) {
                right = lookahead_col;
            }
        }
        for (base = 0; base + span <= SCALER_SRC_H; base += span) {
            unsigned u;
            for (u = 0; u < units; u++) {
                uint16_t a = expect_along(left, base, &spec[u], blend,
                                          SCALER_SRC_H);
                uint16_t b = expect_along(right, base, &spec[u], blend,
                                          SCALER_SRC_H);
                TEST_ASSERT_EQUAL_HEX16(scaler_avg565(a, b),
                                        dst.block[(size_t)c * dst_h + o]);
                o++;
            }
        }
        /* Three divides 144, so the walk ends on dst_h exactly: a tail
         * emitted on this axis would overrun the column. */
        TEST_ASSERT_EQUAL_UINT(dst_h, o);
    }
    assert_canaries_intact(units * dst_h);
}

static void test_col_block_matches_the_spec_in_both_modes(void)
{
    assert_col_block_matches_spec(SCALER_GEOM_5_3, SCALER_MODE_NEAREST,
                                  spec_5_3, lookahead);
    assert_col_block_matches_spec(SCALER_GEOM_5_3, SCALER_MODE_BLEND,
                                  spec_5_3, lookahead);
}

/* And with no next column: the frame's right edge, where the trailing blend
 * columns clamp to the column they already hold. */
static void test_col_block_null_lookahead_clamps_the_trailing_columns(void)
{
    assert_col_block_matches_spec(SCALER_GEOM_5_3, SCALER_MODE_BLEND,
                                  spec_5_3, NULL);
}

/*
 * The tail moved axis with the walk: it is a whole output column that belongs
 * to no block, rather than the row walk's one pixel per row.
 */
static void test_col_tail_is_one_pure_column(void)
{
    unsigned base;
    unsigned o = 0;

    reset_dst();
    TEST_ASSERT_EQUAL_INT(SCALER_OK,
        scaler_scale_col_tail(SCALER_GEOM_5_3, SCALER_MODE_BLEND,
                              lines, dst.block));
    /* Scaled along its length and never across it — a tail column has no next
     * group to reach toward, which is what keeps the frame's far edge pure. */
    for (base = 0; base + 3u <= SCALER_SRC_H; base += 3u) {
        unsigned u;
        for (u = 0; u < 5u; u++) {
            TEST_ASSERT_EQUAL_HEX16(
                expect_along(lines[0], base, &spec_5_3[u], 1, SCALER_SRC_H),
                dst.block[o]);
            o++;
        }
    }
    TEST_ASSERT_EQUAL_UINT(240u, o);
    assert_canaries_intact(240u);
}

/* ── 5/3 ─────────────────────────────────────────────────────────────── */

static void test_5_3_matches_the_spec_in_both_modes(void)
{
    assert_block_matches_spec(SCALER_GEOM_5_3, SCALER_MODE_NEAREST,
                              spec_5_3, lookahead);
    assert_block_matches_spec(SCALER_GEOM_5_3, SCALER_MODE_BLEND,
                              spec_5_3, lookahead);
    /* Frame end: the block's final blend row has no next line. */
    assert_block_matches_spec(SCALER_GEOM_5_3, SCALER_MODE_BLEND,
                              spec_5_3, NULL);
}

static void test_5_3_horizontal_tail_is_the_last_source_pixel_pure(void)
{
    /* 53 groups of 3 cover pixels 0..158; pixel 159 is the tail. Rows 0, 2
     * and 3 are the pure ones, each carrying its own source line's tail
     * untouched. setUp's lines differ at 158 and 159, so a blend that
     * clamped to the group instead of ending would be visible. */
    TEST_ASSERT_TRUE(src[0][158] != src[0][159]);

    reset_dst();
    TEST_ASSERT_EQUAL_INT(SCALER_OK,
        scaler_scale_block(SCALER_GEOM_5_3, SCALER_MODE_BLEND,
                           lines, lookahead, dst.block, scratch.row));

    TEST_ASSERT_EQUAL_HEX16(src[0][159], row_of(0, 266)[265]);
    TEST_ASSERT_EQUAL_HEX16(src[1][159], row_of(2, 266)[265]);
    TEST_ASSERT_EQUAL_HEX16(src[2][159], row_of(3, 266)[265]);
    /* The blend rows carry the vertical average of those same pure tails —
     * never a horizontal one. */
    TEST_ASSERT_EQUAL_HEX16(scaler_avg565(src[0][159], src[1][159]),
                            row_of(1, 266)[265]);
    TEST_ASSERT_EQUAL_HEX16(scaler_avg565(src[2][159], lookahead[159]),
                            row_of(4, 266)[265]);
    assert_canaries_intact(5u * 266u);
}

static void test_5_3_lookahead_row_uses_the_next_block(void)
{
    unsigned x;

    reset_dst();
    TEST_ASSERT_EQUAL_INT(SCALER_OK,
        scaler_scale_block(SCALER_GEOM_5_3, SCALER_MODE_BLEND,
                           lines, lookahead, dst.block, scratch.row));

    /* With a real lookahead the final row is the average of the pure row
     * above it and the scaled lookahead. */
    TEST_ASSERT_FALSE(row_of(3, 266)[0] == row_of(4, 266)[0]);
    for (x = 0; x < 266u; x++) {
        TEST_ASSERT_EQUAL_HEX16(
            scaler_avg565(row_of(3, 266)[x], scratch.row[x]),
            row_of(4, 266)[x]);
    }
    assert_canaries_intact(5u * 266u);

    /* Without one it clamps to the row above and stays pure. */
    reset_dst();
    TEST_ASSERT_EQUAL_INT(SCALER_OK,
        scaler_scale_block(SCALER_GEOM_5_3, SCALER_MODE_BLEND,
                           lines, NULL, dst.block, scratch.row));
    for (x = 0; x < 266u; x++) {
        TEST_ASSERT_EQUAL_HEX16(row_of(3, 266)[x], row_of(4, 266)[x]);
    }
    assert_canaries_intact(5u * 266u);
}

/* ── the fixed kernel against the pattern spec ───────────────────────── */
/*
 * BLEND is served by an unrolled kernel rather than the pattern walk, and
 * its contract is that nobody can tell: the spec sweep above is the pattern
 * walk restated, so the cases below run the kernel over inputs the literal
 * tests never reach — random lines, every mask bit toggling, and both
 * lookahead states.
 */

static uint32_t rng_state;

static uint16_t rng16(void)
{
    rng_state = rng_state * 1103515245u + 12345u;
    return (uint16_t)(rng_state >> 13);
}

static void set_random_lines(void)
{
    unsigned l;
    unsigned i;
    for (l = 0; l < SCALER_SRC_LINES_MAX; l++) {
        for (i = 0; i < SCALER_SRC_W; i++) {
            src[l][i] = rng16();
        }
    }
    for (i = 0; i < SCALER_SRC_W; i++) {
        lookahead[i] = rng16();
    }
}

static void test_5_3_kernel_matches_the_spec_on_random_lines(void)
{
    unsigned n;

    rng_state = 0x0503BEEFu;
    for (n = 0; n < 64u; n++) {
        set_random_lines();
        assert_block_matches_spec(SCALER_GEOM_5_3, SCALER_MODE_BLEND,
                                  spec_5_3, (n & 1u) ? lookahead : NULL);
    }
}

static void test_5_3_kernel_keeps_the_tail_pure(void)
{
    unsigned n;

    /* The kernel's own pixel 265, not just the generic path's: the last
     * whole group ends at source pixel 158, and 159 emits itself. */
    rng_state = 0x0159026Au;
    for (n = 0; n < 16u; n++) {
        set_random_lines();
        reset_dst();
        TEST_ASSERT_EQUAL_INT(SCALER_OK,
            scaler_scale_block(SCALER_GEOM_5_3, SCALER_MODE_BLEND,
                               lines, lookahead, dst.block, scratch.row));
        TEST_ASSERT_EQUAL_HEX16(src[0][159], row_of(0, 266)[265]);
        TEST_ASSERT_EQUAL_HEX16(src[1][159], row_of(2, 266)[265]);
        TEST_ASSERT_EQUAL_HEX16(src[2][159], row_of(3, 266)[265]);
        TEST_ASSERT_EQUAL_HEX16(scaler_avg565(src[0][159], src[1][159]),
                                row_of(1, 266)[265]);
        TEST_ASSERT_EQUAL_HEX16(scaler_avg565(src[2][159], lookahead[159]),
                                row_of(4, 266)[265]);
        assert_canaries_intact(5u * 266u);
    }
}

static void test_5_3_kernel_frame_end_row_is_pure(void)
{
    unsigned x;

    rng_state = 0x05030E0Du;
    set_random_lines();
    reset_dst();
    TEST_ASSERT_EQUAL_INT(SCALER_OK,
        scaler_scale_block(SCALER_GEOM_5_3, SCALER_MODE_BLEND,
                           lines, NULL, dst.block, scratch.row));

    for (x = 0; x < 266u; x++) {
        TEST_ASSERT_EQUAL_HEX16(row_of(3, 266)[x], row_of(4, 266)[x]);
    }
    assert_canaries_intact(5u * 266u);
}

static void test_5_3_kernel_survives_alternating_extremes(void)
{
    unsigned i;

    /* Every channel bit flips between neighbours on both axes, so a blend
     * that borrowed across a channel boundary would show here. */
    for (i = 0; i < SCALER_SRC_W; i++) {
        src[0][i] = (i & 1u) ? 0xFFFFu : 0x0000u;
        src[1][i] = (i & 1u) ? 0x0000u : 0xFFFFu;
        src[2][i] = (i & 1u) ? 0xFFFFu : 0x0000u;
        lookahead[i] = 0xA5A5u;
    }
    assert_block_matches_spec(SCALER_GEOM_5_3, SCALER_MODE_BLEND,
                              spec_5_3, lookahead);
    /* Group 0 is sources (0, 1, 2) = (black, white, black), so row 0 reads
     * black, grey, white, black, grey — the last being avg(s2, s3) with s3
     * white again. */
    TEST_ASSERT_EQUAL_HEX16(0x0000u, row_of(0, 266)[0]);
    TEST_ASSERT_EQUAL_HEX16(0x7BEFu, row_of(0, 266)[1]);
    TEST_ASSERT_EQUAL_HEX16(0xFFFFu, row_of(0, 266)[2]);
    TEST_ASSERT_EQUAL_HEX16(0x0000u, row_of(0, 266)[3]);
    TEST_ASSERT_EQUAL_HEX16(0x7BEFu, row_of(0, 266)[4]);
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
        scaler_scale_block(SCALER_GEOM_5_3, SCALER_MODE_BLEND,
                           NULL, lookahead, dst.block, scratch.row));
    TEST_ASSERT_EQUAL_INT(SCALER_ERR_ARGS,
        scaler_scale_block(SCALER_GEOM_5_3, SCALER_MODE_BLEND,
                           holed, lookahead, dst.block, scratch.row));
    TEST_ASSERT_EQUAL_INT(SCALER_ERR_ARGS,
        scaler_scale_block(SCALER_GEOM_5_3, SCALER_MODE_BLEND,
                           lines, lookahead, NULL, scratch.row));
    TEST_ASSERT_EQUAL_INT(SCALER_ERR_ARGS,
        scaler_scale_block(SCALER_GEOM_5_3, SCALER_MODE_BLEND,
                           lines, lookahead, dst.block, NULL));
    TEST_ASSERT_EQUAL_INT(SCALER_ERR_ARGS,
        scaler_scale_block((enum scaler_geom_e)7, SCALER_MODE_BLEND,
                           lines, lookahead, dst.block, scratch.row));
    TEST_ASSERT_EQUAL_INT(SCALER_ERR_ARGS,
        scaler_scale_block(SCALER_GEOM_5_3, (enum scaler_mode_e)5,
                           lines, lookahead, dst.block, scratch.row));

    /* A rejected call writes nothing. */
    assert_canaries_intact(0u);
}

/*
 * Descending is ascending with the block's columns reversed, and nothing else
 * about the pixels changes. That is the whole contract: the panel's window
 * fills one way and the image runs the other, so the columns swap ends while
 * every column's own contents stay put.
 */
static void assert_col_order_reverses_the_block(enum scaler_geom_e geom,
                                                enum scaler_mode_e mode,
                                                const uint16_t* lookahead_col)
{
    const scaler_geom_info_t* gi = scaler_geom_info(geom);
    unsigned units = gi->dst_rows_per_block;
    unsigned dst_h = SCALER_SRC_H / gi->src_lines_per_block * units;
    static uint16_t up[SCALER_DST_ROWS_MAX * SCALER_DST_H_MAX];
    unsigned c;
    unsigned y;

    reset_dst();
    TEST_ASSERT_EQUAL_INT(SCALER_OK,
        scaler_scale_col_block(geom, mode, lines, lookahead_col,
                               dst.block, scratch.row,
                               SCALER_COLS_ASCENDING));
    for (c = 0; c < units * dst_h; c++) {
        up[c] = dst.block[c];
    }

    reset_dst();
    TEST_ASSERT_EQUAL_INT(SCALER_OK,
        scaler_scale_col_block(geom, mode, lines, lookahead_col,
                               dst.block, scratch.row,
                               SCALER_COLS_DESCENDING));
    for (c = 0; c < units; c++) {
        const uint16_t* asc = up + (size_t)c * dst_h;
        const uint16_t* desc = dst.block + (size_t)(units - 1u - c) * dst_h;

        for (y = 0; y < dst_h; y++) {
            TEST_ASSERT_EQUAL_HEX16_MESSAGE(asc[y], desc[y],
                "descending is not the ascending block reversed");
        }
    }
    /* And it stays inside the same footprint: a sign error on the stride
     * writes before the buffer, which the leading canaries catch. */
    assert_canaries_intact(units * dst_h);
}

static void test_col_order_reverses_the_block_in_both_modes(void)
{
    assert_col_order_reverses_the_block(SCALER_GEOM_5_3,
                                        SCALER_MODE_BLEND, lookahead);
    assert_col_order_reverses_the_block(SCALER_GEOM_5_3,
                                        SCALER_MODE_BLEND, NULL);
    assert_col_order_reverses_the_block(SCALER_GEOM_5_3,
                                        SCALER_MODE_NEAREST, lookahead);
}

/*
 * A column split into ranges is the same pixels as the whole column. This is
 * what makes 2D tiling possible at all: if a range's trailing blend clamped
 * at the range end instead of reading into the next one, every tile boundary
 * would be a permanent seam in the picture — worse than the moving one the
 * tiling exists to break up.
 */
static void assert_col_ranges_join_seamlessly(enum scaler_geom_e geom,
                                              enum scaler_mode_e mode,
                                              unsigned parts)
{
    const scaler_geom_info_t* gi = scaler_geom_info(geom);
    unsigned units = gi->dst_rows_per_block;
    unsigned span = gi->src_lines_per_block;
    unsigned dst_h = SCALER_SRC_H / span * units;
    unsigned rows = SCALER_SRC_H / parts;
    unsigned slice_h = rows / span * units;
    static uint16_t whole[SCALER_DST_ROWS_MAX * SCALER_DST_H_MAX];
    unsigned c;
    unsigned y;
    unsigned k;

    TEST_ASSERT_EQUAL_UINT_MESSAGE(0, rows % span,
        "the range must be whole groups or the phases cannot match");

    reset_dst();
    TEST_ASSERT_EQUAL_INT(SCALER_OK,
        scaler_scale_col_block(geom, mode, lines, lookahead,
                               dst.block, scratch.row,
                               SCALER_COLS_ASCENDING));
    for (c = 0; c < units * dst_h; c++) {
        whole[c] = dst.block[c];
    }

    for (k = 0; k < parts; k++) {
        reset_dst();
        TEST_ASSERT_EQUAL_INT(SCALER_OK,
            scaler_scale_col_rows(geom, mode, lines, lookahead,
                                  dst.block, scratch.row,
                                  SCALER_COLS_ASCENDING,
                                  k * rows, rows));
        for (c = 0; c < units; c++) {
            const uint16_t* got = dst.block + (size_t)c * slice_h;
            const uint16_t* want = whole + (size_t)c * dst_h + k * slice_h;

            for (y = 0; y < slice_h; y++) {
                char msg[80];
                sprintf(msg, "range %u of %u, column %u, row %u",
                        k, parts, c, y);
                TEST_ASSERT_EQUAL_HEX16_MESSAGE(want[y], got[y], msg);
            }
        }
        assert_canaries_intact(units * slice_h);
    }
}

static void test_col_ranges_join_seamlessly(void)
{
    /* 144 splits by 2, 3, 4 and 6, and the group has to divide the range, so
     * three permits every one of them. */
    assert_col_ranges_join_seamlessly(SCALER_GEOM_5_3,
                                      SCALER_MODE_BLEND, 2);
    assert_col_ranges_join_seamlessly(SCALER_GEOM_5_3,
                                      SCALER_MODE_BLEND, 3);
    assert_col_ranges_join_seamlessly(SCALER_GEOM_5_3,
                                      SCALER_MODE_BLEND, 4);
    assert_col_ranges_join_seamlessly(SCALER_GEOM_5_3,
                                      SCALER_MODE_BLEND, 6);
    assert_col_ranges_join_seamlessly(SCALER_GEOM_5_3,
                                      SCALER_MODE_NEAREST, 2);
}

/* A range that does not land on group boundaries has no correct phase, so it
 * is refused rather than quietly producing a shifted slice. */
static void test_col_ranges_reject_a_partial_group(void)
{
    TEST_ASSERT_EQUAL_INT(SCALER_ERR_ARGS,
        scaler_scale_col_rows(SCALER_GEOM_5_3, SCALER_MODE_BLEND,
                              lines, lookahead, dst.block, scratch.row,
                              SCALER_COLS_ASCENDING, 0, 70));
    TEST_ASSERT_EQUAL_INT(SCALER_ERR_ARGS,
        scaler_scale_col_rows(SCALER_GEOM_5_3, SCALER_MODE_BLEND,
                              lines, lookahead, dst.block, scratch.row,
                              SCALER_COLS_ASCENDING, 1, 72));
    TEST_ASSERT_EQUAL_INT(SCALER_ERR_ARGS,
        scaler_scale_col_rows(SCALER_GEOM_5_3, SCALER_MODE_BLEND,
                              lines, lookahead, dst.block, scratch.row,
                              SCALER_COLS_ASCENDING, 0, 0));
    TEST_ASSERT_EQUAL_INT(SCALER_ERR_ARGS,
        scaler_scale_col_rows(SCALER_GEOM_5_3, SCALER_MODE_BLEND,
                              lines, lookahead, dst.block, scratch.row,
                              SCALER_COLS_ASCENDING, 72, 144));
    assert_canaries_intact(0u);
}

static void test_col_null_and_unknown_arguments_are_rejected(void)
{
    const uint16_t* holed[SCALER_SRC_LINES_MAX];
    unsigned i;

    for (i = 0; i < SCALER_SRC_LINES_MAX; i++) {
        holed[i] = lines[i];
    }
    holed[1] = NULL;

    TEST_ASSERT_EQUAL_INT(SCALER_ERR_ARGS,
        scaler_scale_col_block(SCALER_GEOM_5_3, SCALER_MODE_BLEND,
                               NULL, lookahead, dst.block, scratch.row,
                               SCALER_COLS_ASCENDING));
    TEST_ASSERT_EQUAL_INT(SCALER_ERR_ARGS,
        scaler_scale_col_block(SCALER_GEOM_5_3, SCALER_MODE_BLEND,
                               holed, lookahead, dst.block, scratch.row,
                               SCALER_COLS_ASCENDING));
    TEST_ASSERT_EQUAL_INT(SCALER_ERR_ARGS,
        scaler_scale_col_block(SCALER_GEOM_5_3, SCALER_MODE_BLEND,
                               lines, lookahead, NULL, scratch.row,
                               SCALER_COLS_ASCENDING));
    TEST_ASSERT_EQUAL_INT(SCALER_ERR_ARGS,
        scaler_scale_col_block(SCALER_GEOM_5_3, SCALER_MODE_BLEND,
                               lines, lookahead, dst.block, NULL,
                               SCALER_COLS_ASCENDING));
    TEST_ASSERT_EQUAL_INT(SCALER_ERR_ARGS,
        scaler_scale_col_block((enum scaler_geom_e)7, SCALER_MODE_BLEND,
                               lines, lookahead, dst.block, scratch.row,
                               SCALER_COLS_ASCENDING));
    TEST_ASSERT_EQUAL_INT(SCALER_ERR_ARGS,
        scaler_scale_col_block(SCALER_GEOM_5_3, (enum scaler_mode_e)5,
                               lines, lookahead, dst.block, scratch.row,
                               SCALER_COLS_ASCENDING));

    TEST_ASSERT_EQUAL_INT(SCALER_ERR_ARGS,
        scaler_scale_col_block(SCALER_GEOM_5_3, SCALER_MODE_BLEND,
                               lines, lookahead, dst.block, scratch.row,
                               (enum scaler_col_order_e)9));

    /* The tail rejects the same way, over the one column it has to emit. */
    TEST_ASSERT_EQUAL_INT(SCALER_ERR_ARGS,
        scaler_scale_col_tail(SCALER_GEOM_5_3, SCALER_MODE_BLEND,
                              NULL, dst.block));
    TEST_ASSERT_EQUAL_INT(SCALER_ERR_ARGS,
        scaler_scale_col_tail(SCALER_GEOM_5_3, SCALER_MODE_BLEND,
                              lines, NULL));
    TEST_ASSERT_EQUAL_INT(SCALER_ERR_ARGS,
        scaler_scale_col_tail(SCALER_GEOM_5_3, SCALER_MODE_BLEND,
                              holed + 1, dst.block));
    TEST_ASSERT_EQUAL_INT(SCALER_ERR_ARGS,
        scaler_scale_col_tail((enum scaler_geom_e)7, SCALER_MODE_BLEND,
                              lines, dst.block));
    TEST_ASSERT_EQUAL_INT(SCALER_ERR_ARGS,
        scaler_scale_col_tail(SCALER_GEOM_5_3, (enum scaler_mode_e)5,
                              lines, dst.block));

    /* A rejected call writes nothing. */
    assert_canaries_intact(0u);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_avg565_is_the_identity_for_equal_inputs);
    RUN_TEST(test_avg565_is_symmetric_and_stays_within_channel_ranges);
    RUN_TEST(test_geom_info_reports_the_geometry);
    RUN_TEST(test_every_geometry_dst_w_matches_its_group_arithmetic);
    RUN_TEST(test_geom_info_rejects_an_unknown_geometry);
    RUN_TEST(test_5_3_matches_the_spec_in_both_modes);
    RUN_TEST(test_5_3_horizontal_tail_is_the_last_source_pixel_pure);
    RUN_TEST(test_5_3_lookahead_row_uses_the_next_block);
    RUN_TEST(test_5_3_kernel_matches_the_spec_on_random_lines);
    RUN_TEST(test_5_3_kernel_keeps_the_tail_pure);
    RUN_TEST(test_5_3_kernel_frame_end_row_is_pure);
    RUN_TEST(test_5_3_kernel_survives_alternating_extremes);
    RUN_TEST(test_null_and_unknown_arguments_are_rejected);
    RUN_TEST(test_col_block_matches_the_spec_in_both_modes);
    RUN_TEST(test_col_block_null_lookahead_clamps_the_trailing_columns);
    RUN_TEST(test_col_tail_is_one_pure_column);
    RUN_TEST(test_col_order_reverses_the_block_in_both_modes);
    RUN_TEST(test_col_ranges_join_seamlessly);
    RUN_TEST(test_col_ranges_reject_a_partial_group);
    RUN_TEST(test_col_null_and_unknown_arguments_are_rejected);
    return UNITY_END();
}
