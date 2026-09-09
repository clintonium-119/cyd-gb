#include <stddef.h>

#include "level.h"

#define LEVEL_US_PER_S 1000000u

void level_init(level_t* l, uint32_t rate_hz, uint32_t capacity_samples)
{
    if (l == NULL) {
        return;
    }
    l->rate_hz = rate_hz;
    l->capacity = capacity_samples;
    l->written = 0;
    l->t0_us = 0;
    l->running = false;
}

void level_reset(level_t* l)
{
    if (l == NULL) {
        return;
    }
    l->written = 0;
    l->t0_us = 0;
    l->running = false;
}

bool level_running(const level_t* l)
{
    return (l != NULL) && l->running;
}

uint32_t level_queued(const level_t* l, int64_t now_us)
{
    int64_t elapsed_us;
    uint64_t consumed;
    uint64_t queued;

    if (l == NULL || !l->running) {
        return 0;
    }

    elapsed_us = now_us - l->t0_us;
    if (elapsed_us < 0) {
        elapsed_us = 0;
    }

    /* 64-bit throughout: a multi-hour session reaches 10^10 microseconds,
     * and the product with the rate is past 10^14. */
    consumed = ((uint64_t)elapsed_us * (uint64_t)l->rate_hz) / LEVEL_US_PER_S;
    if (consumed >= l->written) {
        return 0;
    }

    queued = l->written - consumed;
    if (queued > (uint64_t)l->capacity) {
        return l->capacity;
    }
    return (uint32_t)queued;
}

void level_note_write(level_t* l, uint32_t n_samples, int64_t now_us)
{
    if (l == NULL) {
        return;
    }
    if (!l->running) {
        l->t0_us = now_us;
        l->running = true;
    }
    l->written += n_samples;
}
