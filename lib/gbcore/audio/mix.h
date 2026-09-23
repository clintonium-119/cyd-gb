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

/*
 * Fast-forward time-stretch (WSOLA). Two emulated frames go into one frame of
 * audio at their own pitch: the dropped frame plays up to MIX_WSOLA_SEAM,
 * then crossfades over MIX_WSOLA_WINDOW frames into the kept frame at
 * MIX_WSOLA_SEAM + d, and the kept frame plays out from there. The shift d is
 * chosen so the two waveforms line up across the join, which is what keeps
 * fast music from turning rough. The frame comes out n - d samples long.
 * Chosen by ear against a plain crossfade, a longer one, a same-index splice
 * and bigger slices, on host renders of Pokemon Red and Black Castle.
 */
#define MIX_WSOLA_SEAM   200
#define MIX_WSOLA_WINDOW 128
#define MIX_WSOLA_SHIFT  240

/*
 * Search for the shift. ref is the dropped frame's MIX_WSOLA_WINDOW stereo
 * frames at the seam, kept the kept frame's n_kept. The range is clamped to
 * +/-MIX_WSOLA_SHIFT, and every d from d_min in steps of 2 whose window fits
 * inside kept is scored by normalised
 * correlation of the mono mix, less a small penalty on |carry - d|: carry is
 * the caller's running sum of -d, so the penalty is what keeps the output's
 * total length on real time. Ties go to the first, most negative, d.
 *
 * Returns MIX_OK with *out_d set, or MIX_ERR_ARGS for a NULL pointer or when
 * no d in the range fits, with *out_d untouched.
 */
int mix_wsola_shift(const int16_t* ref, const int16_t* kept, size_t n_kept,
                    int32_t d_min, int32_t d_max, int32_t carry,
                    int32_t* out_d);

/*
 * Fade the head of a run of stereo in from another, in place: the join's
 * crossfade. Frame 0 comes out as `from` and the weight moves linearly to the
 * run's own samples, reached at n_frames. Both buffers hold 2 * n_frames
 * samples, interleaved. Returns MIX_OK, or MIX_ERR_ARGS for a NULL buffer
 * without writing anything. n_frames == 0 is a no-op.
 */
int mix_crossfade_in(int16_t* stereo, const int16_t* from, size_t n_frames);

#ifdef __cplusplus
}
#endif
