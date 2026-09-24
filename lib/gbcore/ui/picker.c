#include "picker.h"

#include <stddef.h>
#include <string.h>

#define PICKER_DPAD_MASK \
    (COMBO_BTN_UP | COMBO_BTN_DOWN | COMBO_BTN_LEFT | COMBO_BTN_RIGHT)

#define PICKER_SCROLL_MASK (COMBO_BTN_UP | COMBO_BTN_DOWN)

/* Percent, for the hold bar. */
#define PICKER_PCT_FULL 100

/* Append a row, silently dropping anything past the array. The catalog's own
 * cap is CATALOG_MAX and the action row is counted into PICKER_ROWS_MAX, so
 * this cannot trigger for a well-formed index — it is here so a larger one
 * could never write past the end. */
static void add_row(picker_t* p, uint8_t kind, uint16_t cat)
{
    if (p->row_count >= PICKER_ROWS_MAX) {
        return;
    }
    p->rows[p->row_count].kind = kind;
    p->rows[p->row_count].cat = cat;
    p->row_count++;
}

/*
 * The rows each mode offers, in the order they are shown.
 *
 * One action row at most, above the games. Pending mode is for lining up the
 * wildcard's next game and nothing else, so the only action is undoing a
 * pick already made; the wizard's only action is ending setup.
 */
static void build_rows(picker_t* p, bool wild_done, bool pending_set)
{
    uint16_t i;

    p->row_count = 0;
    if (p->cat == NULL) {
        return;
    }

    if (p->mode == PICKER_MODE_PENDING) {
        if (pending_set) {
            add_row(p, PICKER_ROW_CANCEL_PENDING, 0);
        }
        for (i = 0; i < (uint16_t)p->cat->count; i++) {
            add_row(p, PICKER_ROW_GAME, i);
        }
        return;
    }

    /* The wizard. Before the wildcard is written the boot flow treats a
     * finish as invalid, so the row is not offered at all rather than offered
     * and refused. */
    if (wild_done) {
        add_row(p, PICKER_ROW_FINISH, 0);
    }
    for (i = 0; i < (uint16_t)p->cat->count; i++) {
        if (p->cat->e[i].flags & CATALOG_FLAG_STARTER) {
            add_row(p, PICKER_ROW_GAME, i);
        }
    }
}

static const picker_row_t* cursor_row(const picker_t* p)
{
    uint16_t at;

    if (p->row_count == 0) {
        return NULL;
    }
    at = list_cursor(&p->list);
    if (at >= p->row_count) {
        return NULL;
    }
    return &p->rows[at];
}

/*
 * The detail band's scroll: one line per step, clamped at both ends.
 *
 * The cadence is the list module's, taken from the same two constants so a
 * future change still has one place to happen. The timer is separate because
 * the clamping rule differs — list_move() wraps at both ends, and a
 * description that jumps from its last line back to its first reads as a
 * glitch rather than as a wrap.
 *
 * Returns true when the offset actually moved.
 */
static bool scroll_step(picker_t* p, uint8_t dir_bits, uint32_t now_ms)
{
    uint16_t before = p->scroll;
    bool step = false;

    /* Exactly one direction, or nothing happens — the same fumble rule the
     * list machine applies. */
    if (dir_bits == 0 ||
        (uint8_t)(dir_bits & (uint8_t)(dir_bits - 1)) != 0) {
        p->scroll_dir = 0;
        return false;
    }

    if (dir_bits != p->scroll_dir) {
        p->scroll_dir = dir_bits;
        p->scroll_due_ms = now_ms + (uint32_t)COMBO_REPEAT_DELAY_MS;
        step = true;
    } else if ((int32_t)(now_ms - p->scroll_due_ms) >= 0) {
        p->scroll_due_ms = now_ms + (uint32_t)COMBO_REPEAT_MS;
        step = true;
    }
    if (!step) {
        return false;
    }

    if (dir_bits == COMBO_BTN_DOWN) {
        if (p->scroll < p->scroll_max) {
            p->scroll++;
        }
    } else if (p->scroll > 0) {
        p->scroll--;
    }
    return p->scroll != before;
}

/*
 * Move the highlighted label along its timeline: still, scrolling, still at
 * the end, then back to the start. Returns true when it moved.
 */
static bool marquee_step(picker_t* p, uint32_t now_ms)
{
    int16_t before = p->marquee_px;
    uint32_t t = now_ms - p->marquee_t0;
    uint32_t run = (uint32_t)p->marquee_span * PICKER_MARQUEE_STEP_MS;

    if (p->marquee_span <= 0) {
        return false;
    }
    if (t < (uint32_t)PICKER_MARQUEE_DELAY_MS) {
        p->marquee_px = 0;
    } else if (t < (uint32_t)PICKER_MARQUEE_DELAY_MS + run) {
        p->marquee_px =
            (int16_t)((t - PICKER_MARQUEE_DELAY_MS) / PICKER_MARQUEE_STEP_MS);
    } else if (t < (uint32_t)PICKER_MARQUEE_DELAY_MS + run +
                       PICKER_MARQUEE_PAUSE_MS) {
        p->marquee_px = p->marquee_span;
    } else {
        p->marquee_px = 0;
        p->marquee_t0 = now_ms;
    }
    return p->marquee_px != before;
}

/* Turn the open title into the selection the boot flow reads back. What the
 * selection means is the decision table's business; this only says what was
 * chosen. */
static void settle(picker_t* p)
{
    const picker_row_t* row = &p->rows[p->detail_row];

    memset(&p->sel, 0, sizeof(p->sel));
    switch (row->kind) {
    case PICKER_ROW_CANCEL_PENDING:
        p->pick = BOOT_PICK_CANCEL_PENDING;
        break;
    case PICKER_ROW_FINISH:
        p->pick = BOOT_PICK_FINISH;
        break;
    case PICKER_ROW_GAME:
    default:
        p->pick = BOOT_PICK_ROM;
        strncpy(p->sel.rom, p->cat->e[row->cat].filename,
                sizeof(p->sel.rom) - 1);
        p->sel.rom[sizeof(p->sel.rom) - 1] = '\0';
        /* The writer has one target. The wizard's step decides what kind of
         * cart is being made, and a pending write is always aimed at the
         * wildcard. */
        p->sel.target = (uint8_t)BOOT_TARGET_WILDCARD;
        break;
    }
    p->screen = PICKER_SCREEN_DONE;
    p->hold_active = false;
}

/* A new highlight: its images are not loaded yet, and the settle before they
 * are asked for starts now. */
static void hover(picker_t* p, uint32_t now_ms)
{
    const picker_row_t* row = cursor_row(p);

    p->marquee_px = 0;
    p->marquee_span = 0;
    p->marquee_t0 = now_ms;

    p->media_asked = false;
    p->art_state = PICKER_MEDIA_LOADING;
    p->shot_state = PICKER_MEDIA_LOADING;
    p->media_cat = (row != NULL) ? row->cat : 0;
    p->moved_ms = now_ms;
}

/* Open the highlighted row's detail page. Its images are the highlight's,
 * already loaded or on their way, so only the description is asked for. */
static void open_detail(picker_t* p)
{
    p->detail_row = list_cursor(&p->list);
    p->screen = PICKER_SCREEN_DETAIL;
    p->hold_active = false;
    p->hold_elapsed_ms = 0;
    p->scroll = 0;
    p->scroll_dir = 0;
    p->desc_asked = false;
}

int picker_init(picker_t* p, enum picker_mode_e mode,
                const catalog_index_t* cat, bool wild_done, bool pending_set,
                const boot_made_t* made, uint8_t rows_visible)
{
    if (p == NULL) {
        return PICKER_ERR_ARGS;
    }
    memset(p, 0, sizeof(*p));
    p->screen = PICKER_SCREEN_LIST;
    p->pick = BOOT_PICK_NONE;

    if (cat == NULL || rows_visible == 0) {
        return PICKER_ERR_ARGS;
    }
    p->mode = (uint8_t)mode;
    p->cat = cat;
    p->made = made;

    build_rows(p, wild_done, pending_set);
    list_init(&p->list, p->row_count, rows_visible);
    hover(p, 0);
    if (p->row_count == 0) {
        return PICKER_ERR_EMPTY;
    }
    return PICKER_OK;
}

uint8_t picker_input(picker_t* p, uint8_t buttons, uint32_t now_ms)
{
    uint8_t pressed;
    uint8_t ev = PICKER_EVENT_NONE;

    if (p == NULL) {
        return PICKER_EVENT_NONE;
    }
    pressed = (uint8_t)(buttons & ~p->prev_buttons);
    p->prev_buttons = buttons;
    if (p->row_count == 0 || p->screen == PICKER_SCREEN_DONE) {
        return PICKER_EVENT_NONE;
    }

    if (p->screen == PICKER_SCREEN_LIST) {
        uint16_t was = list_cursor(&p->list);
        uint16_t first = list_first(&p->list);

        if (list_input(&p->list, (uint8_t)(buttons & PICKER_DPAD_MASK),
                       now_ms) == LIST_EVENT_MOVED) {
            p->prev_cursor = was;
            hover(p, now_ms);
            /* A move that scrolled the window changed every row. */
            ev = (list_first(&p->list) == first)
                     ? (uint8_t)(PICKER_EVENT_ROWS | PICKER_EVENT_MEDIA |
                                 PICKER_EVENT_HELP)
                     : (uint8_t)(PICKER_EVENT_REDRAW | PICKER_EVENT_HELP);
        } else if (marquee_step(p, now_ms)) {
            ev = PICKER_EVENT_MARQUEE;
        }
        if ((pressed & COMBO_BTN_A) == 0 || cursor_row(p) == NULL) {
            return ev;
        }
        open_detail(p);
        return PICKER_EVENT_REDRAW;
    }

    /* The detail page. Left and Right do nothing here: there is one thing to
     * decide and paging is not it. */
    if (pressed & COMBO_BTN_B) {
        p->screen = PICKER_SCREEN_LIST;
        p->hold_active = false;
        p->hold_elapsed_ms = 0;
        p->scroll_dir = 0;
        /* The label starts again from the left, after the usual delay. */
        p->marquee_px = 0;
        p->marquee_t0 = now_ms;
        return PICKER_EVENT_REDRAW;
    }
    if (scroll_step(p, (uint8_t)(buttons & PICKER_SCROLL_MASK), now_ms)) {
        ev = PICKER_EVENT_BAND;
    }
    if (pressed & COMBO_BTN_A) {
        /* A fresh press, which is why the A that opened this page — still
         * down, but not an edge — cannot confirm it. */
        p->hold_active = true;
        p->hold_start_ms = now_ms;
        p->hold_elapsed_ms = 0;
        return (uint8_t)(ev | PICKER_EVENT_BAR);
    }
    if (buttons & COMBO_BTN_A) {
        if (!p->hold_active) {
            return ev;
        }
        p->hold_elapsed_ms = now_ms - p->hold_start_ms;
        if (p->hold_elapsed_ms >= (uint32_t)PICKER_HOLD_MS) {
            settle(p);
            return PICKER_EVENT_DONE;
        }
        return (uint8_t)(ev | PICKER_EVENT_BAR);
    }
    if (p->hold_active) {
        /* Let go early and the bar goes back to nothing: a part-finished hold
         * is not a part-finished write. */
        p->hold_active = false;
        p->hold_elapsed_ms = 0;
        return (uint8_t)(ev | PICKER_EVENT_BAR);
    }
    return ev;
}

uint8_t picker_hold_pct(const picker_t* p)
{
    uint32_t pct;

    if (p == NULL || !p->hold_active) {
        return 0;
    }
    pct = p->hold_elapsed_ms * PICKER_PCT_FULL / (uint32_t)PICKER_HOLD_MS;
    return (uint8_t)(pct > PICKER_PCT_FULL ? PICKER_PCT_FULL : pct);
}

int picker_set_scroll_span(picker_t* p, uint16_t page_lines,
                           uint8_t band_rows)
{
    uint16_t span;

    if (p == NULL) {
        return PICKER_ERR_ARGS;
    }
    span = (page_lines > (uint16_t)band_rows)
               ? (uint16_t)(page_lines - (uint16_t)band_rows)
               : 0;
    if (span > PICKER_SCROLL_MAX_LINES) {
        span = PICKER_SCROLL_MAX_LINES;
    }
    p->scroll_max = span;
    if (p->scroll > span) {
        p->scroll = span;
    }
    return PICKER_OK;
}

int picker_set_marquee_span(picker_t* p, int16_t overflow_px)
{
    if (p == NULL) {
        return PICKER_ERR_ARGS;
    }
    p->marquee_span = (overflow_px > 0) ? overflow_px : 0;
    if (p->marquee_px > p->marquee_span) {
        p->marquee_px = p->marquee_span;
    }
    return PICKER_OK;
}

bool picker_media_due(picker_t* p, uint32_t now_ms, uint16_t* cat_idx)
{
    const picker_row_t* row;

    if (p == NULL || cat_idx == NULL || p->media_asked) {
        return false;
    }
    /* The detail page is always the highlighted row's, because the cursor
     * cannot move under it. */
    row = cursor_row(p);
    if (row == NULL || row->kind != PICKER_ROW_GAME) {
        return false;
    }
    if (p->screen == PICKER_SCREEN_LIST) {
        if (now_ms - p->moved_ms < (uint32_t)PICKER_MEDIA_SETTLE_MS) {
            return false;
        }
    } else if (p->screen != PICKER_SCREEN_DETAIL) {
        return false;
    }
    p->media_asked = true;
    p->media_cat = row->cat;
    *cat_idx = p->media_cat;
    return true;
}

bool picker_desc_due(picker_t* p, uint16_t* cat_idx)
{
    const picker_row_t* row;

    if (p == NULL || cat_idx == NULL || p->desc_asked ||
        p->screen != PICKER_SCREEN_DETAIL || p->detail_row >= p->row_count) {
        return false;
    }
    row = &p->rows[p->detail_row];
    if (row->kind != PICKER_ROW_GAME) {
        return false;
    }
    p->desc_asked = true;
    *cat_idx = row->cat;
    return true;
}

uint8_t picker_media_loaded(picker_t* p, uint16_t cat_idx, bool art_ok,
                            bool shot_ok)
{
    if (p == NULL) {
        return PICKER_EVENT_NONE;
    }
    /* A report for a title the highlight has already left is stale: taking
     * it would paint the previous title's cover beside the current one. A
     * move clears media_asked, so this also covers a move onto an action
     * row. */
    if (!p->media_asked || cat_idx != p->media_cat) {
        return PICKER_EVENT_NONE;
    }
    p->art_state =
        art_ok ? (uint8_t)PICKER_MEDIA_READY : (uint8_t)PICKER_MEDIA_MISSING;
    p->shot_state =
        shot_ok ? (uint8_t)PICKER_MEDIA_READY : (uint8_t)PICKER_MEDIA_MISSING;
    return PICKER_EVENT_MEDIA;
}

bool picker_row_marked(const picker_t* p, uint16_t row)
{
    if (p == NULL || p->made == NULL || p->cat == NULL ||
        row >= p->row_count) {
        return false;
    }
    if (p->rows[row].kind != PICKER_ROW_GAME) {
        return false;
    }
    return boot_made_has(p->made, p->cat->e[p->rows[row].cat].filename);
}

enum boot_pick_e picker_result(const picker_t* p, boot_selection_t* out)
{
    if (p == NULL || out == NULL || p->screen != PICKER_SCREEN_DONE) {
        return BOOT_PICK_NONE;
    }
    if (p->pick == BOOT_PICK_ROM) {
        *out = p->sel;
    }
    return (enum boot_pick_e)p->pick;
}
