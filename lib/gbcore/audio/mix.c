#include <math.h>
#include <stdlib.h>
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

/* One stereo frame as a 12-bit mono value: small enough that a window's
 * products sum inside int32 (128 * 2048 * 2048 < 2^31). */
static int32_t mono12(const int16_t* p)
{
    return ((int32_t)p[0] + (int32_t)p[1]) / 32;
}

int mix_wsola_shift(const int16_t* ref, const int16_t* kept, size_t n_kept,
                    int32_t d_min, int32_t d_max, int32_t carry,
                    int32_t* out_d)
{
    /* Correlation units per sample of length error. Taken from the host
     * renders the method was chosen on, not tuned on the bench. */
    const float carry_weight = 0.002f;
    int32_t rr = 0;
    int32_t d;
    int found = 0;
    int32_t best_d = 0;
    float best = 0.0f;
    size_t i;

    if (ref == NULL || kept == NULL || out_d == NULL) {
        return MIX_ERR_ARGS;
    }

    for (i = 0; i < MIX_WSOLA_WINDOW; i++) {
        int32_t r = mono12(&ref[i * 2]);
        rr += r * r;
    }

    for (d = d_min; d <= d_max; d += 2) {
        int32_t s = (int32_t)MIX_WSOLA_SEAM + d;
        int32_t ry = 0;
        int32_t yy = 0;
        float score;

        if (s < 0 || (size_t)s + MIX_WSOLA_WINDOW > n_kept) {
            continue;
        }
        for (i = 0; i < MIX_WSOLA_WINDOW; i++) {
            int32_t r = mono12(&ref[i * 2]);
            int32_t y = mono12(&kept[((size_t)s + i) * 2]);
            ry += r * y;
            yy += y * y;
        }
        /* The +1 keeps silence from dividing by zero; it scores 0 there, so
         * the carry penalty alone picks the shift. */
        score = (float)ry / sqrtf((float)rr * (float)yy + 1.0f)
                - carry_weight * (float)abs(carry - d);
        if (!found || score > best) {
            found = 1;
            best = score;
            best_d = d;
        }
    }

    if (!found) {
        return MIX_ERR_ARGS;
    }
    *out_d = best_d;
    return MIX_OK;
}
