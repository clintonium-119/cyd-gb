#pragma once
// The cartridge writer's layout — everything the writer draws, with nothing it
// decides (design §8.2).
//
// A three-call canvas the caller injects, a geometry derived from the window's
// w/h, a word wrap for the description, and one picker_draw() that turns a
// picker_t into fills, text boxes and up to two clipped images. Every
// coordinate is window-relative: the host test paints a w x h buffer and the
// Arduino binding adds the per-unit game_x / game_y origin, and the same code
// serves both.
//
// The writer has two screens. The list uses the whole window width — no art
// slot, no description band — so it fits 11 rows of about 29 characters at
// 240x216. A title's detail page carries its cover, its gameplay snapshot and
// its description in a band that scrolls, with the title and target above it
// and the filename, the hold prompt and the hold bar below, none of which
// scroll away.
//
// Pure C, no Arduino/ESP-IDF headers, no allocation, no framebuffer of its own.

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#include "ui/canvas.h"
#include "ui/picker.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Both images are 96x96 raw RGB565 — 18 432 bytes each (design §6 "Art"). */
#define PICKER_ART_W  96
#define PICKER_ART_H  96
#define PICKER_ART_PX (PICKER_ART_W * PICKER_ART_H)

#define PICKER_HEADER_H 18
#define PICKER_ROW_H    18 /* font 2: 16 + 2 */
#define PICKER_MIN_ROWS 3

/* A description column narrower than this is not worth wrapping into. */
#define PICKER_DESC_MIN_COLS 8

/*
 * The detail page's fixed chrome, spelled out so the arithmetic is checkable.
 *
 * Two title rows because 232 px at about 8 px per glyph is 29 characters a
 * row, and 2 x 29 = 58 is past CATALOG_TITLE_MAX - 1 (47) — so a title is
 * never clipped on the page whose job is to name the game.
 */
#define PICKER_TITLE_ROWS 2

/* Title 36 + gap 2 + target line 10. */
#define PICKER_DETAIL_HEAD_H 48

/* Filename 10 + gap 2 + footer 18 + gap 2 + bar 10 + bottom margin 4. */
#define PICKER_DETAIL_FOOT_H 46

#define PICKER_BAND_GAP 4
#define PICKER_BAR_H    10

/* The longest wrapped line this module will hand back. */
#define PICKER_DESC_LINE_MAX 64

typedef struct picker_layout_s {
    int16_t w, h;
    uint8_t rows;    /* list rows that fit under the header */
    int16_t list_w;  /* width of a row's text box          */

    int16_t band_y;     /* the detail band's top, window-relative */
    int16_t band_h;
    uint8_t band_rows;  /* font-1 lines that fit in the band      */

    int16_t art_x;      /* both images' left edge, band-relative  */
    int16_t desc_x;
    int16_t desc_w;
    uint8_t desc_cols;  /* characters per wrapped line            */

    int16_t shot_page_y; /* the snapshot's top within the page    */
    int16_t page_h;      /* the scrolling page's height in pixels */
} picker_layout_t;

/*
 * Work out every number both screens need from the window size alone, so the
 * three render geometries are the same code with different inputs.
 *
 * PICKER_ERR_ARGS for a NULL out, a list shorter than PICKER_MIN_ROWS, a band
 * with no room for a line, a description column under PICKER_DESC_MIN_COLS, or
 * a window too narrow to hold an image and a column beside it.
 */
int picker_layout(int16_t w, int16_t h, picker_layout_t* out);

/* How many lines `s` wraps to at `cols` characters. 0 for NULL or empty. */
uint16_t picker_desc_lines(const char* s, uint8_t cols);

/*
 * The scrolling page's height in font-1 lines — what picker_set_scroll_span()
 * takes. The two stacked images dominate for any ordinary description, which
 * is why the band scrolls at all.
 */
uint16_t picker_page_lines(const picker_layout_t* g, const char* desc);

/*
 * The text of one wrapped line, NUL-terminated. Breaks at the last space that
 * fits; a word longer than `cols` is broken at `cols`. False when the line is
 * past the end of the string or the arguments are unusable.
 */
bool picker_desc_line(const char* s, uint8_t cols, uint16_t line, char* out,
                      size_t out_sz);

/*
 * Redraw the current screen in full. `desc` is NULL until the description has
 * been loaded; `art` and `shot` are read only while their state in `p` is
 * PICKER_MEDIA_READY. Paints nothing when `p`, `g` or `cv` is NULL.
 */
void picker_draw(const picker_t* p, const picker_layout_t* g, const char* desc,
                 const uint16_t* art, const uint16_t* shot,
                 const ui_canvas_t* cv);

#ifdef __cplusplus
}
#endif
