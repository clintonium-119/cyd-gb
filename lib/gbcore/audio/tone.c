#include <stddef.h>

#include "tone.h"

/* One full cycle, in the accumulator's units. */
#define TONE_PHASE_CYCLE 0x10000u
/* Halfway through a cycle: the sample sign flips here. */
#define TONE_PHASE_HALF  0x8000u

int tone_init(tone_state_t* t, uint16_t freq_hz, uint32_t rate_hz)
{
    if (t == NULL) {
        return TONE_ERR_ARGS;
    }
    if (rate_hz == 0 || freq_hz == 0) {
        return TONE_ERR_ARGS;
    }
    /* Compared doubled rather than halving the rate, so an odd sample rate
     * does not round the limit down and admit a frequency past Nyquist. */
    if ((uint32_t)freq_hz * 2u > rate_hz) {
        return TONE_ERR_ARGS;
    }

    t->phase = 0;
    t->step = (uint32_t)(((uint64_t)freq_hz * TONE_PHASE_CYCLE) / rate_hz);

    return TONE_OK;
}

int tone_fill(tone_state_t* t, int16_t amplitude, int16_t* stereo,
              size_t n_frames)
{
    size_t i;

    if (t == NULL || stereo == NULL) {
        return TONE_ERR_ARGS;
    }

    for (i = 0; i < n_frames; i++) {
        int16_t sample = (t->phase < TONE_PHASE_HALF) ? amplitude
                                                      : (int16_t)-amplitude;

        stereo[i * 2 + 0] = sample;
        stereo[i * 2 + 1] = sample;

        /* Masked, not modulo: the accumulator only ever holds a position
         * inside the current cycle, so a tone left running for hours cannot
         * overflow. */
        t->phase = (t->phase + t->step) & (TONE_PHASE_CYCLE - 1u);
    }

    return TONE_OK;
}
