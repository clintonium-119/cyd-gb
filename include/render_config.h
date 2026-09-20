#pragma once
#include "hw_config.h"

// Render geometry: which of the scaler's three geometries this image is built
// for, and the game-area rectangle that follows from it.

// ─── Geometry ───────────────────────────────────────────────────────────────
// Compile-time constant, deliberately NOT a runtime setting: the scaler's
// block geometry and every buffer size derive from it. 24/16 was built first
// per §2.1; 26/16 is what ships, on ws/perf26's measurement that its fixed
// 13/8 kernel holds 60 fps with about 20 % of core 0 spare, and on the core
// comparison that followed, every capture of which was taken here.
//
// k/16 was a packaging convenience, not a limit of the panel: 160 and 144 are
// both divisible by 16, so any scale of that form gives integer output on both
// axes, and 9k <= 240 then stops at k = 26 — six rows short. Dropping the
// constraint costs one pixel. 144 x 5/3 is 240 exactly, and the width follows
// at 266: 53 groups of 3 source pixels to 5 output, plus one leftover pixel
// emitted pure. Square pixels, a 0.25 % aspect error, every row of the panel.
//
// All three are compiled into gbcore; this header picks one. 26/16 stays the
// default until the bench rules on 5/3.
//
//   PLATFORMIO_BUILD_FLAGS=-DRENDER_GEOM=GEOM_5_3 pio run -e cyd-gnuboy
#define GEOM_24_16 2416
#define GEOM_26_16 2616
#define GEOM_5_3    503

#ifndef RENDER_GEOM
#define RENDER_GEOM GEOM_26_16
#endif

// The gbcore geometry that matches RENDER_GEOM, so Arduino-side callers never
// repeat the numbers. SCALE_GEOM expands to an enum name — the consumer
// includes render/scaler.h itself; this header stays free of gbcore.
#if RENDER_GEOM == GEOM_24_16
#define SCALE_GEOM SCALER_GEOM_24_16   // 3/2:  240 x 216, 90% of the rows
#define UNIT_LINES 2                   // source lines one scaler unit consumes
#define UNIT_ROWS  3                   // output rows it emits
#define GAME_W     240
#define GAME_H     216
#elif RENDER_GEOM == GEOM_26_16
#define SCALE_GEOM SCALER_GEOM_26_16   // 13/8: 260 x 234, 97.5% of the rows
#define UNIT_LINES 8
#define UNIT_ROWS  13
#define GAME_W     260
#define GAME_H     234
#elif RENDER_GEOM == GEOM_5_3
#define SCALE_GEOM SCALER_GEOM_5_3     // 5/3:  266 x 240, every row
#define UNIT_LINES 3
#define UNIT_ROWS  5
#define GAME_W     266
#define GAME_H     240
#else
#error "RENDER_GEOM must be GEOM_24_16, GEOM_26_16 or GEOM_5_3."
#endif

// ─── Frame queue block ──────────────────────────────────────────────────────
// Scaler units per queue block (design §3.3). One unit is the scaler's own
// block — 2 source lines to 3 rows at 24/16 — so a frame is GB_SCREEN_H /
// UNIT_LINES units, and a block of BLOCK_UNITS of them is one DMA transfer.
// Fewer transfers cut the per-transfer overhead in push; the price is two DMA
// buffers of BLOCK_UNITS units each in static DRAM. Must divide the frame; the
// value is measured on the bench, not chosen.
//
// Lives here rather than in hw_config.h because its default now depends on the
// geometry, and hw_config.h is included above — before RENDER_GEOM exists.
#ifndef BLOCK_UNITS
#if RENDER_GEOM == GEOM_5_3
// 2 at 5/3. A unit is 3 source lines, so 48 units a frame and 1 would mean 48
// DMA transfers against today's 18; 2 gives 24, closest to today's count, and
// costs LESS static DRAM than 26/16 does — 10,640 B of dma_buf against 13,520,
// with slot_src and lut_lines shrinking from 9 lines to 7 alongside.
#define BLOCK_UNITS   2
#else
// 1 at 26/16, and forced rather than chosen: a scaler unit is 8 source lines
// at that geometry and 4 does not divide 144, so the build refuses. 2 would
// halve the per-transfer overhead but wants about 27 KB of DMA buffers, which
// ws/perf26 priced and rejected.
#define BLOCK_UNITS   1
#endif
#endif

// UNIT_LINES / UNIT_ROWS repeat the geometry table's numbers because static
// buffer sizes need them at compile time; emu_init() checks the table agrees.
#define BLOCK_LINES (UNIT_LINES * BLOCK_UNITS)   // raw source lines per block
#define BLOCK_ROWS  (UNIT_ROWS * BLOCK_UNITS)    // output rows per block
#if (GB_SCREEN_H % BLOCK_LINES) != 0
#error "BLOCK_UNITS must divide the frame: GB_SCREEN_H % (UNIT_LINES * BLOCK_UNITS) must be 0"
#endif

// ─── Game area ──────────────────────────────────────────────────────────────
// Each geometry states its own GAME_W / GAME_H above, because 5/3's width is
// not 160 x k/16 for any k. 24/16 and 26/16 still land on §2.1's table: 240 x
// 216 at GAME_X/Y 40,12 and 260 x 234 at 30,3. GAME_X / GAME_Y are
// compile-time defaults only: NVS overrides them at runtime through
// settings_t.game_x / settings_t.game_y, because ten hand-built units each
// land slightly differently behind the bezel (design §2.2). At 5/3 the image
// is the full 240 rows, so GAME_Y is 0 and the vertical nudge has nowhere to
// go.
#define GAME_X   ((SCREEN_W - GAME_W) / 2)      // Centred (design §2.2); §11 item 5 (pixel pitch) may move it.
#define GAME_Y   ((SCREEN_H - GAME_H) / 2)      // Centred (design §2.2); §11 item 5 (pixel pitch) may move it.
