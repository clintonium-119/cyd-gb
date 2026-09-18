#pragma once
// Frame stretch — linear resample of one frame of unsigned 8-bit mono.
//
// The emulator hands the speaker exactly one frame of samples per emulated
// frame, so the sample stream's rate is bound to the emulated frame rate. A
// title core 1 cannot run at full speed therefore supplies fewer samples per
// second than the DAC consumes, the DMA queue runs dry, and the I2S chain
// re-clocks its last buffer — a 16.7 ms fragment repeating, which is heard as
// a continuous buzz over the music.
//
// Stretching the frame is what decouples the two: handing the queue n_out
// samples where the APU produced n_in buys back the shortfall, at the cost of
// pitch. A 6 % stretch is about a semitone flat and the game is already
// running that slow, so nothing is lost that the player has not lost already.
//
// Linear interpolation, not sample duplication: repeating one sample every k
// is a periodic artefact at the output rate over k — a tone laid across the
// music — and holding the last sample instead moves that tone to the frame
// rate. Interpolation has neither.
//
// n_out == n_in is an exact copy, so the caller pays nothing on the common
// path where no stretch is needed.
//
// Pure C, no Arduino/ESP-IDF headers, no floating point, no allocation.

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define STRETCH_OK 0
#define STRETCH_ERR_ARGS -1

/*
 * Resample in[0 .. n_in) onto out[0 .. n_out). Both counts must be at least
 * 2 and the buffers must not overlap. The first and last samples are
 * preserved exactly; everything between is interpolated between its two
 * neighbours. Returns STRETCH_ERR_ARGS for a NULL buffer or a count below 2,
 * writing nothing.
 */
int stretch_mono(const uint8_t* in, size_t n_in, uint8_t* out, size_t n_out);

#ifdef __cplusplus
}
#endif
