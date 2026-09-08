#pragma once
// The cartridge writer's state machine — everything the writer decides, with
// nothing it draws (design §8.2).
//
// Which rows exist for a given mode, what the D-pad, A and B do on the list
// and on a title's detail page, how far the detail page has scrolled, how long
// A has to be held before an action counts as confirmed, when a title's cover,
// snapshot and description are wanted, and the {rom, target} selection the boot
// flow gets back. Plain values in — a joypad word and a millisecond timestamp —
// events out. The Arduino binding polls it and draws it; the host suite drives
// it with literal timestamps.
//
// It holds no geometry. Widths, heights, row pitches and column counts belong
// to the layout module, which hands this one two numbers through
// picker_set_scroll_span().
//
// The five rules the writer's interface fixes hold here too, because this is
// where the writer's behaviour actually lives:
//
//   1. One call site — the boot state machine's executor opens the writer and
//      nothing else does. No button combo, no settings entry.
//   2. It returns a pick. It never writes a tag and never launches a game.
//   3. It reaches for nothing below itself: no tag, reader, emulator or ROM
//      storage symbol appears here, in code or in prose.
//   4. When it renders, it renders inside the game window — every coordinate
//      the layout module produces is window-relative.
//   5. Every exit is a halt or a power-off prompt, so its buffers may be
//      static and generous.
//
// Pure C, no Arduino/ESP-IDF headers, no allocation.

#include <stdint.h>
#include <stdbool.h>

#include "cart/boot.h"
#include "cart/catalog.h"
#include "ui/list.h"

#ifdef __cplusplus
extern "C" {
#endif

/* How long A is held before the write is committed. Long enough that a knocked
 * button is not a write; short enough that a deliberate press does not feel
 * like a wait. One figure: the writer has one kind of confirmation, because it
 * has one kind of target. */
#define PICKER_HOLD_MS 1000

/* A ceiling on the detail band's scroll, so a malformed page height cannot
 * make the band scroll off into nothing. Well above the roughly 8 lines a
 * 200-byte description and two stacked images actually need. */
#define PICKER_SCROLL_MAX_LINES 64

/* Cancel pending write, or Finish setup — one per mode, never both. */
#define PICKER_ACTION_MAX 1

#define PICKER_ROWS_MAX (CATALOG_MAX + PICKER_ACTION_MAX)

enum picker_result_e {
    PICKER_OK = 0,
    PICKER_ERR_ARGS = -1,  /* NULL picker or catalog, or rows_visible == 0 */
    PICKER_ERR_EMPTY = -2, /* the mode composed no rows at all            */
};

enum picker_mode_e {
    /* Booting with a menu cart: the pick is recorded and carried out on a
     * later boot, against the wildcard presented then. */
    PICKER_MODE_PENDING = 0,
    /* The first-boot wizard: the tag to write is the one in the field now. */
    PICKER_MODE_IMMEDIATE,
};

enum picker_screen_e {
    PICKER_SCREEN_LIST = 0,
    PICKER_SCREEN_DETAIL,
    PICKER_SCREEN_DONE,
};

enum picker_row_kind_e {
    PICKER_ROW_GAME = 0,
    PICKER_ROW_CANCEL_PENDING,
    PICKER_ROW_FINISH,
};

enum picker_media_e {
    PICKER_MEDIA_LOADING = 0, /* asked for, or about to be */
    PICKER_MEDIA_READY,
    PICKER_MEDIA_MISSING,
};

enum picker_event_e {
    PICKER_EVENT_NONE = 0,
    PICKER_EVENT_REDRAW,
    PICKER_EVENT_DONE,
};

/* `cat` indexes catalog_index_t.e[] and is meaningful only for
 * PICKER_ROW_GAME. */
typedef struct picker_row_s {
    uint8_t kind;
    uint16_t cat;
} picker_row_t;

/*
 * The machine. The layout module in this same library reads these fields
 * directly, because it is the other half of one screen; everything outside
 * gbcore goes through the functions below.
 */
typedef struct picker_s {
    uint8_t mode;
    const catalog_index_t* cat;
    const boot_made_t* made;
    picker_row_t rows[PICKER_ROWS_MAX];
    uint16_t row_count;
    list_state_t list;
    uint8_t screen;
    uint16_t detail_row;
    uint16_t scroll;      /* the band's first visible line          */
    uint16_t scroll_max;  /* the last line it may start on          */
    uint8_t scroll_dir;   /* the one direction bit held, 0 = none   */
    uint32_t scroll_due_ms;
    uint8_t prev_buttons;
    bool hold_active;
    uint32_t hold_start_ms;
    uint32_t hold_elapsed_ms;
    uint8_t art_state;
    uint8_t shot_state;
    uint16_t media_cat;
    bool media_asked;
    uint8_t pick;
    boot_selection_t sel;
} picker_t;

/*
 * Compose the rows for `mode` and start on the list with the cursor at the
 * top.
 *
 * Pending mode offers Cancel pending write, only when one is set, then every
 * catalog entry in file order. Immediate mode — the wizard — offers Finish
 * setup, only once the wildcard is done, because before that the boot flow
 * treats a finish as invalid, and then the `starter` entries only.
 *
 * PICKER_ERR_ARGS for a NULL picker or catalog or rows_visible == 0;
 * PICKER_ERR_EMPTY when the mode composed no rows, with the state left
 * consistent so every other call is a safe no-op. `made` may be NULL, in
 * which case no row is marked.
 *
 * There is no timestamp here: nothing is timed until a detail page opens.
 */
int picker_init(picker_t* p, enum picker_mode_e mode,
                const catalog_index_t* cat, bool wild_done, bool pending_set,
                const boot_made_t* made, uint8_t rows_visible);

/*
 * One button sample taken at now_ms. `buttons` is the joypad word in
 * COMBO_BTN_* bits; edges are worked out here, so the caller only has to
 * sample no faster than the debounce window.
 *
 * On the list, Up and Down move an entry and Left and Right jump a page, at
 * the list module's repeat cadence; A opens the highlighted row's detail page.
 * On the detail page, Up and Down scroll the content band a line and clamp at
 * both ends, B returns to the list, and A has to be held: the hold starts on a
 * fresh press, so the A that opened the page cannot confirm it, and releasing
 * early resets it to zero.
 *
 * Returns PICKER_EVENT_REDRAW when something on screen changed,
 * PICKER_EVENT_DONE once the selection is final, PICKER_EVENT_NONE otherwise.
 */
uint8_t picker_input(picker_t* p, uint8_t buttons, uint32_t now_ms);

/* The hold so far as a percentage of PICKER_HOLD_MS, 0 when not holding. */
uint8_t picker_hold_pct(const picker_t* p);

/*
 * How far the detail band may scroll, in lines. The layout module works out
 * both numbers, because it owns the geometry and the word wrap; this stores
 * the span and clamps the current offset into it. Call it after the layout is
 * known and again whenever a description is loaded.
 */
int picker_set_scroll_span(picker_t* p, uint16_t page_lines,
                           uint8_t band_rows);

/*
 * Whether the open title's cover, snapshot and description should be fetched
 * now. True exactly once per opened title — the caller loads all three and
 * reports back through picker_media_loaded() — and never for an action row.
 * It is an edge, not a timer: media is only ever wanted because someone
 * deliberately opened a title.
 */
bool picker_media_due(picker_t* p, uint16_t* cat_idx);

/*
 * The answer to that request. A report for a catalog entry whose page has
 * already been left is stale and is ignored, so a slow load cannot paint the
 * wrong cover. Returns PICKER_EVENT_REDRAW when the report was taken.
 */
uint8_t picker_media_loaded(picker_t* p, uint16_t cat_idx, bool art_ok,
                            bool shot_ok);

/* Whether row `row` is a game the wizard has already written this setup. */
bool picker_row_marked(const picker_t* p, uint16_t row);

/*
 * What the user chose. BOOT_PICK_NONE with *out untouched until the machine
 * reaches PICKER_SCREEN_DONE. A game row is BOOT_PICK_ROM with the catalog
 * filename and BOOT_TARGET_WILDCARD — the writer has no other target. What a
 * pick means is boot_after_pick()'s business, not this module's.
 */
enum boot_pick_e picker_result(const picker_t* p, boot_selection_t* out);

#ifdef __cplusplus
}
#endif
