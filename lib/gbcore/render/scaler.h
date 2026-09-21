#pragma once
// Render scaler — landscape integer-block upscale with a blended seam.
//
// Geometry model: a geometry is src_units source units to dst_units output
// units, applied identically on both axes, so one call consumes a block of
// src_lines_per_block source lines and emits dst_rows_per_block output rows
// of dst_w pixels each. Three geometries are supported and ALL are always
// compiled; the caller selects one per call:
//
//   SCALER_GEOM_24_16  (3/2)   2 source units -> 3 output units,  160 -> 240
//   SCALER_GEOM_26_16  (13/8)  8 source units -> 13 output units, 160 -> 260
//   SCALER_GEOM_5_3    (5/3)   3 source units -> 5 output units,  160 -> 266
//
// Tail: the two k/16 geometries have a source group that divides 160, so the
// horizontal walk is whole groups all the way across. 5/3's does not — 53
// groups of 3 cover 159 pixels and leave one over — so the walk ends with a
// tail of SCALER_SRC_W % src_units source pixels, each emitted once, pure.
// That makes dst_w exactly
//
//   (160 / src_units) * dst_units + 160 % src_units
//
// for every geometry, and the tail zero-length for both k/16 ones. Vertically
// there is never a tail: 144 is divisible by 2, 8 and 3 alike.
//
// Each output unit either copies a source unit ("pure") or is the average of
// that source unit and the NEXT one ("blend"). The pure units stay sharp and
// only the genuine seam softens; on a 4-shade ramp this reads as natural
// anti-aliasing. SCALER_MODE_NEAREST duplicates the left/top source instead
// of averaging, which is what the golden tests pin as the A/B baseline.
//
// Clamping: a blend whose partner falls past the last source pixel of a line,
// or past the last source line of the frame (lookahead_line NULL), clamps to
// that last source, so the frame's right and bottom edges stay pure. A tail
// pixel never blends at all — there is no next group to reach toward — so at
// 5/3 the right edge is pure source pixel 159 rather than a clamped average.
//
// Lookahead: only a geometry whose block's final blend row reaches into the
// next block needs the next block's first line. 26/16 does (unit 7 blends
// with unit 8) and so does 5/3 (unit 2 blends with unit 3); 24/16 does not
// (its one blend partner is in-block), and uses_lookahead in the geometry
// table says which, so a caller can skip carrying a line nobody reads.
//
// Fast path: every geometry in BLEND mode runs through a fixed, unrolled
// kernel that produces the same pixels as the pattern walk; NEAREST, which
// exists only as the golden suite's A/B baseline, takes the pattern-driven
// code. Both are the same function to the caller.
//
// Transposed walk: scaler_scale_col_block() emits output COLUMNS instead of
// output rows, for a consumer that pushes along the panel's gate lines. A
// geometry applies identically on both axes, so the same src_units ->
// dst_units ratio runs with the two passes swapping roles: the walk ALONG a
// column takes SCALER_SRC_H source rows to dst_h output pixels, and the walk
// ACROSS columns takes SCALER_SRC_W source columns to dst_w output columns.
// Read the geometry table's src_lines_per_block as source COLUMNS and
// dst_rows_per_block as output COLUMNS for this walk, and dst_h as
//
//   SCALER_SRC_H / src_lines_per_block * dst_rows_per_block
//
// which is 216, 234 and 240 for the three geometries.
//
// The tail moves axis with the walk. The row walk's tail is along its walk:
// 5/3's group leaves one source pixel over at the end of every row, emitted
// pure, and 144 divides by 2, 8 and 3 alike so nothing is ever left over
// vertically. Transposed, the along-column walk has no tail of its own and
// the leftover lands ACROSS the walk, as a whole output column: at 5/3 the
// last output column is the pure scale of source column 159 and belongs to no
// block. scaler_scale_col_tail() emits it, and emits nothing for the two k/16
// geometries whose group divides 160. What the row walk reaches by a tail, the
// column walk's along-axis reaches by a clamp — its last whole group's blend
// partner is past the end — and both leave that edge pure.
//
// The two walks are not bit-identical in BLEND mode. At a pixel that is both a
// horizontal and a vertical seam they average the same four sources in a
// different order: the row walk averages two horizontally scaled rows, the
// column walk two vertically scaled columns, and scaler_avg565() is a
// per-channel floor((a + b) / 2), which is not associative across that
// regrouping. They differ by at most 1 per channel, and only there; every pure
// or singly-blended pixel, and the whole of NEAREST mode, transposes exactly.
//
// Byte order: scaler_avg565() assumes NATIVE RGB565 bit layout. Blend BEFORE
// any byte swap — averaging byte-swapped values mixes misaligned channel
// fields and produces colour fringing.
//
// Pure C, no Arduino/ESP-IDF headers, no allocation: all buffers are
// caller-owned.

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SCALER_SRC_W 160        /* Game Boy line width, pixels           */
#define SCALER_SRC_H 144        /* Game Boy frame height, lines          */
#define SCALER_DST_W_MAX 266    /* widest output row (5/3)               */
#define SCALER_DST_H_MAX 240    /* tallest output column (5/3)           */
#define SCALER_DST_ROWS_MAX 13  /* most output rows per block (26/16)    */
#define SCALER_SRC_LINES_MAX 8  /* most source lines per block (26/16)   */

enum scaler_result_e {
    SCALER_OK = 0,
    SCALER_ERR_ARGS = -1, /* NULL buffer or unknown geometry / mode */
};

enum scaler_geom_e {
    SCALER_GEOM_24_16 = 0, /* 3/2:  160x144 -> 240x216 */
    SCALER_GEOM_26_16 = 1, /* 13/8: 160x144 -> 260x234 */
    SCALER_GEOM_5_3 = 2,   /* 5/3:  160x144 -> 266x240 */
};

enum scaler_mode_e {
    SCALER_MODE_NEAREST = 0, /* interpolated unit duplicates left/top source */
    SCALER_MODE_BLEND = 1,   /* interpolated unit is the RGB565 average      */
};

typedef struct scaler_geom_info_s {
    uint8_t src_lines_per_block; /* source lines one block call consumes */
    uint8_t dst_rows_per_block;  /* output rows one block call emits     */
    uint16_t dst_w;              /* output row width, pixels             */
    uint8_t uses_lookahead;      /* a blend row reads the next block     */
} scaler_geom_info_t;

/*
 * Fixed description of a geometry, for callers sizing buffers and stepping
 * the frame. Returns NULL for an unknown geometry.
 */
const scaler_geom_info_t* scaler_geom_info(enum scaler_geom_e geom);

/*
 * Scale one block of source lines into dst_rows_per_block output rows.
 *
 *   src_lines      array of src_lines_per_block pointers to SCALER_SRC_W-px
 *                  source lines, top first
 *   lookahead_line first source line of the NEXT block, or NULL at frame end
 *                  (the trailing blend rows then stay pure); ignored by a
 *                  geometry with uses_lookahead 0
 *   dst            dst_rows_per_block * dst_w pixels, row-major
 *   scratch_row    dst_w scratch pixels, required; holds the horizontally
 *                  scaled lookahead line when a cross-block blend row needs
 *                  it (26/16 and 5/3, but always checked)
 *
 * Returns SCALER_OK or SCALER_ERR_ARGS.
 */
int scaler_scale_block(enum scaler_geom_e geom, enum scaler_mode_e mode,
                       const uint16_t* const* src_lines,
                       const uint16_t* lookahead_line,
                       uint16_t* dst, uint16_t* scratch_row);

/*
 * Scale one block of source COLUMNS into dst_rows_per_block output columns,
 * the transposed counterpart of scaler_scale_block(). Same geometry, same
 * modes, same caller-owned buffers; the axes swap.
 *
 *   src_cols       array of src_lines_per_block pointers to SCALER_SRC_H-px
 *                  source columns, leftmost first
 *   lookahead_col  first source column of the NEXT block, or NULL at the
 *                  frame's right edge (the trailing blend columns then stay
 *                  pure); ignored by a geometry with uses_lookahead 0
 *   dst            dst_rows_per_block * dst_h pixels, COLUMN-major: one
 *                  output column is dst_h contiguous pixels
 *   scratch_col    dst_h scratch pixels, required; holds the along-scaled
 *                  lookahead column when a cross-block blend column needs it
 *
 * Returns SCALER_OK or SCALER_ERR_ARGS.
 */
int scaler_scale_col_block(enum scaler_geom_e geom, enum scaler_mode_e mode,
                           const uint16_t* const* src_cols,
                           const uint16_t* lookahead_col,
                           uint16_t* dst, uint16_t* scratch_col);

/*
 * Emit the transposed walk's tail: the SCALER_SRC_W % src_lines_per_block
 * source columns left over past the last whole block, each scaled along its
 * length into one pure output column of dst_h pixels. One column at 5/3, none
 * at either k/16 geometry, where it returns SCALER_OK having written nothing
 * and src_cols and dst may be NULL.
 *
 *   src_cols  pointers to the leftover source columns, leftmost first
 *   dst       tail columns * dst_h pixels, column-major
 *
 * A tail column never blends across the walk: there is no next group to reach
 * toward, which is what makes the frame's far edge pure.
 */
int scaler_scale_col_tail(enum scaler_geom_e geom, enum scaler_mode_e mode,
                          const uint16_t* const* src_cols, uint16_t* dst);

/*
 * Average of two native-bit-layout RGB565 pixels, per channel, without
 * unpacking. Callers must pass NATIVE (not byte-swapped) values.
 */
uint16_t scaler_avg565(uint16_t a, uint16_t b);

#ifdef __cplusplus
}
#endif
