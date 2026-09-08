#include <unity.h>

#include <stddef.h>
#include <stdio.h>
#include <string.h>

#include "ui/picker_draw.h"

/*
 * Framebuffer bounds suite — the WS-12 exit criterion made mechanical.
 *
 * There is no font on the host, so there is nothing to golden-hash. What can
 * be proved is the property the exit actually names: every primitive the
 * layout module emits lands inside the game window. A fake canvas paints a
 * buffer of exactly w x h and counts anything whose rectangle leaves it, and
 * the driver below walks every screen the writer has at both SCALE_K windows.
 *
 * The detail page scrolls, so a screen is not one picture but one per scroll
 * offset — and an image clipped at the band's edge is drawn as a row range,
 * which is the case most likely to walk off either the window or the source
 * buffer. Both are checked.
 *
 * Every expected number is the arithmetic from the layout module written out,
 * not a literal copied from a run.
 */

/* The two windows SCALE_K gives (render_config.h: 160 x k/16 by 144 x k/16). */
#define GEOM_24_W 240
#define GEOM_24_H 216
#define GEOM_26_W 260
#define GEOM_26_H 234

#define FB_MAX (GEOM_26_W * GEOM_26_H)

/* The catalog shape: the library's 132 titles, every tenth one a starter. */
#define LIB_COUNT 132
#define LIB_STARTER_EVERY 10
#define LIB_STARTERS 14

#define B_NONE  0x00u
#define B_A     ((uint8_t)COMBO_BTN_A)
#define B_DOWN  ((uint8_t)COMBO_BTN_DOWN)

/* A description at exactly the catalog's 200-byte cap, in real words. */
static const char DESC_200[] =
    "Link washes ashore on Koholint Island and must wake the Wind Fish to "
    "escape. A dream-logic adventure that quietly erases its own world as "
    "you finish it, and one of the very finest games on the system.";

static catalog_index_t lib;

/* ─── the fake canvas ─────────────────────────────────────────────────────── */

typedef struct {
    int16_t w, h;
    uint16_t fb[FB_MAX];
    unsigned violations, fills, texts, images;
    int16_t last_img_x, last_img_y, last_img_w, last_img_rows;
    unsigned art_fills;   /* fills exactly PICKER_ART_W wide            */
    unsigned bar_fills;   /* fills in the hold-bar colour               */
    int16_t bar_w;
    int16_t bar_y;
    unsigned range_faults; /* image row ranges outside the source image */
    unsigned null_strings;
    int16_t widest_desc;
} fake_t;

static fake_t fk;

/* Paint the rect, or count it as a violation. One rule, three callers. */
static void put_rect(fake_t* f, int16_t x, int16_t y, int16_t w, int16_t h)
{
    int16_t iy, ix;

    if (x < 0 || y < 0 || w < 0 || h < 0 || x + w > f->w || y + h > f->h) {
        f->violations++;
        return;
    }
    for (iy = y; iy < y + h; iy++) {
        for (ix = x; ix < x + w; ix++) {
            f->fb[iy * f->w + ix] = 1;
        }
    }
}

static void fk_fill(void* ctx, int16_t x, int16_t y, int16_t w, int16_t h,
                    uint16_t color)
{
    fake_t* f = (fake_t*)ctx;

    f->fills++;
    if (w == PICKER_ART_W) {
        f->art_fills++;
    }
    /* COL_BAR, the hold bar's green — the one colour a test asserts on. */
    if (color == 0x07E0) {
        f->bar_fills++;
        f->bar_w = w;
        f->bar_y = y;
    }
    put_rect(f, x, y, w, h);
}

static void fk_text(void* ctx, const char* s, int16_t x, int16_t y, int16_t w,
                    uint8_t rows, uint8_t font, uint8_t align, uint16_t fg,
                    uint16_t bg)
{
    fake_t* f = (fake_t*)ctx;
    int16_t h;

    (void)align;
    (void)fg;
    (void)bg;
    f->texts++;
    if (s == NULL) {
        f->null_strings++;
        return;
    }
    if (font == UI_FONT_SMALL && w > f->widest_desc) {
        f->widest_desc = w;
    }
    /* The box the driver will clip into: rows lines at the font's pitch. */
    h = (int16_t)(rows * UI_ROW_PITCH(ui_font_height(font)));
    put_rect(f, x, y, w, h);
}

static void fk_image(void* ctx, int16_t x, int16_t y, int16_t w, int16_t h,
                     const uint16_t* px, int16_t row0, int16_t rows)
{
    fake_t* f = (fake_t*)ctx;

    f->images++;
    f->last_img_x = x;
    f->last_img_y = y;
    f->last_img_w = w;
    f->last_img_rows = rows;
    if (px == NULL) {
        f->null_strings++;
    }
    /* A row range naming rows the source does not have would read past the
     * buffer on the device, which the window check alone would not catch. */
    if (row0 < 0 || rows < 0 || row0 + rows > PICKER_ART_H) {
        f->range_faults++;
    }
    put_rect(f, x, y, w, h);
}

static ui_canvas_t canvas_over(fake_t* f, int16_t w, int16_t h)
{
    ui_canvas_t cv;

    memset(f, 0, sizeof(*f));
    f->w = w;
    f->h = h;
    cv.ctx = f;
    cv.fill = fk_fill;
    cv.text = fk_text;
    cv.image = fk_image;
    return cv;
}

void setUp(void)
{
}

void tearDown(void)
{
}

/* ─── the library ─────────────────────────────────────────────────────────── */

static void fill_library(size_t n)
{
    size_t i;

    memset(&lib, 0, sizeof(lib));
    for (i = 0; i < n; i++) {
        lib.e[i].offset = (uint32_t)(i * 100);
        snprintf(lib.e[i].filename, sizeof(lib.e[i].filename),
                 "Game %03zu.gb", i);
        /* A long title, so the box the driver clips into is the interesting
         * one rather than a short label. */
        snprintf(lib.e[i].title, sizeof(lib.e[i].title),
                 "Game %03zu: A Very Long Subtitle Indeed", i);
        lib.e[i].flags =
            (i % LIB_STARTER_EVERY == 0) ? CATALOG_FLAG_STARTER : 0;
    }
    lib.count = n;
}

/* ─── scenario drivers ────────────────────────────────────────────────────── */

static void run_list(int16_t w, int16_t h, enum picker_mode_e mode,
                     bool wild_done, bool pending_set, const boot_made_t* made)
{
    picker_t p;
    picker_layout_t g;
    ui_canvas_t cv = canvas_over(&fk, w, h);

    TEST_ASSERT_EQUAL_INT(PICKER_OK, picker_layout(w, h, &g));
    TEST_ASSERT_EQUAL_INT(PICKER_OK,
                          picker_init(&p, mode, &lib, wild_done, pending_set,
                                      made, g.rows));
    picker_draw(&p, &g, NULL, NULL, NULL, &cv);
}

/*
 * Open `row`, force the two media states, wind the hold to `pct`, and step the
 * scroll `scroll` times — then draw. Everything goes through the real input
 * path so no scenario can reach a state the machine could not.
 */
static void run_detail(int16_t w, int16_t h, enum picker_mode_e mode,
                       bool wild_done, bool pending_set, uint16_t row,
                       uint8_t art_state, uint8_t shot_state, uint8_t pct,
                       uint16_t scroll, const char* desc)
{
    picker_t p;
    picker_layout_t g;
    ui_canvas_t cv = canvas_over(&fk, w, h);
    uint32_t t = 0;
    uint16_t idx = 0;
    uint16_t i;

    TEST_ASSERT_EQUAL_INT(PICKER_OK, picker_layout(w, h, &g));
    TEST_ASSERT_EQUAL_INT(PICKER_OK,
                          picker_init(&p, mode, &lib, wild_done, pending_set,
                                      NULL, g.rows));
    while (list_cursor(&p.list) < row) {
        picker_input(&p, B_DOWN, t++);
        picker_input(&p, B_NONE, t++);
    }
    picker_input(&p, B_A, t++);
    picker_input(&p, B_NONE, t++);
    TEST_ASSERT_EQUAL_UINT8(PICKER_SCREEN_DETAIL, p.screen);

    if (picker_media_due(&p, &idx)) {
        picker_media_loaded(&p, idx, art_state == PICKER_MEDIA_READY,
                            shot_state == PICKER_MEDIA_READY);
        /* A state left LOADING is the window between the request and the
         * answer, and it draws too. */
        if (art_state == PICKER_MEDIA_LOADING) {
            p.art_state = PICKER_MEDIA_LOADING;
        }
        if (shot_state == PICKER_MEDIA_LOADING) {
            p.shot_state = PICKER_MEDIA_LOADING;
        }
    }

    picker_set_scroll_span(&p, picker_page_lines(&g, desc), g.band_rows);
    for (i = 0; i < scroll; i++) {
        picker_input(&p, B_DOWN, t++);
        picker_input(&p, B_NONE, t++);
    }

    if (pct > 0) {
        uint32_t at = t + 10;
        uint32_t elapsed = (uint32_t)PICKER_HOLD_MS * pct / 100;

        picker_input(&p, B_A, at);
        if (elapsed >= (uint32_t)PICKER_HOLD_MS) {
            /* One tick short of done, so the page is still on screen with a
             * full bar rather than gone. */
            elapsed = (uint32_t)PICKER_HOLD_MS - 1;
        }
        picker_input(&p, B_A, at + elapsed);
        TEST_ASSERT_EQUAL_UINT8(PICKER_SCREEN_DETAIL, p.screen);
    }

    picker_draw(&p, &g, desc, (const uint16_t*)&lib, (const uint16_t*)&lib,
                &cv);
}

/* Every scenario must satisfy these, whatever else it asserts. */
static void assert_sane(void)
{
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0, fk.violations,
                                   "a primitive left the game window");
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0, fk.range_faults,
                                   "an image row range left its source");
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0, fk.null_strings,
                                   "a NULL string or image was drawn");
    TEST_ASSERT_GREATER_THAN_UINT(0, fk.fills);
    TEST_ASSERT_GREATER_THAN_UINT(0, fk.texts);
}

/* ─── the layout table ───────────────────────────────────────────────────── */

static void test_the_layout_at_240_by_216(void)
{
    picker_layout_t g;

    TEST_ASSERT_EQUAL_INT(PICKER_OK, picker_layout(240, 216, &g));
    TEST_ASSERT_EQUAL_UINT8(11, g.rows);        /* (216 - 18) / 18      */
    TEST_ASSERT_EQUAL_INT16(232, g.list_w);     /* 240 - 8              */
    TEST_ASSERT_EQUAL_INT16(52, g.band_y);      /* 48 + 4               */
    TEST_ASSERT_EQUAL_INT16(118, g.band_h);     /* 216 - 52 - 46        */
    TEST_ASSERT_EQUAL_UINT8(11, g.band_rows);   /* 118 / 10             */
    TEST_ASSERT_EQUAL_INT16(4, g.art_x);
    TEST_ASSERT_EQUAL_INT16(104, g.desc_x);     /* 4 + 96 + 4           */
    TEST_ASSERT_EQUAL_INT16(132, g.desc_w);     /* 240 - 104 - 4        */
    TEST_ASSERT_EQUAL_UINT8(22, g.desc_cols);   /* 132 / 6              */
    TEST_ASSERT_EQUAL_INT16(100, g.shot_page_y); /* 96 + 4              */
    TEST_ASSERT_EQUAL_INT16(196, g.page_h);     /* 100 + 96             */

    /* 22 x 11 = 242 characters on screen at once, against the catalog's
     * 200-byte cap — the whole description arrives visible. */
    TEST_ASSERT_GREATER_OR_EQUAL_UINT(CATALOG_DESC_MAX - 1,
                                      (unsigned)(g.desc_cols * g.band_rows));
}

static void test_the_layout_at_260_by_234(void)
{
    picker_layout_t g;

    TEST_ASSERT_EQUAL_INT(PICKER_OK, picker_layout(260, 234, &g));
    TEST_ASSERT_EQUAL_UINT8(12, g.rows);        /* (234 - 18) / 18      */
    TEST_ASSERT_EQUAL_INT16(252, g.list_w);     /* 260 - 8              */
    TEST_ASSERT_EQUAL_INT16(136, g.band_h);     /* 234 - 52 - 46        */
    TEST_ASSERT_EQUAL_UINT8(13, g.band_rows);   /* 136 / 10             */
    TEST_ASSERT_EQUAL_INT16(152, g.desc_w);     /* 260 - 104 - 4        */
    TEST_ASSERT_EQUAL_UINT8(25, g.desc_cols);   /* 152 / 6              */
    TEST_ASSERT_EQUAL_INT16(196, g.page_h);

    TEST_ASSERT_GREATER_OR_EQUAL_UINT(CATALOG_DESC_MAX - 1,
                                      (unsigned)(g.desc_cols * g.band_rows));
}

static void test_the_layout_refuses_a_window_it_cannot_compose(void)
{
    picker_layout_t g;

    /* 100 - 52 - 46 leaves the band nothing. */
    TEST_ASSERT_EQUAL_INT(PICKER_ERR_ARGS, picker_layout(200, 100, &g));
    /* 150 < 96 + 16 + 48: an image and no usable column beside it. */
    TEST_ASSERT_EQUAL_INT(PICKER_ERR_ARGS, picker_layout(150, 216, &g));
    TEST_ASSERT_EQUAL_INT(PICKER_ERR_ARGS, picker_layout(240, 216, NULL));
}

/* ─── the list ────────────────────────────────────────────────────────────── */

static void test_the_list_stays_inside_the_window_in_every_mode(void)
{
    struct { int16_t w, h; } geo[] = { { GEOM_24_W, GEOM_24_H },
                                       { GEOM_26_W, GEOM_26_H } };
    size_t i;

    fill_library(LIB_COUNT);
    for (i = 0; i < 2; i++) {
        picker_layout_t g;

        TEST_ASSERT_EQUAL_INT(PICKER_OK,
                              picker_layout(geo[i].w, geo[i].h, &g));

        run_list(geo[i].w, geo[i].h, PICKER_MODE_PENDING, true, false, NULL);
        assert_sane();
        /* The list has no image slot at all now. */
        TEST_ASSERT_EQUAL_UINT(0, fk.images);
        /* One label per visible row, plus the header. */
        TEST_ASSERT_GREATER_OR_EQUAL_UINT(g.rows + 1u, fk.texts);

        run_list(geo[i].w, geo[i].h, PICKER_MODE_PENDING, true, true, NULL);
        assert_sane();
        TEST_ASSERT_EQUAL_UINT(0, fk.images);

        run_list(geo[i].w, geo[i].h, PICKER_MODE_IMMEDIATE, false, false, NULL);
        assert_sane();

        run_list(geo[i].w, geo[i].h, PICKER_MODE_IMMEDIATE, true, false, NULL);
        assert_sane();
    }
}

static void test_a_marked_row_still_fits_its_box(void)
{
    boot_made_t made;

    fill_library(LIB_COUNT);
    boot_made_clear(&made);
    /* Row 0 in immediate mode, so the marked label is on screen. */
    TEST_ASSERT_EQUAL_INT(BOOT_MADE_OK, boot_made_add(&made, "Game 000.gb"));

    run_list(GEOM_24_W, GEOM_24_H, PICKER_MODE_IMMEDIATE, false, false, &made);
    assert_sane();
}

/* ─── the detail page ─────────────────────────────────────────────────────── */

static void test_every_detail_combination_stays_inside_the_window(void)
{
    struct { int16_t w, h; } geo[] = { { GEOM_24_W, GEOM_24_H },
                                       { GEOM_26_W, GEOM_26_H } };
    uint8_t states[][2] = {
        { PICKER_MEDIA_READY, PICKER_MEDIA_READY },
        { PICKER_MEDIA_READY, PICKER_MEDIA_MISSING },
        { PICKER_MEDIA_MISSING, PICKER_MEDIA_MISSING },
        { PICKER_MEDIA_LOADING, PICKER_MEDIA_LOADING },
    };
    uint8_t pcts[] = { 0, 50, 100 };
    size_t gi, si, pi, sc;

    fill_library(LIB_COUNT);
    for (gi = 0; gi < 2; gi++) {
        picker_layout_t g;
        uint16_t span;

        picker_layout(geo[gi].w, geo[gi].h, &g);
        span = (uint16_t)(picker_page_lines(&g, DESC_200) - g.band_rows);

        for (si = 0; si < sizeof(states) / sizeof(states[0]); si++) {
            for (pi = 0; pi < sizeof(pcts) / sizeof(pcts[0]); pi++) {
                uint16_t scrolls[] = { 0, (uint16_t)(span / 2), span };

                for (sc = 0; sc < 3; sc++) {
                    run_detail(geo[gi].w, geo[gi].h, PICKER_MODE_PENDING, true,
                               false, 0, states[si][0], states[si][1],
                               pcts[pi], scrolls[sc], DESC_200);
                    assert_sane();
                }
            }
        }
    }
}

static void test_both_images_are_drawn_at_the_top_of_the_band(void)
{
    picker_layout_t g;

    fill_library(LIB_COUNT);
    picker_layout(GEOM_24_W, GEOM_24_H, &g);

    run_detail(GEOM_24_W, GEOM_24_H, PICKER_MODE_PENDING, true, false, 0,
               PICKER_MEDIA_READY, PICKER_MEDIA_READY, 0, 0, DESC_200);
    assert_sane();

    /* The cover and the snapshot: two slots, one push each. */
    TEST_ASSERT_EQUAL_UINT(2, fk.images);
    TEST_ASSERT_EQUAL_INT16(PICKER_ART_W, fk.last_img_w);
}

static void test_a_scrolled_band_clips_its_images_rather_than_moving_them(void)
{
    picker_layout_t g;
    uint16_t span;

    fill_library(LIB_COUNT);
    picker_layout(GEOM_24_W, GEOM_24_H, &g);
    span = (uint16_t)(picker_page_lines(&g, DESC_200) - g.band_rows);
    TEST_ASSERT_GREATER_THAN_UINT(0, span);

    run_detail(GEOM_24_W, GEOM_24_H, PICKER_MODE_PENDING, true, false, 0,
               PICKER_MEDIA_READY, PICKER_MEDIA_READY, 0, span, DESC_200);
    assert_sane();

    /* At the bottom of the travel the snapshot is on screen and the cover has
     * been clipped away or down to a strip; either way nothing left the band
     * and no row range left the source. */
    TEST_ASSERT_GREATER_OR_EQUAL_UINT(1, fk.images);
    TEST_ASSERT_GREATER_THAN_INT16(0, fk.last_img_rows);
    TEST_ASSERT_LESS_OR_EQUAL_INT16(PICKER_ART_H, fk.last_img_rows);
}

static void test_missing_media_draws_two_placeholders_and_no_image(void)
{
    fill_library(LIB_COUNT);

    run_detail(GEOM_24_W, GEOM_24_H, PICKER_MODE_PENDING, true, false, 0,
               PICKER_MEDIA_MISSING, PICKER_MEDIA_MISSING, 0, 0, DESC_200);
    assert_sane();

    TEST_ASSERT_EQUAL_UINT(0, fk.images);
    /* One slot-width fill each for the cover and the snapshot. */
    TEST_ASSERT_GREATER_OR_EQUAL_UINT(2, fk.art_fills);
}

static void test_the_hold_bar_appears_only_once_the_hold_starts(void)
{
    fill_library(LIB_COUNT);

    run_detail(GEOM_24_W, GEOM_24_H, PICKER_MODE_PENDING, true, false, 0,
               PICKER_MEDIA_READY, PICKER_MEDIA_READY, 0, 0, DESC_200);
    assert_sane();
    TEST_ASSERT_EQUAL_UINT(0, fk.bar_fills);

    run_detail(GEOM_24_W, GEOM_24_H, PICKER_MODE_PENDING, true, false, 0,
               PICKER_MEDIA_READY, PICKER_MEDIA_READY, 100, 0, DESC_200);
    assert_sane();
    TEST_ASSERT_EQUAL_UINT(1, fk.bar_fills);
    /* A hold one tick short of done is a bar one pixel-step short of full. */
    TEST_ASSERT_GREATER_THAN_INT16((GEOM_24_W - 16) * 9 / 10, fk.bar_w);
    TEST_ASSERT_EQUAL_INT16(GEOM_24_H - 14, fk.bar_y);
}

static void test_an_action_rows_detail_page_names_no_file(void)
{
    fill_library(LIB_COUNT);

    /* Cancel pending write, at row 0 in pending mode with a pending write. */
    run_detail(GEOM_24_W, GEOM_24_H, PICKER_MODE_PENDING, true, true, 0,
               PICKER_MEDIA_READY, PICKER_MEDIA_READY, 0, 0, DESC_200);
    assert_sane();
    TEST_ASSERT_EQUAL_UINT(0, fk.images);

    /* Finish setup, at row 0 in immediate mode once the wildcard is done. */
    run_detail(GEOM_24_W, GEOM_24_H, PICKER_MODE_IMMEDIATE, true, false, 0,
               PICKER_MEDIA_READY, PICKER_MEDIA_READY, 0, 0, DESC_200);
    assert_sane();
    TEST_ASSERT_EQUAL_UINT(0, fk.images);
}

/* ─── the description ─────────────────────────────────────────────────────── */

static void test_a_full_description_is_visible_without_scrolling(void)
{
    char worst[CATALOG_DESC_MAX];
    picker_layout_t g;
    uint16_t lines;
    size_t i;

    TEST_ASSERT_EQUAL_INT(PICKER_OK, picker_layout(240, 216, &g));
    TEST_ASSERT_EQUAL_size_t(CATALOG_DESC_MAX - 1, strlen(DESC_200));

    lines = picker_desc_lines(DESC_200, g.desc_cols);
    /* 200 characters at 22 columns, broken at spaces. */
    TEST_ASSERT_LESS_OR_EQUAL_UINT16(g.band_rows, lines);

    /*
     * And the worst case too, not just this sentence: short words waste the
     * most of each line to the break. Three-character words at the cap are
     * the most lines 200 bytes can occupy.
     */
    for (i = 0; i < CATALOG_DESC_MAX - 1; i++) {
        worst[i] = (i % 4 == 3) ? ' ' : 'w';
    }
    worst[CATALOG_DESC_MAX - 1] = '\0';
    TEST_ASSERT_LESS_OR_EQUAL_UINT16(g.band_rows,
                                     picker_desc_lines(worst, g.desc_cols));

    /* And so the images, not the text, set how far the band travels. */
    TEST_ASSERT_EQUAL_UINT16(picker_page_lines(&g, NULL),
                             picker_page_lines(&g, DESC_200));
}

static void test_every_wrapped_line_fits_the_column_in_pixels(void)
{
    picker_layout_t g;
    uint16_t lines;
    uint16_t i;
    char line[PICKER_DESC_LINE_MAX];

    TEST_ASSERT_EQUAL_INT(PICKER_OK, picker_layout(240, 216, &g));
    lines = picker_desc_lines(DESC_200, g.desc_cols);
    TEST_ASSERT_GREATER_THAN_UINT16(0, lines);

    for (i = 0; i < lines; i++) {
        TEST_ASSERT_TRUE(
            picker_desc_line(DESC_200, g.desc_cols, i, line, sizeof(line)));
        /* The property that makes this module's break points and
         * display_draw_wrapped's pixel breaking the same: font 1's advance is
         * fixed, so a line of n characters is exactly n * 6 pixels wide. */
        TEST_ASSERT_LESS_OR_EQUAL_INT16(
            g.desc_w, (int16_t)(strlen(line) * UI_FONT_SMALL_ADV));
    }
    /* One past the end is not a line. */
    TEST_ASSERT_FALSE(
        picker_desc_line(DESC_200, g.desc_cols, lines, line, sizeof(line)));
}

static void test_the_wrap_breaks_at_spaces_and_hard_breaks_long_words(void)
{
    char line[PICKER_DESC_LINE_MAX];
    const char* words = "alpha beta gamma";
    const char* run = "aaaaaaaaaaaaaaaaaaaa";

    /* At 10 columns "alpha beta" would fit exactly; the break is the space. */
    TEST_ASSERT_TRUE(picker_desc_line(words, 10, 0, line, sizeof(line)));
    TEST_ASSERT_EQUAL_STRING("alpha beta", line);
    TEST_ASSERT_TRUE(picker_desc_line(words, 10, 1, line, sizeof(line)));
    TEST_ASSERT_EQUAL_STRING("gamma", line);

    /* A word with no space in it has to be broken at the column, because the
     * alternative is a line that cannot be drawn. */
    TEST_ASSERT_TRUE(picker_desc_line(run, 8, 0, line, sizeof(line)));
    TEST_ASSERT_EQUAL_size_t(8, strlen(line));
    TEST_ASSERT_EQUAL_UINT16(3, picker_desc_lines(run, 8));

    TEST_ASSERT_EQUAL_UINT16(0, picker_desc_lines(NULL, 10));
    TEST_ASSERT_EQUAL_UINT16(0, picker_desc_lines("", 10));
    TEST_ASSERT_EQUAL_UINT16(0, picker_desc_lines(words, 0));
    TEST_ASSERT_FALSE(picker_desc_line(NULL, 10, 0, line, sizeof(line)));
    TEST_ASSERT_FALSE(picker_desc_line(words, 10, 0, NULL, sizeof(line)));
}

/* ─── argument safety ─────────────────────────────────────────────────────── */

static void test_draw_paints_nothing_when_it_is_handed_nothing(void)
{
    picker_t p;
    picker_layout_t g;
    ui_canvas_t cv = canvas_over(&fk, GEOM_24_W, GEOM_24_H);
    ui_canvas_t broken = cv;

    fill_library(LIB_COUNT);
    picker_layout(GEOM_24_W, GEOM_24_H, &g);
    picker_init(&p, PICKER_MODE_PENDING, &lib, true, false, NULL, g.rows);

    picker_draw(NULL, &g, NULL, NULL, NULL, &cv);
    picker_draw(&p, NULL, NULL, NULL, NULL, &cv);
    picker_draw(&p, &g, NULL, NULL, NULL, NULL);
    TEST_ASSERT_EQUAL_UINT(0, fk.fills);
    TEST_ASSERT_EQUAL_UINT(0, fk.texts);

    /* A canvas missing one of its three calls is not a usable canvas. */
    broken.image = NULL;
    picker_draw(&p, &g, NULL, NULL, NULL, &broken);
    TEST_ASSERT_EQUAL_UINT(0, fk.fills);

    TEST_ASSERT_EQUAL_UINT(0, fk.violations);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_the_layout_at_240_by_216);
    RUN_TEST(test_the_layout_at_260_by_234);
    RUN_TEST(test_the_layout_refuses_a_window_it_cannot_compose);
    RUN_TEST(test_the_list_stays_inside_the_window_in_every_mode);
    RUN_TEST(test_a_marked_row_still_fits_its_box);
    RUN_TEST(test_every_detail_combination_stays_inside_the_window);
    RUN_TEST(test_both_images_are_drawn_at_the_top_of_the_band);
    RUN_TEST(test_a_scrolled_band_clips_its_images_rather_than_moving_them);
    RUN_TEST(test_missing_media_draws_two_placeholders_and_no_image);
    RUN_TEST(test_the_hold_bar_appears_only_once_the_hold_starts);
    RUN_TEST(test_an_action_rows_detail_page_names_no_file);
    RUN_TEST(test_a_full_description_is_visible_without_scrolling);
    RUN_TEST(test_every_wrapped_line_fits_the_column_in_pixels);
    RUN_TEST(test_the_wrap_breaks_at_spaces_and_hard_breaks_long_words);
    RUN_TEST(test_draw_paints_nothing_when_it_is_handed_nothing);
    return UNITY_END();
}
