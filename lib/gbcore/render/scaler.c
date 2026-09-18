#include "scaler.h"

#define GEOM_COUNT 2

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

static const scaler_geom_info_t geom_table[GEOM_COUNT] = {
    { 2, 3, 240, 0 },  /* SCALER_GEOM_24_16: the blend partner is in-block */
    { 8, 13, 260, 1 }, /* SCALER_GEOM_26_16: unit 7 blends with unit 8     */
};

static const scaler_pattern_t* const pattern_table[GEOM_COUNT] = {
    pattern_24_16,
    pattern_26_16,
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
 * Scale one source line to one output row. Applies the pattern across the
 * whole line, so a horizontal blend may reach into the next block; only the
 * final block's last blend has no partner and clamps to pixel 159.
 */
static void scale_row(const uint16_t* src, uint16_t* dst,
                      const scaler_pattern_t* pat,
                      unsigned src_units, unsigned dst_units, int blend)
{
    unsigned base;
    unsigned o = 0;

    for (base = 0; base < SCALER_SRC_W; base += src_units) {
        unsigned i;
        for (i = 0; i < dst_units; i++) {
            unsigned s = base + pat[i].src_offset;
            if (blend && pat[i].is_blend) {
                unsigned p = s + 1;
                if (p >= SCALER_SRC_W) {
                    p = SCALER_SRC_W - 1; /* right edge stays pure */
                }
                dst[o++] = avg565(src[s], src[p]);
            } else {
                dst[o++] = src[s];
            }
        }
    }
}

/*
 * Fixed 3/2 blend kernel — the shipped geometry, unrolled. One source pair
 * (a, b) becomes (a, avg(a, b), b), and two source lines become three rows:
 * the two scaled lines and, between them, their per-pixel average. These are
 * the pixels the pattern walk produces for 24/16 in BLEND mode, computed in
 * the same order: the middle row averages the two horizontally blended
 * rows, never the four sources, because avg565 is not associative. No
 * right-edge clamp is needed (the partner of pixel 158 is 159) and no
 * lookahead (pattern_24_16's only blend partner is in-block), so there is no
 * frame-end case either. 80 pairs x 3 rows = 720 pixels per call, 72 calls
 * a frame.
 *
 * ponytail: plain 16-bit stores; the toolchain does not merge them. Pack to
 * 32-bit stores (dst must then be 4-byte aligned, which the API does not
 * promise) only if the bench says the kernel is still short.
 */
static void scale_block_24_16_blend(const uint16_t* l0, const uint16_t* l1,
                                    uint16_t* dst)
{
    uint16_t* r0 = dst;
    uint16_t* r1 = dst + 240;
    uint16_t* r2 = dst + 480;
    unsigned x;
    unsigned o = 0;

    for (x = 0; x < SCALER_SRC_W; x += 2) {
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
 * generic path's two-pass shape (scale the 8 pure rows horizontally, then
 * average pairs of already-scaled rows) and unrolls only the horizontal
 * walk, which is where the per-pixel pattern load, is_blend branch and
 * bounds check lived. 20 groups of 8 source pixels become 13 output pixels
 * each: 260 per row, 13 rows, 18 calls a frame.
 *
 * ponytail: plain 16-bit stores, as in the 3/2 kernel; pack to 32-bit only
 * if the bench says this lands short.
 */
static void scale_row_26_16(const uint16_t* src, uint16_t* dst)
{
    unsigned base;
    unsigned o = 0;

    for (base = 0; base < SCALER_SRC_W; base += 8) {
        /* The last group's partner is pixel 160, which does not exist; it
         * clamps to 159, so the final output pixel is avg(s159, s159) and
         * the right edge stays pure. One compare per group, not per pixel. */
        uint16_t s0 = src[base];
        uint16_t s1 = src[base + 1];
        uint16_t s2 = src[base + 2];
        uint16_t s3 = src[base + 3];
        uint16_t s4 = src[base + 4];
        uint16_t s5 = src[base + 5];
        uint16_t s6 = src[base + 6];
        uint16_t s7 = src[base + 7];
        uint16_t s8 = src[base + 8 < SCALER_SRC_W ? base + 8
                                                  : SCALER_SRC_W - 1];

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

/* One vertical blend row: the average of two rows scale_row_26_16 already
 * produced, never of the four sources, because avg565 is not associative. */
static void blend_row_26_16(uint16_t* dst, const uint16_t* a,
                            const uint16_t* b)
{
    unsigned x;

    for (x = 0; x < 260; x++) {
        dst[x] = avg565(a[x], b[x]);
    }
}

/*
 * pattern_26_16 lands source line i on output row {0, 1, 3, 4, 6, 8, 9, 11}
 * and leaves rows 2, 5, 7, 10 and 12 as vertical blends. Row 12's partner is
 * source line 8 — the next block's first line, which arrives as
 * lookahead_line and is NULL on the frame's last block; there the generic
 * path averages the row with itself, so blending row 11 with row 11 is the
 * same pixels without a second code path.
 */
static void scale_block_26_16_blend(const uint16_t* const* src_lines,
                                    const uint16_t* lookahead_line,
                                    uint16_t* dst, uint16_t* scratch_row)
{
    scale_row_26_16(src_lines[0], dst);
    scale_row_26_16(src_lines[1], dst + 260);
    scale_row_26_16(src_lines[2], dst + 3 * 260);
    scale_row_26_16(src_lines[3], dst + 4 * 260);
    scale_row_26_16(src_lines[4], dst + 6 * 260);
    scale_row_26_16(src_lines[5], dst + 8 * 260);
    scale_row_26_16(src_lines[6], dst + 9 * 260);
    scale_row_26_16(src_lines[7], dst + 11 * 260);

    blend_row_26_16(dst + 2 * 260, dst + 260, dst + 3 * 260);
    blend_row_26_16(dst + 5 * 260, dst + 4 * 260, dst + 6 * 260);
    blend_row_26_16(dst + 7 * 260, dst + 6 * 260, dst + 8 * 260);
    blend_row_26_16(dst + 10 * 260, dst + 9 * 260, dst + 11 * 260);

    if (lookahead_line != NULL) {
        scale_row_26_16(lookahead_line, scratch_row);
        blend_row_26_16(dst + 12 * 260, dst + 11 * 260, scratch_row);
    } else {
        blend_row_26_16(dst + 12 * 260, dst + 11 * 260, dst + 11 * 260);
    }
}

int scaler_scale_block(enum scaler_geom_e geom, enum scaler_mode_e mode,
                       const uint16_t* const* src_lines,
                       const uint16_t* lookahead_line,
                       uint16_t* dst, uint16_t* scratch_row)
{
    const scaler_geom_info_t* gi = scaler_geom_info(geom);
    const scaler_pattern_t* pat;
    uint8_t pure_row[SCALER_SRC_LINES_MAX];
    unsigned src_units;
    unsigned dst_units;
    unsigned dst_w;
    unsigned i;
    int blend;
    int scratch_ready = 0;

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
        pure_row[i] = 0;
    }

    if (geom == SCALER_GEOM_24_16 && mode == SCALER_MODE_BLEND) {
        scale_block_24_16_blend(src_lines[0], src_lines[1], dst);
        return SCALER_OK;
    }
    if (geom == SCALER_GEOM_26_16 && mode == SCALER_MODE_BLEND) {
        scale_block_26_16_blend(src_lines, lookahead_line, dst, scratch_row);
        return SCALER_OK;
    }

    pat = pattern_table[(unsigned)geom];
    src_units = gi->src_lines_per_block;
    dst_units = gi->dst_rows_per_block;
    dst_w = gi->dst_w;
    blend = (mode == SCALER_MODE_BLEND);

    /* Pass 1: the pattern emits every source unit as a pure output unit, so
     * each source line is scaled horizontally exactly once, straight into
     * the output row that owns it. */
    for (i = 0; i < dst_units; i++) {
        if (!pat[i].is_blend) {
            pure_row[pat[i].src_offset] = (uint8_t)i;
            scale_row(src_lines[pat[i].src_offset], dst + (size_t)i * dst_w,
                      pat, src_units, dst_units, blend);
        }
    }

    /* Pass 2: a blend row combines two rows pass 1 already scaled, so the
     * horizontal work is never repeated. The one exception is a blend whose
     * partner is the first line of the NEXT block, which goes via
     * scratch_row. */
    for (i = 0; i < dst_units; i++) {
        uint16_t* row = dst + (size_t)i * dst_w;
        const uint16_t* a;
        const uint16_t* b;
        unsigned partner;
        unsigned x;

        if (!pat[i].is_blend) {
            continue;
        }
        a = dst + (size_t)pure_row[pat[i].src_offset] * dst_w;
        if (!blend) {
            /* Nearest-neighbour: duplicate the top source row. */
            for (x = 0; x < dst_w; x++) {
                row[x] = a[x];
            }
            continue;
        }

        partner = (unsigned)pat[i].src_offset + 1u;
        if (partner < src_units) {
            b = dst + (size_t)pure_row[partner] * dst_w;
        } else if (lookahead_line != NULL) {
            if (!scratch_ready) {
                scale_row(lookahead_line, scratch_row,
                          pat, src_units, dst_units, blend);
                scratch_ready = 1;
            }
            b = scratch_row;
        } else {
            b = a; /* frame end: the trailing rows stay pure */
        }
        for (x = 0; x < dst_w; x++) {
            row[x] = avg565(a[x], b[x]);
        }
    }

    return SCALER_OK;
}
