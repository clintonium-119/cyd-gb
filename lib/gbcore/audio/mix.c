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

/* Scale, clamp and round one mono sample to the DAC byte. */
static uint8_t mix_sample(int32_t mono, uint8_t vol_index)
{
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
    return (uint8_t)(biased > 255u ? 255u : biased);
}

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
        out[i] = mix_sample((left + right) / 2, vol_index);
    }

    return MIX_OK;
}

int mix_mono_half(const int16_t* stereo, size_t n_frames, uint8_t vol_index,
                  uint8_t* out)
{
    size_t n_out = n_frames / 2;
    size_t k;

    if (stereo == NULL || out == NULL) {
        return MIX_ERR_ARGS;
    }
    if (vol_index > MIX_VOL_HIGH) {
        return MIX_ERR_ARGS;
    }

    if (vol_index == MIX_VOL_OFF) {
        memset(out, (int)MIX_SILENCE, n_out);
        return MIX_OK;
    }

    for (k = 0; k < n_out; k++) {
        const int16_t* p = &stereo[k * 4];
        /* Four samples, divided for the same reason as mix_mono(); a
         * constant input therefore gives mix_mono()'s bytes exactly. */
        out[k] = mix_sample(((int32_t)p[0] + p[1] + p[2] + p[3]) / 4,
                            vol_index);
    }

    return MIX_OK;
}
