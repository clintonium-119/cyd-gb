#pragma once
// The theme's shared drawing — the pieces every surface is built from.
//
// A header, a plain row, the selected row's pill, the grey help line, the
// button-hint footer and the whole-window notice, each drawn by exactly one
// function here and nowhere else, so the setup notices, the diagnostic pages,
// the in-game menu, the manual view and the cartridge writer read as one UI.
// All of it paints through the injected canvas with window-relative
// coordinates, as the layout modules do.
//
// Also here: rounding an RGB565 image's corners in place, one band at a time,
// because the menu streams its images in bands and never holds a whole one.
//
// Pure C, no Arduino/ESP-IDF headers, no allocation.

#include <stdbool.h>
#include <stdint.h>

#include "ui/canvas.h"
#include "ui/theme.h"

#ifdef __cplusplus
extern "C" {
#endif

/* One button and what it does there: "A" / "Select". */
typedef struct ui_hint_s {
    const char* button;
    const char* label;
} ui_hint_t;

/*
 * Down from the top of an h-tall box to where a line of `font` sits with its
 * capitals centred in it, kept inside the box.
 */
int16_t ui_text_dy(int16_t h, uint8_t font);

/*
 * The title in the header font from UI_TEXT_X, where the rows' text starts,
 * and `right` (may be NULL) in grey prose at the window's right on the same
 * baseline, over a UI_HEADER_H band cleared to the background. No band
 * colour, no rule.
 */
void ui_header(const ui_canvas_t* cv, int16_t w, const char* title,
               const char* right);

/*
 * An unselected row: the w x h box cleared, and `s` in `fg` from the same x
 * the pill's text starts at, so a row does not shift when the pill moves on
 * or off it.
 */
void ui_row_text(const ui_canvas_t* cv, int16_t x, int16_t y, int16_t w,
                 int16_t h, const char* s, uint8_t font, uint16_t fg);

/*
 * The selected row: a white pill that hugs `s`, never wider than max_w, with
 * the text black on it, or the dim-on-pill grey when dim. It is built
 * offscreen when the canvas can, sized to the pill and not the row, and then
 * `s` starts marquee_px to the left and the pill's edges clip it. Without an
 * offscreen buffer the marquee is ignored and the caller clears the row's old
 * extent first. Returns the pill's width.
 */
int16_t ui_pill_row(const ui_canvas_t* cv, int16_t x, int16_t y,
                    int16_t max_w, int16_t h, const char* s, uint8_t font,
                    bool dim, int16_t marquee_px);

/* One line of grey prose from UI_TEXT_X (s may be NULL), clipped to one row
 * of w, at the top of a UI_HELP_H band. */
void ui_help_line(const ui_canvas_t* cv, int16_t w, int16_t y, const char* s);

/*
 * The button-hint footer: a UI_FOOT_H band cleared, and when n > 0 one dark
 * pill right-aligned to the window's inset holding, for each hint, a white
 * circle with the button's letter in bold black, then the label in white.
 * List B before A where both appear, as the buttons sit on the console.
 * Hints that would not fit inside the insets are dropped from the end, so
 * list the ones that matter most first.
 */
void ui_hint_bar(const ui_canvas_t* cv, int16_t w, int16_t y,
                 const ui_hint_t* hints, uint8_t n);

/*
 * A whole-window message: the title centred in the header font when it
 * fits, else wrapped over two rows of the list font, red only when is_error;
 * the body centred under it in grey prose, up to four rows; the hint footer
 * when n > 0.
 */
void ui_notice(const ui_canvas_t* cv, int16_t w, int16_t h, const char* title,
               const char* body, bool is_error, const ui_hint_t* hints,
               uint8_t n);

/*
 * Round a w x img_h image's corners, for the band of image rows
 * [row0, row0 + rows) held in px (px's first row is image row row0): every
 * pixel in the four r x r corners outside the quarter circle of radius r
 * becomes bg. Rows outside the top and bottom r are left alone, so a middle
 * band costs nothing.
 */
void ui_round_corners_565(uint16_t* px, int16_t w, int16_t img_h,
                          int16_t row0, int16_t rows, int16_t r, uint16_t bg);

/*
 * The same, for an image letterboxed on bg inside its w x h file — a
 * screenshot fitted to a square, say — so the corners are the picture's and
 * not the file's. The picture's box is found from its first row that is not
 * all bg, and taken to be centred, which is how the imaging tool pads. Feed
 * it every band in order, after one ui_inset_begin(); an image that is all
 * bg is left alone.
 */
typedef struct ui_inset_s {
    int16_t w, h;
    int16_t top, left;
    bool found;
} ui_inset_t;

void ui_inset_begin(ui_inset_t* in, int16_t w, int16_t h);
void ui_round_inset_565(ui_inset_t* in, uint16_t* px, int16_t row0,
                        int16_t rows, int16_t r, uint16_t bg);

#ifdef __cplusplus
}
#endif
