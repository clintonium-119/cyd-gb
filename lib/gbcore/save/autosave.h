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
// what the save rule measures from.
//
// `wrote` is written by the cartridge-RAM callback and read by the tick, and
// both run on the emulation task, so no `volatile` and no atomic is needed.
//
// Four things ask for a save: the save rule below, the menu opening, a reset
// and a return to the games list. Only the first is decided here; the others
// are events the caller already knows about. There is no low-battery save:
// the board has no battery sense (BUG-0003).
//
// The rule has two arms because the library has two kinds of game. Most go
// quiet once a burst of writes ends — a player's save is one such burst — so
// a save follows AUTOSAVE_QUIET_MS after the last write. Some never go quiet
// (Super Mario Land 2 and Wario Land change cartridge RAM on nearly every
// frame), so a save also follows AUTOSAVE_MAX_AGE_MS after the RAM first went
// dirty. Neither arm can tell a player's save from a game's scratch use, and
// neither needs to: the caller saves in the background, so a save the player
// did not make costs a card write and nothing else (BUG-0025).
//
// "Dirty" means the RAM differs from the caller's last copy of it, not from
// the card: the caller marks it clean when it takes the copy, and defers it
// if that copy then fails to reach the card.
//
// Pure C, no Arduino/ESP-IDF headers, no allocation.

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* How long cartridge RAM must go unwritten before a save: short, because a
 * child switches off as soon as the game says it has saved. */
#define AUTOSAVE_QUIET_MS 500
/* The longest RAM stays dirty without a save, for the games that never go
 * quiet; also how long a failed save waits before the retry. */
#define AUTOSAVE_MAX_AGE_MS 10000

enum autosave_result_e {
    AUTOSAVE_OK = 0,
    AUTOSAVE_ERR_ARGS = -1, /* NULL state */
};

typedef struct autosave_state_s {
    uint32_t save_size;     /* cartridge's real save size; 0 = nothing to save */
    uint8_t wrote;          /* a write landed inside save_size since the tick  */
    uint8_t dirty;          /* the RAM differs from what is on the card        */
    uint32_t last_write_ms; /* tick time of the most recent write              */
    uint32_t dirty_since_ms;/* tick time the RAM went from clean to dirty      */
    uint8_t held;           /* a failed save is waiting out hold_until_ms      */
    uint32_t hold_until_ms; /* no save is due before this                      */
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
 * changes nothing, which is what leaves the quiet deadline where it was.
 */
void autosave_tick(autosave_state_t* s, uint32_t now_ms);

/* Whether the RAM differs from what is on the card. False for NULL. */
bool autosave_dirty(const autosave_state_t* s);

/*
 * Whether a save is due: dirty, not held, and either AUTOSAVE_QUIET_MS has
 * passed since the stamped write or AUTOSAVE_MAX_AGE_MS since the RAM went
 * dirty. Every difference is taken signed so the rule survives the caller's
 * ms counter rolling over rather than parking the save for another 49 days.
 * False for NULL.
 */
bool autosave_due(const autosave_state_t* s, uint32_t now_ms);

/*
 * After a save that failed. The RAM is dirty again — the copy did not reach
 * the card — and no save is due for AUTOSAVE_MAX_AGE_MS, so a bad card costs
 * one attempt every ten seconds rather than one every frame.
 */
void autosave_defer(autosave_state_t* s, uint32_t now_ms);

/* After the caller took its copy of the RAM: clean, and any hold lifted. */
void autosave_flushed(autosave_state_t* s);

#ifdef __cplusplus
}
#endif
