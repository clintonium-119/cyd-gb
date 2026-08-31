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
    { 2, 3, 240 },  /* SCALER_GEOM_24_16 */
    { 8, 13, 260 }, /* SCALER_GEOM_26_16 */
};

static const scaler_pattern_t* const pattern_table[GEOM_COUNT] = {
    pattern_24_16,
    pattern_26_16,
};

uint16_t scaler_avg565(uint16_t a, uint16_t b)
{
    /* Per-channel average without unpacking: bits the two pixels share pass
     * through (a & b), and each differing bit contributes half. The 0xF7DE
     * mask drops each channel's low bit before the shift so no channel
     * borrows from its neighbour. */
    return (uint16_t)((((a ^ b) & 0xF7DEu) >> 1) + (a & b));
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
                dst[o++] = scaler_avg565(src[s], src[p]);
            } else {
                dst[o++] = src[s];
            }
        }
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
            row[x] = scaler_avg565(a[x], b[x]);
        }
    }

    return SCALER_OK;
}
