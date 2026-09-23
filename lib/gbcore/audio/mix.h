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
//   3. clamp to int16 range, then round to the nearest high byte, biased to
//      mid-scale.
//
// No dither. The emulator's output sits well off zero, so dither that skipped
// exact silence still ran all the time, and on the 8-bit DAC it was a hiss at
// a fixed level whatever the volume. At the quiet step it swamped the music;
// the bench preferred plain rounding at every step.
//
// The invariant the host suite pins: zero in gives exactly MIX_SILENCE out at
// every volume index.
//
// MIX_VOL_OFF is not a hardware mute — none exists on this board — it is
// MIX_SILENCE written for every sample, which parks the DAC at mid-scale.
//
// Pure C, no Arduino/ESP-IDF headers, no allocation, no state: all buffers
// are caller-owned.

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Volume index. Same encoding settings_t::volume stores — Off, then three
 * steps, so a bigger number is louder. The firmware static-asserts that the
 * two lists agree; changing one means changing both.
 */
#define MIX_VOL_OFF  0
#define MIX_VOL_LOW  1
#define MIX_VOL_MED  2
#define MIX_VOL_HIGH 3

/* Unsigned mid-scale: silence, and where the DAC parks. */
#define MIX_SILENCE 128u

enum mix_result_e {
    MIX_OK = 0,
    MIX_ERR_ARGS = -2, /* NULL buffer, or vol_index past MIX_VOL_HIGH */
};

/*
 * Mix one frame.
 *
 *   stereo     2 * n_frames samples, interleaved left then right
 *   n_frames   samples per channel
 *   vol_index  MIX_VOL_OFF .. MIX_VOL_HIGH
 *   out        n_frames bytes
 *
 * Returns MIX_OK, or MIX_ERR_ARGS without writing anything.
 */
int mix_mono(const int16_t* stereo, size_t n_frames, uint8_t vol_index,
             uint8_t* out);

/* Stereo frames a fast-forward seam is crossfaded over: about 2 ms. */
#define MIX_XFADE_FRAMES 64

/*
 * Fade the head of one frame of stereo in from another, in place, for
 * fast-forward. It plays one emulated frame's samples of every two at their
 * own rate, so the pitch holds. `from` is the head of the frame that was
 * dropped, which carries on seamlessly from the previous output, so fading
 * from it into the kept frame leaves no jump on either side of the join.
 *
 * Frame 0 comes out as `from`, and the weight moves linearly to the frame's
 * own samples, reached at n_frames. Both buffers hold 2 * n_frames samples,
 * interleaved. Returns MIX_OK, or MIX_ERR_ARGS for a NULL buffer without
 * writing anything. n_frames == 0 is a no-op.
 */
int mix_crossfade_in(int16_t* stereo, const int16_t* from, size_t n_frames);

#ifdef __cplusplus
}
#endif
