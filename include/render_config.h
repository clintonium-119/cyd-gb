#pragma once
#include "hw_config.h"

// Render geometry: the scale this image is built for, and the game-area
// rectangle that follows from it.

// ─── Geometry ───────────────────────────────────────────────────────────────
// Compile-time constant, deliberately NOT a runtime setting: the scaler's
// block geometry and every buffer size derive from it.
//
// 5/3 is the only scale, and it is 266 x 240 — every row of the panel. The
// design reached it by dropping a constraint rather than by finding a better
// ratio: the earlier geometries were k/16, which 160 and 144 are both
// divisible by, so any scale of that form gave integer output on both axes,
// and 9k <= 240 then stopped at k = 26, six rows short of the panel. Dropping
// the constraint costs one pixel. 144 x 5/3 is 240 exactly, and the width
// follows at 266: 53 groups of 3 source pixels to 5 output, plus one leftover
// pixel emitted pure. Square pixels, a 0.25 % aspect error, and the bezel
// aperture clears all 240 rows.
//
// It was chosen on one board in one sitting against 26/16, the geometry that
// shipped before it, on the same fixture with the geometry as the only
// variable: frame peak 13.8 ms of a 16.67 ms budget against 12.4, fps 60 on
// both, aover 0 on both, and emu_core within 0.2 % which is what makes the two
// captures comparable. scale grew 1.5 % for 4.93 % more pixels; push grew
// 8.2 %, the excess being 24 DMA transfers a frame against 18. That comparison
// is settled and the losing arms are gone, so RENDER_GEOM has one legal value.
// It stays a named token rather than becoming inline numbers because the
// numbers below are duplicated from gbcore's geometry table for the sake of
// compile-time buffer sizes, and emu_init() cross-checks the two.
#define GEOM_5_3 503

#ifndef RENDER_GEOM
#define RENDER_GEOM GEOM_5_3
#endif

// The gbcore geometry that matches RENDER_GEOM, so Arduino-side callers never
// repeat the numbers. SCALE_GEOM expands to an enum name — the consumer
// includes render/scaler.h itself; this header stays free of gbcore.
#if RENDER_GEOM == GEOM_5_3
#define SCALE_GEOM SCALER_GEOM_5_3     // 5/3: 266 x 240, every row
#define UNIT_LINES 3                   // source lines one scaler unit consumes
#define UNIT_ROWS  5                   // output rows it emits
#define GAME_W     266
#define GAME_H     240
#else
#error "RENDER_GEOM must be GEOM_5_3."
#endif

// ─── Frame queue block ──────────────────────────────────────────────────────
// Scaler units per queue block (design §3.3). One unit is the scaler's own
// block — 3 source lines to 5 rows — so a frame is GB_SCREEN_H / UNIT_LINES
// units, and a block of BLOCK_UNITS of them is one DMA transfer. Fewer
// transfers cut the per-transfer overhead in push; the price is two DMA
// buffers of BLOCK_UNITS units each in static DRAM. Must divide the frame; the
// value is measured on the bench, not chosen.
//
// Lives here rather than in hw_config.h because it reads against UNIT_LINES,
// which is defined above and which hw_config.h — included before it — knows
// nothing about.
#ifndef BLOCK_UNITS
// A unit is 3 source lines, so there are 48 units a frame and 1 would mean 48
// DMA transfers where 26/16 needed 18; 2 gives 24, closest to that count, and
// costs LESS static DRAM than 26/16 did — 10,640 B of dma_buf against 13,520,
// with slot_src and lut_lines shrinking from 9 lines to 7 alongside.
#define BLOCK_UNITS   2
#endif

// ─── Column block (PUSH_COL only) ───────────────────────────────────────────
// Scaler column-blocks per DMA transfer. One of them is UNIT_ROWS output
// columns of the full GAME_H, and the count is chosen so a transfer lands near
// the row order's, because the per-transfer overhead is a measured quantity on
// this board and not a guess: the row order runs 24 transfers of 2,660 pixels
// a frame, and 2 units gives 10 columns of 240 — 2,400 pixels, 27 transfers —
// while costing LESS DMA buffer than the row order does, 9,600 B against
// 10,640 B.
//
// This one need not divide the frame the way BLOCK_UNITS must. The column
// order hands a whole frame over in a single queue block and the consumer
// walks its own blocks out of a buffer that is stable for the frame, so the
// last transfer is simply shorter: 53 scaler blocks and one tail column come
// out as 26 transfers of 10 columns and one of 6.
#ifndef COL_BLOCK_UNITS
#define COL_BLOCK_UNITS 2
#endif

#define COL_BLOCK_COLS (UNIT_ROWS * COL_BLOCK_UNITS)  // output cols / transfer
#define COL_BLOCK_SRC  (UNIT_LINES * COL_BLOCK_UNITS) // source cols / transfer
// The leftover source columns past the last whole block, one per output
// column. Three does not divide 160, so there is exactly one.
#define COL_TAIL_COLS  (GB_SCREEN_W % UNIT_LINES)

#if COL_BLOCK_COLS < (UNIT_ROWS + COL_TAIL_COLS)
#error "COL_BLOCK_UNITS must leave one transfer room for a block plus the tail."
#endif

// UNIT_LINES / UNIT_ROWS repeat the geometry table's numbers because static
// buffer sizes need them at compile time; emu_init() checks the table agrees.
#define BLOCK_LINES (UNIT_LINES * BLOCK_UNITS)   // raw source lines per block
#define BLOCK_ROWS  (UNIT_ROWS * BLOCK_UNITS)    // output rows per block
#if (GB_SCREEN_H % BLOCK_LINES) != 0
#error "BLOCK_UNITS must divide the frame: GB_SCREEN_H % (UNIT_LINES * BLOCK_UNITS) must be 0"
#endif

// ─── Push order ─────────────────────────────────────────────────────────────
// Which axis the frame is written along. The panel's 320 gate lines run along
// the screen's horizontal under setRotation(1), so a row-major push crosses
// the refresh perpendicularly and smears the tear into a descending diagonal
// staircase; writing along the scan collapses that to a single vertical line,
// absent altogether on the boots whose phase leaves the write ahead of the
// scan for a whole frame.
//
// Both walks are compiled and selected here, the way RENDER_GEOM once
// selected among three geometries and for the same reason: the trade is one
// frame of input latency for a change in what the artefact looks like, and
// that cannot be judged from a description. It needs both behaviours from one
// environment so the comparison has one variable.
//
// PUSH_COL costs a second copy of the indexed frame — one per queue slot, on
// the heap — and one frame of latency, because an output column needs every
// source row and so the push cannot overlap emulation of the same frame.
//
// Select the other order per invocation:
//
//   PLATFORMIO_BUILD_FLAGS=-DPUSH_ORDER=PUSH_COL pio run -e cyd-gnuboy
#define PUSH_ROW 0
#define PUSH_COL 1

// PUSH_COL is the default, settled on the bench of 2026-09-21 — and only in
// company with the panel's reversed scan order. On its own it produces a seam
// permanently on screen; together with PANEL_SCAN_REVERSE, one that is mostly
// absent. Both halves are needed, and the pairing explains why: a row-major
// write sweeps the gate axis once per SCREEN ROW, 240 times a frame, so it
// crosses the scan whichever way the scan runs and reversing it only changes
// which way the artefact leans. Column-major is the geometry that makes the
// write cross the gate axis once, monotonically, which is the only
// arrangement in which running WITH the scan is something it can do.
#ifndef PUSH_ORDER
#define PUSH_ORDER PUSH_COL
#endif

#if PUSH_ORDER != PUSH_ROW && PUSH_ORDER != PUSH_COL
#error "PUSH_ORDER must be PUSH_ROW or PUSH_COL."
#endif

// A name rather than the comparison spelled out at a dozen guards: the
// transposed order shares the whole producer half — the second indexed frame,
// the per-line transpose, the frame handed over in one queue block — and
// differs only in how the consumer walks it.
#define PUSH_TRANSPOSED (PUSH_ORDER == PUSH_COL)

// ─── Game area ──────────────────────────────────────────────────────────────
// The geometry states its own GAME_W / GAME_H above rather than deriving them,
// because 5/3's width is not 160 x k/16 for any k and the old derived formula
// would silently give the wrong one. GAME_X / GAME_Y are compile-time defaults
// only: NVS overrides them at runtime through settings_t.game_x /
// settings_t.game_y, because ten hand-built units each land slightly
// differently behind the bezel (design §2.2). The image is the full 240 rows,
// so GAME_Y is 0 and the vertical nudge has nowhere to go; it is the
// horizontal one that earns its keep.
#define GAME_X   ((SCREEN_W - GAME_W) / 2)      // Centred (design §2.2); §11 item 5 (pixel pitch) may move it.
#define GAME_Y   ((SCREEN_H - GAME_H) / 2)      // Centred (design §2.2); §11 item 5 (pixel pitch) may move it.
