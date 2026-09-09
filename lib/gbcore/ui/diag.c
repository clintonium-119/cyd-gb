#include <stddef.h>

#include "audio/mix.h"
#include "input/combo.h"
#include "ui/diag.h"

/* The four direction bits, as one mask. */
#define DIAG_DPAD_MASK \
    (COMBO_BTN_UP | COMBO_BTN_DOWN | COMBO_BTN_LEFT | COMBO_BTN_RIGHT)

static const char* const page_titles[DIAG_PAGE_COUNT] = {
    "Buttons",
    "SD card",
    "NFC tag",
    "Battery",
    "Audio",
    "Display",
    "Nudge",
    "System",
};

static int16_t clamp_i16(int16_t v, int16_t lo, int16_t hi)
{
    if (v < lo) {
        return lo;
    }
    if (v > hi) {
        return hi;
    }
    return v;
}

/*
 * Arriving at a page can be work in itself: the tag inspector looks once on
 * entry rather than polling, which is the whole reason the reader is quiet
 * unless someone asked it a question.
 */
static uint16_t on_enter(diag_t* d)
{
    if (d->page == DIAG_PAGE_NFC) {
        return DIAG_EV_NFC_SCAN;
    }
    return 0;
}

/*
 * Leaving a page has to undo anything the page left running, or a builder
 * walking through all eight would end up with a tone playing under the system
 * page with nothing on screen to explain it.
 */
static uint16_t on_leave(diag_t* d)
{
    if (d->page == DIAG_PAGE_AUDIO && d->tone_on) {
        d->tone_on = false;
        return DIAG_EV_TONE;
    }
    return 0;
}

static uint16_t change_page(diag_t* d, int8_t dir)
{
    uint16_t ev = DIAG_EV_REDRAW | DIAG_EV_PAGE;

    ev |= on_leave(d);

    if (dir > 0) {
        d->page = (uint8_t)((d->page + 1u) % DIAG_PAGE_COUNT);
    } else {
        d->page = (uint8_t)((d->page + DIAG_PAGE_COUNT - 1u)
                            % DIAG_PAGE_COUNT);
    }

    /* A direction still held across the switch belongs to the page that is
     * gone. Clearing it means the first press on the new page acts at once
     * instead of inheriting a deadline. */
    d->held_dir = 0;

    ev |= on_enter(d);

    return ev;
}

int diag_init(diag_t* d, int16_t panel_w, int16_t panel_h,
              int16_t win_w, int16_t win_h, int16_t x, int16_t y,
              int16_t default_x, int16_t default_y,
              uint8_t volume, uint8_t frameskip)
{
    if (d == NULL) {
        return DIAG_ERR_ARGS;
    }
    if (win_w <= 0 || win_h <= 0 || panel_w <= 0 || panel_h <= 0) {
        return DIAG_ERR_ARGS;
    }
    if (win_w > panel_w || win_h > panel_h) {
        return DIAG_ERR_ARGS;
    }

    d->page = DIAG_PAGE_BUTTONS;
    d->x_max = (int16_t)(panel_w - win_w);
    d->y_max = (int16_t)(panel_h - win_h);
    d->x = clamp_i16(x, 0, d->x_max);
    d->y = clamp_i16(y, 0, d->y_max);
    d->default_x = clamp_i16(default_x, 0, d->x_max);
    d->default_y = clamp_i16(default_y, 0, d->y_max);
    d->held_dir = 0;
    d->repeat_due_ms = 0;
    d->prev_word = 0;
    d->tone_on = false;
    d->volume = combo_step_u8(volume, 0, MIX_VOL_HIGH, MIX_VOL_OFF, 1);
    d->pattern = DIAG_PATTERN_BARS;
    d->frameskip = combo_step_u8(frameskip, 0, 0, DIAG_FRAMESKIP_MAX, 1);
    d->toast_until_ms = 0;
    d->toast = false;

    return DIAG_OK;
}

/* The nudge page: the D-pad moves the window a pixel at a time and clamps at
 * the panel edges, which is the only place on the panel a builder can reach
 * with no computer attached. */
static uint16_t nudge_step(diag_t* d, uint8_t dir_bits)
{
    int16_t before_x = d->x;
    int16_t before_y = d->y;

    switch (dir_bits) {
    case COMBO_BTN_RIGHT:
        d->x = clamp_i16((int16_t)(d->x + 1), 0, d->x_max);
        break;
    case COMBO_BTN_LEFT:
        d->x = clamp_i16((int16_t)(d->x - 1), 0, d->x_max);
        break;
    case COMBO_BTN_DOWN:
        d->y = clamp_i16((int16_t)(d->y + 1), 0, d->y_max);
        break;
    case COMBO_BTN_UP:
        d->y = clamp_i16((int16_t)(d->y - 1), 0, d->y_max);
        break;
    default:
        break;
    }

    return (uint16_t)((d->x != before_x || d->y != before_y) ? DIAG_EV_REDRAW
                                                             : 0);
}

/* Louder counts down towards MIX_VOL_HIGH, matching the stored setting's own
 * encoding, so Up is up on screen as well as in the ear. */
static uint16_t audio_step(diag_t* d, uint8_t dir_bits)
{
    uint8_t before = d->volume;
    int8_t dir = 0;

    if (dir_bits == COMBO_BTN_UP) {
        dir = -1;
    } else if (dir_bits == COMBO_BTN_DOWN) {
        dir = +1;
    } else {
        return 0;
    }

    d->volume = combo_step_u8(d->volume, dir, MIX_VOL_HIGH, MIX_VOL_OFF, 1);

    return (uint16_t)((d->volume != before) ? (DIAG_EV_TONE | DIAG_EV_REDRAW)
                                            : 0);
}

/* The patterns wrap, unlike every other stepped value here: there are three
 * of them and no reason to make a builder walk back the way they came. */
static uint16_t pattern_step(diag_t* d, uint8_t dir_bits)
{
    if (dir_bits == COMBO_BTN_DOWN) {
        d->pattern = (uint8_t)((d->pattern + 1u) % DIAG_PATTERN_COUNT);
    } else if (dir_bits == COMBO_BTN_UP) {
        d->pattern = (uint8_t)((d->pattern + DIAG_PATTERN_COUNT - 1u)
                               % DIAG_PATTERN_COUNT);
    } else {
        return 0;
    }
    return DIAG_EV_REDRAW;
}

static uint16_t frameskip_step(diag_t* d, uint8_t dir_bits)
{
    uint8_t before = d->frameskip;
    int8_t dir = 0;

    if (dir_bits == COMBO_BTN_DOWN) {
        dir = +1;
    } else if (dir_bits == COMBO_BTN_UP) {
        dir = -1;
    } else {
        return 0;
    }

    d->frameskip = combo_step_u8(d->frameskip, dir, 0, DIAG_FRAMESKIP_MAX, 1);

    return (uint16_t)((d->frameskip != before)
                          ? (DIAG_EV_FRAMESKIP | DIAG_EV_REDRAW) : 0);
}

/*
 * The held-direction cadence, taken from the list module so a nudge held down
 * moves at the same rate as a cursor held down: nothing for the first
 * COMBO_REPEAT_DELAY_MS, then one step every COMBO_REPEAT_MS.
 */
static bool direction_due(diag_t* d, uint8_t dir_bits, uint32_t now_ms)
{
    if (dir_bits == 0
        || (uint8_t)(dir_bits & (uint8_t)(dir_bits - 1)) != 0) {
        /* Two at once is a fumble rather than an instruction. Clearing the
         * held bit as well means whichever direction survives it acts
         * immediately. */
        d->held_dir = 0;
        return false;
    }

    if (dir_bits != d->held_dir) {
        d->held_dir = dir_bits;
        d->repeat_due_ms = now_ms + (uint32_t)COMBO_REPEAT_DELAY_MS;
        return true;
    }
    if ((int32_t)(now_ms - d->repeat_due_ms) >= 0) {
        /* The deadline comes off now_ms rather than off its own previous
         * value, matching the combo and list modules: a stall costs one late
         * step instead of a burst of catch-up steps. */
        d->repeat_due_ms = now_ms + (uint32_t)COMBO_REPEAT_MS;
        return true;
    }
    return false;
}

uint16_t diag_input(diag_t* d, uint8_t combo_event, uint8_t joypad,
                    uint32_t now_ms)
{
    uint16_t ev = 0;
    uint8_t pressed;
    uint8_t dir_bits;

    if (d == NULL) {
        return 0;
    }

    if (combo_event == COMBO_EVENT_BRIGHT_UP) {
        d->prev_word = joypad;
        return change_page(d, +1);
    }
    if (combo_event == COMBO_EVENT_BRIGHT_DOWN) {
        d->prev_word = joypad;
        return change_page(d, -1);
    }
    /* Every other event — the menu combo included — means nothing in this
     * mode: there is nothing to open and nowhere to go back to. */

    pressed = (uint8_t)(joypad & ~d->prev_word);
    d->prev_word = joypad;

    dir_bits = (uint8_t)(joypad & DIAG_DPAD_MASK);
    if (direction_due(d, dir_bits, now_ms)) {
        switch (d->page) {
        case DIAG_PAGE_NUDGE:
            ev |= nudge_step(d, dir_bits);
            break;
        case DIAG_PAGE_AUDIO:
            ev |= audio_step(d, dir_bits);
            break;
        case DIAG_PAGE_DISPLAY:
            ev |= pattern_step(d, dir_bits);
            break;
        case DIAG_PAGE_SYSTEM:
            ev |= frameskip_step(d, dir_bits);
            break;
        default:
            /* Buttons, SD, tag and battery are readouts: the D-pad has
             * nothing to move on them. */
            break;
        }
    }

    if (pressed & COMBO_BTN_A) {
        switch (d->page) {
        case DIAG_PAGE_NUDGE:
            d->toast = true;
            d->toast_until_ms = now_ms + (uint32_t)DIAG_TOAST_MS;
            ev |= DIAG_EV_SAVE_NUDGE | DIAG_EV_REDRAW;
            break;
        case DIAG_PAGE_NFC:
            ev |= DIAG_EV_NFC_SCAN;
            break;
        case DIAG_PAGE_AUDIO:
            d->tone_on = !d->tone_on;
            ev |= DIAG_EV_TONE | DIAG_EV_REDRAW;
            break;
        default:
            break;
        }
    }

    if (pressed & COMBO_BTN_B) {
        if (d->page == DIAG_PAGE_NUDGE) {
            d->x = d->default_x;
            d->y = d->default_y;
            ev |= DIAG_EV_REDRAW;
        }
    }

    return ev;
}

uint16_t diag_tick(diag_t* d, uint32_t now_ms)
{
    if (d == NULL) {
        return 0;
    }
    if (d->toast && (int32_t)(now_ms - d->toast_until_ms) >= 0) {
        d->toast = false;
        return DIAG_EV_REDRAW;
    }
    return 0;
}

uint8_t diag_page(const diag_t* d)
{
    return (d != NULL) ? d->page : (uint8_t)DIAG_PAGE_BUTTONS;
}

void diag_origin(const diag_t* d, int16_t* out_x, int16_t* out_y)
{
    if (d == NULL) {
        return;
    }
    if (out_x != NULL) {
        *out_x = d->x;
    }
    if (out_y != NULL) {
        *out_y = d->y;
    }
}

bool diag_tone_on(const diag_t* d)
{
    return (d != NULL) ? d->tone_on : false;
}

uint8_t diag_volume(const diag_t* d)
{
    return (d != NULL) ? d->volume : (uint8_t)MIX_VOL_OFF;
}

uint8_t diag_pattern(const diag_t* d)
{
    return (d != NULL) ? d->pattern : (uint8_t)DIAG_PATTERN_BARS;
}

uint8_t diag_frameskip(const diag_t* d)
{
    return (d != NULL) ? d->frameskip : 0;
}

bool diag_toast_active(const diag_t* d, uint32_t now_ms)
{
    if (d == NULL || !d->toast) {
        return false;
    }
    return (int32_t)(now_ms - d->toast_until_ms) < 0;
}

const char* diag_page_title(uint8_t page)
{
    if (page >= DIAG_PAGE_COUNT) {
        return NULL;
    }
    return page_titles[page];
}
