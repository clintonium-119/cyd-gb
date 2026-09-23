#include <string.h>

#include "mix.h"

/*
 * Gain per volume index, as 8.8 fixed point. Entry 0 is unused: Off never
 * reaches the multiply.
 *
 * Bench-tuned by ear on 2026-09-23, not derived. Low (24/256, about -21 dB)
 * is the quietest step that still sounded clean on the 8-bit DAC; 16 was
 * already mush. High (432/256, about +4.5 dB) goes past unity because the
 * emulator's loudest output measured 37-58% of full scale across Pokemon Red,
 * Black Castle and Tobu Tobu Girl, so it clips nothing there; a louder game
 * is caught by the clamp. Med is their geometric mean, halfway in loudness.
 */
static const uint16_t vol_lut[MIX_VOL_HIGH + 1] = { 0, 24, 102, 432 };

int mix_mono(const int16_t* stereo, size_t n_frames, uint8_t vol_index,
             uint8_t* out)
{
    size_t i;

    if (stereo == NULL || out == NULL) {
        return MIX_ERR_ARGS;
    }
    if (vol_index > MIX_VOL_HIGH) {
        return MIX_ERR_ARGS;
    }

    if (vol_index == MIX_VOL_OFF) {
        memset(out, (int)MIX_SILENCE, n_frames);
        return MIX_OK;
    }

    for (i = 0; i < n_frames; i++) {
        int32_t left = stereo[i * 2 + 0];
        int32_t right = stereo[i * 2 + 1];
        /* Divide rather than shift: a right shift of a negative value is
         * implementation-defined, and it would also turn a sum of -1 into
         * -1 instead of 0, so hard-panned opposites would not cancel. */
        int32_t mono = (left + right) / 2;
        int32_t scaled = (mono * (int32_t)vol_lut[vol_index]) / 256;
        uint32_t biased;

        if (scaled > 32767) {
            scaled = 32767;
        } else if (scaled < -32768) {
            scaled = -32768;
        }

        /* Bias into unsigned range, add half an output step and truncate:
         * round to nearest. Everything is in [0, 65663], so nothing is
         * implementation-defined; only the top of the range rounds past 255,
         * and it is held there. */
        biased = ((uint32_t)(scaled + 32768) + 128u) >> 8;
        out[i] = (uint8_t)(biased > 255u ? 255u : biased);
    }

    return MIX_OK;
}

int mix_crossfade_in(int16_t* stereo, const int16_t* from, size_t n_frames)
{
    size_t i;

    if (stereo == NULL || from == NULL) {
        return MIX_ERR_ARGS;
    }

    for (i = 0; i < n_frames * 2; i++) {
        int32_t w = (int32_t)(i / 2);
        /* A weighted mean of two int16s is an int16; the product is at most
         * 32768 * n_frames, far inside int32 for any frame this sees. */
        stereo[i] = (int16_t)(((int32_t)from[i] * ((int32_t)n_frames - w)
                               + (int32_t)stereo[i] * w)
                              / (int32_t)n_frames);
    }

    return MIX_OK;
}
