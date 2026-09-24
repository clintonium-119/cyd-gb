#include "ui/theme_draw.h"

#include <stddef.h>

int16_t ui_text_dy(int16_t h, uint8_t font)
{
    int16_t dy = (int16_t)((h - ui_font_cap(font)) / 2 - ui_font_lead(font));
    int16_t room = (int16_t)(h - ui_font_height(font));

    if (dy > room) {
        dy = room;
    }
    return (dy > 0) ? dy : 0;
}

void ui_header(const ui_canvas_t* cv, int16_t w, const char* title,
               const char* right)
{
    const int16_t y = ui_text_dy(UI_HEADER_H, UI_FONT_HEADER);
    /* The counter's baseline on the title's. */
    const int16_t ry = (int16_t)(y + ui_font_lead(UI_FONT_HEADER) +
                                 ui_font_cap(UI_FONT_HEADER) -
                                 ui_font_lead(UI_FONT_HELP) -
                                 ui_font_cap(UI_FONT_HELP));

    cv->fill(cv->ctx, 0, 0, w, UI_HEADER_H, UI_COL_BG);
    if (title != NULL) {
        cv->text(cv->ctx, title, UI_TEXT_X, y, (int16_t)(w - 2 * UI_TEXT_X),
                 1, UI_FONT_HEADER, UI_ALIGN_LEFT, UI_COL_TEXT, UI_COL_BG);
    }
    if (right != NULL) {
        cv->text(cv->ctx, right, UI_TEXT_X, ry, (int16_t)(w - 2 * UI_TEXT_X),
                 1, UI_FONT_HELP, UI_ALIGN_RIGHT, UI_COL_SUB, UI_COL_BG);
    }
}

void ui_row_text(const ui_canvas_t* cv, int16_t x, int16_t y, int16_t w,
                 int16_t h, const char* s, uint8_t font, uint16_t fg)
{
    int16_t tw = (int16_t)(w - 2 * UI_PILL_PAD);

    cv->fill(cv->ctx, x, y, w, h, UI_COL_BG);
    if (s == NULL) {
        return;
    }
    /* Boxed to the text itself when it fits, so the box lies wholly under
     * where the pill will go when this row is selected. */
    if (cv->measure != NULL && cv->measure(cv->ctx, s, font) < tw) {
        tw = cv->measure(cv->ctx, s, font);
    }
    cv->text(cv->ctx, s, (int16_t)(x + UI_PILL_PAD),
             (int16_t)(y + ui_text_dy(h, font)), tw, 1, font, UI_ALIGN_LEFT,
             fg, UI_COL_BG);
}

int16_t ui_pill_row(const ui_canvas_t* cv, int16_t x, int16_t y,
                    int16_t max_w, int16_t h, const char* s, uint8_t font,
                    bool dim, int16_t marquee_px)
{
    int16_t tw;
    int16_t pw;
    int16_t tx;
    int16_t txw;
    uint16_t fg;
    bool off;

    if (s == NULL) {
        s = "";
    }
    tw = cv->measure(cv->ctx, s, font);
    pw = (int16_t)(tw + 2 * UI_PILL_PAD);
    if (pw > max_w) {
        pw = max_w;
    }
    tx = (int16_t)(x + UI_PILL_PAD);
    txw = (int16_t)(pw - 2 * UI_PILL_PAD);
    fg = dim ? UI_COL_PILL_DIM : UI_COL_PILL_TEXT;

    off = cv->begin != NULL && cv->end != NULL &&
          cv->begin(cv->ctx, x, y, pw, h);
    if (off) {
        /* The buffer is the pill, so it clips the label wherever it runs. */
        tx = (int16_t)(tx - marquee_px);
        txw = (int16_t)(tw + marquee_px + UI_PILL_PAD);
    }
    /* The pill's corners blend toward black, so its box is black first. */
    cv->fill(cv->ctx, x, y, pw, h, UI_COL_BG);
    cv->round_fill(cv->ctx, x, y, pw, h, UI_PILL_R(h), UI_COL_PILL, UI_COL_BG);
    /* Transparent, fg == bg, so the pill's round ends survive the box. */
    cv->text(cv->ctx, s, tx, (int16_t)(y + ui_text_dy(h, font)), txw, 1, font,
             UI_ALIGN_LEFT, fg, fg);
    if (off) {
        cv->end(cv->ctx);
    }
    return pw;
}

void ui_help_line(const ui_canvas_t* cv, int16_t w, int16_t y, const char* s)
{
    cv->fill(cv->ctx, 0, y, w, UI_HELP_H, UI_COL_BG);
    if (s != NULL) {
        cv->text(cv->ctx, s, UI_TEXT_X, y, (int16_t)(w - 2 * UI_TEXT_X), 1,
                 UI_FONT_HELP, UI_ALIGN_LEFT, UI_COL_SUB, UI_COL_BG);
    }
}

/* A button's circle: round for one letter, stretched for a word. */
static int16_t glyph_w(const ui_canvas_t* cv, const char* button)
{
    int16_t w = (int16_t)(cv->measure(cv->ctx, button, UI_FONT_HINT) + 6);

    return (w > UI_GLYPH_D) ? w : UI_GLYPH_D;
}

void ui_hint_bar(const ui_canvas_t* cv, int16_t w, int16_t y,
                 const ui_hint_t* hints, uint8_t n)
{
    /* The outer pill sits a pixel inside the band, and its circles sit as
     * far inside it on the left as they do top and bottom. */
    const int16_t oh = UI_FOOT_H - 2;
    const int16_t oy = (int16_t)(y + 1);
    const int16_t m = (int16_t)((oh - UI_GLYPH_D) / 2);
    int16_t ow = (int16_t)(m + UI_HINT_GAP);
    int16_t cx;
    uint8_t i;

    cv->fill(cv->ctx, 0, y, w, UI_FOOT_H, UI_COL_BG);
    if (hints == NULL || n == 0) {
        return;
    }
    /* As many hints as fit inside the window's insets, first ones first. */
    for (i = 0; i < n; i++) {
        int16_t more = (int16_t)(glyph_w(cv, hints[i].button) + UI_HINT_GAP +
                                 cv->measure(cv->ctx, hints[i].label,
                                             UI_FONT_HINT) +
                                 (i > 0 ? UI_HINT_GAP : 0));

        if (ow + more > w - 2 * UI_PAD) {
            break;
        }
        ow = (int16_t)(ow + more);
    }
    n = i;
    if (n == 0) {
        return;
    }
    cx = (int16_t)(w - UI_PAD - ow);
    cv->round_fill(cv->ctx, cx, oy, ow, oh, UI_PILL_R(oh), UI_COL_HINT_BG,
                   UI_COL_BG);
    cx = (int16_t)(cx + m);
    for (i = 0; i < n; i++) {
        int16_t gw = glyph_w(cv, hints[i].button);
        int16_t lw = cv->measure(cv->ctx, hints[i].label, UI_FONT_HINT);
        int16_t gy = (int16_t)(oy + m + ui_text_dy(UI_GLYPH_D, UI_FONT_HINT));
        int16_t dx;

        cv->round_fill(cv->ctx, cx, (int16_t)(oy + m), gw, UI_GLYPH_D,
                       UI_PILL_R(UI_GLYPH_D), UI_COL_GLYPH, UI_COL_HINT_BG);
        /* Bold, struck twice a pixel apart: the 8 px font has no bold.
         * ui_text_dy puts the letter on its capitals' middle. */
        for (dx = 0; dx <= 1; dx++) {
            cv->text(cv->ctx, hints[i].button, (int16_t)(cx + dx), gy, gw, 1,
                     UI_FONT_HINT, UI_ALIGN_CENTER, UI_COL_GLYPH_FG,
                     UI_COL_GLYPH_FG);
        }
        cx = (int16_t)(cx + gw + UI_HINT_GAP);
        cv->text(cv->ctx, hints[i].label, cx,
                 (int16_t)(oy + ui_text_dy(oh, UI_FONT_HINT)), lw, 1,
                 UI_FONT_HINT, UI_ALIGN_LEFT, UI_COL_TEXT, UI_COL_HINT_BG);
        cx = (int16_t)(cx + lw + UI_HINT_GAP);
    }
}

void ui_notice(const ui_canvas_t* cv, int16_t w, int16_t h, const char* title,
               const char* body, bool is_error, const ui_hint_t* hints,
               uint8_t n)
{
    const int16_t cy = (int16_t)(h / 2);
    const int16_t bw = (int16_t)(w - 2 * UI_PAD);
    const uint16_t tc = is_error ? UI_COL_WARN : UI_COL_TEXT;
    int16_t body_y = (int16_t)(cy + 10);

    cv->fill(cv->ctx, 0, 0, w, h, UI_COL_BG);
    if (title != NULL) {
        if (cv->measure(cv->ctx, title, UI_FONT_NOTICE) <= bw) {
            /* Centred on cy - 20, as the boot screen always put it. */
            cv->text(cv->ctx, title, UI_PAD,
                     (int16_t)(cy - 20 - ui_font_height(UI_FONT_NOTICE) / 2),
                     bw, 1, UI_FONT_NOTICE, UI_ALIGN_CENTER, tc, UI_COL_BG);
        } else {
            cv->text(cv->ctx, title, UI_PAD, (int16_t)(cy - 30), bw, 2,
                     UI_FONT_LIST, UI_ALIGN_CENTER, tc, UI_COL_BG);
            body_y = (int16_t)(cy - 30 +
                               2 * UI_ROW_PITCH(ui_font_height(UI_FONT_LIST)) +
                               4);
        }
    }
    if (body != NULL && body[0] != '\0') {
        cv->text(cv->ctx, body, UI_PAD, body_y, bw, 4, UI_FONT_DESC,
                 UI_ALIGN_CENTER, UI_COL_SUB, UI_COL_BG);
    }
    if (n > 0) {
        ui_hint_bar(cv, w, (int16_t)(h - UI_FOOT_H), hints, n);
    }
}

/* The corner pixels of one row of a cw-wide box starting at x0, dy rows from
 * its arc centres, that lie outside the radius. */
static void mask_row(uint16_t* line, int16_t x0, int16_t cw, int32_t dy,
                     int16_t r, uint16_t bg)
{
    int16_t j;

    for (j = 0; j < r; j++) {
        int32_t dx = r - j;

        if (dx * dx + dy * dy > (int32_t)r * r) {
            line[x0 + j] = bg;
            line[x0 + cw - 1 - j] = bg;
        }
    }
}

/* Rows [row0, row0 + rows) of px, masked against the ch-tall box whose top
 * image row is y0. */
static void mask_band(uint16_t* px, int16_t w, int16_t row0, int16_t rows,
                      int16_t x0, int16_t cw, int16_t y0, int16_t ch,
                      int16_t r, uint16_t bg)
{
    int16_t i;

    if (px == NULL || r <= 0 || 2 * r > cw || 2 * r > ch) {
        return;
    }
    for (i = 0; i < rows; i++) {
        int16_t iy = (int16_t)(row0 + i - y0);

        /* Distance from the arc's centre row, or not a corner row at all. */
        if (iy >= 0 && iy < r) {
            mask_row(px + (size_t)i * (size_t)w, x0, cw, r - iy, r, bg);
        } else if (iy >= ch - r && iy < ch) {
            mask_row(px + (size_t)i * (size_t)w, x0, cw,
                     iy - (ch - 1 - r), r, bg);
        }
    }
}

void ui_round_corners_565(uint16_t* px, int16_t w, int16_t img_h,
                          int16_t row0, int16_t rows, int16_t r, uint16_t bg)
{
    mask_band(px, w, row0, rows, 0, w, 0, img_h, r, bg);
}

void ui_inset_begin(ui_inset_t* in, int16_t w, int16_t h)
{
    in->w = w;
    in->h = h;
    in->top = 0;
    in->left = 0;
    in->found = false;
}

void ui_round_inset_565(ui_inset_t* in, uint16_t* px, int16_t row0,
                        int16_t rows, int16_t r, uint16_t bg)
{
    int16_t i;

    if (px == NULL) {
        return;
    }
    /* ponytail: the box comes from the first row with any picture in it and
     * is assumed centred; a picture whose own top rows are all bg is rounded
     * a little further in, where its corners are bg anyway. */
    for (i = 0; i < rows && !in->found; i++) {
        const uint16_t* line = px + (size_t)i * (size_t)in->w;
        int16_t a = 0;
        int16_t b = (int16_t)(in->w - 1);

        while (a < in->w && line[a] == bg) {
            a++;
        }
        if (a == in->w) {
            continue;
        }
        while (line[b] == bg) {
            b--;
        }
        in->found = true;
        in->top = (int16_t)(row0 + i);
        in->left = (a < in->w - 1 - b) ? a : (int16_t)(in->w - 1 - b);
    }
    if (!in->found) {
        return;
    }
    mask_band(px, in->w, row0, rows, in->left,
              (int16_t)(in->w - 2 * in->left), in->top,
              (int16_t)(in->h - 2 * in->top), r, bg);
}
