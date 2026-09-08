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

#include "ui/picker.h"

#ifdef __cplusplus
extern "C" {
#endif

enum ui_align_e {
    UI_ALIGN_LEFT = 0,
    UI_ALIGN_CENTER,
    UI_ALIGN_RIGHT,
};

/*
 * The injected draw seam. Three calls, in the shape catalog_reader_t
 * established for gbcore: a ctx the caller owns plus function pointers.
 *
 * `text` is a box, not a baseline: top-left x, y, width w, up to `rows` lines
 * of `font`, aligned within the box. The implementation clips at w and rows,
 * which is what display_draw_wrapped() already does.
 *
 * `image` takes a row range so the scrolling band can clip an image at its
 * edge: draw rows [row0, row0 + rows) of a w-wide image, with the top of that
 * range landing at y. The binding hands the driver px + row0 * w.
 */
typedef struct ui_canvas_s {
    void* ctx;
    void (*fill)(void* ctx, int16_t x, int16_t y, int16_t w, int16_t h,
                 uint16_t color);
    void (*text)(void* ctx, const char* s, int16_t x, int16_t y, int16_t w,
                 uint8_t rows, uint8_t font, uint8_t align, uint16_t fg,
                 uint16_t bg);
    void (*image)(void* ctx, int16_t x, int16_t y, int16_t w, int16_t h,
                  const uint16_t* px, int16_t row0, int16_t rows);
} ui_canvas_t;

/* TFT_eSPI built-in font ids. */
#define UI_FONT_SMALL 1
#define UI_FONT_ROW   2
#define UI_FONT_TITLE 4

/*
 * Font 1 is GLCD: a FIXED 6-pixel advance per glyph. This is the one glyph
 * width this module may assume, and it is what makes the description's word
 * wrap exact — a line of `cols` characters measures cols * 6 pixels, so the
 * break points computed here and the ones display_draw_wrapped() arrives at by
 * measuring agree to the pixel. Fonts 2 and 4 are proportional and are never
 * measured here; the driver clips them at the box the caller asks for.
 */
#define UI_FONT_SMALL_ADV 6

/* Heights of the three fonts, and the row pitch display_draw_wrapped() uses. */
uint8_t ui_font_height(uint8_t font);
#define UI_ROW_PITCH(font_h) ((font_h) + 2)

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
 * two SCALE_K geometries are the same code with different inputs.
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
