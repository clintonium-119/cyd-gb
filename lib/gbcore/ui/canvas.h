#pragma once
// The injected draw seam, and the three font ids that go with it.
//
// Two layout modules in this library paint through it — the cartridge writer's
// and the diagnostic mode's — and one Arduino binding implements it against
// the panel. It lives in its own header for exactly that reason: it is the
// boundary between the pure layout code and the driver, and neither of the
// layout modules owns it.
//
// Every coordinate that crosses it is window-relative. The host tests paint a
// w x h buffer and the binding adds the per-unit game_x / game_y origin, and
// the same layout code serves both.
//
// Pure C, no Arduino/ESP-IDF headers, no allocation.

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum ui_align_e {
    UI_ALIGN_LEFT = 0,
    UI_ALIGN_CENTER,
    UI_ALIGN_RIGHT,
};

/*
 * The injected draw seam, in the shape catalog_reader_t
 * established for gbcore: a ctx the caller owns plus function pointers.
 *
 * `text` is a box, not a baseline: top-left x, y, width w, up to `rows` lines
 * of `font`, aligned within the box. The implementation clips at w and rows,
 * which is what display_draw_wrapped() already does.
 *
 * `image` takes a row range so the scrolling band can clip an image at its
 * edge: draw rows [row0, row0 + rows) of a w-wide image, with the top of that
 * range landing at y. The binding hands the driver px + row0 * w.
 *
 * A `text` call whose bg == fg paints only the glyph pixels and leaves the
 * background alone. That is how a label is struck twice, one pixel apart, for
 * bold: an opaque second pass would wipe the first pass's right edge.
 *
 * `round_fill` is `fill` with corners of radius r, anti-aliased toward bg:
 * the colour the rectangle sits on, which the edge pixels blend into. It is
 * what the selected row's pill and the button glyphs are drawn with.
 *
 * `measure` is the pixel width `s` takes in `font` on one row. It is required,
 * because a layout that scrolls an overflowing label has to know it overflows.
 *
 * `begin` and `end` are optional; NULL means "draw straight to the panel".
 * Between them, every fill, text and image lands in an offscreen w x h buffer
 * whose top-left is window position x, y, clipped to that buffer, and `end`
 * pushes the buffer to the panel in one go. `begin` returns false when it has
 * no buffer to give, and then nothing is redirected: the caller must draw as
 * if there were no clip, and need not call `end`. That is how one row repaints
 * without the panel ever showing it blank between its fill and its text.
 * Coordinates stay window-relative throughout.
 */
typedef struct ui_canvas_s {
    void* ctx;
    void (*fill)(void* ctx, int16_t x, int16_t y, int16_t w, int16_t h,
                 uint16_t color);
    void (*round_fill)(void* ctx, int16_t x, int16_t y, int16_t w, int16_t h,
                       int16_t r, uint16_t color, uint16_t bg);
    void (*text)(void* ctx, const char* s, int16_t x, int16_t y, int16_t w,
                 uint8_t rows, uint8_t font, uint8_t align, uint16_t fg,
                 uint16_t bg);
    void (*image)(void* ctx, int16_t x, int16_t y, int16_t w, int16_t h,
                  const uint16_t* px, int16_t row0, int16_t rows);
    int16_t (*measure)(void* ctx, const char* s, uint8_t font);
    bool (*begin)(void* ctx, int16_t x, int16_t y, int16_t w, int16_t h);
    void (*end)(void* ctx);
} ui_canvas_t;

/* TFT_eSPI built-in font ids. */
#define UI_FONT_SMALL 1
#define UI_FONT_ROW   2
#define UI_FONT_TITLE 4

/*
 * The theme's three GFX outline fonts, which the binding selects with
 * setFreeFont(): FreeSans 9pt for prose, FreeSansBold 9pt for list rows and
 * FreeSansBold 12pt for headers. Their ids sit past the built-in ones so the
 * two kinds never collide. All three are proportional: a layout measures
 * them through the canvas, and never assumes an advance.
 */
#define UI_FONT_TEXT 10
#define UI_FONT_BOLD 11
#define UI_FONT_HEAD 12

/*
 * Font 1 is GLCD: a FIXED 6-pixel advance per glyph. This is the one glyph
 * width a layout module may assume, and it is what makes the writer's word
 * wrap exact — a line of `cols` characters measures cols * 6 pixels, so the
 * break points a layout computes and the ones display_draw_wrapped() arrives
 * at by measuring agree to the pixel. Fonts 2 and 4 are proportional and are
 * never measured by a layout; the driver clips them at the box it is given.
 */
#define UI_FONT_SMALL_ADV 6

/* Heights of the fonts — for a GFX font its tallest ascent plus its deepest
 * descent, the box its text fills — and the row pitch display_draw_wrapped()
 * uses. */
uint8_t ui_font_height(uint8_t font);

/* A font's capital height, and the blank rows above its capitals inside its
 * box — what centring a line on its capitals, rather than on its box, needs.
 */
uint8_t ui_font_cap(uint8_t font);
uint8_t ui_font_lead(uint8_t font);
#define UI_ROW_PITCH(font_h) ((font_h) + 2)

#ifdef __cplusplus
}
#endif
