#pragma once
// The cartridge writer's layout — everything the writer draws, with nothing it
// decides (design §8.2).
//
// A canvas the caller injects, a geometry derived from the window's
// w/h, a word wrap for the description, and one picker_draw() that turns a
// picker_t into fills, text boxes and up to two clipped images. Every
// coordinate is window-relative: the host test paints a w x h buffer and the
// Arduino binding adds the per-unit game_x / game_y origin, and the same code
// serves both.
//
// The writer has two screens. The list is a split: titles down the left, and
// the highlighted title's cover with its snapshot stacked under it on the
// right, so it fits 10 rows of about 18 characters at 266x240. The
// highlighted title sits in a white pill that hugs it; a title too wide for
// its row scrolls inside the pill, drawn offscreen so the scroll never blanks
// the row. The button-hint footer runs under both screens. A title's detail
// page is laid out like the in-game Game Details page: the title on top over
// up to two rows, the cover and the snapshot side by side under it, and the
// description below them in a band that is the only thing that scrolls, with
// a chevron in the margin for more above or below. The hold bar appears
// between the band and the footer while A is held.
//
// Pure C, no Arduino/ESP-IDF headers, no allocation, no framebuffer of its own.

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#include "ui/canvas.h"
#include "ui/picker.h"
#include "ui/theme.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Both images are 96x96 raw RGB565 — 18 432 bytes each (design §6 "Art"). */
#define PICKER_ART_W  96
#define PICKER_ART_H  96
#define PICKER_ART_PX (PICKER_ART_W * PICKER_ART_H)

/* The list has no header and no help line: its rows and its images run from
 * the top inset to the hints. */
#define PICKER_LIST_TOP UI_PAD
#define PICKER_ROW_H    UI_PILL_H_LIST
#define PICKER_MIN_ROWS 3

/*
 * The detail page, mirroring Cart Info's numbers: the rows' text margin, the
 * images 8 px apart with 8 px under them.
 *
 * Two title rows because 242 px at about 10 px per header glyph is 24
 * characters a row, and 2 x 24 = 48 is past CATALOG_TITLE_MAX - 1 (47) — so
 * a title is never clipped on the page whose job is to name the game. A title
 * that fits one row gives the other to the description.
 */
#define PICKER_TITLE_ROWS 2
#define PICKER_DETAIL_X   UI_TEXT_X
#define PICKER_DETAIL_GAP 8

/* The help line's band, which holds the hold bar while A is held, and the
 * hint footer. */
#define PICKER_DETAIL_FOOT_H (UI_HELP_H + UI_FOOT_H)

#define PICKER_BAR_H 10

/* One glyph advance per printable ASCII character, measured once through the
 * canvas for the description's font: what the word wrap sums, so it breaks
 * where the driver would without measuring a string per character. */
typedef struct picker_adv_s {
    uint8_t w[95];
} picker_adv_t;

/* The longest wrapped line this module will hand back. */
#define PICKER_DESC_LINE_MAX 64

typedef struct picker_layout_s {
    int16_t w, h;
    uint8_t rows;    /* list rows above the hints                  */
    int16_t list_w;  /* width of a row's text inside its pill      */
    int16_t list_art_x;  /* the list's image column, left edge     */
    int16_t list_art_y;  /* the cover's top                       */
    int16_t list_shot_y; /* the snapshot's top, under the cover   */
    int16_t help_y;      /* the hold bar's band, on a detail page  */
    int16_t foot_y;      /* the hint footer's top, both screens    */

    /* The detail page, for a two-row title; a one-row title lifts media_y
     * and band_y by a font-2 row pitch and gives the band that much more. */
    int16_t detail_x;   /* left margin of everything on the page  */
    int16_t detail_w;   /* the title's, the band's and the bar's width */
    int16_t title_y;
    int16_t media_y;    /* both images' top                       */
    int16_t shot_x;     /* the snapshot's left edge, beside the cover */
    int16_t band_y;     /* the description band's top             */
    int16_t band_h;
    uint8_t band_rows;  /* description lines that fit in the band  */
    picker_adv_t adv;   /* the description font's advances        */
} picker_layout_t;

/*
 * Work out every number both screens need from the window size alone, so the
 * three render geometries are the same code with different inputs.
 *
 * PICKER_ERR_ARGS for a NULL out, a list shorter than PICKER_MIN_ROWS, a band
 * with no room for a line, a description column under PICKER_DESC_MIN_COLS, a
 * window too narrow to hold an image and a column beside it, or one too short
 * to stack both images above the help line.
 */
int picker_layout(int16_t w, int16_t h, picker_layout_t* out);

/*
 * Measure the description font's advances into g->adv. Until this is called
 * picker_layout() leaves a stand-in of 8 px a glyph, which is what the host
 * suite's fake canvas measures. Does nothing when either is NULL or the
 * canvas cannot measure.
 */
void picker_layout_measure(picker_layout_t* g, const ui_canvas_t* cv);

/* One font's advances, measured through the canvas; what
 * picker_layout_measure() does for the description font. Does nothing when
 * the canvas cannot measure. */
void picker_adv_measure(const ui_canvas_t* cv, uint8_t font,
                        picker_adv_t* out);

/*
 * How many lines `s` wraps to at `cols` characters. A newline is a hard break,
 * so "\n\n" between paragraphs counts a blank line. 0 for NULL or empty.
 */
uint16_t picker_desc_lines(const char* s, uint8_t cols);

/* The same, and picker_desc_line()'s, at `max_w` pixels of advances `a`. */
uint16_t picker_desc_lines_px(const char* s, const picker_adv_t* a,
                              int16_t max_w);
bool picker_desc_line_px(const char* s, const picker_adv_t* a, int16_t max_w,
                         uint16_t line, char* out, size_t out_sz);

/*
 * The description's height in lines, never less than band_rows — what
 * picker_set_scroll_span() takes as page_lines. Only the description scrolls.
 */
uint16_t picker_page_lines(const picker_layout_t* g, const char* desc);

/*
 * How many description lines the open detail page's band shows: band_rows,
 * plus what a one-row title frees, measured with the canvas. What
 * picker_set_scroll_span() takes as band_rows. g->band_rows when any argument
 * is NULL, the canvas cannot measure, or no page is open.
 */
uint8_t picker_band_rows(const picker_t* p, const picker_layout_t* g,
                         const ui_canvas_t* cv);

/*
 * The text of one wrapped line, NUL-terminated. A newline is a hard break and
 * is never copied out, so the blank line between paragraphs comes back empty.
 * Otherwise breaks at the last space that fits; a word longer than `cols` is
 * broken at `cols`. False when the line is
 * past the end of the string or the arguments are unusable.
 */
bool picker_desc_line(const char* s, uint8_t cols, uint16_t line, char* out,
                      size_t out_sz);

/*
 * How many pixels the highlighted list label runs past list_w, measured with
 * the canvas, or 0 when it fits. What picker_set_marquee_span() takes. 0 when
 * any argument is NULL or the canvas cannot measure.
 */
int16_t picker_row_overflow(const picker_t* p, const picker_layout_t* g,
                            const ui_canvas_t* cv);

/*
 * Redraw the current screen in full. `desc` is NULL until the description has
 * been loaded; `art` and `shot` are read only while their state in `p` is
 * PICKER_MEDIA_READY. Paints nothing when `p`, `g` or `cv` is NULL.
 */
void picker_draw(const picker_t* p, const picker_layout_t* g, const char* desc,
                 const uint16_t* art, const uint16_t* shot,
                 const ui_canvas_t* cv);

/*
 * Repaint only what `events` (picker_event_e bits from picker_input() or
 * picker_media_loaded()) names: the two rows a move touched, the highlighted
 * row alone for a marquee step, the image slots, the description band, or
 * the hold bar — where a rising bar paints only its filled part, never its
 * track. PICKER_EVENT_REDRAW is picker_draw(). The screen must already show
 * the state before these events, as the last draw left it. Same arguments
 * otherwise.
 */
void picker_draw_events(const picker_t* p, const picker_layout_t* g,
                        uint8_t events, const char* desc, const uint16_t* art,
                        const uint16_t* shot, const ui_canvas_t* cv);

#ifdef __cplusplus
}
#endif
