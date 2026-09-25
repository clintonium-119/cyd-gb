#include <unity.h>

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "ui/list.h"
#include "ui/menu_draw.h"

/*
 * The in-game menu's layout, over a fake canvas.
 *
 * The fake paints a w x h buffer the way the picker suite's does: a fill or
 * an opaque text box sets its pixels to a value that is a function of the
 * call alone, and transparent text marks over whatever is there. So two draws
 * that make the same calls in the same places leave the same pixels, and a
 * partial repaint can be compared against a full one byte for byte.
 */

#define W 266
#define H 240

enum { PAINT_SET, PAINT_OVER };

typedef struct {
    uint16_t fb[W * H];
    unsigned violations;
    unsigned fills, texts, images;
    struct {
        char s[40];
        int16_t x, y;
        uint16_t fg, bg;
    } log[64];
    unsigned logged;
    struct {
        int16_t x, y, w, h;
        uint16_t color;
    } flog[32];
    unsigned flogged;
    struct {
        int16_t x, y, w, h, r;
        uint16_t color;
    } rlog[16];
    unsigned rlogged;
} fake_t;

static fake_t fk;
static fake_t fk_ref;

static uint16_t hash_call(const char* s, uint16_t fg, uint16_t bg, int16_t x,
                          int16_t y)
{
    uint32_t h = 2166136261u;

    while (s != NULL && *s) {
        h = (h ^ (uint8_t)*s++) * 16777619u;
    }
    h ^= (uint32_t)fg * 31u ^ (uint32_t)bg * 7u ^ (uint32_t)x * 131u ^
         (uint32_t)y;
    return (uint16_t)(h ^ (h >> 16));
}

static void put_rect(fake_t* f, int16_t x, int16_t y, int16_t w, int16_t h,
                     int mode, uint16_t v)
{
    int16_t iy, ix;

    if (x < 0 || y < 0 || w < 0 || h < 0 || x + w > W || y + h > H) {
        f->violations++;
        return;
    }
    for (iy = y; iy < y + h; iy++) {
        for (ix = x; ix < x + w; ix++) {
            uint16_t* px = &f->fb[iy * W + ix];

            *px = (mode == PAINT_SET) ? v : (uint16_t)(*px ^ (v | 1u));
        }
    }
}

static void fk_fill(void* ctx, int16_t x, int16_t y, int16_t w, int16_t h,
                    uint16_t color)
{
    fake_t* f = (fake_t*)ctx;

    f->fills++;
    if (f->flogged < sizeof(f->flog) / sizeof(f->flog[0])) {
        f->flog[f->flogged].x = x;
        f->flog[f->flogged].y = y;
        f->flog[f->flogged].w = w;
        f->flog[f->flogged].h = h;
        f->flog[f->flogged].color = color;
        f->flogged++;
    }
    put_rect(f, x, y, w, h, PAINT_SET, color);
}

static void fk_round_fill(void* ctx, int16_t x, int16_t y, int16_t w,
                          int16_t h, int16_t r, uint16_t color, uint16_t bg)
{
    fake_t* f = (fake_t*)ctx;

    (void)bg;
    if (f->rlogged < sizeof(f->rlog) / sizeof(f->rlog[0])) {
        f->rlog[f->rlogged].x = x;
        f->rlog[f->rlogged].y = y;
        f->rlog[f->rlogged].w = w;
        f->rlog[f->rlogged].h = h;
        f->rlog[f->rlogged].r = r;
        f->rlog[f->rlogged].color = color;
        f->rlogged++;
    }
    put_rect(f, x, y, w, h, PAINT_SET, color);
}

static void fk_text(void* ctx, const char* s, int16_t x, int16_t y, int16_t w,
                    uint8_t rows, uint8_t font, uint8_t align, uint16_t fg,
                    uint16_t bg)
{
    fake_t* f = (fake_t*)ctx;
    int16_t h = (int16_t)(rows * UI_ROW_PITCH(ui_font_height(font)) - 2);

    (void)align;
    f->texts++;
    if (f->logged < sizeof(f->log) / sizeof(f->log[0])) {
        snprintf(f->log[f->logged].s, sizeof(f->log[0].s), "%s",
                 s ? s : "(null)");
        f->log[f->logged].x = x;
        f->log[f->logged].y = y;
        f->log[f->logged].fg = fg;
        f->log[f->logged].bg = bg;
        f->logged++;
    }
    put_rect(f, x, y, w, h, (fg == bg) ? PAINT_OVER : PAINT_SET,
             hash_call(s, fg, bg, x, y));
}

static void fk_image(void* ctx, int16_t x, int16_t y, int16_t w, int16_t h,
                     const uint16_t* px, int16_t row0, int16_t rows)
{
    (void)px;
    (void)row0;
    (void)rows;
    ((fake_t*)ctx)->images++;
    put_rect((fake_t*)ctx, x, y, w, h, PAINT_SET, 0x5555);
}

static int16_t fk_measure(void* ctx, const char* s, uint8_t font)
{
    (void)ctx;
    return (s == NULL) ? 0
                       : (int16_t)(strlen(s) * ((font == UI_FONT_SMALL)
                                                    ? UI_FONT_SMALL_ADV
                                                    : 8));
}

static ui_canvas_t canvas_over(fake_t* f)
{
    ui_canvas_t cv;

    memset(&cv, 0, sizeof(cv));
    memset(f, 0, sizeof(*f));
    cv.ctx = f;
    cv.fill = fk_fill;
    cv.round_fill = fk_round_fill;
    cv.text = fk_text;
    cv.image = fk_image;
    cv.measure = fk_measure;
    return cv;
}

/* ─── the list the binding builds ─────────────────────────────────────────── */

#define N_ROWS 9
#define ROW_MANUAL 2
#define ROW_VOLUME 5

static const ui_hint_t HINTS[] = { { "B", "Resume" }, { "A", "Select" } };

static menu_item_t items[N_ROWS];
static menu_layout_t g;
static menu_view_t v;
static ui_canvas_t cv;

void setUp(void)
{
    static const char* const labels[N_ROWS] = {
        "Resume", "Save State", "Game Manual (Unavailable)", "Cart Info",
        "Color Palette", "Volume", "Brightness", "Hotkeys", "Reset",
    };
    uint8_t i;

    for (i = 0; i < N_ROWS; i++) {
        items[i].label = labels[i];
        items[i].value = NULL;
        items[i].off = false;
    }
    items[ROW_MANUAL].off = true;
    items[4].value = "Auto";
    items[ROW_VOLUME].value = "Med";
    items[6].value = "5/8";
    TEST_ASSERT_TRUE(menu_layout(W, H, &g));
    v.items = items;
    v.n = N_ROWS;
    v.first = 0;
    v.cursor = 0;
    v.help = "Back to the game.";
    v.hints = HINTS;
    v.n_hints = 2;
    cv = canvas_over(&fk);
}

void tearDown(void)
{
}

/* The screen as a fresh full draw of the current view would leave it. */
static void assert_same_as_full(void)
{
    ui_canvas_t ref = canvas_over(&fk_ref);

    menu_draw(&ref, &g, &v);
    TEST_ASSERT_EQUAL_UINT(0, fk.violations);
    TEST_ASSERT_EQUAL_MEMORY_MESSAGE(fk_ref.fb, fk.fb, sizeof(fk.fb),
                                     "a partial draw left a different screen");
}

/* ─── the list ────────────────────────────────────────────────────────────── */

static void test_the_layout_fits_seven_rows_over_the_help_line(void)
{
    TEST_ASSERT_EQUAL_UINT8(MENU_VISIBLE, g.visible);
    TEST_ASSERT_EQUAL_INT16(220, g.foot_y);
    TEST_ASSERT_EQUAL_INT16(202, g.help_y);
    TEST_ASSERT_TRUE(MENU_TOP + MENU_VISIBLE * MENU_ROW_H <= g.help_y);
    TEST_ASSERT_FALSE(menu_layout(W, 223, &g));
    TEST_ASSERT_FALSE(menu_layout(W, H, NULL));
}

/* The footer's band cleared: one fill across the window at foot_y. */
static bool drew_footer(void)
{
    unsigned i;

    for (i = 0; i < fk.flogged; i++) {
        if (fk.flog[i].y == g.foot_y && fk.flog[i].w == W &&
            fk.flog[i].h == UI_FOOT_H) {
            return true;
        }
    }
    return false;
}

static void test_the_full_list_stays_inside_the_window(void)
{
    unsigned i;
    bool help = false;

    menu_draw(&cv, &g, &v);
    TEST_ASSERT_EQUAL_UINT(0, fk.violations);
    /* No header: a label per visible row plus the values among them, the
     * help line and the hints. */
    TEST_ASSERT_EQUAL_STRING("Resume", fk.log[0].s);
    TEST_ASSERT_GREATER_OR_EQUAL_UINT(MENU_VISIBLE, fk.texts);
    for (i = 0; i < fk.logged; i++) {
        help = help || strcmp(fk.log[i].s, "Back to the game.") == 0;
    }
    TEST_ASSERT_TRUE(help);
    TEST_ASSERT_TRUE(drew_footer());
}

static void test_a_cursor_move_repaints_to_the_full_screen(void)
{
    menu_draw(&cv, &g, &v);
    v.cursor = 1;
    v.help = "Save or load a snapshot of the game.";
    menu_draw_row(&cv, &g, &v, 0);
    menu_draw_row(&cv, &g, &v, 1);
    menu_draw_footer(&cv, &g, v.help, v.hints, v.n_hints);
    assert_same_as_full();
}

static void test_a_scroll_repaints_the_rows_and_leaves_the_title(void)
{
    static uint16_t title[W * MENU_TOP];

    menu_draw(&cv, &g, &v);
    memcpy(title, fk.fb, sizeof(title));
    v.first = 2;
    v.cursor = 8;
    fk.flogged = 0;
    menu_draw_rows(&cv, &g, &v);
    TEST_ASSERT_EQUAL_MEMORY(title, fk.fb, sizeof(title));
    assert_same_as_full();
}

/* A list-launched game's menu has a tenth row. Up from the top wraps to it,
 * the window follows, and it draws highlighted inside the window. */
static void test_a_ten_row_menu_reaches_and_pills_its_last_row(void)
{
    menu_item_t ten[N_ROWS + 1];
    list_state_t ls;
    unsigned i;
    bool pilled = false;

    memcpy(ten, items, sizeof(items));
    ten[N_ROWS].label = "Return to Games List";
    ten[N_ROWS].value = NULL;
    ten[N_ROWS].off = false;

    TEST_ASSERT_EQUAL_INT(0, list_init(&ls, N_ROWS + 1, MENU_VISIBLE));
    list_move(&ls, -1);
    TEST_ASSERT_EQUAL_UINT16(N_ROWS, list_cursor(&ls));
    TEST_ASSERT_EQUAL_UINT16(N_ROWS + 1 - MENU_VISIBLE, list_first(&ls));

    v.items = ten;
    v.n = N_ROWS + 1;
    v.first = list_first(&ls);
    v.cursor = list_cursor(&ls);
    menu_draw(&cv, &g, &v);
    TEST_ASSERT_EQUAL_UINT(0, fk.violations);
    for (i = 0; i < fk.logged; i++) {
        pilled = pilled
                 || (strcmp(fk.log[i].s, "Return to Games List") == 0
                     && fk.log[i].fg == UI_COL_PILL_TEXT);
    }
    TEST_ASSERT_TRUE(pilled);
}

static void test_a_value_change_repaints_to_the_full_screen(void)
{
    v.first = 0;
    v.cursor = ROW_VOLUME;
    menu_draw(&cv, &g, &v);
    items[ROW_VOLUME].value = "High";
    menu_draw_row(&cv, &g, &v, ROW_VOLUME);
    assert_same_as_full();
}

static void test_the_highlight_is_a_bold_pill_that_hugs_its_label(void)
{
    menu_draw_bar(&cv, &g, MENU_TOP, "Resume", NULL, true, false);
    /* One transparent black pass in the bold list font. */
    TEST_ASSERT_EQUAL_UINT(1, fk.logged);
    TEST_ASSERT_EQUAL_HEX16(fk.log[0].fg, fk.log[0].bg);
    TEST_ASSERT_EQUAL_HEX16(UI_COL_PILL_TEXT, fk.log[0].fg);
    TEST_ASSERT_EQUAL_INT16(UI_TEXT_X, fk.log[0].x);
    TEST_ASSERT_EQUAL_UINT(1, fk.rlogged);
    TEST_ASSERT_EQUAL_INT16(6 * 8 + 2 * UI_PILL_PAD, fk.rlog[0].w);
    TEST_ASSERT_EQUAL_INT16(11, fk.rlog[0].r);
    TEST_ASSERT_EQUAL_HEX16(UI_COL_PILL, fk.rlog[0].color);
    TEST_ASSERT_EQUAL_UINT(0, fk.violations);
}

static void test_a_dimmed_row_is_grey_and_never_bold(void)
{
    menu_draw_bar(&cv, &g, MENU_TOP, "Game Manual (Unavailable)", NULL, false,
                  true);
    TEST_ASSERT_EQUAL_UINT(1, fk.logged);
    TEST_ASSERT_EQUAL_HEX16(UI_COL_DIM, fk.log[0].fg);

    cv = canvas_over(&fk);
    menu_draw_bar(&cv, &g, MENU_TOP, "Game Manual (Unavailable)", NULL, true,
                  true);
    TEST_ASSERT_EQUAL_UINT(1, fk.logged);
    TEST_ASSERT_EQUAL_HEX16(UI_COL_PILL_DIM, fk.log[0].fg);
}

static void test_a_value_sits_outside_the_pill(void)
{
    int16_t value_w = 4 * 8; /* "High" */
    unsigned i;

    menu_draw_bar(&cv, &g, MENU_TOP, "Volume", "High", true, false);
    TEST_ASSERT_EQUAL_UINT(1, fk.rlogged);
    TEST_ASSERT_TRUE(fk.rlog[0].w <=
                     W - 2 * UI_PAD - value_w - UI_PILL_PAD);
    /* The value is set right against the pill's text inset, and its glyphs
     * therefore start past the pill's end. */
    for (i = 0; i < fk.logged; i++) {
        if (strcmp(fk.log[i].s, "High") == 0) {
            TEST_ASSERT_TRUE(fk.log[i].x + (W - 2 * (UI_PAD + UI_PILL_PAD)) -
                                 value_w >
                             fk.rlog[0].x + fk.rlog[0].w);
            TEST_ASSERT_EQUAL_HEX16(UI_COL_TEXT, fk.log[i].fg);
        }
    }
    TEST_ASSERT_EQUAL_UINT(0, fk.violations);
}

static void test_the_save_state_choice_pills_only_the_highlight(void)
{
    menu_draw_bar(&cv, &g, 100, "No", NULL, true, false);
    menu_draw_bar(&cv, &g, 100 + MENU_ROW_H, "Yes", NULL, false, false);
    TEST_ASSERT_EQUAL_UINT(1, fk.rlogged);
    TEST_ASSERT_EQUAL_INT16(100 + (MENU_ROW_H - UI_PILL_H_ROW) / 2,
                            fk.rlog[0].y);
    TEST_ASSERT_EQUAL_UINT(0, fk.violations);
}

/* ─── the pages ───────────────────────────────────────────────────────────── */

static void test_the_hotkeys_page_stays_inside_the_window(void)
{
    static const char* const keys[][2] = {
        { "Select + Start", "Menu" },
        { "Select + Up/Down", "Volume" },
        { "Select + Right/Left", "Brightness" },
        { "Select + A + B", "Fast-forward" },
    };

    menu_draw_hotkeys(&cv, &g, keys, 4);
    TEST_ASSERT_EQUAL_UINT(0, fk.violations);
    TEST_ASSERT_EQUAL_STRING("Hotkeys", fk.log[0].s);
    TEST_ASSERT_EQUAL_STRING("Back", fk.log[fk.logged - 1].s);
    TEST_ASSERT_TRUE(drew_footer());
}

static void test_a_one_row_cart_title_gives_the_images_a_row(void)
{
    int16_t one = menu_draw_cart_title(&cv, &g, "Tetris");
    int16_t two = menu_draw_cart_title(
        &cv, &g, "The Legend of Zelda: Link's Awakening DX Edition");

    TEST_ASSERT_EQUAL_INT16(UI_PAD + 22 + 2, one);
    TEST_ASSERT_EQUAL_INT16(UI_PAD + 2 * 22 + 2, two);
    TEST_ASSERT_EQUAL_UINT(0, fk.violations);
}

static void test_the_band_fills_to_the_back_line_and_scrolls_inside_it(void)
{
    static const char text[] =
        "Link washes ashore on Koholint Island and must wake the Wind Fish to "
        "escape. A dream-logic adventure that quietly erases its own world as "
        "you finish it, and one of the very finest games on the system.";
    menu_band_t b;
    int16_t y = (int16_t)(UI_PAD + 2 * 22 + 2 + 96 + 8);
    unsigned i;

    menu_band_fit(&cv, &g, text, y, &b);
    /* (220 - 2 - 154) / 15: down to the hints, the description being the
     * page's help. */
    TEST_ASSERT_EQUAL_UINT16(4, b.rows);
    TEST_ASSERT_EQUAL_INT16(W - 2 * UI_TEXT_X, b.w);
    /* The fake's 8 px advance, measured rather than assumed. */
    TEST_ASSERT_EQUAL_UINT8(8, b.adv.w['e' - 32]);
    menu_draw_band(&cv, &b);
    b.first = 1;
    menu_draw_band(&cv, &b);
    TEST_ASSERT_EQUAL_UINT(0, fk.violations);
    TEST_ASSERT_TRUE(b.y + (int16_t)b.rows * 15 <= g.foot_y);
    /* Every line fits the band at the measured advances. */
    for (i = 0; i < fk.logged; i++) {
        TEST_ASSERT_TRUE(strlen(fk.log[i].s) * 8 <= (size_t)b.w);
    }

    menu_band_fit(&cv, &g, NULL, y, &b);
    TEST_ASSERT_EQUAL_UINT16(0, b.rows);
    fk.fills = 0;
    menu_draw_band(&cv, &b);
    TEST_ASSERT_EQUAL_UINT(0, fk.fills);
}

static void test_the_save_state_screens_stay_inside_the_window(void)
{
    static const ui_hint_t hints[] = { { "A", "Select" }, { "B", "Back" } };

    menu_draw_title(&cv, &g, "Save State");
    menu_draw_question(&cv, &g,
                       "Load state? Progress since it was saved will be lost.");
    menu_draw_slot(&cv, (W - 80) / 2, MENU_TOP + 4, 80, 72, "No saved state");
    menu_draw_footer(&cv, &g, NULL, hints, 2);
    TEST_ASSERT_EQUAL_UINT(0, fk.violations);
    TEST_ASSERT_TRUE(drew_footer());
    /* The slot is rounded like an image. */
    TEST_ASSERT_EQUAL_INT16(UI_IMG_R, fk.rlog[0].r);
    TEST_ASSERT_EQUAL_HEX16(UI_COL_SLOT, fk.rlog[0].color);
}

/* The save-state thumbnail streams in three 24-row bands: the first and last
 * lose their corners, the middle is untouched — so there is no seam. */
static void test_the_thumbnail_bands_round_only_the_outer_corners(void)
{
    static uint16_t band[80 * 24];
    size_t i;
    int16_t b;

    for (b = 0; b < 3; b++) {
        for (i = 0; i < 80 * 24; i++) {
            band[i] = 0xABCD;
        }
        ui_round_corners_565(band, 80, 72, (int16_t)(b * 24), 24, UI_IMG_R,
                             UI_COL_BG);
        if (b == 0) {
            TEST_ASSERT_EQUAL_HEX16(UI_COL_BG, band[0]);
            TEST_ASSERT_EQUAL_HEX16(UI_COL_BG, band[79]);
            TEST_ASSERT_EQUAL_HEX16(0xABCD, band[23 * 80]);
        } else if (b == 1) {
            for (i = 0; i < 80 * 24; i++) {
                TEST_ASSERT_EQUAL_HEX16(0xABCD, band[i]);
            }
        } else {
            TEST_ASSERT_EQUAL_HEX16(UI_COL_BG, band[23 * 80]);
            TEST_ASSERT_EQUAL_HEX16(UI_COL_BG, band[23 * 80 + 79]);
            TEST_ASSERT_EQUAL_HEX16(0xABCD, band[0]);
        }
    }
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_the_layout_fits_seven_rows_over_the_help_line);
    RUN_TEST(test_the_full_list_stays_inside_the_window);
    RUN_TEST(test_a_cursor_move_repaints_to_the_full_screen);
    RUN_TEST(test_a_scroll_repaints_the_rows_and_leaves_the_title);
    RUN_TEST(test_a_value_change_repaints_to_the_full_screen);
    RUN_TEST(test_a_ten_row_menu_reaches_and_pills_its_last_row);
    RUN_TEST(test_the_highlight_is_a_bold_pill_that_hugs_its_label);
    RUN_TEST(test_a_dimmed_row_is_grey_and_never_bold);
    RUN_TEST(test_a_value_sits_outside_the_pill);
    RUN_TEST(test_the_save_state_choice_pills_only_the_highlight);
    RUN_TEST(test_the_hotkeys_page_stays_inside_the_window);
    RUN_TEST(test_a_one_row_cart_title_gives_the_images_a_row);
    RUN_TEST(test_the_band_fills_to_the_back_line_and_scrolls_inside_it);
    RUN_TEST(test_the_save_state_screens_stay_inside_the_window);
    RUN_TEST(test_the_thumbnail_bands_round_only_the_outer_corners);
    return UNITY_END();
}
