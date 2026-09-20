#pragma once

// ─── Panel rate trim (bench only) ───────────────────────────────────────────
// The tear is the emulator's audio-locked 59.7275 fps beating against the
// panel's own oscillator. PANEL_PROBE established that this panel drives
// nothing on MISO, so the firmware cannot read the scan position and no
// closed loop is possible. What is left is an open loop with the builder's
// eye in it: trim the panel's frame rate until the seam stops moving.
//
// The knob is ST7789's PORCTRL (0xB2) front porch, which lengthens the frame
// by whole lines — 1/344 of a frame, or about 0.17 Hz a step, which is
// coarser than the error we are chasing. So the porch alternates between two
// neighbouring values at a ratio the D-pad sets, a fractional divider that
// makes the average rate continuous: one part in 64 of a line, about
// 0.003 Hz, which parks the seam for minutes rather than seconds.
//
// Runs instead of the boot, needs no cartridge and no SD card, and never
// returns. Pushes through the real frame path at the real cadence so the bus
// occupies the same share of every frame that a game does.
//
//   Left / Right  coarse: front porch +/- 1 line
//   Down / Up     fine:   dither ratio +/- 1/64 of a line
//   A             print the current state
//   B             back to the panel's power-on porch
//
//   PLATFORMIO_BUILD_FLAGS='-DPANEL_TRIM' pio run -e cyd-gnuboy
#ifdef PANEL_TRIM
void panel_trim_run();
#endif
