#pragma once
// Input debounce + combo state machine seam — WS-05 fills the real logic.
//
// The caller samples the raw button word (one bit per button, active-high)
// and feeds it with a millisecond timestamp; the module owns debounce and
// combo detection. No hardware access here.
//
// Placeholder semantics (this workstream): combo_init() zeroes the state
// and returns COMBO_OK; combo_update() rejects every update with
// COMBO_ERR_NOT_IMPLEMENTED and reports COMBO_EVENT_NONE.
//
// Pure C, no Arduino/ESP-IDF headers, no allocation.

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum combo_result_e {
    COMBO_OK = 0,
    COMBO_ERR_NOT_IMPLEMENTED = -1, /* stub: state machine not implemented */
    COMBO_ERR_ARGS = -2,            /* NULL state or event pointer */
};

enum combo_event_e {
    COMBO_EVENT_NONE = 0, /* WS-05 adds the real combo events */
};

typedef struct combo_state_s {
    uint16_t stable_word;    /* debounced button word */
    uint16_t raw_word;       /* last raw sample */
    uint32_t last_change_ms; /* timestamp of the last raw change */
} combo_state_t;

/* Zero the state machine. Returns COMBO_OK or COMBO_ERR_ARGS. */
int combo_init(combo_state_t* s);

/*
 * Feed one raw button-word sample taken at now_ms. Writes the detected
 * event (enum combo_event_e) to *out_event. WS-05 fills debounce + combo
 * detection.
 */
int combo_update(combo_state_t* s, uint16_t raw_word, uint32_t now_ms,
                 uint8_t* out_event);

#ifdef __cplusplus
}
#endif
