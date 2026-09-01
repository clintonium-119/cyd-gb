#include "combo.h"

#include <stddef.h>

#define DPAD_MASK (COMBO_BTN_UP | COMBO_BTN_DOWN | COMBO_BTN_LEFT | COMBO_BTN_RIGHT)
#define MENU_MASK (COMBO_BTN_START | COMBO_BTN_SELECT)

/*
 * The direction the caller is holding, checked in a fixed order so a sloppy
 * two-direction press resolves the same way every time. Whichever direction
 * is picked keeps the combo until it is released, even if another goes down
 * meanwhile.
 */
static const uint8_t dir_order[4] = {
    COMBO_BTN_UP,
    COMBO_BTN_DOWN,
    COMBO_BTN_LEFT,
    COMBO_BTN_RIGHT,
};

static uint8_t event_for_dir(uint8_t dir)
{
    switch (dir) {
    case COMBO_BTN_UP:
        return (uint8_t)COMBO_EVENT_VOL_UP;
    case COMBO_BTN_DOWN:
        return (uint8_t)COMBO_EVENT_VOL_DOWN;
    case COMBO_BTN_RIGHT:
        return (uint8_t)COMBO_EVENT_BRIGHT_UP;
    case COMBO_BTN_LEFT:
        return (uint8_t)COMBO_EVENT_BRIGHT_DOWN;
    default:
        return (uint8_t)COMBO_EVENT_NONE;
    }
}

int combo_init(combo_state_t* s)
{
    if (s == NULL) {
        return COMBO_ERR_ARGS;
    }
    s->stable_word = 0;
    s->raw_word = 0;
    s->last_change_ms = 0;
    s->repeat_due_ms = 0;
    s->active_dir = 0;
    s->menu_latch = 0;
    return COMBO_OK;
}

/*
 * Debounce: any change to the raw word restarts the clock, and the word is
 * only promoted to stable_word once it has been sampled unchanged for
 * COMBO_DEBOUNCE_MS. A glitch shorter than that is therefore never visible to
 * combo detection at all, because it is replaced before it can settle.
 */
static void combo_debounce(combo_state_t* s, uint16_t raw_word, uint32_t now_ms)
{
    if (raw_word != s->raw_word) {
        s->raw_word = raw_word;
        s->last_change_ms = now_ms;
        return;
    }
    if (s->raw_word == s->stable_word) {
        return;
    }
    if ((uint32_t)(now_ms - s->last_change_ms) >= (uint32_t)COMBO_DEBOUNCE_MS) {
        s->stable_word = s->raw_word;
    }
}

/*
 * Start + Select, one-shot. The latch is what makes it one-shot: it is set
 * when the pair goes down and only cleared once the pair is no longer both
 * held, so a long hold produces exactly one event.
 */
static uint8_t combo_menu(combo_state_t* s)
{
    if ((s->stable_word & MENU_MASK) != MENU_MASK) {
        s->menu_latch = 0;
        return (uint8_t)COMBO_EVENT_NONE;
    }
    if (s->menu_latch) {
        return (uint8_t)COMBO_EVENT_NONE;
    }
    s->menu_latch = 1;
    return (uint8_t)COMBO_EVENT_MENU;
}

/*
 * Select + a direction, edge-triggered then repeating. The deadline is set
 * from now_ms rather than advanced by COMBO_REPEAT_MS from its previous value,
 * so a stall — the menu was open, a frame ran long — costs one late step
 * instead of a burst of catch-up events.
 */
static uint8_t combo_adjust(combo_state_t* s, uint32_t now_ms)
{
    uint16_t dpad = (uint16_t)(s->stable_word & DPAD_MASK);
    size_t i;

    if ((s->stable_word & COMBO_BTN_SELECT) == 0 || dpad == 0) {
        s->active_dir = 0;
        return (uint8_t)COMBO_EVENT_NONE;
    }

    if (s->active_dir != 0 && (dpad & s->active_dir) != 0) {
        if ((int32_t)(now_ms - s->repeat_due_ms) < 0) {
            return (uint8_t)COMBO_EVENT_NONE;
        }
        s->repeat_due_ms = now_ms + (uint32_t)COMBO_REPEAT_MS;
        return event_for_dir(s->active_dir);
    }

    for (i = 0; i < sizeof(dir_order) / sizeof(dir_order[0]); i++) {
        if ((dpad & dir_order[i]) != 0) {
            s->active_dir = dir_order[i];
            s->repeat_due_ms = now_ms + (uint32_t)COMBO_REPEAT_DELAY_MS;
            return event_for_dir(s->active_dir);
        }
    }
    return (uint8_t)COMBO_EVENT_NONE;
}

int combo_update(combo_state_t* s, uint16_t raw_word, uint32_t now_ms,
                 uint8_t* out_event)
{
    uint8_t event;

    if (s == NULL || out_event == NULL) {
        return COMBO_ERR_ARGS;
    }

    combo_debounce(s, raw_word, now_ms);

    /*
     * Menu first, and it returns before the adjustment machine runs at all.
     * That is what gives it priority: the adjustment edge or due repeat is
     * still there on the next call, since the debounced word has not moved.
     */
    event = combo_menu(s);
    if (event == (uint8_t)COMBO_EVENT_NONE) {
        event = combo_adjust(s, now_ms);
    }

    *out_event = event;
    return COMBO_OK;
}

uint8_t combo_joypad(const combo_state_t* s)
{
    uint16_t word;

    if (s == NULL) {
        return 0;
    }
    word = s->stable_word;
    if ((word & COMBO_BTN_SELECT) != 0) {
        word = (uint16_t)(word & ~(uint16_t)DPAD_MASK);
    }
    if (s->menu_latch) {
        word = (uint16_t)(word & ~(uint16_t)MENU_MASK);
    }
    return (uint8_t)(word & 0xFFu);
}

uint8_t combo_step_u8(uint8_t val, int8_t dir, uint8_t min, uint8_t max,
                      uint8_t step)
{
    int32_t v;

    if (min > max) {
        return val;
    }
    v = (int32_t)val;
    if (v < (int32_t)min) {
        v = (int32_t)min;
    }
    if (v > (int32_t)max) {
        v = (int32_t)max;
    }
    if (dir > 0) {
        v += (int32_t)step;
    } else if (dir < 0) {
        v -= (int32_t)step;
    }
    if (v < (int32_t)min) {
        v = (int32_t)min;
    }
    if (v > (int32_t)max) {
        v = (int32_t)max;
    }
    return (uint8_t)v;
}
