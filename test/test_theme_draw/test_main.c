#include <unity.h>

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "ui/theme_draw.h"

/*
 * The shared themed helpers, over a fake canvas.
 *
 * The fake is not the picker's or the diagnostics' copy: those two already
 * differ (one hashes pixels for partial-equals-full, the other counts page
 * features), and what these helpers are pinned by is the calls themselves —
 * which op, where, how wide, in what colour. So this one logs every call and
 * bounds-checks it the same way the other two do, and nothing else.
 *
 * Every expected number is the helper's arithmetic written out.
 */

#define W 266
#define H 240

enum { OP_FILL, OP_ROUND, OP_TEXT };

typedef struct {
    int op;
    int16_t x, y, w, h, r;
    uint8_t rows, font, align;
    uint16_t color, bg;
    char s[48];
} call_t;

typedef struct {
    call_t log[64];
    unsigned n;
    unsigned violations;
    bool off;
    int16_t off_x, off_y, off_w, off_h;
    unsigned begins, ends;
    unsigned off_violations;
    bool refuse_begin;
} fake_t;

static fake_t fk;

/* Inside the window, and inside the offscreen buffer while one is open. A
 * text box may run sideways past the buffer — the sprite clips it, which is
 * how a marquee scrolls — but never past the window's edge unclipped. */
static void check(fake_t* f, int16_t x, int16_t y, int16_t w, int16_t h,
                  bool clips_sideways)
{
    if (f->off && clips_sideways) {
        int16_t l = (x > f->off_x) ? x : f->off_x;
        int16_t r = (x + w < f->off_x + f->off_w) ? (int16_t)(x + w)
                                                  : (int16_t)(f->off_x + f->off_w);

        x = l;
        w = (int16_t)((r > l) ? r - l : 0);
    }
    if (x < 0 || y < 0 || w < 0 || h < 0 || x + w > W || y + h > H) {
        f->violations++;
    }
    if (f->off && (x < f->off_x || y < f->off_y ||
                   x + w > f->off_x + f->off_w ||
                   y + h > f->off_y + f->off_h)) {
        f->off_violations++;
    }
}

static call_t* push(fake_t* f, int op)
{
    call_t* c = &f->log[f->n < 63 ? f->n : 63];

    if (f->n < 64) {
        f->n++;
    }
    memset(c, 0, sizeof(*c));
    c->op = op;
    return c;
}

static void fk_fill(void* ctx, int16_t x, int16_t y, int16_t w, int16_t h,
                    uint16_t color)
{
    fake_t* f = (fake_t*)ctx;
    call_t* c = push(f, OP_FILL);

    c->x = x;
    c->y = y;
    c->w = w;
    c->h = h;
    c->color = color;
    check(f, x, y, w, h, false);
}

static void fk_round_fill(void* ctx, int16_t x, int16_t y, int16_t w,
                          int16_t h, int16_t r, uint16_t color, uint16_t bg)
{
    fake_t* f = (fake_t*)ctx;
    call_t* c = push(f, OP_ROUND);

    c->x = x;
    c->y = y;
    c->w = w;
    c->h = h;
    c->r = r;
    c->color = color;
    c->bg = bg;
    check(f, x, y, w, h, false);
}

static void fk_text(void* ctx, const char* s, int16_t x, int16_t y, int16_t w,
                    uint8_t rows, uint8_t font, uint8_t align, uint16_t fg,
                    uint16_t bg)
{
    fake_t* f = (fake_t*)ctx;
    call_t* c = push(f, OP_TEXT);

    c->x = x;
    c->y = y;
    c->w = w;
    c->h = (int16_t)(rows * UI_ROW_PITCH(ui_font_height(font)) - 2);
    c->rows = rows;
    c->font = font;
    c->align = align;
    c->color = fg;
    c->bg = bg;
    snprintf(c->s, sizeof(c->s), "%s", s ? s : "(null)");
    check(f, x, y, w, c->h, true);
}

static void fk_image(void* ctx, int16_t x, int16_t y, int16_t w, int16_t h,
                     const uint16_t* px, int16_t row0, int16_t rows)
{
    (void)px;
    (void)row0;
    (void)rows;
    check((fake_t*)ctx, x, y, w, h, false);
}

/* 8 px a glyph at font 2, 6 at font 1 (exact: GLCD is fixed advance), 14 at
 * font 4. */
static int16_t fk_measure(void* ctx, const char* s, uint8_t font)
{
    int16_t adv = (font == UI_FONT_SMALL) ? UI_FONT_SMALL_ADV
                : (font == UI_FONT_TITLE) ? 14
                                          : 8;

    (void)ctx;
    return (s == NULL) ? 0 : (int16_t)(strlen(s) * adv);
}

static bool fk_begin(void* ctx, int16_t x, int16_t y, int16_t w, int16_t h)
{
    fake_t* f = (fake_t*)ctx;

    if (f->refuse_begin) {
        return false;
    }
    f->begins++;
    check(f, x, y, w, h, false);
    f->off = true;
    f->off_x = x;
    f->off_y = y;
    f->off_w = w;
    f->off_h = h;
    return true;
}

static void fk_end(void* ctx)
{
    fake_t* f = (fake_t*)ctx;

    f->ends++;
    f->off = false;
}

static ui_canvas_t cv;

void setUp(void)
{
    memset(&fk, 0, sizeof(fk));
    memset(&cv, 0, sizeof(cv));
    cv.ctx = &fk;
    cv.fill = fk_fill;
    cv.round_fill = fk_round_fill;
    cv.text = fk_text;
    cv.image = fk_image;
    cv.measure = fk_measure;
    cv.begin = fk_begin;
    cv.end = fk_end;
}

void tearDown(void)
{
}

static unsigned count(int op)
{
    unsigned i;
    unsigned k = 0;

    for (i = 0; i < fk.n; i++) {
        k += (fk.log[i].op == op);
    }
    return k;
}

/* The k-th call of op, or fail. */
static const call_t* nth(int op, unsigned k)
{
    unsigned i;

    for (i = 0; i < fk.n; i++) {
        if (fk.log[i].op == op && k-- == 0) {
            return &fk.log[i];
        }
    }
    TEST_FAIL_MESSAGE("no such call");
    return NULL;
}

/* ─── the pill ────────────────────────────────────────────────────────────── */

static void test_the_pill_hugs_a_short_label(void)
{
    int16_t pw = ui_pill_row(&cv, 4, 40, 200, UI_PILL_H_ROW, "Resume",
                             UI_FONT_LIST, false, 0);

    TEST_ASSERT_EQUAL_INT16(6 * 8 + 2 * UI_PILL_PAD, pw);
    TEST_ASSERT_EQUAL_UINT(0, fk.violations);
    TEST_ASSERT_EQUAL_UINT(0, fk.off_violations);
}

static void test_an_overflowing_label_is_held_to_max_w(void)
{
    int16_t pw = ui_pill_row(&cv, 4, 40, 120, UI_PILL_H_LIST,
                             "The Legend of Zelda: Link's Awakening",
                             UI_FONT_LIST, false, 0);

    TEST_ASSERT_EQUAL_INT16(120, pw);
    TEST_ASSERT_EQUAL_INT16(120, nth(OP_ROUND, 0)->w);
    TEST_ASSERT_EQUAL_UINT(0, fk.violations);
    TEST_ASSERT_EQUAL_UINT(0, fk.off_violations);
}

static void test_the_pill_is_one_white_round_fill_of_half_its_height(void)
{
    const call_t* c;

    ui_pill_row(&cv, 4, 40, 200, UI_PILL_H_ROW, "Resume", UI_FONT_LIST, false, 0);
    TEST_ASSERT_EQUAL_UINT(1, count(OP_ROUND));
    c = nth(OP_ROUND, 0);
    TEST_ASSERT_EQUAL_INT16(UI_PILL_H_ROW / 2, c->r);
    TEST_ASSERT_EQUAL_HEX16(UI_COL_PILL, c->color);
    TEST_ASSERT_EQUAL_HEX16(UI_COL_BG, c->bg);
    TEST_ASSERT_EQUAL_INT16(4, c->x);
    TEST_ASSERT_EQUAL_INT16(40, c->y);
    TEST_ASSERT_EQUAL_INT16(UI_PILL_H_ROW, c->h);
}

static void test_the_pill_is_built_offscreen_at_its_own_size(void)
{
    int16_t pw = ui_pill_row(&cv, 4, 40, 200, UI_PILL_H_ROW, "Resume",
                             UI_FONT_LIST, false, 0);

    TEST_ASSERT_EQUAL_UINT(1, fk.begins);
    TEST_ASSERT_EQUAL_UINT(1, fk.ends);
    TEST_ASSERT_EQUAL_INT16(pw, fk.off_w);
    TEST_ASSERT_EQUAL_INT16(UI_PILL_H_ROW, fk.off_h);
}

static void test_the_label_is_one_transparent_black_pass(void)
{
    const call_t* a;

    ui_pill_row(&cv, 4, 40, 200, UI_PILL_H_ROW, "Resume", UI_FONT_LIST, false,
                0);
    TEST_ASSERT_EQUAL_UINT(1, count(OP_TEXT));
    a = nth(OP_TEXT, 0);
    TEST_ASSERT_EQUAL_HEX16(UI_COL_PILL_TEXT, a->color);
    TEST_ASSERT_EQUAL_HEX16(a->color, a->bg);
    TEST_ASSERT_EQUAL_UINT8(UI_FONT_LIST, a->font);
}

static void test_a_dim_label_is_in_the_dim_on_pill_grey(void)
{
    int16_t pw = ui_pill_row(&cv, 4, 40, 200, UI_PILL_H_ROW, "Manual",
                             UI_FONT_LIST, true, 0);

    TEST_ASSERT_EQUAL_UINT(1, count(OP_TEXT));
    TEST_ASSERT_EQUAL_HEX16(UI_COL_PILL_DIM, nth(OP_TEXT, 0)->color);
    TEST_ASSERT_EQUAL_INT16(6 * 8 + 2 * UI_PILL_PAD, pw);
}

static void test_a_marquee_moves_the_text_and_not_the_pill(void)
{
    int16_t still;
    int16_t x0;
    int16_t moved;
    const char* s = "The Legend of Zelda: Link's Awakening";

    still = ui_pill_row(&cv, 4, 40, 120, UI_PILL_H_LIST, s, UI_FONT_LIST, false, 0);
    x0 = nth(OP_TEXT, 0)->x;
    setUp();
    moved = ui_pill_row(&cv, 4, 40, 120, UI_PILL_H_LIST, s, UI_FONT_LIST, false, 5);
    TEST_ASSERT_EQUAL_INT16(still, moved);
    TEST_ASSERT_EQUAL_INT16(x0 - 5, nth(OP_TEXT, 0)->x);
    TEST_ASSERT_EQUAL_UINT(0, fk.violations);
    TEST_ASSERT_EQUAL_UINT(0, fk.off_violations);
}

static void test_without_a_buffer_the_marquee_is_ignored_and_the_text_fits(void)
{
    const char* s = "The Legend of Zelda: Link's Awakening";

    fk.refuse_begin = true;
    ui_pill_row(&cv, 4, 40, 120, UI_PILL_H_LIST, s, UI_FONT_LIST, false,
                5);
    TEST_ASSERT_EQUAL_INT16(4 + UI_PILL_PAD, nth(OP_TEXT, 0)->x);
    TEST_ASSERT_EQUAL_INT16(120 - 2 * UI_PILL_PAD, nth(OP_TEXT, 0)->w);
    TEST_ASSERT_EQUAL_UINT(0, fk.ends);
    TEST_ASSERT_EQUAL_UINT(0, fk.violations);
}

static void test_a_plain_row_and_a_pill_start_their_text_at_one_x(void)
{
    int16_t plain;

    ui_row_text(&cv, 4, 40, 200, UI_PILL_H_ROW, "Resume", UI_FONT_LIST,
                UI_COL_TEXT);
    plain = nth(OP_TEXT, 0)->x;
    setUp();
    ui_pill_row(&cv, 4, 40, 200, UI_PILL_H_ROW, "Resume", UI_FONT_LIST, false, 0);
    TEST_ASSERT_EQUAL_INT16(plain, nth(OP_TEXT, 0)->x);
    TEST_ASSERT_EQUAL_INT16(4 + UI_PILL_PAD, plain);
}

static void test_a_plain_row_is_cleared_to_black_with_no_bar(void)
{
    ui_row_text(&cv, 4, 40, 200, UI_PILL_H_ROW, "Resume", UI_FONT_LIST,
                UI_COL_TEXT);
    TEST_ASSERT_EQUAL_UINT(1, count(OP_FILL));
    TEST_ASSERT_EQUAL_HEX16(UI_COL_BG, nth(OP_FILL, 0)->color);
    TEST_ASSERT_EQUAL_UINT(0, count(OP_ROUND));
    TEST_ASSERT_EQUAL_UINT(0, fk.violations);
}

/* ─── header, help, hints ─────────────────────────────────────────────────── */

static void test_the_header_is_white_left_and_grey_right_on_black(void)
{
    ui_header(&cv, W, "Menu", "2/9");
    TEST_ASSERT_EQUAL_HEX16(UI_COL_BG, nth(OP_FILL, 0)->color);
    TEST_ASSERT_EQUAL_INT16(UI_HEADER_H, nth(OP_FILL, 0)->h);
    TEST_ASSERT_EQUAL_HEX16(UI_COL_TEXT, nth(OP_TEXT, 0)->color);
    TEST_ASSERT_EQUAL_UINT8(UI_ALIGN_LEFT, nth(OP_TEXT, 0)->align);
    TEST_ASSERT_EQUAL_INT16(UI_TEXT_X, nth(OP_TEXT, 0)->x);
    TEST_ASSERT_EQUAL_UINT8(UI_FONT_HEADER, nth(OP_TEXT, 0)->font);
    TEST_ASSERT_EQUAL_HEX16(UI_COL_DIM, nth(OP_TEXT, 1)->color);
    TEST_ASSERT_EQUAL_UINT8(UI_ALIGN_RIGHT, nth(OP_TEXT, 1)->align);
    TEST_ASSERT_EQUAL_UINT(0, fk.violations);
}

static void test_the_help_line_is_one_grey_row_inside_its_band(void)
{
    const call_t* t;

    ui_help_line(&cv, W, 200, "Pick up where you left off.");
    TEST_ASSERT_EQUAL_INT16(UI_HELP_H, nth(OP_FILL, 0)->h);
    t = nth(OP_TEXT, 0);
    TEST_ASSERT_EQUAL_UINT8(1, t->rows);
    TEST_ASSERT_EQUAL_UINT8(UI_FONT_HELP, t->font);
    TEST_ASSERT_EQUAL_HEX16(UI_COL_TEXT, t->color);
    TEST_ASSERT_EQUAL_INT16(UI_TEXT_X, t->x);
    TEST_ASSERT_TRUE(t->y >= 200);
    TEST_ASSERT_TRUE(t->y + t->h <= 200 + UI_HELP_H);
    TEST_ASSERT_TRUE(t->x + t->w <= W);
    TEST_ASSERT_EQUAL_UINT(0, fk.violations);
}

static void test_a_null_help_line_only_clears(void)
{
    ui_help_line(&cv, W, 200, NULL);
    TEST_ASSERT_EQUAL_UINT(1, fk.n);
    TEST_ASSERT_EQUAL_UINT(1, count(OP_FILL));
}

static void test_no_hints_paints_only_the_background(void)
{
    ui_hint_bar(&cv, W, H - UI_FOOT_H, NULL, 0);
    TEST_ASSERT_EQUAL_UINT(1, fk.n);
    TEST_ASSERT_EQUAL_HEX16(UI_COL_BG, nth(OP_FILL, 0)->color);
    TEST_ASSERT_EQUAL_INT16(UI_FOOT_H, nth(OP_FILL, 0)->h);
}

static void test_two_hints_right_align_and_a_word_button_stretches(void)
{
    static const ui_hint_t hints[] = {
        { "SEL", "Palette" },
        { "A", "OK" },
    };
    const call_t* outer;
    const call_t* g0;
    const call_t* g1;
    int16_t m = (int16_t)((UI_FOOT_H - 2 - UI_GLYPH_D) / 2);
    int16_t gw0 = (int16_t)(3 * UI_FONT_SMALL_ADV + 6);
    int16_t ow = (int16_t)(m + gw0 + UI_HINT_GAP + 7 * UI_FONT_SMALL_ADV +
                           UI_HINT_GAP + UI_GLYPH_D + UI_HINT_GAP +
                           2 * UI_FONT_SMALL_ADV + UI_HINT_GAP);

    ui_hint_bar(&cv, W, H - UI_FOOT_H, hints, 2);
    TEST_ASSERT_EQUAL_UINT(3, count(OP_ROUND));
    outer = nth(OP_ROUND, 0);
    TEST_ASSERT_EQUAL_INT16(W - UI_PAD, outer->x + outer->w);
    TEST_ASSERT_EQUAL_INT16(ow, outer->w);
    g0 = nth(OP_ROUND, 1);
    g1 = nth(OP_ROUND, 2);
    TEST_ASSERT_EQUAL_INT16(gw0, g0->w);
    TEST_ASSERT_TRUE(g0->w > UI_GLYPH_D);
    TEST_ASSERT_EQUAL_INT16(UI_GLYPH_D, g1->w);
    TEST_ASSERT_EQUAL_HEX16(UI_COL_HINT_BG, outer->color);
    TEST_ASSERT_EQUAL_HEX16(UI_COL_GLYPH, g0->color);
    /* The button's letter, struck twice a pixel apart in black, then its
     * label in white. */
    TEST_ASSERT_EQUAL_HEX16(UI_COL_GLYPH_FG, nth(OP_TEXT, 0)->color);
    TEST_ASSERT_EQUAL_INT16(nth(OP_TEXT, 0)->x + 1, nth(OP_TEXT, 1)->x);
    TEST_ASSERT_EQUAL_STRING("Palette", nth(OP_TEXT, 2)->s);
    TEST_ASSERT_EQUAL_HEX16(UI_COL_TEXT, nth(OP_TEXT, 2)->color);
    TEST_ASSERT_EQUAL_UINT(0, fk.violations);
}

static void test_hints_that_do_not_fit_are_dropped_from_the_end(void)
{
    static const ui_hint_t hints[] = {
        { "Start", "Run" }, { "D-pad", "Porch" }, { "A", "Save" },
        { "B", "Default" },
    };
    unsigned i;

    ui_hint_bar(&cv, 240, H - UI_FOOT_H, hints, 4);
    TEST_ASSERT_EQUAL_UINT(0, fk.violations);
    /* The outer pill and three glyphs: "B Default" did not fit. */
    TEST_ASSERT_EQUAL_UINT(4, count(OP_ROUND));
    TEST_ASSERT_TRUE(nth(OP_ROUND, 0)->x >= UI_PAD);
    for (i = 0; i < fk.n; i++) {
        TEST_ASSERT_TRUE(strcmp(fk.log[i].s, "Default") != 0);
    }
}

/* ─── the notice ─────────────────────────────────────────────────────────── */

static void test_the_notice_title_is_red_only_for_an_error(void)
{
    ui_notice(&cv, W, H, "Blank cart", "Put a game on it.", false, NULL, 0);
    TEST_ASSERT_EQUAL_HEX16(UI_COL_TEXT, nth(OP_TEXT, 0)->color);
    TEST_ASSERT_EQUAL_HEX16(UI_COL_TEXT, nth(OP_TEXT, 1)->color);
    setUp();
    ui_notice(&cv, W, H, "Unreadable tag", NULL, true, NULL, 0);
    TEST_ASSERT_EQUAL_HEX16(UI_COL_WARN, nth(OP_TEXT, 0)->color);
    TEST_ASSERT_EQUAL_UINT(1, count(OP_TEXT));
}

static void test_a_long_notice_title_wraps_in_the_list_font(void)
{
    ui_notice(&cv, W, H, "This title is far too long for the large font",
              "body", false, NULL, 0);
    TEST_ASSERT_EQUAL_UINT8(UI_FONT_LIST, nth(OP_TEXT, 0)->font);
    TEST_ASSERT_EQUAL_UINT8(2, nth(OP_TEXT, 0)->rows);
    TEST_ASSERT_TRUE(nth(OP_TEXT, 1)->y >=
                     nth(OP_TEXT, 0)->y + nth(OP_TEXT, 0)->h);
    TEST_ASSERT_EQUAL_UINT(0, fk.violations);
}

static void test_a_notice_without_hints_has_no_footer_and_a_four_row_body(void)
{
    ui_notice(&cv, W, H, "Write failed", "code -3 and a detail line long enough "
              "to wrap over more rows than the notice will give it", true,
              NULL, 0);
    /* The window's fill, and nothing round: no hint bar. */
    TEST_ASSERT_EQUAL_UINT(1, count(OP_FILL));
    TEST_ASSERT_EQUAL_UINT(0, count(OP_ROUND));
    TEST_ASSERT_EQUAL_UINT8(4, nth(OP_TEXT, 1)->rows);
    TEST_ASSERT_EQUAL_UINT8(UI_FONT_DESC, nth(OP_TEXT, 1)->font);
    TEST_ASSERT_TRUE(nth(OP_TEXT, 1)->y + nth(OP_TEXT, 1)->h <= H);
    TEST_ASSERT_EQUAL_UINT(0, fk.violations);
}

static void test_a_notice_with_hints_ends_in_the_footer(void)
{
    static const ui_hint_t hints[] = { { "A", "Continue" } };

    ui_notice(&cv, W, H, "Ready", "Insert a cart.", false, hints, 1);
    TEST_ASSERT_EQUAL_INT16(H - UI_FOOT_H, nth(OP_FILL, 1)->y);
    TEST_ASSERT_EQUAL_INT16(UI_FOOT_H, nth(OP_FILL, 1)->h);
    /* Title, body, then the hint's button, twice, and label. */
    TEST_ASSERT_EQUAL_UINT(5, count(OP_TEXT));
    TEST_ASSERT_EQUAL_UINT(0, fk.violations);
}

/* ─── image corners ───────────────────────────────────────────────────────── */

#define IMG 96
static uint16_t img[IMG * IMG];

static void fill_img(void)
{
    size_t i;

    for (i = 0; i < IMG * IMG; i++) {
        img[i] = 0xABCD;
    }
}

static void test_the_top_band_loses_its_corner_pixels_only(void)
{
    fill_img();
    ui_round_corners_565(img, IMG, IMG, 0, 16, UI_IMG_R, 0);
    TEST_ASSERT_EQUAL_HEX16(0, img[0]);
    TEST_ASSERT_EQUAL_HEX16(0, img[IMG - 1]);
    TEST_ASSERT_EQUAL_HEX16(0xABCD, img[4]);
    TEST_ASSERT_EQUAL_HEX16(0xABCD, img[4 * IMG]);
    TEST_ASSERT_EQUAL_HEX16(0xABCD, img[4 * IMG + IMG - 1]);
    TEST_ASSERT_EQUAL_HEX16(0xABCD, img[IMG / 2]);
}

static void test_a_middle_band_is_untouched(void)
{
    size_t i;

    fill_img();
    ui_round_corners_565(img, IMG, IMG, 16, 16, UI_IMG_R, 0);
    for (i = 0; i < 16 * IMG; i++) {
        TEST_ASSERT_EQUAL_HEX16(0xABCD, img[i]);
    }
}

static void test_the_bottom_band_rounds_the_last_corner(void)
{
    fill_img();
    /* The band's buffer holds image rows 80..95 as its rows 0..15. */
    ui_round_corners_565(img, IMG, IMG, 80, 16, UI_IMG_R, 0);
    TEST_ASSERT_EQUAL_HEX16(0, img[15 * IMG + 95]);
    TEST_ASSERT_EQUAL_HEX16(0, img[15 * IMG + 0]);
    TEST_ASSERT_EQUAL_HEX16(0xABCD, img[11 * IMG + 0]);
    TEST_ASSERT_EQUAL_HEX16(0xABCD, img[0]);
}

static void test_a_one_row_band_masks_only_its_own_corner_run(void)
{
    size_t i;
    unsigned masked = 0;

    fill_img();
    ui_round_corners_565(img, IMG, IMG, 0, 1, UI_IMG_R, 0);
    for (i = 0; i < IMG; i++) {
        masked += (img[i] == 0);
    }
    /* Row 0 of a radius-4 corner: x = 0..3 each side lie outside the arc. */
    TEST_ASSERT_EQUAL_UINT(8, masked);
    for (i = IMG; i < IMG * IMG; i++) {
        TEST_ASSERT_EQUAL_HEX16(0xABCD, img[i]);
    }
}

/* The manual's overview rounds one expanded row at a time. */
static void test_one_row_bands_round_an_overview_row_by_row(void)
{
    enum { OW = 168, OH = 240 };
    static uint16_t row[OW];
    int16_t ys[] = { 0, UI_IMG_R, OH - 1 };
    unsigned want[] = { 2 * UI_IMG_R, 0, 2 * UI_IMG_R };
    size_t k;
    size_t i;

    for (k = 0; k < 3; k++) {
        unsigned masked = 0;

        for (i = 0; i < OW; i++) {
            row[i] = 0xABCD;
        }
        ui_round_corners_565(row, OW, OH, ys[k], 1, UI_IMG_R, 0);
        for (i = 0; i < OW; i++) {
            masked += (row[i] == 0);
        }
        TEST_ASSERT_EQUAL_UINT(want[k], masked);
        if (want[k]) {
            TEST_ASSERT_EQUAL_HEX16(0, row[UI_IMG_R - 1]);
            TEST_ASSERT_EQUAL_HEX16(0xABCD, row[UI_IMG_R]);
            TEST_ASSERT_EQUAL_HEX16(0, row[OW - UI_IMG_R]);
        }
    }
}

/* ─── text placement ─────────────────────────────────────────────────────── */

static void test_text_is_centred_on_its_capitals_and_kept_in_its_box(void)
{
    const int16_t cap = ui_font_cap(UI_FONT_LIST);
    const int16_t lead = ui_font_lead(UI_FONT_LIST);
    const int16_t fh = ui_font_height(UI_FONT_LIST);

    /* The capitals' middle on the box's middle. */
    TEST_ASSERT_EQUAL_INT16((22 - cap) / 2 - lead,
                            ui_text_dy(22, UI_FONT_LIST));
    TEST_ASSERT_EQUAL_INT16((26 - cap) / 2 - lead,
                            ui_text_dy(26, UI_FONT_LIST));
    /* A box barely taller than the font keeps the font inside it. */
    TEST_ASSERT_EQUAL_INT16(1, ui_text_dy((int16_t)(fh + 1), UI_FONT_LIST));
    TEST_ASSERT_EQUAL_INT16(0, ui_text_dy(10, UI_FONT_LIST));
    /* The 8 px font's 7 px capitals in a 12 px circle. */
    TEST_ASSERT_EQUAL_INT16(2, ui_text_dy(UI_GLYPH_D, UI_FONT_HINT));
}

/* ─── scroll marks ───────────────────────────────────────────────────────── */

/* The fill whose top-left is x, y, if one was made. */
static bool filled_at(int16_t x, int16_t y)
{
    unsigned i;

    for (i = 0; i < fk.n; i++) {
        if (fk.log[i].op == OP_FILL && fk.log[i].x == x && fk.log[i].y == y) {
            return true;
        }
    }
    return false;
}

static void test_a_caret_points_up_or_down_from_its_apex(void)
{
    ui_caret(&cv, 100, 50, true, UI_COL_DIM);
    /* Apex on the centre column at the top; the arms reach the corners at
     * the bottom. */
    TEST_ASSERT_TRUE(filled_at(100 + UI_CARET_W / 2, 50));
    TEST_ASSERT_TRUE(filled_at(100, 50 + UI_CARET_H - 1));
    TEST_ASSERT_TRUE(filled_at(100 + UI_CARET_W - 1, 50 + UI_CARET_H - 1));
    TEST_ASSERT_FALSE(filled_at(100, 50));
    setUp();
    ui_caret(&cv, 100, 50, false, UI_COL_DIM);
    TEST_ASSERT_TRUE(filled_at(100 + UI_CARET_W / 2, 50 + UI_CARET_H - 1));
    TEST_ASSERT_TRUE(filled_at(100, 50));
    TEST_ASSERT_TRUE(filled_at(100 + UI_CARET_W - 1, 50));
    TEST_ASSERT_FALSE(filled_at(100, 50 + UI_CARET_H - 1));
    TEST_ASSERT_EQUAL_UINT(0, fk.violations);
}

/* ─── letterboxed corners ────────────────────────────────────────────────── */

/* A 96-square file holding an 86-row picture 5 rows down, black around it,
 * the way the imaging tool pads a screenshot. */
static void test_a_letterboxed_picture_is_rounded_at_its_own_corners(void)
{
    ui_inset_t in;
    size_t y;
    size_t x;
    int16_t b;

    for (y = 0; y < IMG; y++) {
        for (x = 0; x < IMG; x++) {
            img[y * IMG + x] = (y >= 5 && y < 91) ? 0xABCD : 0;
        }
    }
    ui_inset_begin(&in, IMG, IMG);
    for (b = 0; b < IMG; b += 16) {
        ui_round_inset_565(&in, img + (size_t)b * IMG, b, 16, UI_IMG_R, 0);
    }
    TEST_ASSERT_TRUE(in.found);
    TEST_ASSERT_EQUAL_INT16(5, in.top);
    TEST_ASSERT_EQUAL_INT16(0, in.left);
    /* The picture's top-left and bottom-right corners, not the file's. */
    TEST_ASSERT_EQUAL_HEX16(0, img[5 * IMG + 0]);
    TEST_ASSERT_EQUAL_HEX16(0, img[90 * IMG + 95]);
    TEST_ASSERT_EQUAL_HEX16(0xABCD, img[5 * IMG + UI_IMG_R]);
    TEST_ASSERT_EQUAL_HEX16(0xABCD, img[(5 + UI_IMG_R) * IMG + 0]);
    TEST_ASSERT_EQUAL_HEX16(0xABCD, img[48 * IMG + 0]);
}

static void test_an_all_black_image_is_left_alone(void)
{
    ui_inset_t in;
    size_t i;

    for (i = 0; i < IMG * IMG; i++) {
        img[i] = 0;
    }
    ui_inset_begin(&in, IMG, IMG);
    ui_round_inset_565(&in, img, 0, IMG, UI_IMG_R, 0);
    TEST_ASSERT_FALSE(in.found);
    for (i = 0; i < IMG * IMG; i++) {
        TEST_ASSERT_EQUAL_HEX16(0, img[i]);
    }
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_the_pill_hugs_a_short_label);
    RUN_TEST(test_an_overflowing_label_is_held_to_max_w);
    RUN_TEST(test_the_pill_is_one_white_round_fill_of_half_its_height);
    RUN_TEST(test_the_pill_is_built_offscreen_at_its_own_size);
    RUN_TEST(test_the_label_is_one_transparent_black_pass);
    RUN_TEST(test_a_dim_label_is_in_the_dim_on_pill_grey);
    RUN_TEST(test_a_marquee_moves_the_text_and_not_the_pill);
    RUN_TEST(test_without_a_buffer_the_marquee_is_ignored_and_the_text_fits);
    RUN_TEST(test_a_plain_row_and_a_pill_start_their_text_at_one_x);
    RUN_TEST(test_a_plain_row_is_cleared_to_black_with_no_bar);
    RUN_TEST(test_the_header_is_white_left_and_grey_right_on_black);
    RUN_TEST(test_the_help_line_is_one_grey_row_inside_its_band);
    RUN_TEST(test_a_null_help_line_only_clears);
    RUN_TEST(test_no_hints_paints_only_the_background);
    RUN_TEST(test_two_hints_right_align_and_a_word_button_stretches);
    RUN_TEST(test_hints_that_do_not_fit_are_dropped_from_the_end);
    RUN_TEST(test_the_notice_title_is_red_only_for_an_error);
    RUN_TEST(test_a_long_notice_title_wraps_in_the_list_font);
    RUN_TEST(test_a_notice_without_hints_has_no_footer_and_a_four_row_body);
    RUN_TEST(test_a_notice_with_hints_ends_in_the_footer);
    RUN_TEST(test_the_top_band_loses_its_corner_pixels_only);
    RUN_TEST(test_a_middle_band_is_untouched);
    RUN_TEST(test_the_bottom_band_rounds_the_last_corner);
    RUN_TEST(test_a_one_row_band_masks_only_its_own_corner_run);
    RUN_TEST(test_one_row_bands_round_an_overview_row_by_row);
    RUN_TEST(test_text_is_centred_on_its_capitals_and_kept_in_its_box);
    RUN_TEST(test_a_caret_points_up_or_down_from_its_apex);
    RUN_TEST(test_a_letterboxed_picture_is_rounded_at_its_own_corners);
    RUN_TEST(test_an_all_black_image_is_left_alone);
    return UNITY_END();
}
