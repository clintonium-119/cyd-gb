#include "ui/menu_draw.h"

#include <stddef.h>

#include "ui/picker_draw.h"

/* Font 2 is 16 px and font 4 is 26: what a middle datum subtracts to put a
 * row's centre on a line. */
#define HALF_ROW   (ui_font_height(UI_FONT_ROW) / 2)
#define HALF_TITLE (ui_font_height(UI_FONT_TITLE) / 2)

/* Where the title's middle sits, as it always has. */
#define TITLE_MID 18

/* The Back line's top, bottom left of every page the list opens. */
#define FOOT_Y(g) ((int16_t)((g)->h - 18))

static int16_t font_pitch(uint8_t font)
{
    return (int16_t)UI_ROW_PITCH(ui_font_height(font));
}

bool menu_layout(int16_t w, int16_t h, menu_layout_t* out)
{
    if (out == NULL || w <= 16 || h < MENU_TOP + MENU_VISIBLE * MENU_ROW_H) {
        return false;
    }
    out->w = w;
    out->h = h;
    out->visible = MENU_VISIBLE;
    return true;
}

void menu_draw_bar(const ui_canvas_t* cv, const menu_layout_t* g, int16_t y,
                   const char* label, const char* value, bool highlighted,
                   bool off)
{
    const uint16_t bg = highlighted ? MENU_HL_BG : MENU_ROW_BG;
    const int16_t bold = (highlighted && !off) ? 1 : 0;
    const int16_t text_y = (int16_t)(y + MENU_ROW_H / 2 - HALF_ROW);
    const int16_t text_w = (int16_t)(g->w - 16);
    uint16_t fg;
    int16_t dx;

    if (highlighted) {
        fg = off ? MENU_HL_DIM : MENU_HL_FG;
    } else {
        fg = off ? MENU_DIM : MENU_TEXT;
    }

    /* Bold is the same glyphs struck twice a pixel apart, so the text is
     * drawn transparent over the bar the fill already laid down; an opaque
     * second pass would wipe the first one's edge. */
    cv->fill(cv->ctx, 4, y, (int16_t)(g->w - 8), MENU_ROW_H - 2, bg);
    for (dx = 0; dx <= bold; dx++) {
        cv->text(cv->ctx, label, (int16_t)(8 + dx), text_y, text_w, 1,
                 UI_FONT_ROW, UI_ALIGN_LEFT, fg, fg);
        if (value != NULL) {
            cv->text(cv->ctx, value, 8, text_y, (int16_t)(text_w - dx), 1,
                     UI_FONT_ROW, UI_ALIGN_RIGHT, fg, fg);
        }
    }
}

void menu_draw_row(const ui_canvas_t* cv, const menu_layout_t* g,
                   const menu_view_t* v, uint16_t idx)
{
    const menu_item_t* it;

    if (idx < v->first || idx >= v->n || idx - v->first >= g->visible) {
        return;
    }
    it = &v->items[idx];
    menu_draw_bar(cv, g,
                  (int16_t)(MENU_TOP + (idx - v->first) * MENU_ROW_H),
                  it->label, it->value, idx == v->cursor, it->off);
}

void menu_draw_rows(const ui_canvas_t* cv, const menu_layout_t* g,
                    const menu_view_t* v)
{
    uint16_t i;

    for (i = v->first; i < v->n && i - v->first < g->visible; i++) {
        menu_draw_row(cv, g, v, i);
    }
}

void menu_draw_title(const ui_canvas_t* cv, const menu_layout_t* g,
                     const char* title)
{
    cv->fill(cv->ctx, 0, 0, g->w, g->h, MENU_BG);
    cv->text(cv->ctx, title, 0, (int16_t)(TITLE_MID - HALF_TITLE), g->w, 1,
             UI_FONT_TITLE, UI_ALIGN_CENTER, MENU_TITLE, MENU_BG);
}

void menu_draw(const ui_canvas_t* cv, const menu_layout_t* g,
               const menu_view_t* v)
{
    menu_draw_title(cv, g, "PAUSED");
    menu_draw_rows(cv, g, v);
}

void menu_draw_back(const ui_canvas_t* cv, const menu_layout_t* g)
{
    cv->text(cv->ctx, "B: Back", 8, FOOT_Y(g), (int16_t)(g->w - 16), 1,
             UI_FONT_ROW, UI_ALIGN_LEFT, MENU_TEXT, MENU_BG);
}

void menu_draw_hotkeys(const ui_canvas_t* cv, const menu_layout_t* g,
                       const char* const (*keys)[2], uint8_t n)
{
    int16_t y = (int16_t)(MENU_TOP + MENU_ROW_H / 2 - HALF_ROW);
    uint8_t i;

    menu_draw_title(cv, g, "HOTKEYS");
    for (i = 0; i < n; i++) {
        cv->text(cv->ctx, keys[i][0], 8, y, (int16_t)(g->w - 16), 1,
                 UI_FONT_ROW, UI_ALIGN_LEFT, MENU_TEXT, MENU_BG);
        cv->text(cv->ctx, keys[i][1], 8, y, (int16_t)(g->w - 16), 1,
                 UI_FONT_ROW, UI_ALIGN_RIGHT, MENU_TEXT, MENU_BG);
        y = (int16_t)(y + MENU_ROW_H);
    }
    menu_draw_back(cv, g);
}

int16_t menu_draw_cart_title(const ui_canvas_t* cv, const menu_layout_t* g,
                             const char* title)
{
    int16_t rows;

    cv->fill(cv->ctx, 0, 0, g->w, g->h, MENU_BG);
    if (title == NULL) {
        return MENU_CART_X;
    }
    /* Two rows because a catalog title runs to 47 characters; one that fits
     * a row leaves the other to the description. */
    rows = (cv->measure(cv->ctx, title, UI_FONT_ROW) <= MENU_CART_W(g)) ? 1
                                                                        : 2;
    cv->text(cv->ctx, title, MENU_CART_X, MENU_CART_X, MENU_CART_W(g),
             (uint8_t)rows, UI_FONT_ROW, UI_ALIGN_LEFT, MENU_TEXT, MENU_BG);
    return (int16_t)(MENU_CART_X + rows * font_pitch(UI_FONT_ROW) + 2);
}

void menu_band_fit(const menu_layout_t* g, const char* text, int16_t y,
                   menu_band_t* b)
{
    const int16_t pitch = font_pitch(UI_FONT_SMALL);
    const int16_t room = (int16_t)(FOOT_Y(g) - y - 2);

    b->text = text;
    b->x = MENU_CART_X;
    b->y = y;
    b->w = MENU_CART_W(g);
    b->cols = (uint8_t)(b->w / UI_FONT_SMALL_ADV);
    b->rows = (text != NULL && text[0] != '\0' && room > 0)
                  ? (uint16_t)(room / pitch)
                  : 0;
    b->lines = picker_desc_lines(text, b->cols);
    b->first = 0;
}

void menu_draw_band(const ui_canvas_t* cv, const menu_band_t* b)
{
    char line[PICKER_DESC_LINE_MAX];
    const int16_t pitch = font_pitch(UI_FONT_SMALL);
    uint16_t i;

    if (b->rows == 0) {
        return;
    }
    cv->fill(cv->ctx, b->x, b->y, b->w, (int16_t)(b->rows * pitch), MENU_BG);
    for (i = 0; i < b->rows; i++) {
        if (!picker_desc_line(b->text, b->cols, (uint16_t)(b->first + i), line,
                              sizeof(line))) {
            break;
        }
        cv->text(cv->ctx, line, b->x, (int16_t)(b->y + i * pitch), b->w, 1,
                 UI_FONT_SMALL, UI_ALIGN_LEFT, MENU_TEXT, MENU_BG);
    }
}

void menu_draw_question(const ui_canvas_t* cv, const menu_layout_t* g,
                        const char* q)
{
    cv->text(cv->ctx, q, 16, MENU_TOP + 8, (int16_t)(g->w - 32), 3,
             UI_FONT_ROW, UI_ALIGN_CENTER, MENU_TEXT, MENU_BG);
}

void menu_draw_message(const ui_canvas_t* cv, const menu_layout_t* g,
                       const char* msg, int16_t y_mid, uint16_t fg)
{
    cv->text(cv->ctx, msg, 0, (int16_t)(y_mid - HALF_ROW), g->w, 1,
             UI_FONT_ROW, UI_ALIGN_CENTER, fg, MENU_BG);
}
