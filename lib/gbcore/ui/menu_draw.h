#pragma once
// The in-game menu's layout — everything the menu draws, with nothing it
// decides.
//
// The list of rows under the title, the Hotkeys page, the Cart Info page's
// title and description band, and the Save State screens' bars, question and
// message, each one function over the injected canvas. Every coordinate is
// window-relative: the host test paints a w x h buffer and the Arduino
// binding adds the per-unit game_x / game_y origin, and the same code serves
// both.
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

#ifdef __cplusplus
extern "C" {
#endif

/* More entries than fit, so the list scrolls a window of MENU_VISIBLE of
 * them. 7 x 26 + 40 = 222, inside the window's 240. */
#define MENU_VISIBLE 7
#define MENU_ROW_H   26
#define MENU_TOP     40 /* the title band above the first row */

/* The fork's row colour, kept so this looks like the rest of the UI. The
 * highlighted row is inverted, bold black on a white bar, because the fork's
 * near-black highlight was hard to find at a glance. The dimmed grey is a
 * bench value, not derived. */
#define MENU_ROW_BG 0x1082
#define MENU_HL_BG  0xFFFF
#define MENU_HL_FG  0x0000
#define MENU_HL_DIM 0x6B4D
#define MENU_TITLE  0xFFE0
#define MENU_DIM    0x7BEF
#define MENU_TEXT   0xFFFF
#define MENU_BG     0x0000

typedef struct menu_layout_s {
    int16_t w, h;
    uint8_t visible; /* list rows under the title */
} menu_layout_t;

/* False for a NULL out or a window too short for MENU_VISIBLE rows. */
bool menu_layout(int16_t w, int16_t h, menu_layout_t* out);

/* One list row: its label, its value on the right or NULL for an action, and
 * whether it is unavailable — dimmed, and never bold even highlighted. */
typedef struct menu_item_s {
    const char* label;
    const char* value;
    bool off;
} menu_item_t;

/* The list as the binding holds it: every row, the window's top one and the
 * highlighted one. */
typedef struct menu_view_s {
    const menu_item_t* items;
    uint8_t n;
    uint16_t first;
    uint16_t cursor;
} menu_view_t;

/* The whole list screen: the window cleared, the title, the rows. */
void menu_draw(const ui_canvas_t* cv, const menu_layout_t* g,
               const menu_view_t* v);

/* The rows in the window only; the title band is left alone, so a scroll
 * redraws without blanking the screen. */
void menu_draw_rows(const ui_canvas_t* cv, const menu_layout_t* g,
                    const menu_view_t* v);

/* Row idx alone, if it is in the window: a move repaints two of these, a
 * value change one. */
void menu_draw_row(const ui_canvas_t* cv, const menu_layout_t* g,
                   const menu_view_t* v, uint16_t idx);

/* One bar whose top is y: the label left, the value (if any) right. What a
 * list row is, and what the Save State screens choose between. */
void menu_draw_bar(const ui_canvas_t* cv, const menu_layout_t* g, int16_t y,
                   const char* label, const char* value, bool highlighted,
                   bool off);

/* The window cleared and a page's title across its top. */
void menu_draw_title(const ui_canvas_t* cv, const menu_layout_t* g,
                     const char* title);

/* "B: Back", bottom left: the way off every page the list opens. */
void menu_draw_back(const ui_canvas_t* cv, const menu_layout_t* g);

/* The Hotkeys page in full: n combo / action pairs under its title. */
void menu_draw_hotkeys(const ui_canvas_t* cv, const menu_layout_t* g,
                       const char* const (*keys)[2], uint8_t n);

/* Cart Info's left margin and width, and its images' gap. */
#define MENU_CART_X   8
#define MENU_CART_W(g) ((int16_t)((g)->w - 2 * MENU_CART_X))
#define MENU_CART_GAP 8

/*
 * Cart Info's top: the window cleared and the title over up to two rows.
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
    uint8_t cols;
    uint16_t rows;
    uint16_t lines;
    uint16_t first;
} menu_band_t;

/* The band from y down to the Back line, as many lines as fit. */
void menu_band_fit(const menu_layout_t* g, const char* text, int16_t y,
                   menu_band_t* b);

/* The band cleared and drawn alone — what a scroll repaints. */
void menu_draw_band(const ui_canvas_t* cv, const menu_band_t* b);

/* The Save State question, wrapped over up to three rows under the title. */
void menu_draw_question(const ui_canvas_t* cv, const menu_layout_t* g,
                        const char* q);

/* One centred line of font 2 whose middle is y_mid. */
void menu_draw_message(const ui_canvas_t* cv, const menu_layout_t* g,
                       const char* msg, int16_t y_mid, uint16_t fg);

#ifdef __cplusplus
}
#endif
