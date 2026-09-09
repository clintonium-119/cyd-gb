#pragma once
// Audio mix — one frame of the APU's interleaved stereo output becomes the
// unsigned 8-bit mono stream the built-in DAC takes (design §4).
//
// Per sample, in order:
//
//   1. sum left and right and halve. Never drop a channel: games pan effects
//      hard, and a channel pick would silence them.
//   2. scale in the 16-bit domain by vol_lut[vol_index] / 256. Scaling after
//      truncation would throw away the low bits the quiet steps live in.
//   3. add uniform dither in [-256, 255] — one output LSB either way — but
//      only when the summed sample is non-zero, so silence stays exactly
//      MIX_SILENCE and the always-live amplifier does not flicker 127/129.
//   4. clamp to int16 range, then take the high byte biased to mid-scale.
//
// Two invariants the host suite pins: zero in gives exactly MIX_SILENCE out
// at every volume index, and a dithered sample is within 1 of the same
// sample undithered.
//
// MIX_VOL_OFF is not a hardware mute — none exists on this board — it is
// MIX_SILENCE written for every sample, which parks the DAC at mid-scale.
//
// Pure C, no Arduino/ESP-IDF headers, no allocation: all buffers are
// caller-owned.

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Volume index. Same encoding settings_t::volume stores — an index into the
 * {high, med, low} table, with the step past the end meaning off, so louder
 * counts down towards MIX_VOL_HIGH. The firmware static-asserts that the two
 * lists agree; changing one means changing both.
 */
#define MIX_VOL_HIGH 0
#define MIX_VOL_MED  1
#define MIX_VOL_LOW  2
#define MIX_VOL_OFF  3

/* Unsigned mid-scale: silence, and where the DAC parks. */
#define MIX_SILENCE 128u

enum mix_result_e {
    MIX_OK = 0,
    MIX_ERR_ARGS = -2, /* NULL buffer, or vol_index past MIX_VOL_OFF */
};

/*
 * Dither generator state. One xorshift32 word; the caller owns it and keeps
 * it across frames so the noise does not restart every 16.7 ms.
 */
typedef struct mix_state_s {
    uint32_t lfsr;
} mix_state_t;

/*
 * Seed the dither generator. A zero seed is replaced by a fixed non-zero
 * constant, because xorshift32 sticks at zero forever.
 */
void mix_init(mix_state_t* s, uint32_t seed);

/*
 * Mix one frame.
 *
 *   s          dither state, advanced once per non-zero sample; untouched
 *              when vol_index is MIX_VOL_OFF
 *   stereo     2 * n_frames samples, interleaved left then right
 *   n_frames   samples per channel
 *   vol_index  MIX_VOL_HIGH .. MIX_VOL_OFF
 *   out        n_frames bytes
 *
 * Returns MIX_OK, or MIX_ERR_ARGS without writing anything.
 */
int mix_mono(mix_state_t* s, const int16_t* stereo, size_t n_frames,
             uint8_t vol_index, uint8_t* out);

#ifdef __cplusplus
}
#endif
