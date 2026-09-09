#pragma once
// Test tone — one frame of interleaved stereo int16 samples in the same shape
// the APU emits, so the diagnostic audio page can push a known signal through
// mix_mono() and speaker_write_frame() and prove the volume path end to end.
//
// A square wave, not a sine: on a small speaker it is unambiguous by ear, and
// every sample is exactly +/- the amplitude, which lets the host suite assert
// equality rather than a tolerance.
//
// Volume "off" is not a hardware mute — none exists on this board — it is the
// mixer writing mid-scale for every sample. Demonstrating that is the whole
// reason the tone goes through the mixer instead of straight to the DAC.
//
// The phase accumulator is caller-owned state, so a tone held across many
// frames has no seam at the frame boundary.
//
// Pure C, no Arduino/ESP-IDF headers, no floating point, no allocation: the
// output buffer belongs to the caller.

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * The test signal's own definition, not hardware: these two live with the
 * generator rather than in hw_config.h because no bench measurement settles
 * them.
 *
 * 440 Hz is high enough to come through a small speaker and low enough that
 * the square's harmonics stay inside the DAC's band at 32768 Hz.
 *
 * 6000 is about 18 % of full scale. The loudest volume step scales by
 * 256/256, so the mixer's dither can move a sample by one output LSB without
 * the result reaching either rail — a test tone that clipped would sound
 * like a fault rather than reporting one.
 */
#define TONE_HZ         440
#define TONE_AMPLITUDE  6000

enum tone_result_e {
    TONE_OK = 0,
    TONE_ERR_ARGS = -1, /* NULL state or buffer, zero rate or frequency, or
                         * a frequency past the Nyquist limit */
};

/*
 * Phase accumulator. `phase` is a position inside the current cycle in
 * 1/65536ths of a cycle and wraps in its low 16 bits; `step` is how far one
 * frame advances it, in the same units.
 */
typedef struct tone_state_s {
    uint32_t phase;
    uint32_t step;
} tone_state_t;

/*
 * Set up a tone.
 *
 *   t         accumulator, zeroed and given its step
 *   freq_hz   tone frequency
 *   rate_hz   the stream's sample rate
 *
 * Returns TONE_OK, or TONE_ERR_ARGS without writing anything when t is NULL,
 * either rate is zero, or freq_hz is at or past half the sample rate.
 */
int tone_init(tone_state_t* t, uint16_t freq_hz, uint32_t rate_hz);

/*
 * Fill one frame.
 *
 *   t          accumulator, advanced once per frame
 *   amplitude  peak sample value; every sample is +amplitude or -amplitude
 *   stereo     2 * n_frames samples, interleaved left then right
 *   n_frames   samples per channel
 *
 * Left and right are identical: the mixer sums and halves them, so a mono
 * tone written to both channels arrives at the DAC at its own amplitude.
 *
 * Returns TONE_OK, or TONE_ERR_ARGS without writing anything.
 */
int tone_fill(tone_state_t* t, int16_t amplitude, int16_t* stereo,
              size_t n_frames);

#ifdef __cplusplus
}
#endif
