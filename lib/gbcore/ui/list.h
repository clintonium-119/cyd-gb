#pragma once
// List cursor and window state machine (design §8.1).
//
// One machine for every scrolling list in the firmware: the six-entry in-game
// menu and the 132-title cartridge catalog are the same problem at two sizes.
// It owns a cursor over `count` entries, the `rows`-tall window of entries
// that are on screen, wrap at both ends of a single-entry move, a page jump
// that clamps instead, and key repeat for a held direction. It knows nothing
// about what the entries are, how they are drawn or where they came from: the
// caller feeds it a D-pad bit word and a timestamp in ms, and reads back the
// cursor and the window to draw from.
//
// Feed only the bits you want acted on. The in-game menu masks Left and Right
// out because it uses them to adjust values; a catalog list feeds all four and
// gets the page jump.
//
// Debounce is not here: the caller's poll period is longer than
// COMBO_DEBOUNCE_MS, so successive samples are already stable. The button
// masks and the repeat cadence come from the combo module rather than being
// copied — one cadence, one mask set, no second definition to drift.
//
// Pure C, no Arduino/ESP-IDF headers, no allocation.

#include <stdint.h>
#include <stdbool.h>

#include "input/combo.h"

#ifdef __cplusplus
extern "C" {
#endif

enum list_result_e {
    LIST_OK = 0,
    LIST_ERR_ARGS = -1, /* NULL state, or rows == 0 */
};

enum list_event_e {
    LIST_EVENT_NONE = 0,
    LIST_EVENT_MOVED,
};

typedef struct list_state_s {
    uint16_t count;          /* entries in the list                        */
    uint8_t rows;            /* entries visible at once                    */
    uint16_t cursor;         /* highlighted entry, 0 .. count - 1          */
    uint16_t first;          /* topmost visible entry                      */
    uint8_t held_dir;        /* the one direction bit held, 0 = none       */
    uint32_t repeat_due_ms;  /* when the held direction's next step is due */
} list_state_t;

/*
 * Zero the machine over `count` entries with `rows` of them visible. A count
 * of 0 is valid — an empty list — and leaves every move a no-op. Returns
 * LIST_OK, or LIST_ERR_ARGS for a NULL state or rows == 0.
 */
int list_init(list_state_t* s, uint16_t count, uint8_t rows);

/*
 * Re-point the machine at a list that changed length: the cursor is clamped
 * into range and the window back onto the last full page, so a list that
 * shrank under the cursor still draws.
 */
int list_set_count(list_state_t* s, uint16_t count);

/*
 * One entry in the direction of dir (> 0 down the list, < 0 up), wrapping at
 * both ends. The window follows by the one row the cursor needed, and snaps
 * to the far end on a wrap.
 */
void list_move(list_state_t* s, int8_t dir);

/*
 * One page — `rows` entries — in the direction of dir. This one clamps rather
 * than wrapping: a page jump is for covering ground, and wrapping it would
 * make the two ends hard to arrive at deliberately.
 */
void list_page(list_state_t* s, int8_t dir);

/*
 * Feed one D-pad sample taken at now_ms: Up/Down move an entry, Left/Right
 * jump a page. A newly pressed direction acts at once, then waits
 * COMBO_REPEAT_DELAY_MS before repeating every COMBO_REPEAT_MS for as long as
 * it stays held; releasing re-arms the immediate step. Two directions at once
 * do nothing at all.
 *
 * Returns LIST_EVENT_MOVED when the cursor ended up somewhere else, so the
 * caller redraws only when there is something to redraw, and LIST_EVENT_NONE
 * otherwise — including for a page held against an end.
 */
uint8_t list_input(list_state_t* s, uint8_t dpad, uint32_t now_ms);

uint16_t list_cursor(const list_state_t* s);
uint16_t list_first(const list_state_t* s);

/* Whether entry idx is inside the window, and so has a row to be drawn on. */
bool list_visible(const list_state_t* s, uint16_t idx);

#ifdef __cplusplus
}
#endif
