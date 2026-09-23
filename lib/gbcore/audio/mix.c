#include <string.h>

#include "mix.h"

/*
 * Gain per volume level, as 8.8 fixed point. Entry 0 is unused: level 0 is
 * off and never reaches the multiply. Level 8 is full scale, and each rung
 * below is about 4 dB down from the next.
 *
 * These are a bench-tuned knob, not a derivation. The bottom rung is meant
 * to be far quieter than the old three-step Low (96/256); at 10/256 roughly
 * 3.3 bits of signal remain above the dither, so retune by ear rather than
 * by arithmetic.
 */
static const uint16_t vol_lut[MIX_VOL_MAX + 1] = {
    0, 10, 16, 26, 40, 64, 102, 161, 256
};

/* Any non-zero constant works; this is the usual xorshift32 seed. */
#define MIX_SEED_FALLBACK 0x2545F491u

static inline uint32_t xorshift32(uint32_t* state)
{
    uint32_t x = *state;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    *state = x;
    return x;
}

void mix_init(mix_state_t* s, uint32_t seed)
{
    if (s == NULL) {
        return;
    }
    s->lfsr = (seed != 0) ? seed : MIX_SEED_FALLBACK;
}

int mix_mono(mix_state_t* s, const int16_t* stereo, size_t n_frames,
             uint8_t vol_index, uint8_t* out)
{
    size_t i;

    if (s == NULL || stereo == NULL || out == NULL) {
        return MIX_ERR_ARGS;
    }
    if (vol_index > MIX_VOL_MAX) {
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

        if (mono != 0) {
            scaled += (int32_t)(xorshift32(&s->lfsr) & 0x1FFu) - 256;
        }

        if (scaled > 32767) {
            scaled = 32767;
        } else if (scaled < -32768) {
            scaled = -32768;
        }

        /* Bias into unsigned range first, then truncate: the shift is on a
         * value known to be in [0, 65535], so nothing is implementation-
         * defined and the result is already the DAC's mid-scale encoding. */
        out[i] = (uint8_t)(((uint32_t)(scaled + 32768)) >> 8);
    }

    return MIX_OK;
}
