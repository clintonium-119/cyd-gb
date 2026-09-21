#pragma once
// The panel rate trim's arithmetic: porch to refresh rate, rate back to
// porch, and the fractional divider that makes a whole-line register express
// a fractional line count.
//
// The tear this exists to park is the emulator's audio-paced cadence beating
// against the panel's free-running refresh. The panel answers no reads, so
// the beat cannot be servoed; it can be nulled, by lengthening the frame with
// ST7789's PORCTRL front porch until the two rates agree. One line of porch
// is about 0.17 Hz, which is coarser than the error being cancelled, so the
// porch alternates between neighbouring values and the average is fractional
// even though every frame's is not.
//
// The anchor is a parameter, never a constant here. It is one porch setting
// whose true rate is known, and it is per unit — it measures one panel's
// oscillator error, which no register reports and the datasheet does not give
// — so a number pinned in this file would be one board's measurement wearing
// a library's authority.
//
// Pure C, no Arduino headers, no allocation: every caller owns its state.

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Lines in a frame that the front porch does not contribute: the panel's 320
 * active lines plus its back porch and pulse width at this project's PORCTRL
 * defaults. The rate scales inversely with the total, so this is the term
 * that decides how much a line of porch is worth.
 */
#define PANEL_RATE_BASE_LINES 332.0

/* 64ths of a line is what the divider can express, so it is what the porch
 * solver rounds to and what the caller stores. */
#define PANEL_RATE_RATIO_DEN 64

/*
 * One porch setting whose true rate is known. Per unit, measured by eye
 * against a null — the only observable this hardware offers.
 */
typedef struct {
    uint8_t fpa;    /* front porch, whole lines                 */
    uint8_t ratio;  /* plus this many 64ths of a line           */
    double hz;      /* the refresh rate that pair really gives  */
} panel_anchor_t;

/* The rate a porch setting gives, relative to the anchor's. */
double panel_rate_hz(const panel_anchor_t* a, uint8_t fpa, uint8_t ratio);

/*
 * The porch that would put the panel on `hz`, rounded to 64ths of a line.
 * False when the answer is outside what PORCTRL can express, in which case
 * *fpa and *ratio are untouched: a cadence that far off is not a trim's to
 * fix.
 */
bool panel_rate_porch_for(const panel_anchor_t* a, double hz, uint8_t* fpa,
                          uint8_t* ratio);

/*
 * One frame of the fractional divider. Returns the porch to program for this
 * frame: `fpa` on 64 - ratio frames out of every 64, and fpa + 1 on the other
 * `ratio`, so the long-run average line count is fpa + ratio/64.
 *
 * `acc` is the caller's whole state, one byte, and starts at 0. A separate
 * accumulator per caller is deliberate — the diagnostic page runs the divider
 * on a cadence of its own, and sharing one would make each spend the other's
 * frames on the long porch.
 */
uint8_t panel_rate_next_porch(uint8_t* acc, uint8_t fpa, uint8_t ratio);

#ifdef __cplusplus
}
#endif
