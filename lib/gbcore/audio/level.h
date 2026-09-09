#pragma once
// Output-queue level estimate (design §4, the underflow counter).
//
// I2S in built-in-DAC mode reports neither a fill level nor a starvation
// event, so the queue depth the exit criterion asks about cannot be observed
// — it has to be derived from the writer's own clock. This module is that
// derivation and nothing else: it is told the sample rate and the queue's
// capacity once, is told how many samples each write handed over and when,
// and answers how many of them are likely still queued at a given instant.
//
// The estimate is written minus rate x elapsed, clamped to [0, capacity]. It
// is an estimate, not an observation: it assumes the DAC drains at exactly
// the nominal rate from the moment of the first write. The bench cross-check
// is a scope on the DAC pin.
//
// The speaker counts one underflow when level_running() is true and
// level_queued() is 0 immediately before a write — the queue ran dry while
// the emulator was still producing. A deliberate pause is not that, which is
// what level_reset() is for: it stops the clock, and the next write restarts
// it at its own timestamp.
//
// Times are microseconds because esp_timer_get_time() is the clock the
// speaker has. No clock is read here; the caller passes every instant in.
//
// Pure C, no Arduino/ESP-IDF headers, no floating point, no allocation.

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct level_s {
    uint32_t rate_hz;  /* samples the sink consumes per second     */
    uint32_t capacity; /* samples the queue holds when full        */
    uint64_t written;  /* samples handed over since the clock started */
    int64_t t0_us;     /* timestamp of the write that started it   */
    bool running;      /* false before the first write, and after a reset */
} level_t;

/* Zero the state and record the sink's rate and the queue's capacity. */
void level_init(level_t* l, uint32_t rate_hz, uint32_t capacity_samples);

/*
 * Stop the clock and forget what was written. The queue is assumed drained
 * or deliberately refilled by the caller; the next level_note_write() starts
 * a fresh clock at its own timestamp, so the pause is not counted as an
 * underflow.
 */
void level_reset(level_t* l);

/* Whether a write has started the clock. False for NULL. */
bool level_running(const level_t* l);

/*
 * Samples likely still queued at now_us. 0 before the first write and after
 * a reset. A now_us earlier than the clock's start counts as no elapsed
 * time rather than as a negative drain.
 */
uint32_t level_queued(const level_t* l, int64_t now_us);

/* Record a write of n_samples at now_us, starting the clock if it is stopped. */
void level_note_write(level_t* l, uint32_t n_samples, int64_t now_us);

#ifdef __cplusplus
}
#endif
