#pragma once
// Render scaler — landscape integer-block upscale with a blended seam.
//
// Geometry model: a geometry is src_units source units to dst_units output
// units, applied identically on both axes, so one call consumes a block of
// src_lines_per_block source lines and emits dst_rows_per_block output rows
// of dst_w pixels each. One geometry ships and one is compiled:
//
//   SCALER_GEOM_5_3    (5/3)   3 source units -> 5 output units,  160 -> 266
//
// The caller still names it per call, and the geometry table still describes
// it rather than the numbers being spelled into the kernels, because that
// table is what the firmware's own duplicated compile-time sizes are
// cross-checked against at startup.
//
// Tail: 5/3's source group of three does not divide 160 — 53 groups cover 159
// pixels and leave one over — so the horizontal walk ends with a tail of
// SCALER_SRC_W % src_units source pixels, each emitted once, pure. That makes
// dst_w exactly
//
//   (160 / src_units) * dst_units + 160 % src_units
//
// which is 266. Vertically there is never a tail: three divides 144.
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
// Lookahead: 5/3's block ends on a blend row that reaches into the next block
// — unit 4 blends unit 2 with the next group's first — so a block needs the
// next block's first line. uses_lookahead in the geometry table says so, and
// it is a table field rather than an assumption because a geometry whose
// every blend partner is in-block lets the caller skip carrying a line that
// nobody reads.
//
// Fast path: BLEND runs through a fixed, unrolled kernel that produces the
// same pixels as the pattern walk; NEAREST, which exists only as the golden
// suite's A/B baseline, takes the pattern-driven code. Both are the same
// function to the caller.
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
// which is 240, every row of the panel.
//
// The tail moves axis with the walk. The row walk's tail is along its walk:
// the group leaves one source pixel over at the end of every row, emitted
// pure, and three divides 144 so nothing is ever left over vertically.
// Transposed, the along-column walk has no tail of its own and the leftover
// lands ACROSS the walk, as a whole output column: the last output column is
// the pure scale of source column 159 and belongs to no block.
// scaler_scale_col_tail() emits it, and writes nothing for a geometry whose
// group divides 160. What the row walk reaches by a tail, the column walk's
// along-axis reaches by a clamp — its last whole group's blend partner is
// past the end — and both leave that edge pure.
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
// Packed output: the _444 entry points emit RGB444, two pixels in three
// bytes, where their 565 counterparts emit one pixel in two. A quarter fewer
// bytes reach the panel for the same pixels, which is the whole of the point;
// the price is four bits per channel.
//
// The pack is the FINAL store, never the input to anything. Blending stays in
// native RGB565 for the reason the Byte order note below gives, so a packed
// call scales its source units into a caller-owned 565 scratch and packs each
// output unit out of it on the way to dst. That is why the scratch is
// (src_lines_per_block + 1) units where the 565 calls take one: a blend unit
// reads two scaled units that must both still exist.
//
// Nibble order is the panel's: pixels p0 and p1 share three bytes as
//
//   byte 0 = R0 G0     byte 1 = B0 R1     byte 2 = G1 B1
//
// with each channel the top four bits of its 565 field. So an output unit is
// unit_len * 3 / 2 bytes and MUST have an even unit_len, or the next one
// would start mid-byte; every geometry's dst_w and dst_h is even, and a
// partial column range whose output height is odd is rejected rather than
// packed into a layout nothing can address.
//
// BLEND only. SCALER_MODE_NEAREST exists as the golden suite's A/B baseline
// and nothing pushes it, so the packed entry points reject it rather than
// carrying a second kernel no frame reaches.
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
#define SCALER_DST_ROWS_MAX 5   /* most output rows per block (5/3)      */
#define SCALER_SRC_LINES_MAX 3  /* most source lines per block (5/3)     */

/* Scratch one packed call needs, in pixels: one 565 unit per source unit the
 * block consumes, plus one for the scaled lookahead. The row walk's unit is
 * the wider of the two. */
#define SCALER_SCRATCH_444_MAX ((SCALER_SRC_LINES_MAX + 1) * SCALER_DST_W_MAX)

/* Bytes n pixels occupy packed. n must be even. */
#define SCALER_PACKED_BYTES(n) ((size_t)(n) * 3u / 2u)

enum scaler_result_e {
    SCALER_OK = 0,
    SCALER_ERR_ARGS = -1, /* NULL buffer or unknown geometry / mode */
};

enum scaler_geom_e {
    SCALER_GEOM_5_3 = 0, /* 5/3: 160x144 -> 266x240 */
};

enum scaler_mode_e {
    SCALER_MODE_NEAREST = 0, /* interpolated unit duplicates left/top source */
    SCALER_MODE_BLEND = 1,   /* interpolated unit is the RGB565 average      */
};

/*
 * Which way a block's output columns are laid out in dst, for the transposed
 * walk. A panel's address window fills in one fixed direction, set by its
 * scan mapping and not by the caller, so if that direction runs against the
 * image's own axis the columns have to come out reversed. Choosing it here
 * costs a sign on a stride; doing it anywhere downstream costs moving every
 * pixel a second time.
 */
enum scaler_col_order_e {
    SCALER_COLS_ASCENDING = 0,  /* column i of the block at dst + i * dst_h */
    SCALER_COLS_DESCENDING = 1, /* column i at dst + (n - 1 - i) * dst_h    */
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
 *                  it, which at 5/3 it does — but it is always checked
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
 *   order          which end of dst the block's leftmost column goes to
 *
 * Returns SCALER_OK or SCALER_ERR_ARGS.
 */
int scaler_scale_col_block(enum scaler_geom_e geom, enum scaler_mode_e mode,
                           const uint16_t* const* src_cols,
                           const uint16_t* lookahead_col,
                           uint16_t* dst, uint16_t* scratch_col,
                           enum scaler_col_order_e order);

/*
 * A row RANGE of the transposed walk, for a consumer that writes the frame in
 * 2D tiles rather than whole columns: the same block, but only output rows
 * src_first / src_units * dst_units onward, src_rows / src_units *
 * dst_rows_per_block of them.
 *
 * src_cols and lookahead_col still point at whole SCALER_SRC_H columns; the
 * range selects within them. Both ends must land on a group boundary, or the
 * pattern's phase inside the group would differ from the whole-column walk's
 * and the slices would not join.
 *
 * The join is seamless rather than merely close. A blend at the end of a
 * range reads one row past it, which mid-column is the next range's first row
 * — a real pixel — so two ranges together produce exactly what one
 * whole-column call produces. Only at the column's true end is there nothing
 * to reach for, and there it clamps and the edge stays pure.
 *
 * scaler_scale_col_block() is this with the whole column.
 */
int scaler_scale_col_rows(enum scaler_geom_e geom, enum scaler_mode_e mode,
                          const uint16_t* const* src_cols,
                          const uint16_t* lookahead_col,
                          uint16_t* dst, uint16_t* scratch_col,
                          enum scaler_col_order_e order,
                          unsigned src_first, unsigned src_rows);

/*
 * Emit the transposed walk's tail: the SCALER_SRC_W % src_lines_per_block
 * source columns left over past the last whole block, each scaled along its
 * length into one pure output column of dst_h pixels. One column at 5/3. A
 * geometry whose group divides 160 leaves none, and there it returns
 * SCALER_OK having written nothing and src_cols and dst may be NULL.
 *
 *   src_cols  pointers to the leftover source columns, leftmost first
 *   dst       tail columns * dst_h pixels, column-major
 *
 * A tail column never blends across the walk: there is no next group to reach
 * toward, which is what makes the frame's far edge pure. It takes no column
 * order: the tail is one column or none, and one column has only one place to
 * go.
 */
int scaler_scale_col_tail(enum scaler_geom_e geom, enum scaler_mode_e mode,
                          const uint16_t* const* src_cols, uint16_t* dst);

/*
 * The packed counterparts of the four calls above: same geometry, same source
 * units, same column order, BLEND only, and dst is BYTES rather than pixels —
 * SCALER_PACKED_BYTES(pixels) of them, in the nibble order the Packed output
 * note documents. dst is a byte buffer in the signature as well as in the
 * prose, because a packed buffer sized as though it held uint16_t is three
 * quarters of the frame and the compiler is the cheapest place to catch it.
 *
 *   scratch  SCALER_SCRATCH_444_MAX pixels, required; holds every scaled
 *            source unit of the block and the scaled lookahead, which the
 *            packed units are then built from
 *
 * Returns SCALER_OK, or SCALER_ERR_ARGS for the 565 calls' reasons plus two
 * of its own: SCALER_MODE_NEAREST, and a column range whose output height is
 * odd and so has no byte-aligned layout.
 */
int scaler_scale_block_444(enum scaler_geom_e geom, enum scaler_mode_e mode,
                           const uint16_t* const* src_lines,
                           const uint16_t* lookahead_line,
                           uint8_t* dst, uint16_t* scratch);

int scaler_scale_col_block_444(enum scaler_geom_e geom,
                               enum scaler_mode_e mode,
                               const uint16_t* const* src_cols,
                               const uint16_t* lookahead_col,
                               uint8_t* dst, uint16_t* scratch,
                               enum scaler_col_order_e order);

int scaler_scale_col_rows_444(enum scaler_geom_e geom, enum scaler_mode_e mode,
                              const uint16_t* const* src_cols,
                              const uint16_t* lookahead_col,
                              uint8_t* dst, uint16_t* scratch,
                              enum scaler_col_order_e order,
                              unsigned src_first, unsigned src_rows);

int scaler_scale_col_tail_444(enum scaler_geom_e geom, enum scaler_mode_e mode,
                              const uint16_t* const* src_cols, uint8_t* dst,
                              uint16_t* scratch);

/*
 * Pack n finished RGB565 pixels into SCALER_PACKED_BYTES(n) bytes, in the
 * nibble order the Packed output note documents. n must be even.
 *
 * The walks above pack their own output and do not need this; it is here so a
 * caller that produces a line of 565 pixels some other way — a test fixture
 * pushing into the same window the frame path uses — reaches the panel's
 * format through the same code the frame does, rather than restating a nibble
 * order in a second place.
 */
void scaler_pack_444(uint8_t* dst, const uint16_t* src, unsigned n);

/*
 * Average of two native-bit-layout RGB565 pixels, per channel, without
 * unpacking. Callers must pass NATIVE (not byte-swapped) values.
 */
uint16_t scaler_avg565(uint16_t a, uint16_t b);

#ifdef __cplusplus
}
#endif
