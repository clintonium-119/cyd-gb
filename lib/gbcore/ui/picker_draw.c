#include "picker_draw.h"

#include <stdio.h>
#include <string.h>

/*
 * The in-game menu's colours, copied rather than shared because this side is
 * pure C, so the writer and the menu read as one UI (src/menu.cpp). The
 * highlight is the menu's: bold black on a white bar. Raw RGB565, as
 * everywhere else in this firmware — there is no named-colour layer to reach
 * for.
 */
#define COL_BG     0x0000
#define COL_ROW_BG 0x1082
#define COL_HL_BG  0xFFFF
#define COL_HL_FG  0x0000
#define COL_TITLE  0xFFE0
#define COL_DIM    0x7BEF
#define COL_TEXT   0xFFFF
#define COL_BAR    0xFFFF

#define PCT_FULL 100

/* Between the list's two stacked images. */
#define PICKER_ART_GAP 4

/* The right edge of the list's title column: its rows' backgrounds end here,
 * and the image column starts. */
#define TITLE_COL_W(g) ((int16_t)((g)->list_art_x - 4))

/* ─── strings ─────────────────────────────────────────────────────────────── */

static const char* const LIST_HEADER[] = {
    "Choose a Game",      /* PICKER_MODE_PENDING   */
    "Setup: pick a game", /* PICKER_MODE_IMMEDIATE */
};

static const char* const ACTION_LABEL[] = {
    "",                     /* PICKER_ROW_GAME — the catalog title  */
    "Cancel pending write", /* PICKER_ROW_CANCEL_PENDING            */
    "Finish setup",         /* PICKER_ROW_FINISH                    */
};

/* What confirming this row will do, in one line under the title. */
static const char* const DETAIL_TARGET[] = {
    "Your wildcard becomes this game.",
    "This blank cart becomes this game.",
    "The write you lined up is dropped.",
    "Setup ends. Only a MENU cart reopens this.",
};
#define TARGET_WILD   0
#define TARGET_BLANK  1
#define TARGET_CANCEL 2
#define TARGET_FINISH 3

#define FOOTER_PROMPT "Hold A to confirm    B: Back"
#define ART_MISSING   "no art"
#define SHOT_MISSING  "no snapshot"
#define MARK_PREFIX   "* "
#define SCROLL_MORE   "v"
#define SCROLL_BACK   "^"

/* ─── fonts ───────────────────────────────────────────────────────────────── */

static uint8_t small_pitch(void)
{
    return (uint8_t)UI_ROW_PITCH(ui_font_height(UI_FONT_SMALL));
}

/* ─── geometry ────────────────────────────────────────────────────────────── */

int picker_layout(int16_t w, int16_t h, picker_layout_t* out)
{
    uint8_t pitch = small_pitch();

    if (out == NULL || pitch == 0) {
        return PICKER_ERR_ARGS;
    }
    memset(out, 0, sizeof(*out));

    /* An image plus a column narrow enough to still wrap into, and both of
     * the list's images stacked under its header. */
    if (w < (int16_t)(PICKER_ART_W + 16 + 48) ||
        h < (int16_t)(PICKER_HEADER_H + 2 * PICKER_ART_H + PICKER_ART_GAP)) {
        return PICKER_ERR_ARGS;
    }

    out->w = w;
    out->h = h;

    out->rows = (uint8_t)((h - PICKER_HEADER_H) / PICKER_ROW_H);
    /* The image column hugs the right edge; the titles get the rest, less a
     * 4 px inset each side of their text. */
    out->list_art_x = (int16_t)(w - 4 - PICKER_ART_W);
    out->list_art_y = PICKER_HEADER_H;
    out->list_shot_y = (int16_t)(out->list_art_y + PICKER_ART_H + PICKER_ART_GAP);
    out->list_w = (int16_t)(out->list_art_x - 4 - 4);

    /* Cart Info's page: title, then the images, then the band, each 8 px
     * from the next except the title's own 2 px row gap. */
    out->detail_x = PICKER_DETAIL_X;
    out->detail_w = (int16_t)(w - 2 * PICKER_DETAIL_X);
    out->title_y = PICKER_DETAIL_X;
    out->media_y = (int16_t)(out->title_y +
                             PICKER_TITLE_ROWS * UI_ROW_PITCH(
                                 ui_font_height(UI_FONT_ROW)) + 2);
    out->shot_x = (int16_t)(out->detail_x + PICKER_ART_W + PICKER_DETAIL_GAP);
    out->band_y = (int16_t)(out->media_y + PICKER_ART_H + PICKER_DETAIL_GAP);
    out->band_h = (int16_t)(h - PICKER_DETAIL_FOOT_H - 2 - out->band_y);
    if (out->band_h < (int16_t)pitch) {
        return PICKER_ERR_ARGS;
    }
    out->band_rows = (uint8_t)(out->band_h / pitch);
    out->desc_cols = (uint8_t)(out->detail_w / UI_FONT_SMALL_ADV);

    if (out->rows < PICKER_MIN_ROWS || out->band_rows < 1 ||
        out->desc_cols < PICKER_DESC_MIN_COLS) {
        return PICKER_ERR_ARGS;
    }
    return PICKER_OK;
}

/* ─── description word wrap ───────────────────────────────────────────────── */

/*
 * How many characters of `s + at` belong on one line of `cols`, and how many
 * to skip before the next line starts.
 *
 * A newline that fits ends the line there, and is skipped rather than drawn;
 * a newline at the start of the remainder is an empty line, which is how the
 * blank line between two paragraphs is drawn. Otherwise the break is the last
 * space that fits, so prose does not break mid-word the way a filename may. A
 * word longer than the column is broken at the column, because the
 * alternative is a line that cannot be drawn. The skipped space itself is not
 * drawn.
 *
 * Exact because font 1's advance is fixed: `cols` characters measure
 * cols * UI_FONT_SMALL_ADV pixels, which is the width the caller asked for.
 */
static void wrap_one(const char* s, size_t at, uint8_t cols, size_t* take,
                     size_t* skip)
{
    size_t len = strlen(s);
    size_t left = len - at;
    size_t last_space = 0;
    size_t i;

    /* One past the column, like the space below: a newline right after a
     * full-width line ends that line rather than an earlier space. */
    for (i = 0; i <= (size_t)cols && i < left; i++) {
        if (s[at + i] == '\n') {
            *take = i;
            *skip = i + 1;
            return;
        }
    }
    if (left <= (size_t)cols) {
        *take = left;
        *skip = left;
        return;
    }
    /* One past the column, so a space landing exactly at the break is used
     * rather than pushed to the next line. */
    for (i = 0; i <= (size_t)cols; i++) {
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

uint16_t picker_desc_lines(const char* s, uint8_t cols)
{
    size_t at = 0;
    size_t len;
    uint16_t lines = 0;

    if (s == NULL || cols == 0) {
        return 0;
    }
    len = strlen(s);
    while (at < len) {
        size_t take = 0;
        size_t skip = 0;

        wrap_one(s, at, cols, &take, &skip);
        if (skip == 0) {
            break;
        }
        at += skip;
        lines++;
    }
    return lines;
}

bool picker_desc_line(const char* s, uint8_t cols, uint16_t line, char* out,
                      size_t out_sz)
{
    size_t at = 0;
    size_t len;
    uint16_t n = 0;

    if (s == NULL || out == NULL || out_sz == 0 || cols == 0) {
        return false;
    }
    out[0] = '\0';
    len = strlen(s);
    while (at < len) {
        size_t take = 0;
        size_t skip = 0;

        wrap_one(s, at, cols, &take, &skip);
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

uint16_t picker_page_lines(const picker_layout_t* g, const char* desc)
{
    uint16_t lines;

    if (g == NULL) {
        return 0;
    }
    lines = picker_desc_lines(desc, g->desc_cols);
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
 * One image, or its placeholder, whole: a plain slot while it loads and the
 * missing word once it is known not to exist.
 */
static void draw_media(const ui_canvas_t* cv, int16_t x, int16_t y,
                       uint8_t state, const uint16_t* px, const char* missing)
{
    if (state == PICKER_MEDIA_READY && px != NULL) {
        cv->image(cv->ctx, x, y, PICKER_ART_W, PICKER_ART_H, px, 0,
                  PICKER_ART_H);
        return;
    }
    cv->fill(cv->ctx, x, y, PICKER_ART_W, PICKER_ART_H, COL_ROW_BG);
    if (state == PICKER_MEDIA_MISSING) {
        cv->text(cv->ctx, missing, x,
                 (int16_t)(y + PICKER_ART_H / 2 -
                           ui_font_height(UI_FONT_SMALL) / 2),
                 PICKER_ART_W, 1, UI_FONT_SMALL, UI_ALIGN_CENTER, COL_DIM,
                 COL_ROW_BG);
    }
}

static void draw_header(const picker_t* p, const picker_layout_t* g,
                        const ui_canvas_t* cv)
{
    cv->fill(cv->ctx, 0, 0, g->w, PICKER_HEADER_H, COL_ROW_BG);
    /*
     * Text sits at the row's own top, with no inset. A font-2 box is
     * UI_ROW_PITCH(16) = 18 px, exactly PICKER_ROW_H, so a one-pixel inset
     * would push the last row's box one pixel past the window — and the
     * pitch's own trailing two pixels already are the gap an inset was for.
     */
    cv->text(cv->ctx, LIST_HEADER[p->mode == PICKER_MODE_IMMEDIATE ? 1 : 0], 4,
             0, (int16_t)(g->w - 8), 1, UI_FONT_ROW, UI_ALIGN_LEFT, COL_TITLE,
             COL_ROW_BG);
}

/* The highlighted title's cover, and its snapshot under it. An action row has
 * neither, and leaves the column empty. */
static void draw_list_media(const picker_t* p, const picker_layout_t* g,
                            const ui_canvas_t* cv, const uint16_t* art,
                            const uint16_t* shot)
{
    uint16_t cursor = list_cursor(&p->list);

    if (cursor >= p->row_count || p->rows[cursor].kind != PICKER_ROW_GAME) {
        cv->fill(cv->ctx, TITLE_COL_W(g), PICKER_HEADER_H,
                 (int16_t)(g->w - TITLE_COL_W(g)),
                 (int16_t)(g->h - PICKER_HEADER_H), COL_BG);
        return;
    }
    draw_media(cv, g->list_art_x, g->list_art_y, p->art_state, art,
               ART_MISSING);
    draw_media(cv, g->list_art_x, g->list_shot_y, p->shot_state, shot,
               SHOT_MISSING);
}

/*
 * One list row, if it is on screen. The highlighted one is the in-game menu's
 * bar: bold black on white, the glyphs struck twice a pixel apart and
 * transparent, because an opaque second pass would wipe the first one's right
 * edge. It is built offscreen when the canvas can, so a marquee step replaces
 * the row in one push rather than blanking it; its label then starts
 * marquee_px to the left and is as wide as it needs to be to stay on one row.
 */
static void draw_row(const picker_t* p, const picker_layout_t* g,
                     const ui_canvas_t* cv, uint16_t idx)
{
    char buf[CATALOG_TITLE_MAX + 4];
    uint16_t first = list_first(&p->list);
    int16_t y;
    const char* label;
    bool off;
    int16_t dx;
    int16_t x = 4;
    int16_t w = g->list_w;

    if (idx < first || idx >= p->row_count ||
        idx - first >= (uint16_t)g->rows) {
        return;
    }
    y = (int16_t)(PICKER_HEADER_H + (idx - first) * PICKER_ROW_H);
    label = row_label(p, idx, buf, sizeof(buf));

    if (idx != list_cursor(&p->list)) {
        cv->fill(cv->ctx, 0, y, TITLE_COL_W(g), PICKER_ROW_H, COL_BG);
        cv->text(cv->ctx, label, 4, y, g->list_w, 1, UI_FONT_ROW,
                 UI_ALIGN_LEFT, COL_TEXT, COL_BG);
        return;
    }

    off = cv->begin != NULL && cv->end != NULL &&
          cv->begin(cv->ctx, 0, y, TITLE_COL_W(g), PICKER_ROW_H);
    if (off) {
        x = (int16_t)(4 - p->marquee_px);
        w = (int16_t)(g->list_w + p->marquee_span);
    }
    cv->fill(cv->ctx, 0, y, TITLE_COL_W(g), PICKER_ROW_H, COL_HL_BG);
    for (dx = 0; dx <= 1; dx++) {
        cv->text(cv->ctx, label, (int16_t)(x + dx), y, w, 1, UI_FONT_ROW,
                 UI_ALIGN_LEFT, COL_HL_FG, COL_HL_FG);
    }
    if (off) {
        cv->end(cv->ctx);
    }
}

static void draw_list(const picker_t* p, const picker_layout_t* g,
                      const uint16_t* art, const uint16_t* shot,
                      const ui_canvas_t* cv)
{
    uint16_t first = list_first(&p->list);
    uint8_t i;

    cv->fill(cv->ctx, 0, 0, g->w, g->h, COL_BG);
    draw_header(p, g, cv);
    for (i = 0; i < g->rows; i++) {
        draw_row(p, g, cv, (uint16_t)(first + i));
    }
    draw_list_media(p, g, cv, art, shot);
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
    /* One more pixel for the bold pass. */
    over = (int16_t)(cv->measure(cv->ctx, row_label(p, cursor, buf, sizeof(buf)),
                                 UI_FONT_ROW) +
                     1 - g->list_w);
    return (over > 0) ? over : 0;
}

/* Which target wording the open row confirms. */
static uint8_t detail_target(const picker_t* p)
{
    switch (p->rows[p->detail_row].kind) {
    case PICKER_ROW_CANCEL_PENDING:
        return TARGET_CANCEL;
    case PICKER_ROW_FINISH:
        return TARGET_FINISH;
    default:
        return (p->mode == PICKER_MODE_IMMEDIATE) ? TARGET_BLANK
                                                  : TARGET_WILD;
    }
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
        cv->measure(cv->ctx, detail_title(p), UI_FONT_ROW) > g->detail_w) {
        return 0;
    }
    return (int16_t)UI_ROW_PITCH(ui_font_height(UI_FONT_ROW));
}

uint8_t picker_band_rows(const picker_t* p, const picker_layout_t* g,
                         const ui_canvas_t* cv)
{
    uint8_t pitch = small_pitch();

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
    uint8_t pitch = small_pitch();
    int16_t lift = detail_lift(p, g, cv);
    int16_t y = (int16_t)(g->band_y - lift);
    int16_t h = (int16_t)(g->band_h + lift);
    uint8_t rows = (uint8_t)(h / pitch);
    int16_t mark_x = (int16_t)(g->detail_x + g->detail_w);
    uint8_t i;

    cv->fill(cv->ctx, 0, y, g->w, h, COL_BG);
    if (p->rows[p->detail_row].kind != PICKER_ROW_GAME) {
        return;
    }
    if (desc != NULL && desc[0] != '\0') {
        for (i = 0; i < rows; i++) {
            if (!picker_desc_line(desc, g->desc_cols,
                                  (uint16_t)(p->scroll + i), line,
                                  sizeof(line))) {
                break;
            }
            cv->text(cv->ctx, line, g->detail_x, (int16_t)(y + i * pitch),
                     g->detail_w, 1, UI_FONT_SMALL, UI_ALIGN_LEFT, COL_TEXT,
                     COL_BG);
        }
    }
    if (p->scroll > 0) {
        cv->text(cv->ctx, SCROLL_BACK, mark_x, y, UI_FONT_SMALL_ADV, 1,
                 UI_FONT_SMALL, UI_ALIGN_RIGHT, COL_DIM, COL_BG);
    }
    if (p->scroll < p->scroll_max) {
        cv->text(cv->ctx, SCROLL_MORE, mark_x,
                 (int16_t)(y + (rows - 1) * pitch), UI_FONT_SMALL_ADV, 1,
                 UI_FONT_SMALL, UI_ALIGN_RIGHT, COL_DIM, COL_BG);
    }
}

/*
 * The hold bar: its track, and the part filled so far.
 *
 * `grow` is a hold tick's repaint: the fill only ever gets longer while A is
 * held, so the filled part is painted over what is already there and the
 * track is left alone. Blanking the track first on every 16 ms tick, under the
 * panel's own refresh, was the flicker. A release, or a full draw, lays the
 * track down again.
 */
static void draw_bar(const picker_t* p, const picker_layout_t* g,
                     const ui_canvas_t* cv, bool grow)
{
    uint8_t pct = picker_hold_pct(p);
    int16_t y = (int16_t)(g->h - 4 - PICKER_BAR_H);

    if (!grow || pct == 0) {
        cv->fill(cv->ctx, g->detail_x, y, g->detail_w, PICKER_BAR_H,
                 COL_ROW_BG);
    }
    if (pct > 0) {
        cv->fill(cv->ctx, g->detail_x, y,
                 (int16_t)(g->detail_w * pct / PCT_FULL), PICKER_BAR_H,
                 COL_BAR);
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
    int16_t foot_y = (int16_t)(g->h - PICKER_DETAIL_FOOT_H);

    cv->fill(cv->ctx, 0, 0, g->w, g->h, COL_BG);

    /* White, as Cart Info draws its title. */
    cv->text(cv->ctx, detail_title(p), g->detail_x, g->title_y, g->detail_w,
             PICKER_TITLE_ROWS, UI_FONT_ROW, UI_ALIGN_LEFT, COL_TEXT, COL_BG);

    draw_detail_media(p, g, art, shot, cv);
    draw_band(p, g, desc, cv);

    /* What confirming does, how to confirm it, and how far the hold has got.
     * No filename: the title already names the game. */
    cv->text(cv->ctx, DETAIL_TARGET[detail_target(p)], g->detail_x, foot_y,
             g->detail_w, 1, UI_FONT_SMALL, UI_ALIGN_LEFT, COL_DIM, COL_BG);
    cv->text(cv->ctx, FOOTER_PROMPT, g->detail_x, (int16_t)(foot_y + 12),
             g->detail_w, 1, UI_FONT_ROW, UI_ALIGN_CENTER, COL_DIM, COL_BG);
    draw_bar(p, g, cv, false);
}

/* Whether there is anything to draw, and a canvas to draw it with. */
static bool drawable(const picker_t* p, const picker_layout_t* g,
                     const ui_canvas_t* cv)
{
    if (p == NULL || g == NULL || cv == NULL) {
        return false;
    }
    if (cv->fill == NULL || cv->text == NULL || cv->image == NULL) {
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
        draw_list(p, g, art, shot, cv);
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
