#include "ui/theme_draw.h"

#include <stddef.h>

/* Down from a box's top to where one row of `font` sits centred in it. */
static int16_t text_dy(int16_t h, uint8_t font)
{
    return (int16_t)((h - UI_ROW_PITCH(ui_font_height(font))) / 2);
}

void ui_header(const ui_canvas_t* cv, int16_t w, const char* title,
               const char* right)
{
    int16_t y = text_dy(UI_HEADER_H, UI_FONT_HEADER);
    int16_t tw = (int16_t)(w - 2 * UI_PAD);

    cv->fill(cv->ctx, 0, 0, w, UI_HEADER_H, UI_COL_BG);
    if (title != NULL) {
        cv->text(cv->ctx, title, UI_PAD, y, tw, 1, UI_FONT_HEADER,
                 UI_ALIGN_LEFT, UI_COL_TEXT, UI_COL_BG);
    }
    if (right != NULL) {
        cv->text(cv->ctx, right, UI_PAD, y, tw, 1, UI_FONT_HEADER,
                 UI_ALIGN_RIGHT, UI_COL_DIM, UI_COL_BG);
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
             (int16_t)(y + text_dy(h, font)), tw, 1, font, UI_ALIGN_LEFT, fg,
             UI_COL_BG);
}

int16_t ui_pill_row(const ui_canvas_t* cv, int16_t x, int16_t y,
                    int16_t max_w, int16_t h, const char* s, uint8_t font,
                    bool bold, bool dim, int16_t marquee_px)
{
    int16_t tw;
    int16_t pw;
    int16_t tx;
    int16_t txw;
    int16_t ty;
    int16_t dx;
    uint16_t fg;
    bool off;

    if (s == NULL) {
        s = "";
    }
    if (dim) {
        bold = false;
    }
    tw = cv->measure(cv->ctx, s, font);
    pw = (int16_t)(tw + 2 * UI_PILL_PAD + (bold ? 1 : 0));
    if (pw > max_w) {
        pw = max_w;
    }
    tx = (int16_t)(x + UI_PILL_PAD);
    txw = (int16_t)(pw - 2 * UI_PILL_PAD);
    ty = (int16_t)(y + text_dy(h, font));
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
    /* Transparent, fg == bg: an opaque second pass would wipe the first
     * pass's right edge. */
    for (dx = 0; dx <= (bold ? 1 : 0); dx++) {
        cv->text(cv->ctx, s, (int16_t)(tx + dx), ty, txw, 1, font,
                 UI_ALIGN_LEFT, fg, fg);
    }
    if (off) {
        cv->end(cv->ctx);
    }
    return pw;
}

void ui_help_line(const ui_canvas_t* cv, int16_t w, int16_t y, const char* s)
{
    cv->fill(cv->ctx, 0, y, w, UI_HELP_H, UI_COL_BG);
    if (s != NULL) {
        cv->text(cv->ctx, s, UI_PAD, (int16_t)(y + text_dy(UI_HELP_H,
                                                             UI_FONT_HELP)),
                 (int16_t)(w - 2 * UI_PAD), 1, UI_FONT_HELP, UI_ALIGN_LEFT,
                 UI_COL_DIM, UI_COL_BG);
    }
}

/* A button's glyph pill: round for one letter, stretched for a word. */
static int16_t glyph_w(const ui_canvas_t* cv, const char* button)
{
    int16_t w = (int16_t)(cv->measure(cv->ctx, button, UI_FONT_HINT) + 6);

    return (w > UI_GLYPH_D) ? w : UI_GLYPH_D;
}

void ui_hint_bar(const ui_canvas_t* cv, int16_t w, int16_t y,
                 const ui_hint_t* hints, uint8_t n)
{
    /* The outer pill sits a pixel inside the band, and its glyph pills sit
     * as far inside it on the left as they do top and bottom. */
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
    cv->round_fill(cv->ctx, cx, oy, ow, oh, UI_PILL_R(oh), UI_COL_PILL,
                   UI_COL_BG);
    cx = (int16_t)(cx + m);
    for (i = 0; i < n; i++) {
        int16_t gw = glyph_w(cv, hints[i].button);
        int16_t lw = cv->measure(cv->ctx, hints[i].label, UI_FONT_HINT);

        cv->round_fill(cv->ctx, cx, (int16_t)(oy + m), gw, UI_GLYPH_D,
                       UI_PILL_R(UI_GLYPH_D), UI_COL_GLYPH, UI_COL_PILL);
        /* Transparent, so the glyph pill's round ends survive the text box. */
        cv->text(cv->ctx, hints[i].button, cx,
                 (int16_t)(oy + m + text_dy(UI_GLYPH_D, UI_FONT_HINT)), gw, 1,
                 UI_FONT_HINT, UI_ALIGN_CENTER, UI_COL_TEXT, UI_COL_TEXT);
        cx = (int16_t)(cx + gw + UI_HINT_GAP);
        cv->text(cv->ctx, hints[i].label, cx,
                 (int16_t)(oy + text_dy(oh, UI_FONT_HINT)), lw, 1,
                 UI_FONT_HINT, UI_ALIGN_LEFT, UI_COL_HINT, UI_COL_PILL);
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
            cv->text(cv->ctx, title, UI_PAD, (int16_t)(cy - 28), bw, 2,
                     UI_FONT_ROW, UI_ALIGN_CENTER, tc, UI_COL_BG);
            body_y = (int16_t)(cy - 28 +
                               2 * UI_ROW_PITCH(ui_font_height(UI_FONT_ROW)) +
                               6);
        }
    }
    if (body != NULL && body[0] != '\0') {
        cv->text(cv->ctx, body, UI_PAD, body_y, bw, 4, UI_FONT_ROW,
                 UI_ALIGN_CENTER, UI_COL_DIM, UI_COL_BG);
    }
    if (n > 0) {
        ui_hint_bar(cv, w, (int16_t)(h - UI_FOOT_H), hints, n);
    }
}

void ui_round_corners_565(uint16_t* px, int16_t w, int16_t img_h,
                          int16_t row0, int16_t rows, int16_t r, uint16_t bg)
{
    int16_t i;
    int16_t j;

    if (px == NULL || r <= 0 || 2 * r > w || 2 * r > img_h) {
        return;
    }
    for (i = 0; i < rows; i++) {
        int16_t iy = (int16_t)(row0 + i);
        int32_t dy;
        uint16_t* line = px + (size_t)i * (size_t)w;

        /* Distance from the arc's centre row, or not a corner row at all. */
        if (iy < r) {
            dy = r - iy;
        } else if (iy >= img_h - r) {
            dy = iy - (img_h - 1 - r);
        } else {
            continue;
        }
        for (j = 0; j < r; j++) {
            int32_t dx = r - j;

            if (dx * dx + dy * dy > (int32_t)r * r) {
                line[j] = bg;
                line[w - 1 - j] = bg;
            }
        }
    }
}
