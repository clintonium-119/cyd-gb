#include "scaler.h"

#define GEOM_COUNT 1

/*
 * One output unit of a geometry's pattern. The pattern is indexed by the
 * output unit's position within a block and applied identically on both
 * axes: is_blend means "average source unit src_offset with src_offset + 1".
 */
typedef struct {
    uint8_t src_offset;
    uint8_t is_blend;
} scaler_pattern_t;

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
    { 3, 5, 266, 1 }, /* SCALER_GEOM_5_3: unit 2 blends with unit 3 */
};

static const scaler_pattern_t* const pattern_table[GEOM_COUNT] = {
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
                       unsigned src_len, unsigned src_end)
{
    unsigned base;
    unsigned o = 0;

    for (base = 0; base + src_units <= src_len; base += src_units) {
        unsigned i;
        for (i = 0; i < dst_units; i++) {
            unsigned s = base + pat[i].src_offset;
            if (blend && pat[i].is_blend) {
                unsigned p = s + 1;
                if (p >= src_end) {
                    p = src_end - 1; /* the far edge stays pure */
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
     * length on the 144 axis, which three divides. */
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
 * Fixed 5/3 blend kernel — the shipped geometry, unrolled. Its group is small
 * enough to hold outright: four live source pixels become five output pixels.
 * The across-walk half keeps the generic path's two-pass shape regardless,
 * because avg565 is not associative and a blend unit must average two units
 * that were already scaled along the axis, never the four sources.
 *
 * The 3-pixel group divides 144 but not 160, so the two axes reach their far
 * edge by opposite mechanisms and this kernel carries both. On the 160 axis,
 * 53 groups cover 159 pixels, every group's partner exists, and the one
 * leftover pixel emits itself: 53 x 5 + 1 = 266. On the 144 axis, 48 groups
 * cover it exactly, nothing is left over, and the last group's partner is
 * past the end and clamps — avg(s2, s2) is s2 — for 48 x 5 = 240. Either way
 * the last output pixel is pure. 5 units per call, 48 calls a frame.
 *
 * ponytail: plain 16-bit stores; the toolchain does not merge them. Pack to
 * 32-bit stores (dst must then be 4-byte aligned, which the API does not
 * promise) only if the bench says this lands short.
 */
static void scale_line_5_3(const uint16_t* src, uint16_t* dst,
                           unsigned src_len, unsigned src_end)
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
         * partner is past the WALK. Whether it is also past the readable
         * source decides what happens: at the frame's real edge it clamps,
         * and avg(s2, s2) is s2, the same pure edge the tail below gives on
         * the other axis. Mid-frame — a walk that stops short because the
         * caller wants only part of the axis — the partner is a real pixel
         * and gets read, so the join is seamless. */
        uint16_t s0 = src[base];
        uint16_t s1 = src[base + 1];
        uint16_t s2 = src[base + 2];
        uint16_t s3 = (base + 3 < src_end) ? src[base + 3] : s2;

        dst[o] = s0;
        dst[o + 1] = avg565(s0, s1);
        dst[o + 2] = s1;
        dst[o + 3] = s2;
        dst[o + 4] = avg565(s2, s3);
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
                                  unsigned src_len, unsigned src_end,
                                  ptrdiff_t unit_step, unsigned unit_len)
{
    scale_line_5_3(src_lines[0], dst, src_len, src_end);
    scale_line_5_3(src_lines[1], dst + 2 * unit_step, src_len, src_end);
    scale_line_5_3(src_lines[2], dst + 3 * unit_step, src_len, src_end);

    blend_line(dst + unit_step, dst, dst + 2 * unit_step, unit_len);

    if (lookahead_line != NULL) {
        scale_line_5_3(lookahead_line, scratch_row, src_len, src_end);
        blend_line(dst + 4 * unit_step, dst + 3 * unit_step, scratch_row,
                   unit_len);
    } else {
        blend_line(dst + 4 * unit_step, dst + 3 * unit_step,
                   dst + 3 * unit_step, unit_len);
    }
}

/*
 * The pattern-driven two-pass walk, which NEAREST always takes, and which the
 * 5/3 kernel above is checked against pixel for pixel. Axis-agnostic: dst is
 * dst_units contiguous units of unit_len pixels, whether those are rows of
 * dst_w or columns of dst_h.
 */
static void scale_block_generic(const scaler_geom_info_t* gi,
                                const scaler_pattern_t* pat, int blend,
                                const uint16_t* const* src_lines,
                                const uint16_t* lookahead_line,
                                uint16_t* dst, uint16_t* scratch_row,
                                unsigned src_len, unsigned src_end,
                                ptrdiff_t unit_step, unsigned unit_len)
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
                       dst + (ptrdiff_t)i * unit_step,
                       pat, src_units, dst_units, blend, src_len, src_end);
        }
    }

    /* Pass 2: a blend unit combines two units pass 1 already scaled, so the
     * along-axis work is never repeated. The one exception is a blend whose
     * partner is the first line of the NEXT block, which goes via
     * scratch_row. */
    for (i = 0; i < dst_units; i++) {
        uint16_t* unit = dst + (ptrdiff_t)i * unit_step;
        const uint16_t* a;
        const uint16_t* b;
        unsigned partner;
        unsigned x;

        if (!pat[i].is_blend) {
            continue;
        }
        a = dst + (ptrdiff_t)pure_unit[pat[i].src_offset] * unit_step;
        if (!blend) {
            /* Nearest-neighbour: duplicate the top or left source unit. */
            for (x = 0; x < unit_len; x++) {
                unit[x] = a[x];
            }
            continue;
        }

        partner = (unsigned)pat[i].src_offset + 1u;
        if (partner < src_units) {
            b = dst + (ptrdiff_t)pure_unit[partner] * unit_step;
        } else if (lookahead_line != NULL) {
            if (!scratch_ready) {
                scale_line(lookahead_line, scratch_row,
                           pat, src_units, dst_units, blend, src_len, src_end);
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
        scale_block_5_3_blend(src_lines, lookahead_line, dst, scratch_row,
                              SCALER_SRC_W, SCALER_SRC_W, (ptrdiff_t)dst_w,
                              dst_w);
        return SCALER_OK;
    }

    scale_block_generic(gi, pattern_table[(unsigned)geom], 0,
                        src_lines, lookahead_line, dst, scratch_row,
                        SCALER_SRC_W, SCALER_SRC_W, (ptrdiff_t)dst_w, dst_w);
    return SCALER_OK;
}

/* Output column height: the along-column walk's src_units -> dst_units ratio
 * applied to the frame's 144 lines. 240. */
static unsigned dst_h_of(const scaler_geom_info_t* gi)
{
    return (unsigned)SCALER_SRC_H / gi->src_lines_per_block
           * gi->dst_rows_per_block;
}

int scaler_scale_col_rows(enum scaler_geom_e geom, enum scaler_mode_e mode,
                          const uint16_t* const* src_cols,
                          const uint16_t* lookahead_col,
                          uint16_t* dst, uint16_t* scratch_col,
                          enum scaler_col_order_e order,
                          unsigned src_first, unsigned src_rows)
{
    const scaler_geom_info_t* gi;
    const uint16_t* off_cols[SCALER_SRC_LINES_MAX];
    const uint16_t* off_look = NULL;
    unsigned src_units;
    unsigned slice_h;
    unsigned src_end;
    ptrdiff_t step;
    unsigned i;
    int rc = check_args(geom, mode, src_cols, dst, scratch_col, &gi);

    if (rc != SCALER_OK) {
        return rc;
    }
    if (order != SCALER_COLS_ASCENDING && order != SCALER_COLS_DESCENDING) {
        return SCALER_ERR_ARGS;
    }
    src_units = gi->src_lines_per_block;
    /* Both ends must land on a group boundary, or the pattern's phase within
     * the group would differ from the whole-column walk's and the slices
     * would not join. */
    if (src_rows == 0 || src_first % src_units != 0
        || src_rows % src_units != 0
        || src_first + src_rows > SCALER_SRC_H) {
        return SCALER_ERR_ARGS;
    }
    slice_h = src_rows / src_units * gi->dst_rows_per_block;

    /* The walk covers src_rows, but a blend at the end of it may read one
     * past — into the next slice's first row, which is a real pixel. Only at
     * the column's true end is there nothing to reach for, and there it
     * clamps and the edge stays pure. */
    for (i = 0; i < src_units; i++) {
        off_cols[i] = src_cols[i] + src_first;
    }
    if (lookahead_col != NULL) {
        off_look = lookahead_col + src_first;
    }
    src_end = SCALER_SRC_H - src_first;

    /* Descending reverses the stride and starts at the last column of the
     * block, so the walk itself is unchanged: the panel's address window
     * fills in one fixed direction, and this is the only place that can put
     * the columns in that order without moving pixels twice. */
    if (order == SCALER_COLS_DESCENDING) {
        step = -(ptrdiff_t)slice_h;
        dst += (size_t)(gi->dst_rows_per_block - 1u) * slice_h;
    } else {
        step = (ptrdiff_t)slice_h;
    }

    if (mode == SCALER_MODE_BLEND) {
        scale_block_5_3_blend(off_cols, off_look, dst, scratch_col,
                              src_rows, src_end, step, slice_h);
        return SCALER_OK;
    }

    scale_block_generic(gi, pattern_table[(unsigned)geom], 0,
                        off_cols, off_look, dst, scratch_col,
                        src_rows, src_end, step, slice_h);
    return SCALER_OK;
}

int scaler_scale_col_block(enum scaler_geom_e geom, enum scaler_mode_e mode,
                           const uint16_t* const* src_cols,
                           const uint16_t* lookahead_col,
                           uint16_t* dst, uint16_t* scratch_col,
                           enum scaler_col_order_e order)
{
    return scaler_scale_col_rows(geom, mode, src_cols, lookahead_col, dst,
                                 scratch_col, order, 0, SCALER_SRC_H);
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
                   mode == SCALER_MODE_BLEND, SCALER_SRC_H, SCALER_SRC_H);
    }
    return SCALER_OK;
}
