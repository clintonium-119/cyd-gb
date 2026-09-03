#include "list.h"

#include <stddef.h>

#define LIST_DPAD_MASK \
    (COMBO_BTN_UP | COMBO_BTN_DOWN | COMBO_BTN_LEFT | COMBO_BTN_RIGHT)

/* The furthest the window can slide and still show a full page. A list
 * shorter than the window never scrolls at all. */
static uint16_t last_first(const list_state_t* s)
{
    return (uint16_t)(s->count > (uint16_t)s->rows
                          ? s->count - (uint16_t)s->rows
                          : 0);
}

/*
 * Slide the window by the least that brings the cursor back inside it. Every
 * cursor move ends here, which is what keeps "the window follows the cursor"
 * a single rule rather than one per direction.
 */
static void follow(list_state_t* s)
{
    if (s->cursor < s->first) {
        s->first = s->cursor;
    } else if (s->cursor >= (uint16_t)(s->first + s->rows)) {
        s->first = (uint16_t)(s->cursor - s->rows + 1);
    }
}

/* Nothing to move around, or no window to move it in. */
static bool inert(const list_state_t* s)
{
    return s == NULL || s->count == 0 || s->rows == 0;
}

int list_init(list_state_t* s, uint16_t count, uint8_t rows)
{
    if (s == NULL || rows == 0) {
        return LIST_ERR_ARGS;
    }
    s->count = count;
    s->rows = rows;
    s->cursor = 0;
    s->first = 0;
    s->held_dir = 0;
    s->repeat_due_ms = 0;
    return LIST_OK;
}

int list_set_count(list_state_t* s, uint16_t count)
{
    if (s == NULL) {
        return LIST_ERR_ARGS;
    }
    s->count = count;
    if (s->cursor >= count) {
        s->cursor = (count > 0) ? (uint16_t)(count - 1) : 0;
    }
    if (s->rows == 0) {
        return LIST_OK;
    }
    if (s->first > last_first(s)) {
        s->first = last_first(s);
    }
    follow(s);
    return LIST_OK;
}

void list_move(list_state_t* s, int8_t dir)
{
    if (inert(s) || dir == 0) {
        return;
    }
    if (dir > 0) {
        /* Off the bottom: back to the top, window and all. */
        if ((uint16_t)(s->cursor + 1) >= s->count) {
            s->cursor = 0;
            s->first = 0;
            return;
        }
        s->cursor++;
    } else {
        /* Off the top: to the last entry, with the window on the last page. */
        if (s->cursor == 0) {
            s->cursor = (uint16_t)(s->count - 1);
            s->first = last_first(s);
            return;
        }
        s->cursor--;
    }
    follow(s);
}

void list_page(list_state_t* s, int8_t dir)
{
    int32_t step;
    int32_t cursor;
    int32_t first;

    if (inert(s) || dir == 0) {
        return;
    }
    /* Cursor and window step together, so the highlighted row stays where it
     * was on screen and the page underneath it changes. */
    step = (dir > 0) ? (int32_t)s->rows : -(int32_t)s->rows;
    cursor = (int32_t)s->cursor + step;
    first = (int32_t)s->first + step;

    if (cursor < 0) {
        cursor = 0;
    }
    if (cursor > (int32_t)s->count - 1) {
        cursor = (int32_t)s->count - 1;
    }
    if (first < 0) {
        first = 0;
    }
    if (first > (int32_t)last_first(s)) {
        first = (int32_t)last_first(s);
    }
    s->cursor = (uint16_t)cursor;
    s->first = (uint16_t)first;
    follow(s);
}

uint8_t list_input(list_state_t* s, uint8_t dpad, uint32_t now_ms)
{
    uint8_t dir_bits;
    uint16_t before;
    bool step = false;

    if (s == NULL) {
        return (uint8_t)LIST_EVENT_NONE;
    }
    dir_bits = (uint8_t)(dpad & LIST_DPAD_MASK);

    /*
     * Exactly one direction, or nothing happens: two at once is a fumble
     * rather than an instruction. Clearing held_dir as well means the
     * direction that survives the fumble is treated as a fresh press and acts
     * immediately, instead of inheriting a deadline from before it.
     */
    if (dir_bits == 0 || (uint8_t)(dir_bits & (uint8_t)(dir_bits - 1)) != 0) {
        s->held_dir = 0;
        return (uint8_t)LIST_EVENT_NONE;
    }

    if (dir_bits != s->held_dir) {
        s->held_dir = dir_bits;
        s->repeat_due_ms = now_ms + (uint32_t)COMBO_REPEAT_DELAY_MS;
        step = true;
    } else if ((int32_t)(now_ms - s->repeat_due_ms) >= 0) {
        /*
         * The deadline comes off now_ms rather than off its own previous
         * value, matching the combo module: a stall — a slow redraw, a frame
         * that ran long — then costs one late step instead of a burst of
         * catch-up steps once the caller comes back.
         */
        s->repeat_due_ms = now_ms + (uint32_t)COMBO_REPEAT_MS;
        step = true;
    }
    if (!step) {
        return (uint8_t)LIST_EVENT_NONE;
    }

    before = s->cursor;
    switch (dir_bits) {
    case COMBO_BTN_UP:
        list_move(s, -1);
        break;
    case COMBO_BTN_DOWN:
        list_move(s, +1);
        break;
    case COMBO_BTN_LEFT:
        list_page(s, -1);
        break;
    case COMBO_BTN_RIGHT:
        list_page(s, +1);
        break;
    default:
        break;
    }
    return (uint8_t)((s->cursor != before) ? LIST_EVENT_MOVED
                                           : LIST_EVENT_NONE);
}

uint16_t list_cursor(const list_state_t* s)
{
    return (s != NULL) ? s->cursor : 0;
}

uint16_t list_first(const list_state_t* s)
{
    return (s != NULL) ? s->first : 0;
}

bool list_visible(const list_state_t* s, uint16_t idx)
{
    if (s == NULL || idx >= s->count) {
        return false;
    }
    return idx >= s->first && idx < (uint16_t)(s->first + s->rows);
}
