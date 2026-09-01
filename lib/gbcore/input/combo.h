#pragma once
// Input debounce + combo state machine (design §5).
//
// The caller samples the raw button word once per frame (one bit per button,
// active-high) and feeds it with a millisecond timestamp. This module owns
// debounce, combo detection and key repeat; it has no hardware access and no
// side effects. Two things come out:
//
//   * a stream of combo events from combo_update(), at most one per call;
//   * the debounced, masked joypad word from combo_joypad(), which is the
//     only word that should ever reach the emulator.
//
// Acting on an event — NVS writes, backlight PWM, opening the menu — is the
// caller's job. combo_step_u8() is here rather than in firmware only because
// "clamps, never wraps" is worth pinning with host tests.
//
// Pure C, no Arduino/ESP-IDF headers, no allocation.

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Button bits. Same order and values as the firmware's GB_BTN_* masks and the
 * Game Boy joypad register, which is what lets combo_joypad()'s result go
 * straight to the emulator. Mirrored here rather than included so this module
 * stays free of firmware headers; the two lists must not drift.
 */
#define COMBO_BTN_RIGHT  0x01
#define COMBO_BTN_LEFT   0x02
#define COMBO_BTN_UP     0x04
#define COMBO_BTN_DOWN   0x08
#define COMBO_BTN_A      0x10
#define COMBO_BTN_B      0x20
#define COMBO_BTN_SELECT 0x40
#define COMBO_BTN_START  0x80

/* The expander has no hardware filter, so 8 ms of stability is the gate. */
#define COMBO_DEBOUNCE_MS 8

/* Held adjustment combos repeat: nothing for the first 400 ms, then one
 * event every 200 ms. The menu combo never repeats. */
#define COMBO_REPEAT_DELAY_MS 400
#define COMBO_REPEAT_MS 200

enum combo_result_e {
    COMBO_OK = 0,
    COMBO_ERR_ARGS = -1, /* NULL state or event pointer */
};

/*
 * Combo events. VOL_UP means louder and BRIGHT_UP means brighter; how that
 * maps onto a stored value is the caller's business (the volume setting is
 * indexed 0 = high .. 3 = off, so louder decrements it).
 */
enum combo_event_e {
    COMBO_EVENT_NONE = 0,
    COMBO_EVENT_MENU,         /* Start + Select, one-shot per press  */
    COMBO_EVENT_VOL_UP,       /* Select + Up                         */
    COMBO_EVENT_VOL_DOWN,     /* Select + Down                       */
    COMBO_EVENT_BRIGHT_UP,    /* Select + Right                      */
    COMBO_EVENT_BRIGHT_DOWN,  /* Select + Left                       */
};

typedef struct combo_state_s {
    uint16_t stable_word;        /* debounced button word                     */
    uint16_t raw_word;           /* last raw sample                           */
    uint32_t last_change_ms;     /* timestamp of the last raw change          */
    uint32_t repeat_due_ms;      /* when the held combo's next event is due   */
    uint8_t active_dir;          /* held adjustment direction bit, 0 = none   */
    uint8_t menu_latch;          /* menu fired; held until the pair releases  */
} combo_state_t;

/* Zero the state machine. Returns COMBO_OK or COMBO_ERR_ARGS. */
int combo_init(combo_state_t* s);

/*
 * Feed one raw button-word sample taken at now_ms and read back the detected
 * event (enum combo_event_e) through out_event. Events come only from the
 * debounced word, and at most one is reported per call: the menu combo wins
 * when it lands on the same call as an adjustment event, and the adjustment
 * event is then reported on the following call.
 *
 * Returns COMBO_OK, or COMBO_ERR_ARGS with *out_event untouched.
 */
int combo_update(combo_state_t* s, uint16_t raw_word, uint32_t now_ms,
                 uint8_t* out_event);

/*
 * The debounced word to hand the emulator: the D-pad is masked out while
 * Select is held, so adjusting volume or brightness does not walk the
 * character around, and Start + Select are masked while the menu combo is
 * latched. Returns 0 for a NULL state.
 */
uint8_t combo_joypad(const combo_state_t* s);

/*
 * Step val by step in the direction of dir (> 0 up, < 0 down, 0 no move) and
 * clamp to [min, max]. Saturating, never wrapping, so an adjustment held
 * against either end sits there. A val that starts outside the range is
 * clamped into it first, which is how a stale out-of-range stored value
 * repairs itself on the first adjustment. Returns val unchanged if min > max.
 */
uint8_t combo_step_u8(uint8_t val, int8_t dir, uint8_t min, uint8_t max,
                      uint8_t step);

#ifdef __cplusplus
}
#endif
