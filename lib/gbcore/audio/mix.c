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
     * renders the method was chosen on, not tuned on the bench: 0.0002 let
     * the running length wander by about 200 ms, 0.002 holds it to 10. */
    const float carry_weight = 0.002f;
    /* Both signals go to mono once, not once per shift: the search visits
     * about 200 shifts, and recomputing the windows each time cost 2.7 ms
     * a frame on the device. On the stack: 1.4 KB. */
    int16_t rm[MIX_WSOLA_WINDOW];
    int16_t km[MIX_WSOLA_SEAM + MIX_WSOLA_SHIFT + MIX_WSOLA_WINDOW];
    int32_t rr = 0;
    int32_t yy = 0;
    int32_t s_lo;
    int32_t s_hi;
    int32_t s;
    int32_t best_d = 0;
    float best = 0.0f;
    size_t i;

    if (ref == NULL || kept == NULL || out_d == NULL) {
        return MIX_ERR_ARGS;
    }
    if (d_min < -MIX_WSOLA_SHIFT) {
        d_min = -MIX_WSOLA_SHIFT;
    }
    if (d_max > MIX_WSOLA_SHIFT) {
        d_max = MIX_WSOLA_SHIFT;
    }

    /* The shifts whose window fits, stepping by 2 from d_min: the first s
     * at or past 0 on d_min's parity, the last whose window ends by n_kept. */
    s_lo = (int32_t)MIX_WSOLA_SEAM + d_min;
    if (s_lo < 0) {
        s_lo += ((-s_lo + 1) / 2) * 2;
    }
    s_hi = (int32_t)MIX_WSOLA_SEAM + d_max;
    if ((int64_t)s_hi + MIX_WSOLA_WINDOW > (int64_t)n_kept) {
        s_hi = (int32_t)n_kept - MIX_WSOLA_WINDOW;
    }
    if (s_hi < s_lo) {
        return MIX_ERR_ARGS;
    }

    for (i = 0; i < MIX_WSOLA_WINDOW; i++) {
        rm[i] = (int16_t)mono12(&ref[i * 2]);
        rr += (int32_t)rm[i] * rm[i];
    }
    for (i = 0; i < (size_t)(s_hi - s_lo) + MIX_WSOLA_WINDOW; i++) {
        km[i] = (int16_t)mono12(&kept[((size_t)s_lo + i) * 2]);
    }
    for (i = 0; i < MIX_WSOLA_WINDOW; i++) {
        yy += (int32_t)km[i] * km[i];
    }

    for (s = s_lo; s <= s_hi; s += 2) {
        const int16_t* y = &km[s - s_lo];
        int32_t d = s - (int32_t)MIX_WSOLA_SEAM;
        int32_t ry = 0;
        float score;

        for (i = 0; i < MIX_WSOLA_WINDOW; i++) {
            ry += (int32_t)rm[i] * y[i];
        }
        /* The +1 keeps silence from dividing by zero; it scores 0 there, so
         * the carry penalty alone picks the shift. */
        score = (float)ry / sqrtf((float)rr * (float)yy + 1.0f)
                - carry_weight * (float)abs(carry - d);
        if (s == s_lo || score > best) {
            best = score;
            best_d = d;
        }
        /* Slide the window's energy on by two frames. */
        if (s + 2 <= s_hi) {
            yy += (int32_t)y[MIX_WSOLA_WINDOW] * y[MIX_WSOLA_WINDOW]
                  + (int32_t)y[MIX_WSOLA_WINDOW + 1] * y[MIX_WSOLA_WINDOW + 1]
                  - (int32_t)y[0] * y[0] - (int32_t)y[1] * y[1];
        }
    }

    *out_d = best_d;
    return MIX_OK;
}
