#pragma once

// ─── Transposed scale cost (bench only) ─────────────────────────────────────
// The go/no-go number for the column-major push. Writing along the panel's
// gate lines means the scaler walks the frame's 144 axis and emits output
// columns, and the estimate says that is the same work rearranged: the ratio
// is 5/3 on both axes, and both the source and the destination live in
// internal SRAM, which the ESP32 does not cache, so a transposed walk should
// cost what a sequential one costs. That is an assumption, and everything
// after it is built on this number.
//
// So both arms run here, on one board in one sitting with the walk as the
// only variable, and each is split into its two halves:
//
//   lut    colourizing raw pixel bytes into RGB565 through the palette LUT.
//          The row arm does it a block at a time into six lines plus the
//          lookahead, exactly as the consumer does. The column arm has to
//          transpose while it colourizes — the producer receives lines as
//          they are rendered and cannot reorder them — so its stores are
//          strided by the frame height. Same store count, different pattern,
//          and whether that is free is half of what this measures.
//   scale  the scaler itself: 24 row blocks against 53 column blocks and the
//          tail column.
//
// Runs instead of the boot, before the tag read, so it needs no cartridge, no
// SD card and no display. The measurement itself sits on a task pinned to
// core 0 at the push task's priority, because core 0 is where the scaler
// really runs and core 0's budget is the question.
//
// The source frame is synthetic, not a game's. The cost here is
// data-independent — avg565 is branch-free, the pattern walk reads the same
// tables whatever the pixels are, and all 64 LUT entries fit in 128 bytes —
// so a frame of varied indices measures what dmg-acid2 measures, without
// needing a card in the slot.
//
//   PLATFORMIO_BUILD_FLAGS='-DSCALE_BENCH' pio run -e cyd-gnuboy -t upload
#ifdef SCALE_BENCH
void scale_bench_run();
#endif
