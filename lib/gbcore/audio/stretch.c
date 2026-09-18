#include <stddef.h>

#include "stretch.h"

/* 16.16 fixed point: the step is at most (n_in - 1) << 16 over one output
 * frame, which for any frame this project pushes stays far inside 32 bits. */
#define STRETCH_FRAC_BITS 16
#define STRETCH_ONE (1u << STRETCH_FRAC_BITS)
#define STRETCH_FRAC_MASK (STRETCH_ONE - 1u)

int stretch_mono(const uint8_t* in, size_t n_in, uint8_t* out, size_t n_out)
{
    uint32_t step;
    uint32_t pos;
    size_t j;

    if (in == NULL || out == NULL || n_in < 2 || n_out < 2) {
        return STRETCH_ERR_ARGS;
    }

    /* Both endpoints pinned: position runs 0 .. (n_in - 1) across
     * 0 .. (n_out - 1) output samples, so out[0] == in[0] and the last
     * output sample lands exactly on the last input one. */
    step = (uint32_t)(((uint64_t)(n_in - 1) << STRETCH_FRAC_BITS)
                      / (uint32_t)(n_out - 1));
    pos = 0;

    for (j = 0; j < n_out; j++) {
        size_t i = (size_t)(pos >> STRETCH_FRAC_BITS);
        uint32_t frac = pos & STRETCH_FRAC_MASK;

        if (i >= n_in - 1) {
            /* The final sample, and the guard that keeps the lookahead read
             * below inside the buffer when rounding puts the position on the
             * last input sample early. */
            out[j] = in[n_in - 1];
        } else {
            int32_t a = (int32_t)in[i];
            int32_t b = (int32_t)in[i + 1];

            out[j] = (uint8_t)(a + (((b - a) * (int32_t)frac)
                                    >> STRETCH_FRAC_BITS));
        }
        pos += step;
    }

    /* The step is truncated, so the accumulated position stops just short of
     * the last input sample and the final output would land a fraction of a
     * sample early. Frames are pushed back to back, so pin it: an endpoint
     * that drifts leaves a step at every frame boundary, which is the tick
     * this module exists to avoid. */
    out[n_out - 1] = in[n_in - 1];

    return STRETCH_OK;
}
