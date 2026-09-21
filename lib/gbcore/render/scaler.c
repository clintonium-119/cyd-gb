#include "scaler.h"

#define GEOM_COUNT 3

/*
 * One output unit of a geometry's pattern. The pattern is indexed by the
 * output unit's position within a block and applied identically on both
 * axes: is_blend means "average source unit src_offset with src_offset + 1".
 */
typedef struct {
    uint8_t src_offset;
    uint8_t is_blend;
} scaler_pattern_t;

/* 24/16 (3/2): the design's regular 1,2 duplication rhythm. */
static const scaler_pattern_t pattern_24_16[3] = {
    { 0, 0 }, { 0, 1 }, { 1, 0 },
};

/*
 * 26/16 (13/8): the design's 1,2,1,2,2,1,2,2 duplication rhythm. Every
 * source unit emits itself, and where the rhythm says 2 the second copy is
 * the blend with the next source unit — 13 output units from 8 source units.
 */
static const scaler_pattern_t pattern_26_16[13] = {
    { 0, 0 },
    { 1, 0 }, { 1, 1 },
    { 2, 0 },
    { 3, 0 }, { 3, 1 },
    { 4, 0 }, { 4, 1 },
    { 5, 0 },
    { 6, 0 }, { 6, 1 },
    { 7, 0 }, { 7, 1 },
};

/*
 * 5/3: three source units to five output units. The ideal sample positions
 * are 0, 0.6, 1.2, 1.8 and 2.4; a 50/50 avg565 reaches half-steps, so the
 * pattern lands them on 0, 0.5, 1, 2 and 2.5 — a maximum phase error of
 * 0.2 px, with three of the five left pure.
 */
static const scaler_pattern_t pattern_5_3[5] = {
    { 0, 0 }, { 0, 1 }, { 1, 0 }, { 2, 0 }, { 2, 1 },
};

static const scaler_geom_info_t geom_table[GEOM_COUNT] = {
    { 2, 3, 240, 0 },  /* SCALER_GEOM_24_16: the blend partner is in-block */
    { 8, 13, 260, 1 }, /* SCALER_GEOM_26_16: unit 7 blends with unit 8     */
    { 3, 5, 266, 1 },  /* SCALER_GEOM_5_3:   unit 2 blends with unit 3     */
};

static const scaler_pattern_t* const pattern_table[GEOM_COUNT] = {
    pattern_24_16,
    pattern_26_16,
    pattern_5_3,
};

/*
 * Per-channel average without unpacking: bits the two pixels share pass
 * through (a & b), and each differing bit contributes half. The 0xF7DE mask
 * drops each channel's low bit before the shift so no channel borrows from
 * its neighbour. Forced inline because the target toolchain, at its
 * effective -Os, otherwise leaves it a call — -mlongcalls turns each use
 * into an l32r + callx8 with a register-window spill — and the kernel calls
 * it five times per source pair.
 */
static inline __attribute__((always_inline)) uint16_t avg565(uint16_t a,
                                                             uint16_t b)
{
    return (uint16_t)((((a ^ b) & 0xF7DEu) >> 1) + (a & b));
}

uint16_t scaler_avg565(uint16_t a, uint16_t b)
{
    return avg565(a, b);
}

const scaler_geom_info_t* scaler_geom_info(enum scaler_geom_e geom)
{
    if ((unsigned)geom >= (unsigned)GEOM_COUNT) {
        return NULL;
    }
    return &geom_table[(unsigned)geom];
}

/*
 * Every walk below takes the length of the axis it walks (src_len) and the
 * stride of one output unit (unit_len) rather than reading SCALER_SRC_W and
 * dst_w, because the two walks differ in exactly those two numbers: the row
 * walk passes SCALER_SRC_W and dst_w, the column walk SCALER_SRC_H and dst_h.
 * One copy of each kernel then serves both axes, which costs a register
 * compare where an immediate one stood and saves carrying a transposed
 * duplicate of the arithmetic in flash — and leaves it no second place to
 * drift. Neither walk's store count moves.
 */

/*
 * Scale one source line to one output row, or one source column to one output
 * column. Applies the pattern along the axis, so a blend may reach into the
 * next block; only the final block's last blend has no partner and clamps to
 * the last source. A group that does not divide src_len leaves a tail,
 * emitted pure after the last whole group.
 */
static void scale_line(const uint16_t* src, uint16_t* dst,
                       const scaler_pattern_t* pat,
                       unsigned src_units, unsigned dst_units, int blend,
                       unsigned src_len)
{
    unsigned base;
    unsigned o = 0;

    for (base = 0; base + src_units <= src_len; base += src_units) {
        unsigned i;
        for (i = 0; i < dst_units; i++) {
            unsigned s = base + pat[i].src_offset;
            if (blend && pat[i].is_blend) {
                unsigned p = s + 1;
                if (p >= src_len) {
                    p = src_len - 1; /* the far edge stays pure */
                }
                dst[o++] = avg565(src[s], src[p]);
            } else {
                dst[o++] = src[s];
            }
        }
    }

    /* A geometry whose group does not divide the axis leaves a tail. Each
     * leftover source pixel emits itself once: there is no next group to
     * blend toward, and the alternative is one more iteration of the group
     * loop, which emits dst_units pixels and overruns the output unit. Zero
     * length for both k/16 geometries on either axis, and for 5/3 on the 144
     * axis. */
    for (; base < src_len; base++) {
        dst[o++] = src[base];
    }
}

/*
 * One blend unit: the average of two units a kernel already scaled along the
 * axis, never of the four sources, because avg565 is not associative.
 */
static void blend_line(uint16_t* dst, const uint16_t* a, const uint16_t* b,
                       unsigned unit_len)
{
    unsigned x;

    for (x = 0; x < unit_len; x++) {
        dst[x] = avg565(a[x], b[x]);
    }
}

/*
 * Fixed 3/2 blend kernel — the shipped geometry, unrolled. One source pair
 * (a, b) becomes (a, avg(a, b), b), and two source lines become three rows:
 * the two scaled lines and, between them, their per-pixel average. These are
 * the pixels the pattern walk produces for 24/16 in BLEND mode, computed in
 * the same order: the middle row averages the two already scaled rows, never
 * the four sources, because avg565 is not associative. No partner clamp is
 * needed (both axes are even, so the partner of the last pair's first pixel
 * is the last pixel) and no lookahead (pattern_24_16's only blend partner is
 * in-block), so there is no frame-end case either. Transposed it is the same
 * kernel over a 144-pixel axis: two source columns to three output columns.
 * 80 pairs x 3 rows = 720 pixels per call, 72 calls a frame.
 *
 * ponytail: plain 16-bit stores; the toolchain does not merge them. Pack to
 * 32-bit stores (dst must then be 4-byte aligned, which the API does not
 * promise) only if the bench says the kernel is still short.
 */
static void scale_block_24_16_blend(const uint16_t* l0, const uint16_t* l1,
                                    uint16_t* dst, unsigned src_len,
                                    unsigned unit_len)
{
    uint16_t* r0 = dst;
    uint16_t* r1 = dst + unit_len;
    uint16_t* r2 = dst + 2u * unit_len;
    unsigned x;
    unsigned o = 0;

    for (x = 0; x < src_len; x += 2) {
        uint16_t a0 = l0[x];
        uint16_t b0 = l0[x + 1];
        uint16_t a1 = l1[x];
        uint16_t b1 = l1[x + 1];
        uint16_t m0 = avg565(a0, b0);
        uint16_t m1 = avg565(a1, b1);

        r0[o] = a0;
        r0[o + 1] = m0;
        r0[o + 2] = b0;
        r1[o] = avg565(a0, a1);
        r1[o + 1] = avg565(m0, m1);
        r1[o + 2] = avg565(b0, b1);
        r2[o] = a1;
        r2[o + 1] = m1;
        r2[o + 2] = b1;
        o += 3;
    }
}

/*
 * Fixed 13/8 blend kernel for 26/16 — the same deletion the 3/2 kernel made,
 * against an irregular rhythm four times the size. Nine live source pixels
 * across eight lines will not sit in the LX6's window, so this keeps the
 * generic path's two-pass shape (scale the 8 pure units along the axis, then
 * average pairs of already-scaled units) and unrolls only the along-axis
 * walk, which is where the per-pixel pattern load, is_blend branch and bounds
 * check lived. 20 groups of 8 source pixels become 13 output pixels each: 260
 * per row, 13 rows, 18 calls a frame. Transposed it is 18 groups to 234.
 *
 * ponytail: plain 16-bit stores, as in the 3/2 kernel; pack to 32-bit only
 * if the bench says this lands short.
 */
static void scale_line_26_16(const uint16_t* src, uint16_t* dst,
                             unsigned src_len)
{
    unsigned base;
    unsigned o = 0;

    for (base = 0; base < src_len; base += 8) {
        /* 8 divides both axes, so the last group's partner is always past the
         * end; it clamps to the final source pixel, the last output pixel is
         * avg(s, s) and the far edge stays pure. One compare per group, not
         * per pixel. */
        uint16_t s0 = src[base];
        uint16_t s1 = src[base + 1];
        uint16_t s2 = src[base + 2];
        uint16_t s3 = src[base + 3];
        uint16_t s4 = src[base + 4];
        uint16_t s5 = src[base + 5];
        uint16_t s6 = src[base + 6];
        uint16_t s7 = src[base + 7];
        uint16_t s8 = src[base + 8 < src_len ? base + 8 : src_len - 1];

        dst[o] = s0;
        dst[o + 1] = s1;
        dst[o + 2] = avg565(s1, s2);
        dst[o + 3] = s2;
        dst[o + 4] = s3;
        dst[o + 5] = avg565(s3, s4);
        dst[o + 6] = s4;
        dst[o + 7] = avg565(s4, s5);
        dst[o + 8] = s5;
        dst[o + 9] = s6;
        dst[o + 10] = avg565(s6, s7);
        dst[o + 11] = s7;
        dst[o + 12] = avg565(s7, s8);
        o += 13;
    }
}

/*
 * pattern_26_16 lands source unit i on output unit {0, 1, 3, 4, 6, 8, 9, 11}
 * and leaves units 2, 5, 7, 10 and 12 as blends across the walk. Unit 12's
 * partner is source unit 8 — the next block's first line or column, which
 * arrives as lookahead and is NULL at the frame's far edge; there the generic
 * path averages the unit with itself, so blending unit 11 with unit 11 is the
 * same pixels without a second code path.
 */
static void scale_block_26_16_blend(const uint16_t* const* src_lines,
                                    const uint16_t* lookahead_line,
                                    uint16_t* dst, uint16_t* scratch_row,
                                    unsigned src_len, unsigned unit_len)
{
    scale_line_26_16(src_lines[0], dst, src_len);
    scale_line_26_16(src_lines[1], dst + unit_len, src_len);
    scale_line_26_16(src_lines[2], dst + 3u * unit_len, src_len);
    scale_line_26_16(src_lines[3], dst + 4u * unit_len, src_len);
    scale_line_26_16(src_lines[4], dst + 6u * unit_len, src_len);
    scale_line_26_16(src_lines[5], dst + 8u * unit_len, src_len);
    scale_line_26_16(src_lines[6], dst + 9u * unit_len, src_len);
    scale_line_26_16(src_lines[7], dst + 11u * unit_len, src_len);

    blend_line(dst + 2u * unit_len, dst + unit_len, dst + 3u * unit_len,
               unit_len);
    blend_line(dst + 5u * unit_len, dst + 4u * unit_len, dst + 6u * unit_len,
               unit_len);
    blend_line(dst + 7u * unit_len, dst + 6u * unit_len, dst + 8u * unit_len,
               unit_len);
    blend_line(dst + 10u * unit_len, dst + 9u * unit_len, dst + 11u * unit_len,
               unit_len);

    if (lookahead_line != NULL) {
        scale_line_26_16(lookahead_line, scratch_row, src_len);
        blend_line(dst + 12u * unit_len, dst + 11u * unit_len, scratch_row,
                   unit_len);
    } else {
        blend_line(dst + 12u * unit_len, dst + 11u * unit_len,
                   dst + 11u * unit_len, unit_len);
    }
}

/*
 * Fixed 5/3 blend kernel — the third geometry, unrolled. Its group is small
 * enough to hold outright: four live source pixels become five output
 * pixels, where 13/8 needed nine and had to stay two-pass. The across-walk
 * half keeps the two-pass shape regardless, because avg565 is not associative
 * and a blend unit must average two units that were already scaled along the
 * axis, never the four sources.
 *
 * The 3-pixel group divides 144 but not 160, so the two axes reach their far
 * edge by opposite mechanisms and this kernel carries both. On the 160 axis,
 * 53 groups cover 159 pixels, every group's partner exists, and the one
 * leftover pixel emits itself: 53 x 5 + 1 = 266. On the 144 axis, 48 groups
 * cover it exactly, nothing is left over, and the last group's partner is
 * past the end and clamps — avg(s2, s2) is s2 — for 48 x 5 = 240. Either way
 * the last output pixel is pure. 5 units per call, 48 calls a frame.
 *
 * ponytail: plain 16-bit stores, as in the other two kernels; pack to 32-bit
 * only if the bench says this lands short.
 */
static void scale_line_5_3(const uint16_t* src, uint16_t* dst,
                           unsigned src_len)
{
    unsigned base;
    unsigned o = 0;

    for (base = 0; base + 3 < src_len; base += 3) {
        uint16_t s0 = src[base];
        uint16_t s1 = src[base + 1];
        uint16_t s2 = src[base + 2];
        uint16_t s3 = src[base + 3]; /* in range: base + 3 < src_len */

        dst[o] = s0;
        dst[o + 1] = avg565(s0, s1);
        dst[o + 2] = s1;
        dst[o + 3] = s2;
        dst[o + 4] = avg565(s2, s3);
        o += 5;
    }

    if (base + 3 == src_len) {
        /* The group divides this axis, so one whole group is left whose blend
         * partner is past the end. It clamps to the last source pixel, and
         * avg(s2, s2) is s2 — the same pure edge the tail below gives on the
         * other axis. */
        uint16_t s0 = src[base];
        uint16_t s1 = src[base + 1];
        uint16_t s2 = src[base + 2];

        dst[o] = s0;
        dst[o + 1] = avg565(s0, s1);
        dst[o + 2] = s1;
        dst[o + 3] = s2;
        dst[o + 4] = s2;
        o += 5;
        base += 3;
    }

    for (; base < src_len; base++) {
        dst[o++] = src[base]; /* the tail: one pixel on the 160 axis */
    }
}

/*
 * pattern_5_3 lands source unit i on output units 0, 2 and 3, leaving units 1
 * and 4 as blends across the walk. Unit 4's partner is source unit 3 — the
 * next block's first line or column, which arrives as lookahead and is NULL
 * at the frame's far edge; there the generic path averages the unit with
 * itself, so blending unit 3 with unit 3 is the same pixels without a second
 * code path.
 */
static void scale_block_5_3_blend(const uint16_t* const* src_lines,
                                  const uint16_t* lookahead_line,
                                  uint16_t* dst, uint16_t* scratch_row,
                                  unsigned src_len, unsigned unit_len)
{
    scale_line_5_3(src_lines[0], dst, src_len);
    scale_line_5_3(src_lines[1], dst + 2u * unit_len, src_len);
    scale_line_5_3(src_lines[2], dst + 3u * unit_len, src_len);

    blend_line(dst + unit_len, dst, dst + 2u * unit_len, unit_len);

    if (lookahead_line != NULL) {
        scale_line_5_3(lookahead_line, scratch_row, src_len);
        blend_line(dst + 4u * unit_len, dst + 3u * unit_len, scratch_row,
                   unit_len);
    } else {
        blend_line(dst + 4u * unit_len, dst + 3u * unit_len,
                   dst + 3u * unit_len, unit_len);
    }
}

/*
 * The pattern-driven two-pass walk, which NEAREST always takes and BLEND
 * takes only for a geometry with no fixed kernel. Axis-agnostic: dst is
 * dst_units contiguous units of unit_len pixels, whether those are rows of
 * dst_w or columns of dst_h.
 */
static void scale_block_generic(const scaler_geom_info_t* gi,
                                const scaler_pattern_t* pat, int blend,
                                const uint16_t* const* src_lines,
                                const uint16_t* lookahead_line,
                                uint16_t* dst, uint16_t* scratch_row,
                                unsigned src_len, unsigned unit_len)
{
    uint8_t pure_unit[SCALER_SRC_LINES_MAX];
    unsigned src_units = gi->src_lines_per_block;
    unsigned dst_units = gi->dst_rows_per_block;
    unsigned i;
    int scratch_ready = 0;

    for (i = 0; i < src_units; i++) {
        pure_unit[i] = 0;
    }

    /* Pass 1: the pattern emits every source unit as a pure output unit, so
     * each source line is scaled along the axis exactly once, straight into
     * the output unit that owns it. */
    for (i = 0; i < dst_units; i++) {
        if (!pat[i].is_blend) {
            pure_unit[pat[i].src_offset] = (uint8_t)i;
            scale_line(src_lines[pat[i].src_offset],
                       dst + (size_t)i * unit_len,
                       pat, src_units, dst_units, blend, src_len);
        }
    }

    /* Pass 2: a blend unit combines two units pass 1 already scaled, so the
     * along-axis work is never repeated. The one exception is a blend whose
     * partner is the first line of the NEXT block, which goes via
     * scratch_row. */
    for (i = 0; i < dst_units; i++) {
        uint16_t* unit = dst + (size_t)i * unit_len;
        const uint16_t* a;
        const uint16_t* b;
        unsigned partner;
        unsigned x;

        if (!pat[i].is_blend) {
            continue;
        }
        a = dst + (size_t)pure_unit[pat[i].src_offset] * unit_len;
        if (!blend) {
            /* Nearest-neighbour: duplicate the top or left source unit. */
            for (x = 0; x < unit_len; x++) {
                unit[x] = a[x];
            }
            continue;
        }

        partner = (unsigned)pat[i].src_offset + 1u;
        if (partner < src_units) {
            b = dst + (size_t)pure_unit[partner] * unit_len;
        } else if (lookahead_line != NULL) {
            if (!scratch_ready) {
                scale_line(lookahead_line, scratch_row,
                           pat, src_units, dst_units, blend, src_len);
                scratch_ready = 1;
            }
            b = scratch_row;
        } else {
            b = a; /* the frame's far edge: the trailing units stay pure */
        }
        blend_line(unit, a, b, unit_len);
    }
}

/*
 * Shared argument check for both walks: an unknown geometry or mode, a NULL
 * buffer, or a NULL source unit. Fills *info on success.
 */
static int check_args(enum scaler_geom_e geom, enum scaler_mode_e mode,
                      const uint16_t* const* src_lines, const uint16_t* dst,
                      const uint16_t* scratch_row,
                      const scaler_geom_info_t** info)
{
    const scaler_geom_info_t* gi = scaler_geom_info(geom);
    unsigned i;

    if (gi == NULL) {
        return SCALER_ERR_ARGS;
    }
    if (mode != SCALER_MODE_NEAREST && mode != SCALER_MODE_BLEND) {
        return SCALER_ERR_ARGS;
    }
    if (src_lines == NULL || dst == NULL || scratch_row == NULL) {
        return SCALER_ERR_ARGS;
    }
    for (i = 0; i < gi->src_lines_per_block; i++) {
        if (src_lines[i] == NULL) {
            return SCALER_ERR_ARGS;
        }
    }
    *info = gi;
    return SCALER_OK;
}

int scaler_scale_block(enum scaler_geom_e geom, enum scaler_mode_e mode,
                       const uint16_t* const* src_lines,
                       const uint16_t* lookahead_line,
                       uint16_t* dst, uint16_t* scratch_row)
{
    const scaler_geom_info_t* gi;
    unsigned dst_w;
    int rc = check_args(geom, mode, src_lines, dst, scratch_row, &gi);

    if (rc != SCALER_OK) {
        return rc;
    }
    dst_w = gi->dst_w;

    if (mode == SCALER_MODE_BLEND) {
        if (geom == SCALER_GEOM_24_16) {
            scale_block_24_16_blend(src_lines[0], src_lines[1], dst,
                                    SCALER_SRC_W, dst_w);
            return SCALER_OK;
        }
        if (geom == SCALER_GEOM_26_16) {
            scale_block_26_16_blend(src_lines, lookahead_line, dst,
                                    scratch_row, SCALER_SRC_W, dst_w);
            return SCALER_OK;
        }
        scale_block_5_3_blend(src_lines, lookahead_line, dst, scratch_row,
                              SCALER_SRC_W, dst_w);
        return SCALER_OK;
    }

    scale_block_generic(gi, pattern_table[(unsigned)geom], 0,
                        src_lines, lookahead_line, dst, scratch_row,
                        SCALER_SRC_W, dst_w);
    return SCALER_OK;
}

/* Output column height: the along-column walk's src_units -> dst_units ratio
 * applied to the frame's 144 lines. 216, 234 and 240. */
static unsigned dst_h_of(const scaler_geom_info_t* gi)
{
    return (unsigned)SCALER_SRC_H / gi->src_lines_per_block
           * gi->dst_rows_per_block;
}

int scaler_scale_col_block(enum scaler_geom_e geom, enum scaler_mode_e mode,
                           const uint16_t* const* src_cols,
                           const uint16_t* lookahead_col,
                           uint16_t* dst, uint16_t* scratch_col)
{
    const scaler_geom_info_t* gi;
    unsigned dst_h;
    int rc = check_args(geom, mode, src_cols, dst, scratch_col, &gi);

    if (rc != SCALER_OK) {
        return rc;
    }
    dst_h = dst_h_of(gi);

    if (mode == SCALER_MODE_BLEND) {
        if (geom == SCALER_GEOM_24_16) {
            scale_block_24_16_blend(src_cols[0], src_cols[1], dst,
                                    SCALER_SRC_H, dst_h);
            return SCALER_OK;
        }
        if (geom == SCALER_GEOM_26_16) {
            scale_block_26_16_blend(src_cols, lookahead_col, dst,
                                    scratch_col, SCALER_SRC_H, dst_h);
            return SCALER_OK;
        }
        scale_block_5_3_blend(src_cols, lookahead_col, dst, scratch_col,
                              SCALER_SRC_H, dst_h);
        return SCALER_OK;
    }

    scale_block_generic(gi, pattern_table[(unsigned)geom], 0,
                        src_cols, lookahead_col, dst, scratch_col,
                        SCALER_SRC_H, dst_h);
    return SCALER_OK;
}

int scaler_scale_col_tail(enum scaler_geom_e geom, enum scaler_mode_e mode,
                          const uint16_t* const* src_cols, uint16_t* dst)
{
    const scaler_geom_info_t* gi = scaler_geom_info(geom);
    const scaler_pattern_t* pat;
    unsigned tail;
    unsigned i;

    if (gi == NULL) {
        return SCALER_ERR_ARGS;
    }
    if (mode != SCALER_MODE_NEAREST && mode != SCALER_MODE_BLEND) {
        return SCALER_ERR_ARGS;
    }
    tail = SCALER_SRC_W % gi->src_lines_per_block;
    if (tail == 0) {
        return SCALER_OK; /* the group divides 160: nothing is left over */
    }
    if (src_cols == NULL || dst == NULL) {
        return SCALER_ERR_ARGS;
    }
    for (i = 0; i < tail; i++) {
        if (src_cols[i] == NULL) {
            return SCALER_ERR_ARGS;
        }
    }

    /* One column of 240 pixels a frame at 5/3, of 63,840. It takes the
     * pattern walk rather than a kernel of its own: the kernels exist to
     * delete the per-pixel branch from the hot 99.6 %, not from this. */
    pat = pattern_table[(unsigned)geom];
    for (i = 0; i < tail; i++) {
        scale_line(src_cols[i], dst + (size_t)i * dst_h_of(gi),
                   pat, gi->src_lines_per_block, gi->dst_rows_per_block,
                   mode == SCALER_MODE_BLEND, SCALER_SRC_H);
    }
    return SCALER_OK;
}
