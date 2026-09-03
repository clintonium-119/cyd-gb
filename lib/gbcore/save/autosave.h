#pragma once
// Cartridge-RAM dirty tracker and flush policy (design §7).
//
// The emulator writes cartridge RAM far more often than the RAM is worth
// saving, so the decision of *when* to write it to the SD card is separated
// from the writing. This module owns that decision and nothing else: it is
// told the cartridge's real save size once, is told that a write landed, is
// ticked once a frame with a timestamp, and answers questions.
//
// The split between the write and the timestamp is deliberate. The write
// notice comes from the emulator's cartridge-RAM callback, which is IRAM
// resident and runs per access, so it may not read a clock: it sets a flag
// and returns. The per-frame tick turns that flag into the dirty state and
// stamps it with the frame's time, which is at worst one frame stale and is
// what the idle rule measures from.
//
// `wrote` is written by the cartridge-RAM callback and read by the tick, and
// both run on the emulation task, so no `volatile` and no atomic is needed.
//
// Four things ask for a flush (design §7): the idle rule below, the menu
// opening, a reset, and the battery crossing its low threshold. Only the
// first and the last are decided here; the other two are events the caller
// already knows about. There is deliberately no maximum-dirty-age rule — a
// game that writes cartridge RAM continuously never goes idle and never
// flushes, which is a measurement to take on hardware rather than a policy
// to guess at.
//
// Pure C, no Arduino/ESP-IDF headers, no allocation.

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* How long cartridge RAM must go unwritten before it is worth saving
 * (design §7, "idle ~10 s"). */
#define AUTOSAVE_IDLE_MS 10000

enum autosave_result_e {
    AUTOSAVE_OK = 0,
    AUTOSAVE_ERR_ARGS = -1, /* NULL state */
};

typedef struct autosave_state_s {
    uint32_t save_size;     /* cartridge's real save size; 0 = nothing to save */
    uint8_t wrote;          /* a write landed inside save_size since the tick  */
    uint8_t dirty;          /* the RAM differs from what is on the card        */
    uint32_t last_write_ms; /* tick time of the most recent write              */
    uint8_t batt_low;       /* the low-battery flush has already fired         */
} autosave_state_t;

/*
 * Zero the state and record the cartridge's save size. A save_size of 0 means
 * this cartridge has nothing to save — no write ever dirties it, so a cart
 * whose header declares no RAM, or whose RAM-size code was not recognised,
 * costs nothing but the compare in autosave_note_write().
 *
 * Returns AUTOSAVE_OK or AUTOSAVE_ERR_ARGS.
 */
int autosave_init(autosave_state_t* s, uint32_t save_size);

/*
 * Note that a write landed at addr. Called from the emulator's cartridge-RAM
 * write callback, which is why it is inline here rather than a call into
 * flash-resident code: one compare against save_size and one byte store on
 * the in-range path, the compare alone on the out-of-range path. Writes at or
 * past the real save size are the cartridge's unmapped mirror region and are
 * not worth saving.
 *
 * No clock is read and no state but `wrote` is touched — autosave_tick() does
 * the rest.
 */
static inline void autosave_note_write(autosave_state_t* s, uint32_t addr)
{
    if (addr < s->save_size) {
        s->wrote = 1;
    }
}

/*
 * Once per frame, with the frame's timestamp. A tick that follows at least
 * one write marks the RAM dirty and stamps it; a tick with no write behind it
 * changes nothing, which is what leaves the idle deadline where it was.
 */
void autosave_tick(autosave_state_t* s, uint32_t now_ms);

/* Whether the RAM differs from what is on the card. False for NULL. */
bool autosave_dirty(const autosave_state_t* s);

/*
 * Whether the idle rule is satisfied: dirty, and AUTOSAVE_IDLE_MS has passed
 * since the stamped write. The difference is taken signed so the rule
 * survives the caller's ms counter rolling over rather than parking the save
 * for another 49 days. False for NULL.
 */
bool autosave_idle_due(const autosave_state_t* s, uint32_t now_ms);

/*
 * After a flush that failed. The RAM stays dirty — it has not reached the
 * card — but the idle clock restarts, so the retry waits a full idle period
 * instead of firing on every frame and freezing the game against a bad card.
 */
void autosave_defer(autosave_state_t* s, uint32_t now_ms);

/* After a flush that succeeded. */
void autosave_flushed(autosave_state_t* s);

/*
 * Whether the battery has just crossed below low_mv. True exactly once per
 * crossing: the first reading below low_mv latches, every reading while
 * latched is false, and the latch re-arms only once a reading reaches
 * low_mv + hyst_mv — so a cell sagging under load does not flush every
 * second.
 *
 * Independent of the dirty state on purpose: the latch tracks the voltage,
 * and the caller's flush is already a no-op on clean RAM. RAM dirtied later
 * under a still-low battery is caught by the idle rule.
 *
 * False for NULL.
 */
bool autosave_battery(autosave_state_t* s, uint16_t mv, uint16_t low_mv,
                      uint16_t hyst_mv);

#ifdef __cplusplus
}
#endif
