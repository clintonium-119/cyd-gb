#include "ui/menu_draw.h"

#include <stddef.h>

#include "ui/picker_draw.h"

/* A row's box spans the window less its inset; its pill is a little shorter
 * than the row, so two selected rows never touch. */
#define ROW_X            UI_PAD
#define ROW_W(g)         ((int16_t)((g)->w - 2 * UI_PAD))
#define PILL_DY          ((MENU_ROW_H - UI_PILL_H_ROW) / 2)

/* The right edge a value is set against: the pill's own text inset. */
#define VALUE_RIGHT(g)   ((int16_t)((g)->w - UI_PAD - UI_PILL_PAD))
#define VALUE_BOX_X      ((int16_t)(UI_PAD + UI_PILL_PAD))

/* Down from a box's top to where one row of `font` sits centred in it. */
static int16_t text_dy(int16_t h, uint8_t font)
{
    return (int16_t)((h - UI_ROW_PITCH(ui_font_height(font))) / 2);
}

static int16_t font_pitch(uint8_t font)
{
    return (int16_t)UI_ROW_PITCH(ui_font_height(font));
}

bool menu_layout(int16_t w, int16_t h, menu_layout_t* out)
{
    if (out == NULL || w <= 2 * (UI_PAD + UI_PILL_PAD)) {
        return false;
    }
    out->w = w;
    out->h = h;
    out->foot_y = (int16_t)(h - UI_FOOT_H);
    out->help_y = (int16_t)(out->foot_y - UI_HELP_H);
    out->visible = MENU_VISIBLE;
    return out->help_y >= MENU_TOP + MENU_VISIBLE * MENU_ROW_H;
}

void menu_draw_bar(const ui_canvas_t* cv, const menu_layout_t* g, int16_t y,
                   const char* label, const char* value, bool highlighted,
                   bool off)
{
    const int16_t ty = (int16_t)(y + text_dy(MENU_ROW_H, UI_FONT_ROW));
    const int16_t bold = (highlighted && !off) ? 1 : 0;
    const uint16_t fg = off ? UI_COL_DIM : UI_COL_TEXT;
    int16_t dx;

    if (!highlighted) {
        ui_row_text(cv, ROW_X, y, ROW_W(g), MENU_ROW_H, label, UI_FONT_ROW,
                    fg);
    } else {
        /* The pill stops short of the value, which sits outside it. */
        int16_t max_w = ROW_W(g);

        if (value != NULL) {
            max_w = (int16_t)(max_w - cv->measure(cv->ctx, value, UI_FONT_ROW) -
                              UI_PILL_PAD);
        }
        cv->fill(cv->ctx, ROW_X, y, ROW_W(g), MENU_ROW_H, UI_COL_BG);
        ui_pill_row(cv, ROW_X, (int16_t)(y + PILL_DY), max_w, UI_PILL_H_ROW,
                    label, UI_FONT_ROW, true, off, 0);
    }
    if (value == NULL) {
        return;
    }
    /* Transparent, and struck twice for bold on the selected row, as the
     * label is. */
    for (dx = 0; dx <= bold; dx++) {
        cv->text(cv->ctx, value, VALUE_BOX_X, ty,
                 (int16_t)(VALUE_RIGHT(g) - VALUE_BOX_X - dx), 1, UI_FONT_ROW,
                 UI_ALIGN_RIGHT, fg, fg);
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
    cv->fill(cv->ctx, 0, 0, g->w, g->h, UI_COL_BG);
    ui_header(cv, g->w, title, NULL);
}

void menu_draw_footer(const ui_canvas_t* cv, const menu_layout_t* g,
                      const char* help, const ui_hint_t* hints, uint8_t n)
{
    ui_help_line(cv, g->w, g->help_y, help);
    ui_hint_bar(cv, g->w, g->foot_y, hints, n);
}

void menu_draw(const ui_canvas_t* cv, const menu_layout_t* g,
               const menu_view_t* v)
{
    menu_draw_title(cv, g, "Paused");
    menu_draw_rows(cv, g, v);
    menu_draw_footer(cv, g, v->help, v->hints, v->n_hints);
}

void menu_draw_hotkeys(const ui_canvas_t* cv, const menu_layout_t* g,
                       const char* const (*keys)[2], uint8_t n)
{
    static const ui_hint_t HINTS[] = { { "B", "Back" } };
    int16_t y = (int16_t)(MENU_TOP + text_dy(MENU_ROW_H, UI_FONT_ROW));
    uint8_t i;

    menu_draw_title(cv, g, "Hotkeys");
    for (i = 0; i < n; i++) {
        cv->text(cv->ctx, keys[i][0], VALUE_BOX_X, y,
                 (int16_t)(VALUE_RIGHT(g) - VALUE_BOX_X), 1, UI_FONT_ROW,
                 UI_ALIGN_LEFT, UI_COL_TEXT, UI_COL_BG);
        cv->text(cv->ctx, keys[i][1], VALUE_BOX_X, y,
                 (int16_t)(VALUE_RIGHT(g) - VALUE_BOX_X), 1, UI_FONT_ROW,
                 UI_ALIGN_RIGHT, UI_COL_DIM, UI_COL_BG);
        y = (int16_t)(y + MENU_ROW_H);
    }
    menu_draw_footer(cv, g, "Button combos that work while a game runs.",
                     HINTS, 1);
}

int16_t menu_draw_cart_title(const ui_canvas_t* cv, const menu_layout_t* g,
                             const char* title)
{
    int16_t rows;

    cv->fill(cv->ctx, 0, 0, g->w, g->h, UI_COL_BG);
    if (title == NULL) {
        return MENU_CART_X;
    }
    /* Two rows because a catalog title runs to 47 characters; one that fits
     * a row leaves the other to the description. */
    rows = (cv->measure(cv->ctx, title, UI_FONT_ROW) <= MENU_CART_W(g)) ? 1
                                                                        : 2;
    cv->text(cv->ctx, title, MENU_CART_X, MENU_CART_X, MENU_CART_W(g),
             (uint8_t)rows, UI_FONT_ROW, UI_ALIGN_LEFT, UI_COL_TEXT,
             UI_COL_BG);
    return (int16_t)(MENU_CART_X + rows * font_pitch(UI_FONT_ROW) + 2);
}

void menu_band_fit(const menu_layout_t* g, const char* text, int16_t y,
                   menu_band_t* b)
{
    const int16_t pitch = font_pitch(UI_FONT_SMALL);
    const int16_t room = (int16_t)(g->foot_y - y - 2);

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
    cv->fill(cv->ctx, b->x, b->y, b->w, (int16_t)(b->rows * pitch),
             UI_COL_BG);
    for (i = 0; i < b->rows; i++) {
        if (!picker_desc_line(b->text, b->cols, (uint16_t)(b->first + i), line,
                              sizeof(line))) {
            break;
        }
        cv->text(cv->ctx, line, b->x, (int16_t)(b->y + i * pitch), b->w, 1,
                 UI_FONT_SMALL, UI_ALIGN_LEFT, UI_COL_DIM, UI_COL_BG);
    }
}

void menu_draw_question(const ui_canvas_t* cv, const menu_layout_t* g,
                        const char* q)
{
    cv->text(cv->ctx, q, 16, MENU_TOP + 8, (int16_t)(g->w - 32), 3,
             UI_FONT_ROW, UI_ALIGN_CENTER, UI_COL_TEXT, UI_COL_BG);
}

void menu_draw_slot(const ui_canvas_t* cv, int16_t x, int16_t y, int16_t w,
                    int16_t h, const char* msg)
{
    cv->round_fill(cv->ctx, x, y, w, h, UI_IMG_R, UI_COL_SLOT, UI_COL_BG);
    cv->text(cv->ctx, msg, x,
             (int16_t)(y + h / 2 - ui_font_height(UI_FONT_SMALL) / 2), w, 1,
             UI_FONT_SMALL, UI_ALIGN_CENTER, UI_COL_DIM, UI_COL_SLOT);
}
