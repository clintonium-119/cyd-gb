#pragma once
// The diagnostic mode's layout — everything it draws, with nothing it decides
// (design §2.2, §8.2).
//
// A geometry derived from the window's w/h, and one diag_draw() that turns a
// diag_t and a diag_data_t into fills, text boxes and images over the injected
// canvas. Every coordinate is window-relative: the host test paints a w x h
// buffer and the Arduino binding adds the per-unit game_x / game_y origin, and
// the same code serves both.
//
// The pages share the theme's chrome: the page's title on the left and its
// number on the right, a grey help line saying what the page is for, and the
// button-hint footer naming what the buttons do there. Between them each page
// is its own thing: eight live button rows,
// four readouts, a tag dump, a battery, a tone, three test patterns, the
// nudge, and the build's own version.
//
// The checkerboard pattern is the one that needs the scaler. It is built at
// Game Boy resolution and pushed through scaler_scale_block() in blend mode,
// so the blend a builder judges by eye is the blend the game path produces —
// if the two ever look different, the difference is in the push, not here.
//
// Pure C, no Arduino/ESP-IDF headers, no allocation, no framebuffer of its
// own: the checkerboard's working buffers are caller-owned.

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#include "render/palette.h"
#include "render/scaler.h"
#include "ui/canvas.h"
#include "ui/diag.h"
#include "ui/theme.h"

#ifdef __cplusplus
extern "C" {
#endif

/* The theme's header, as the writer and the menu draw it. */
#define DIAG_HEADER_H UI_HEADER_H
/* Font 1's pitch: UI_ROW_PITCH(8). Every body row is one of these. */
#define DIAG_ROW_H 10
/* A page with fewer body rows than this cannot say anything useful, so the
 * geometry refuses the window rather than clipping silently. */
#define DIAG_MIN_ROWS 8
/* One Game Boy pixel per checkerboard cell. The smallest cell there is, and
 * the one that puts a blend seam between every pair of pixels — which is the
 * case worth looking at. */
#define DIAG_CHECKER_CELL 1

typedef struct diag_layout_s {
    int16_t w, h;
    int16_t body_y;   /* first body row's top: header plus a 2 px gap    */
    uint8_t rows;     /* body rows between body_y and the help line      */
    int16_t col_w;    /* width of a row's value box                      */
    int16_t label_w;  /* width of a row's label box, 12 font-1 columns   */
    int16_t bar_w;    /* one colour bar, w / 8                           */
    int16_t help_y;   /* the grey help line's top                        */
    int16_t foot_y;   /* the button-hint footer's top                    */
} diag_layout_t;

/*
 * Work out every number the pages need from the window size alone, so
 * the three render geometries are the same code with different inputs.
 *
 * DIAG_ERR_ARGS for a NULL out or a window with fewer than DIAG_MIN_ROWS body
 * rows.
 */
int diag_layout(int16_t w, int16_t h, diag_layout_t* out);

/*
 * The checkerboard's working buffers, caller-owned: the palette LUT, the two
 * alternating source lines, the pointer array one block call takes, one
 * scaled output block and the scaler's scratch row. About 8 KB, built once
 * per draw of that pattern and never on the frame path.
 */
typedef struct diag_checker_s {
    uint16_t lut[PALETTE_LUT_SIZE];
    uint16_t line[2][SCALER_SRC_W];
    const uint16_t* lines[SCALER_SRC_LINES_MAX];
    uint16_t block[SCALER_DST_ROWS_MAX * SCALER_DST_W_MAX];
    uint16_t scratch[SCALER_DST_W_MAX];
} diag_checker_t;

/*
 * Redraw the current page in full.
 *
 * Paints nothing when `d`, `data`, `g` or `cv` is NULL. `ck` may be NULL: the
 * checkerboard pattern then draws its border and says so instead, which is
 * what a binding that could not spare the buffer should show.
 *
 * `now_ms` is only ever compared against the state machine's own toast
 * deadline; nothing here keeps time.
 */
void diag_draw(const diag_t* d, const diag_data_t* data,
               const diag_layout_t* g, diag_checker_t* ck, uint32_t now_ms,
               const ui_canvas_t* cv);

/*
 * Fill `ck` for palette `idx`: the LUT, and the two source lines as a
 * one-pixel checkerboard of that palette's darkest and lightest background
 * shades. Called by diag_draw() for the checkerboard pattern; exposed so the
 * host suite can read back the two colours it is meant to see.
 *
 * Does nothing for a NULL ck.
 */
void diag_checker_build(diag_checker_t* ck, uint8_t idx);

#ifdef __cplusplus
}
#endif
