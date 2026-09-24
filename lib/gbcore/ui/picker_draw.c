#include "picker_draw.h"

#include <stdio.h>
#include <string.h>

#include "ui/theme_draw.h"

#define PCT_FULL 100

/* Between the list's two stacked images: as much as the help line under them
 * leaves. */
#define PICKER_ART_GAP 2

/* The right edge of the list's title column: its rows' backgrounds end here,
 * and the image column starts. A row starts at the window's inset. */
#define TITLE_COL_W(g) ((int16_t)((g)->list_art_x - 4))
#define ROW_W(g)       ((int16_t)(TITLE_COL_W(g) - UI_PAD))

/* ─── strings ─────────────────────────────────────────────────────────────── */

static const char* const ACTION_LABEL[] = {
    "",                     /* PICKER_ROW_GAME — the catalog title  */
    "Cancel pending write", /* PICKER_ROW_CANCEL_PENDING            */
    "Finish setup",         /* PICKER_ROW_FINISH                    */
};


/* The footer's buttons: A opens a title, and on its page A is held to
 * confirm and B goes back. B does nothing on the list. */
static const ui_hint_t LIST_HINTS[] = {
    { "A", "Select" },
};
static const ui_hint_t DETAIL_HINTS[] = {
    { "B", "Back" },
    { "A", "Hold to install to cart" },
};
#define N_HINTS(a) ((uint8_t)(sizeof(a) / sizeof((a)[0])))

#define ART_MISSING   "no art"
#define SHOT_MISSING  "no snapshot"
#define MARK_PREFIX   "* "

/* ─── fonts ───────────────────────────────────────────────────────────────── */

static uint8_t desc_pitch(void)
{
    return (uint8_t)UI_ROW_PITCH(ui_font_height(UI_FONT_DESC));
}

static uint8_t title_pitch(void)
{
    return (uint8_t)UI_ROW_PITCH(ui_font_height(UI_FONT_HEADER));
}

/* ─── geometry ────────────────────────────────────────────────────────────── */

int picker_layout(int16_t w, int16_t h, picker_layout_t* out)
{
    uint8_t pitch = desc_pitch();

    if (out == NULL || pitch == 0) {
        return PICKER_ERR_ARGS;
    }
    memset(out, 0, sizeof(*out));
    memset(out->adv.w, 8, sizeof(out->adv.w));

    /* An image plus a column narrow enough to still wrap into, and both of
     * the list's images stacked above the help line. */
    if (w < (int16_t)(PICKER_ART_W + 16 + 48) ||
        h < (int16_t)(PICKER_LIST_TOP + 2 * PICKER_ART_H + PICKER_ART_GAP +
                      UI_HELP_H + UI_FOOT_H)) {
        return PICKER_ERR_ARGS;
    }

    out->w = w;
    out->h = h;
    out->foot_y = (int16_t)(h - UI_FOOT_H);
    out->help_y = (int16_t)(out->foot_y - UI_HELP_H);

    /* The list has no help line: its rows run down to the hints. */
    out->rows = (uint8_t)((out->foot_y - PICKER_LIST_TOP) / PICKER_ROW_H);
    /* The image column hugs the right edge; the titles get the rest, and a
     * title's text sits the pill's padding inside it. */
    out->list_art_x = (int16_t)(w - 4 - PICKER_ART_W);
    out->list_art_y = PICKER_LIST_TOP;
    out->list_shot_y = (int16_t)(out->list_art_y + PICKER_ART_H + PICKER_ART_GAP);
    out->list_w = (int16_t)(ROW_W(out) - 2 * UI_PILL_PAD);

    /* Cart Info's page: title, then the images, then the band, each 8 px
     * from the next except the title's own 2 px row gap. */
    out->detail_x = PICKER_DETAIL_X;
    out->detail_w = (int16_t)(w - 2 * PICKER_DETAIL_X);
    out->title_y = UI_PAD;
    out->media_y = (int16_t)(out->title_y + PICKER_TITLE_ROWS * title_pitch() +
                             2);
    out->shot_x = (int16_t)(out->detail_x + PICKER_ART_W + PICKER_DETAIL_GAP);
    out->band_y = (int16_t)(out->media_y + PICKER_ART_H + PICKER_DETAIL_GAP);
    out->band_h = (int16_t)(h - PICKER_DETAIL_FOOT_H - 2 - out->band_y);
    if (out->band_h < (int16_t)pitch) {
        return PICKER_ERR_ARGS;
    }
    out->band_rows = (uint8_t)(out->band_h / pitch);

    if (out->rows < PICKER_MIN_ROWS || out->band_rows < 1) {
        return PICKER_ERR_ARGS;
    }
    return PICKER_OK;
}

void picker_adv_measure(const ui_canvas_t* cv, uint8_t font,
                        picker_adv_t* out)
{
    char one[2] = { 0, 0 };
    char two[3] = { 0, 0, 0 };
    uint8_t i;

    if (cv == NULL || cv->measure == NULL || out == NULL) {
        return;
    }
    /* A string's measured width counts its last glyph's ink, not its
     * advance, so the advance is what a second copy adds. */
    for (i = 0; i < sizeof(out->w); i++) {
        int16_t d;

        one[0] = two[0] = two[1] = (char)(' ' + i);
        d = (int16_t)(cv->measure(cv->ctx, two, font) -
                      cv->measure(cv->ctx, one, font));
        out->w[i] = (uint8_t)((d > 0) ? d : 0);
    }
}

void picker_layout_measure(picker_layout_t* g, const ui_canvas_t* cv)
{
    if (g != NULL) {
        picker_adv_measure(cv, UI_FONT_DESC, &g->adv);
    }
}

/* ─── description word wrap ───────────────────────────────────────────────── */

/*
 * How many characters of `s + at` belong on one line `max_w` wide, and how
 * many to skip before the next line starts.
 *
 * A newline that fits ends the line there, and is skipped rather than drawn;
 * a newline at the start of the remainder is an empty line, which is how the
 * blank line between two paragraphs is drawn. Otherwise the break is the last
 * space that fits, so prose does not break mid-word the way a filename may. A
 * word longer than the line is broken where the line runs out, because the
 * alternative is a line that cannot be drawn. The skipped space itself is not
 * drawn.
 *
 * Widths are summed advances, `a` per printable character, or one per
 * character when `a` is NULL, which makes max_w a column count. A summed
 * advance is never less than the driver's measure of the same string, so a
 * line this breaks is one the driver draws whole.
 */
static uint16_t adv_of(const picker_adv_t* a, char c)
{
    unsigned u = (unsigned char)c;

    if (a == NULL) {
        return 1;
    }
    if (u < 32 || u > 126) {
        u = '?';
    }
    return a->w[u - 32];
}

static void wrap_one(const char* s, size_t at, const picker_adv_t* a,
                     int16_t max_w, size_t* take, size_t* skip)
{
    size_t len = strlen(s);
    size_t left = len - at;
    size_t last_space = 0;
    size_t cols = 0;
    int32_t used = 0;
    size_t i;

    /* How many characters fit: the column count the rules below use. */
    while (cols < left && s[at + cols] != '\n' &&
           used + adv_of(a, s[at + cols]) <= max_w) {
        used += adv_of(a, s[at + cols]);
        cols++;
    }
    if (cols == 0 && left > 0 && s[at] != '\n') {
        cols = 1;
    }

    /* One past the column, like the space below: a newline right after a
     * full-width line ends that line rather than an earlier space. */
    for (i = 0; i <= cols && i < left; i++) {
        if (s[at + i] == '\n') {
            *take = i;
            *skip = i + 1;
            return;
        }
    }
    if (left <= cols) {
        *take = left;
        *skip = left;
        return;
    }
    /* One past the column, so a space landing exactly at the break is used
     * rather than pushed to the next line. */
    for (i = 0; i <= cols; i++) {
        if (s[at + i] == ' ') {
            last_space = i;
        }
    }
    if (last_space > 0) {
        *take = last_space;
        *skip = last_space + 1;
        return;
    }
    *take = cols;
    *skip = cols;
}

uint16_t picker_desc_lines_px(const char* s, const picker_adv_t* a,
                              int16_t max_w)
{
    size_t at = 0;
    size_t len;
    uint16_t lines = 0;

    if (s == NULL || max_w <= 0) {
        return 0;
    }
    len = strlen(s);
    while (at < len) {
        size_t take = 0;
        size_t skip = 0;

        wrap_one(s, at, a, max_w, &take, &skip);
        if (skip == 0) {
            break;
        }
        at += skip;
        lines++;
    }
    return lines;
}

bool picker_desc_line_px(const char* s, const picker_adv_t* a, int16_t max_w,
                         uint16_t line, char* out, size_t out_sz)
{
    size_t at = 0;
    size_t len;
    uint16_t n = 0;

    if (s == NULL || out == NULL || out_sz == 0 || max_w <= 0) {
        return false;
    }
    out[0] = '\0';
    len = strlen(s);
    while (at < len) {
        size_t take = 0;
        size_t skip = 0;

        wrap_one(s, at, a, max_w, &take, &skip);
        if (skip == 0) {
            break;
        }
        if (n == line) {
            if (take > out_sz - 1) {
                take = out_sz - 1;
            }
            memcpy(out, s + at, take);
            out[take] = '\0';
            return true;
        }
        at += skip;
        n++;
    }
    return false;
}

uint16_t picker_desc_lines(const char* s, uint8_t cols)
{
    return picker_desc_lines_px(s, NULL, cols);
}

bool picker_desc_line(const char* s, uint8_t cols, uint16_t line, char* out,
                      size_t out_sz)
{
    return picker_desc_line_px(s, NULL, cols, line, out, out_sz);
}

uint16_t picker_page_lines(const picker_layout_t* g, const char* desc)
{
    uint16_t lines;

    if (g == NULL) {
        return 0;
    }
    lines = picker_desc_lines_px(desc, &g->adv, g->detail_w);
    return (lines > g->band_rows) ? lines : g->band_rows;
}

/* ─── drawing ─────────────────────────────────────────────────────────────── */

/* The label a row shows: the catalog title for a game, marked when the wizard
 * has already written it, otherwise the action wording. */
static const char* row_label(const picker_t* p, uint16_t row, char* buf,
                             size_t buf_sz)
{
    const picker_row_t* r = &p->rows[row];

    if (r->kind != PICKER_ROW_GAME) {
        return ACTION_LABEL[r->kind];
    }
    if (picker_row_marked(p, row)) {
        snprintf(buf, buf_sz, "%s%s", MARK_PREFIX, p->cat->e[r->cat].title);
        return buf;
    }
    return p->cat->e[r->cat].title;
}

/*
 * One image, or its placeholder, whole: a rounded dark slot while it loads and
 * the missing word once it is known not to exist. The image's own corners
 * are rounded by the binding, which owns the buffer.
 */
static void draw_media(const ui_canvas_t* cv, int16_t x, int16_t y,
                       uint8_t state, const uint16_t* px, const char* missing)
{
    if (state == PICKER_MEDIA_READY && px != NULL) {
        cv->image(cv->ctx, x, y, PICKER_ART_W, PICKER_ART_H, px, 0,
                  PICKER_ART_H);
        return;
    }
    cv->fill(cv->ctx, x, y, PICKER_ART_W, PICKER_ART_H, UI_COL_BG);
    cv->round_fill(cv->ctx, x, y, PICKER_ART_W, PICKER_ART_H, UI_IMG_R,
                   UI_COL_SLOT, UI_COL_BG);
    if (state == PICKER_MEDIA_MISSING) {
        cv->text(cv->ctx, missing, x,
                 (int16_t)(y + ui_text_dy(PICKER_ART_H, UI_FONT_HELP)),
                 PICKER_ART_W, 1, UI_FONT_HELP, UI_ALIGN_CENTER, UI_COL_DIM,
                 UI_COL_SLOT);
    }
}

/* The highlighted title's cover, and its snapshot under it. An action row has
 * neither, and leaves the column empty. */
static void draw_list_media(const picker_t* p, const picker_layout_t* g,
                            const ui_canvas_t* cv, const uint16_t* art,
                            const uint16_t* shot)
{
    uint16_t cursor = list_cursor(&p->list);

    if (cursor >= p->row_count || p->rows[cursor].kind != PICKER_ROW_GAME) {
        cv->fill(cv->ctx, TITLE_COL_W(g), PICKER_LIST_TOP,
                 (int16_t)(g->w - TITLE_COL_W(g)),
                 (int16_t)(g->foot_y - PICKER_LIST_TOP), UI_COL_BG);
        return;
    }
    draw_media(cv, g->list_art_x, g->list_art_y, p->art_state, art,
               ART_MISSING);
    draw_media(cv, g->list_art_x, g->list_shot_y, p->shot_state, shot,
               SHOT_MISSING);
}

/*
 * One list row, if it is on screen: white on black, or for the highlighted
 * one black in the theme's pill, which scrolls a label too wide for it by
 * marquee_px. Every row's text starts at the same x, so nothing shifts as the
 * pill moves.
 */
static void draw_row(const picker_t* p, const picker_layout_t* g,
                     const ui_canvas_t* cv, uint16_t idx)
{
    char buf[CATALOG_TITLE_MAX + 4];
    uint16_t first = list_first(&p->list);
    int16_t y;
    const char* label;

    if (idx < first || idx >= p->row_count ||
        idx - first >= (uint16_t)g->rows) {
        return;
    }
    y = (int16_t)(PICKER_LIST_TOP + (idx - first) * PICKER_ROW_H);
    label = row_label(p, idx, buf, sizeof(buf));

    if (idx != list_cursor(&p->list)) {
        ui_row_text(cv, UI_PAD, y, ROW_W(g), PICKER_ROW_H, label, UI_FONT_LIST,
                    UI_COL_TEXT);
        return;
    }
    ui_pill_row(cv, UI_PAD, y, ROW_W(g), PICKER_ROW_H, label, UI_FONT_LIST,
                false, p->marquee_px);
}

static void draw_list(const picker_t* p, const picker_layout_t* g,
                      const char* desc, const uint16_t* art,
                      const uint16_t* shot, const ui_canvas_t* cv)
{
    uint16_t first = list_first(&p->list);
    uint8_t i;

    cv->fill(cv->ctx, 0, 0, g->w, g->h, UI_COL_BG);
    for (i = 0; i < g->rows; i++) {
        draw_row(p, g, cv, (uint16_t)(first + i));
    }
    draw_list_media(p, g, cv, art, shot);
    ui_hint_bar(cv, g->w, g->foot_y, LIST_HINTS, N_HINTS(LIST_HINTS));
}

int16_t picker_row_overflow(const picker_t* p, const picker_layout_t* g,
                            const ui_canvas_t* cv)
{
    char buf[CATALOG_TITLE_MAX + 4];
    uint16_t cursor;
    int16_t over;

    if (p == NULL || g == NULL || cv == NULL || cv->measure == NULL ||
        p->row_count == 0 || p->cat == NULL) {
        return 0;
    }
    cursor = list_cursor(&p->list);
    if (cursor >= p->row_count) {
        return 0;
    }
    /* list_w is the text's room inside the widest pill. */
    over = (int16_t)(cv->measure(cv->ctx, row_label(p, cursor, buf, sizeof(buf)),
                                 UI_FONT_LIST) -
                     g->list_w);
    return (over > 0) ? over : 0;
}

/* The open row's title: the catalog's for a game, the action's wording
 * otherwise. */
static const char* detail_title(const picker_t* p)
{
    const picker_row_t* row = &p->rows[p->detail_row];

    return (row->kind == PICKER_ROW_GAME) ? p->cat->e[row->cat].title
                                          : ACTION_LABEL[row->kind];
}

/* How far a one-row title lets everything under it move up: one font-2 row
 * pitch, the way Cart Info spends the row display_draw_wrapped() did not
 * use. 0 when the canvas cannot say. */
static int16_t detail_lift(const picker_t* p, const picker_layout_t* g,
                           const ui_canvas_t* cv)
{
    if (cv->measure == NULL ||
        cv->measure(cv->ctx, detail_title(p), UI_FONT_HEADER) > g->detail_w) {
        return 0;
    }
    return (int16_t)title_pitch();
}

uint8_t picker_band_rows(const picker_t* p, const picker_layout_t* g,
                         const ui_canvas_t* cv)
{
    uint8_t pitch = desc_pitch();

    if (g == NULL) {
        return 0;
    }
    if (p == NULL || cv == NULL || p->screen != PICKER_SCREEN_DETAIL ||
        p->detail_row >= p->row_count || p->cat == NULL || pitch == 0) {
        return g->band_rows;
    }
    return (uint8_t)((g->band_h + detail_lift(p, g, cv)) / pitch);
}

/*
 * The description band, cleared and redrawn on its own the way Cart Info's
 * is, so a scroll repaints nothing else. It runs the window's full width so
 * the scroll marks at its right edge are cleared with it.
 */
static void draw_band(const picker_t* p, const picker_layout_t* g,
                      const char* desc, const ui_canvas_t* cv)
{
    char line[PICKER_DESC_LINE_MAX];
    uint8_t pitch = desc_pitch();
    int16_t lift = detail_lift(p, g, cv);
    int16_t y = (int16_t)(g->band_y - lift);
    int16_t h = (int16_t)(g->band_h + lift);
    uint8_t rows = (uint8_t)(h / pitch);
    int16_t mark_x = (int16_t)(g->detail_x + g->detail_w + 1);
    uint8_t i;

    cv->fill(cv->ctx, 0, y, g->w, h, UI_COL_BG);
    if (p->rows[p->detail_row].kind != PICKER_ROW_GAME) {
        return;
    }
    if (desc != NULL && desc[0] != '\0') {
        for (i = 0; i < rows; i++) {
            if (!picker_desc_line_px(desc, &g->adv, g->detail_w,
                                     (uint16_t)(p->scroll + i), line,
                                     sizeof(line))) {
                break;
            }
            cv->text(cv->ctx, line, g->detail_x, (int16_t)(y + i * pitch),
                     g->detail_w, 1, UI_FONT_DESC, UI_ALIGN_LEFT, UI_COL_TEXT,
                     UI_COL_BG);
        }
    }
    /* More above on the first line, more below on the last, in the margin
     * right of the text. */
    if (p->scroll > 0) {
        ui_caret(cv, mark_x, (int16_t)(y + ui_caret_dy(UI_FONT_DESC)), true,
                 UI_COL_DIM);
    }
    if (p->scroll < p->scroll_max) {
        ui_caret(cv, mark_x,
                 (int16_t)(y + (rows - 1) * pitch + ui_caret_dy(UI_FONT_DESC)),
                 false, UI_COL_DIM);
    }
}

/*
 * The help line's band: empty, or while A is held the hold bar — rounded
 * like the pill, its track and the part filled so far.
 *
 * `grow` is a hold tick's repaint: the fill only ever gets longer while A is
 * held, so the filled part is painted over what is already there and the
 * track is left alone. Blanking the track first on every 16 ms tick, under the
 * panel's own refresh, was the flicker. The press that starts a hold lays the
 * track down, and letting go clears it.
 */
static void draw_bar(const picker_t* p, const picker_layout_t* g,
                     const ui_canvas_t* cv, bool grow)
{
    uint8_t pct = picker_hold_pct(p);
    /* Level with the words it replaces: their capitals start a pixel down
     * and run 12. */
    int16_t y = (int16_t)(g->help_y + 2);

    if (!p->hold_active) {
        ui_help_line(cv, g->w, g->help_y, NULL);
        return;
    }
    if (!grow || pct == 0) {
        cv->fill(cv->ctx, 0, g->help_y, g->w, UI_HELP_H, UI_COL_BG);
        cv->round_fill(cv->ctx, g->detail_x, y, g->detail_w, PICKER_BAR_H,
                       PICKER_BAR_H / 2, UI_COL_SLOT, UI_COL_BG);
    }
    if (pct > 0) {
        cv->round_fill(cv->ctx, g->detail_x, y,
                       (int16_t)(g->detail_w * pct / PCT_FULL), PICKER_BAR_H,
                       PICKER_BAR_H / 2, UI_COL_PILL, UI_COL_SLOT);
    }
}

/* Side by side and fixed: only the band under them scrolls. An action row
 * has neither. */
static void draw_detail_media(const picker_t* p, const picker_layout_t* g,
                              const uint16_t* art, const uint16_t* shot,
                              const ui_canvas_t* cv)
{
    int16_t y = (int16_t)(g->media_y - detail_lift(p, g, cv));

    if (p->rows[p->detail_row].kind != PICKER_ROW_GAME) {
        return;
    }
    draw_media(cv, g->detail_x, y, p->art_state, art, ART_MISSING);
    draw_media(cv, g->shot_x, y, p->shot_state, shot, SHOT_MISSING);
}

static void draw_detail(const picker_t* p, const picker_layout_t* g,
                        const char* desc, const uint16_t* art,
                        const uint16_t* shot, const ui_canvas_t* cv)
{
    cv->fill(cv->ctx, 0, 0, g->w, g->h, UI_COL_BG);

    /* White, in the header font, as Game Details draws its title. */
    cv->text(cv->ctx, detail_title(p), g->detail_x, g->title_y, g->detail_w,
             PICKER_TITLE_ROWS, UI_FONT_HEADER, UI_ALIGN_LEFT, UI_COL_TEXT,
             UI_COL_BG);

    draw_detail_media(p, g, art, shot, cv);
    draw_band(p, g, desc, cv);

    /* What confirming does, or how far the hold has got, and how to confirm
     * it. No filename: the title already names the game. */
    draw_bar(p, g, cv, false);
    ui_hint_bar(cv, g->w, g->foot_y, DETAIL_HINTS, N_HINTS(DETAIL_HINTS));
}

/* Whether there is anything to draw, and a canvas to draw it with. */
static bool drawable(const picker_t* p, const picker_layout_t* g,
                     const ui_canvas_t* cv)
{
    if (p == NULL || g == NULL || cv == NULL) {
        return false;
    }
    if (cv->fill == NULL || cv->round_fill == NULL || cv->text == NULL ||
        cv->image == NULL || cv->measure == NULL) {
        return false;
    }
    return p->row_count != 0 && p->cat != NULL;
}

void picker_draw(const picker_t* p, const picker_layout_t* g, const char* desc,
                 const uint16_t* art, const uint16_t* shot,
                 const ui_canvas_t* cv)
{
    if (!drawable(p, g, cv)) {
        return;
    }
    switch (p->screen) {
    case PICKER_SCREEN_LIST:
        draw_list(p, g, desc, art, shot, cv);
        break;
    case PICKER_SCREEN_DETAIL:
        if (p->detail_row < p->row_count) {
            draw_detail(p, g, desc, art, shot, cv);
        }
        break;
    default:
        /* PICKER_SCREEN_DONE draws nothing: the binding returns and the
         * caller's halt screen takes over. */
        break;
    }
}

void picker_draw_events(const picker_t* p, const picker_layout_t* g,
                        uint8_t events, const char* desc, const uint16_t* art,
                        const uint16_t* shot, const ui_canvas_t* cv)
{
    if (!drawable(p, g, cv)) {
        return;
    }
    if (events & PICKER_EVENT_REDRAW) {
        picker_draw(p, g, desc, art, shot, cv);
        return;
    }
    if (p->screen == PICKER_SCREEN_LIST) {
        if (events & PICKER_EVENT_ROWS) {
            draw_row(p, g, cv, p->prev_cursor);
        }
        if (events & (PICKER_EVENT_ROWS | PICKER_EVENT_MARQUEE)) {
            draw_row(p, g, cv, list_cursor(&p->list));
        }
        if (events & PICKER_EVENT_MEDIA) {
            draw_list_media(p, g, cv, art, shot);
        }
        return;
    }
    if (p->screen != PICKER_SCREEN_DETAIL || p->detail_row >= p->row_count) {
        return;
    }
    if (events & PICKER_EVENT_MEDIA) {
        draw_detail_media(p, g, art, shot, cv);
    }
    if (events & PICKER_EVENT_BAND) {
        draw_band(p, g, desc, cv);
    }
    if (events & PICKER_EVENT_BAR) {
        draw_bar(p, g, cv, true);
    }
}
