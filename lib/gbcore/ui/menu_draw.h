#pragma once
// The in-game menu's layout — everything the menu draws, with nothing it
// decides.
//
// The list of rows under the header, the Hotkeys page, the Cart Info page's
// title and description band, and the Save State screens' choices, question
// and message, each one function over the injected canvas, all in the theme's
// grammar: white on black, the selected row in a white pill, a grey help line
// over the button-hint footer. Every coordinate is window-relative: the host
// test paints a w x h buffer and the Arduino binding adds the per-unit
// game_x / game_y origin, and the same code serves both.
//
// The binding keeps the state machine, the card reads and the image bands:
// the cover, the snapshot and the save-state thumbnail arrive in bands small
// enough for the game-time heap and go to the canvas as they are read, so no
// function here ever sees a whole image.
//
// Pure C, no Arduino/ESP-IDF headers, no allocation, no framebuffer of its own.

#include <stdbool.h>
#include <stdint.h>

#include "ui/canvas.h"
#include "ui/picker_draw.h"
#include "ui/theme_draw.h"

#ifdef __cplusplus
extern "C" {
#endif

/* More entries than fit, so the list scrolls a window of MENU_VISIBLE of
 * them. The list has no header: 4 + 7 x 26 = 186, over the help line at
 * 202. */
#define MENU_VISIBLE 7
#define MENU_ROW_H   26
#define MENU_TOP     UI_PAD

typedef struct menu_layout_s {
    int16_t w, h;
    uint8_t visible; /* list rows under the header      */
    int16_t help_y;  /* the help line's top             */
    int16_t foot_y;  /* the hint footer's top           */
} menu_layout_t;

/* False for a NULL out or a window too short for MENU_VISIBLE rows above the
 * help line. */
bool menu_layout(int16_t w, int16_t h, menu_layout_t* out);

/* One list row: its label, its value on the right or NULL for an action, and
 * whether it is unavailable — dimmed, and never bold even highlighted. */
typedef struct menu_item_s {
    const char* label;
    const char* value;
    bool off;
} menu_item_t;

/* The list as the binding holds it: every row, the window's top one and the
 * highlighted one, and the help line and hints for that highlight. */
typedef struct menu_view_s {
    const menu_item_t* items;
    uint8_t n;
    uint16_t first;
    uint16_t cursor;
    const char* help;
    const ui_hint_t* hints;
    uint8_t n_hints;
} menu_view_t;

/* The whole list screen: the window cleared, the rows, the help line and
 * the hints. */
void menu_draw(const ui_canvas_t* cv, const menu_layout_t* g,
               const menu_view_t* v);

/* The rows in the window only; the title band is left alone, so a scroll
 * redraws without blanking the screen. */
void menu_draw_rows(const ui_canvas_t* cv, const menu_layout_t* g,
                    const menu_view_t* v);

/* Row idx alone, if it is in the window: a move repaints two of these and
 * the footer, a value change one. */
void menu_draw_row(const ui_canvas_t* cv, const menu_layout_t* g,
                   const menu_view_t* v, uint16_t idx);

/* One row whose top is y: the label left, the value (if any) right and
 * outside the pill. What a list row is, and what the Save State screens
 * choose between. The selected label sits in a pill that hugs it, bold
 * unless the row is off. */
void menu_draw_bar(const ui_canvas_t* cv, const menu_layout_t* g, int16_t y,
                   const char* label, const char* value, bool highlighted,
                   bool off);

/* One of the Save State screens' choices, a row w wide from the inset. */
void menu_draw_choice(const ui_canvas_t* cv, int16_t y, int16_t w,
                      const char* label, bool highlighted, bool off);

/* The window cleared and a page's header. */
void menu_draw_title(const ui_canvas_t* cv, const menu_layout_t* g,
                     const char* title);

/* The help line (help may be NULL) and the hint footer under it. */
void menu_draw_footer(const ui_canvas_t* cv, const menu_layout_t* g,
                      const char* help, const ui_hint_t* hints, uint8_t n);

/* The Hotkeys page in full: n combo / action pairs under its header, and
 * its footer. */
void menu_draw_hotkeys(const ui_canvas_t* cv, const menu_layout_t* g,
                       const char* const (*keys)[2], uint8_t n);

/* Cart Info's left margin and width, and its images' gap. */
#define MENU_CART_X   UI_TEXT_X
#define MENU_CART_W(g) ((int16_t)((g)->w - 2 * MENU_CART_X))
#define MENU_CART_GAP 8

/*
 * Game Details' top: the window cleared and the game's title over up to two
 * rows in the header font, which is the page's header, as on the writer's
 * page for a title.
 * Returns the y under it — one row lower for a title that fits one — which is
 * where the images go.
 */
int16_t menu_draw_cart_title(const ui_canvas_t* cv, const menu_layout_t* g,
                             const char* title);

/* The description under the images: `rows` lines of font 1 from wrapped line
 * `first`. rows == 0 means there is nothing to draw or scroll. */
typedef struct menu_band_s {
    const char* text;
    int16_t x;
    int16_t y;
    int16_t w;
    picker_adv_t adv;
    uint16_t rows;
    uint16_t lines;
    uint16_t first;
} menu_band_t;

/* The band from y down to the hint footer, as many lines as fit: Cart Info's
 * help line is its description. */
void menu_band_fit(const ui_canvas_t* cv, const menu_layout_t* g,
                   const char* text, int16_t y, menu_band_t* b);

/* The band cleared and drawn alone — what a scroll repaints. */
void menu_draw_band(const ui_canvas_t* cv, const menu_band_t* b);

/* The Save State question, wrapped over up to three rows under the header. */
void menu_draw_question(const ui_canvas_t* cv, const menu_layout_t* g,
                        const char* q);

/* An empty image slot: rounded, dark, with a grey word in it. */
void menu_draw_slot(const ui_canvas_t* cv, int16_t x, int16_t y, int16_t w,
                    int16_t h, const char* msg);

#ifdef __cplusplus
}
#endif
