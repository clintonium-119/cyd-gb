#include <unity.h>

#include <stddef.h>
#include <stdio.h>
#include <string.h>

#include "audio/mix.h"
#include "input/combo.h"
#include "ui/diag_draw.h"

/* A stored trim to init from; the same two numbers the state suite uses. */
#define TRIM_FPA 20
#define TRIM_RATIO 32

/* And the compile-time porch B restores, a THIRD pair — distinct from both the
 * stored value and the shipping constant, so a test that confused the two
 * fails here rather than on a bench. */
#define DEF_FPA 15
#define DEF_RATIO 8

/*
 * Framebuffer bounds suite — the exit criterion "every page renders on host in
 * a framebuffer test" made mechanical, the way the writer's suite did it.
 *
 * There is no font on the host, so there is nothing to golden-hash. What can
 * be proved is the property the criterion actually names: every primitive the
 * layout module emits lands inside the game window. A fake canvas paints a
 * buffer of exactly w x h and counts anything whose rectangle leaves it, and
 * the driver below walks all eight pages, all three patterns and both windows.
 *
 * Every expected number is the arithmetic from the layout module written out,
 * not a literal copied from a run.
 */

/* Three window sizes. Only 266 x 240 is a render geometry now — the other two
 * were, and stay here because diag_layout() takes any window and the small
 * ones are where a layout runs out of room first. */
#define GEOM_24_W 240
#define GEOM_24_H 216
#define GEOM_26_W 260
#define GEOM_26_H 234
#define GEOM_53_W 266
#define GEOM_53_H 240

/* 5/3 is both the widest and the tallest, so its window bounds the canvas. */
#define FB_MAX (GEOM_53_W * GEOM_53_H)

/* body_y is DIAG_HEADER_H + 2 = 20, so the checkerboard has 220 rows to fill,
 * and only whole blocks are pushed. */
#define CHECKER_BLOCKS_53 ((GEOM_53_H - (DIAG_HEADER_H + 2)) / 5)   /* 44 */

/* ─── the fake canvas ─────────────────────────────────────────────────────── */

typedef struct {
    int16_t w, h;
    uint16_t fb[FB_MAX];
    unsigned violations, fills, texts, images;
    unsigned round_fills;
    unsigned null_strings;

    /* The theme's chrome, found by where it lands. */
    int16_t help_y, foot_y;
    unsigned help_fills;     /* the help line's band cleared             */
    unsigned foot_fills;     /* the hint footer's band cleared           */
    unsigned help_texts;     /* text inside the help line's band         */
    unsigned header_bands;   /* a dark band behind the header            */
    unsigned header_rules;   /* a 1 px rule under the header             */
    unsigned body_overruns;  /* body text running into the help line     */
    unsigned help_too_wide;  /* a help line longer than its one row      */
    unsigned small_overflow; /* font-1 text longer than its box holds    */
    unsigned range_faults;   /* image row ranges outside the source block */

    unsigned bar_fills;      /* fills exactly bar_w wide                  */
    unsigned hold_rounds;    /* UI_BAR_H round fills in the help band     */
    int16_t hold_w[2];       /* the first two of them: track, then fill   */
    unsigned row_pills;      /* DIAG_ROW_H round fills above the help band */
    int16_t row_pill_y;      /* where the last of them landed             */
    int16_t bar_w;

    unsigned edge_top, edge_bottom, edge_left, edge_right;
    unsigned thick_edges;    /* a "1 px" edge fill that was not 1 px      */

    unsigned lit_boxes;      /* button boxes in the pressed colour        */
    unsigned dark_boxes;

    unsigned saved_texts;    /* text calls whose string is "Saved"        */
    char seen[48][40];       /* every string drawn, for the pages that
                              * report a number rather than a picture     */
    unsigned seen_n;

    int16_t last_img_w, last_img_rows;
    unsigned img_w_faults, img_rows_faults;
    uint16_t first_block[SCALER_DST_ROWS_MAX * SCALER_DST_W_MAX];
    bool have_first_block;
    uint16_t second_block[SCALER_DST_ROWS_MAX * SCALER_DST_W_MAX];
    bool have_second_block;
} fake_t;

static fake_t fk;

/* The three colours the layout uses that a test asserts on. */
#define COL_PRESSED 0x07E0
#define COL_ROW_BG  0x1082
#define COL_EDGE    0xFFFF

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
    if (x == 0 && w == f->w && y == f->help_y && h == UI_HELP_H) {
        f->help_fills++;
    }
    if (x == 0 && w == f->w && y == f->foot_y && h == UI_FOOT_H) {
        f->foot_fills++;
    }
    if (y == 0 && h == DIAG_HEADER_H && color == COL_ROW_BG) {
        f->header_bands++;
    }
    if (y == DIAG_HEADER_H && h == 1) {
        f->header_rules++;
    }
    if (w == f->bar_w && h > DIAG_ROW_H) {
        f->bar_fills++;
    }
    /*
     * The four window edges. Only the border draws in COL_EDGE, so the
     * header's own full-width band and the background clear — which have the
     * same rectangle as a top edge — are not mistaken for one.
     *
     * Each edge is counted when it lies along its side, and its thickness is
     * checked separately, so "exactly one pixel" is a check rather than an
     * assumption about the fill that was found.
     */
    if (color == COL_EDGE) {
        if (x == 0 && y == 0 && w == f->w) {
            f->edge_top++;
            if (h != 1) {
                f->thick_edges++;
            }
        }
        if (x == 0 && w == f->w && y == (int16_t)(f->h - h)) {
            f->edge_bottom++;
            if (h != 1) {
                f->thick_edges++;
            }
        }
        if (x == 0 && y == 0 && h == f->h) {
            f->edge_left++;
            if (w != 1) {
                f->thick_edges++;
            }
        }
        if (y == 0 && h == f->h && x == (int16_t)(f->w - w)) {
            f->edge_right++;
            if (w != 1) {
                f->thick_edges++;
            }
        }
    }
    if (w == 16 && h == 8) {
        if (color == COL_PRESSED) {
            f->lit_boxes++;
        } else if (color == COL_ROW_BG) {
            f->dark_boxes++;
        }
    }
    put_rect(f, x, y, w, h);
}

static void fk_round_fill(void* ctx, int16_t x, int16_t y, int16_t w,
                          int16_t h, int16_t r, uint16_t color, uint16_t bg)
{
    fake_t* f = (fake_t*)ctx;

    (void)r;
    (void)color;
    (void)bg;
    f->round_fills++;
    if (h == UI_BAR_H && y >= f->help_y && y + h <= f->help_y + UI_HELP_H) {
        if (f->hold_rounds < 2) {
            f->hold_w[f->hold_rounds] = w;
        }
        f->hold_rounds++;
    }
    if (h == DIAG_ROW_H && y + h <= f->help_y) {
        f->row_pills++;
        f->row_pill_y = y;
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
    if (strcmp(s, "Saved") == 0) {
        f->saved_texts++;
    }
    if (f->seen_n < sizeof f->seen / sizeof f->seen[0]) {
        strncpy(f->seen[f->seen_n], s, sizeof f->seen[0] - 1);
        f->seen[f->seen_n][sizeof f->seen[0] - 1] = '\0';
        f->seen_n++;
    }
    /* The box the driver will clip into: rows lines at the font's pitch, the
     * last without the gap under it. */
    h = (int16_t)(rows * UI_ROW_PITCH(ui_font_height(font)) - 2);
    /* Font 1's advance is fixed, so a string too long for its box is known
     * exactly; a box of several rows gets a column's slack per row for the
     * word it pushes down. */
    if (font == UI_FONT_SMALL &&
        strlen(s) * UI_FONT_SMALL_ADV >
            (size_t)(w * rows - (rows - 1) * 8 * UI_FONT_SMALL_ADV)) {
        f->small_overflow++;
    }
    if (y >= f->help_y && y + h <= f->help_y + UI_HELP_H) {
        f->help_texts++;
        if (strlen(s) * UI_FONT_SMALL_ADV > (size_t)(f->w - 2 * UI_PAD)) {
            f->help_too_wide++;
        }
    } else if (y < f->help_y && y + h > f->help_y) {
        f->body_overruns++;
    }
    put_rect(f, x, y, w, h);
}

/* A fixed advance: exact at font 1, a stand-in elsewhere. */
static int16_t fk_measure(void* ctx, const char* s, uint8_t font)
{
    (void)ctx;
    return (s == NULL) ? 0
                       : (int16_t)(strlen(s) * ((font == UI_FONT_SMALL)
                                                    ? UI_FONT_SMALL_ADV
                                                    : 8));
}

static void fk_image(void* ctx, int16_t x, int16_t y, int16_t w, int16_t h,
                     const uint16_t* px, int16_t row0, int16_t rows)
{
    fake_t* f = (fake_t*)ctx;

    f->images++;
    f->last_img_w = w;
    f->last_img_rows = rows;
    if (px == NULL) {
        f->null_strings++;
        return;
    }
    if (w != f->w) {
        f->img_w_faults++;
    }
    if (rows != h) {
        f->img_rows_faults++;
    }
    /* A row range naming rows the block does not have would read past the
     * buffer on the device, which the window check alone would not catch. */
    if (row0 < 0 || rows < 0 || row0 + rows > h) {
        f->range_faults++;
    }
    if (rows > 0
        && (size_t)rows * (size_t)w
               <= sizeof(f->first_block) / sizeof(f->first_block[0])) {
        /* The first two blocks, because a geometry with an odd
         * lines-per-block gives them opposite source-line parity and that is
         * a per-block property block 0 alone cannot show. */
        if (!f->have_first_block) {
            memcpy(f->first_block, px,
                   (size_t)rows * (size_t)w * sizeof(px[0]));
            f->have_first_block = true;
        } else if (!f->have_second_block) {
            memcpy(f->second_block, px,
                   (size_t)rows * (size_t)w * sizeof(px[0]));
            f->have_second_block = true;
        }
    }
    put_rect(f, x, y, w, h);
}

static ui_canvas_t canvas_over(fake_t* f, const diag_layout_t* g)
{
    ui_canvas_t cv;

    memset(&cv, 0, sizeof(cv));
    memset(f, 0, sizeof(*f));
    f->w = g->w;
    f->h = g->h;
    f->bar_w = g->bar_w;
    f->help_y = g->help_y;
    f->foot_y = g->foot_y;
    cv.ctx = f;
    cv.fill = fk_fill;
    cv.round_fill = fk_round_fill;
    cv.text = fk_text;
    cv.image = fk_image;
    cv.measure = fk_measure;
    return cv;
}

/* ─── fixtures ────────────────────────────────────────────────────────────── */

static diag_t st;
static diag_data_t data;
static diag_layout_t geom;
static diag_checker_t ck;

void setUp(void)
{
    memset(&st, 0, sizeof(st));
    memset(&data, 0, sizeof(data));
    memset(&ck, 0, sizeof(ck));
    memset(&geom, 0, sizeof(geom));
}

void tearDown(void)
{
}

/* A snapshot with something in every field, so no page draws an empty
 * placeholder where a real string would be longer. */
static void fill_data(void)
{
    size_t i;

    data.buttons = 0xFF;
    for (i = 0; i < 8; i++) {
        data.gpa[i] = (uint8_t)(7 - i);
    }
    data.sd_ok = true;
    data.rom_count = 132;
    data.catalog_count = 132;
    data.catalog_ok = true;
    data.sd_total_mb = 30512;
    data.sd_used_mb = 1704;
    data.sd_stats_ok = true;

    data.nfc_state = DIAG_NFC_ONE;
    data.nfc_fw = 0x32010607u;
    snprintf(data.uid_hex, sizeof(data.uid_hex), "04A1B2C3D4E5F6");
    data.version_ok = true;
    for (i = 0; i < 8; i++) {
        data.version[i] = (uint8_t)(0xC0 + i);
    }
    data.cfg_ok = true;
    data.auth0 = 0x04;
    data.access = 0x85;
    for (i = 0; i < NDEF_BUF_MAX; i++) {
        data.ndef_raw[i] = (uint8_t)i;
    }
    data.ndef_read_ok = true;
    /* 47 characters — one under the catalog's own title cap, so the two-row
     * box is the one being exercised. */
    snprintf(data.payload, sizeof(data.payload),
             "GAME:The Legend of Zelda Links Awakening.gb");
    data.cls = 3;

    data.bat_raw = 2210;
    data.bat_pin_mv = 1783;
    data.bat_cell_mv = 3566;
    data.bat_divider_x100 = 200;

    data.palette = 0;
    snprintf(data.fw_version, sizeof(data.fw_version), "v0.9.3-14-gdeadbee");
    snprintf(data.build_time, sizeof(data.build_time), "2026-09-09 20:14 UTC");
}

/* Put the machine on `page` and draw it into a fresh canvas. */
static void draw_page(int16_t w, int16_t h, uint8_t page, uint8_t pattern,
                      bool with_ck, uint32_t now_ms)
{
    ui_canvas_t cv;
    uint8_t i;

    TEST_ASSERT_EQUAL_INT(DIAG_OK, diag_layout(w, h, &geom));
    TEST_ASSERT_EQUAL_INT(DIAG_OK,
        diag_init(&st, 320, 240, w, h, 40, 12, 40, 12, MIX_VOL_MED, 1,
                  TRIM_FPA, TRIM_RATIO, DEF_FPA, DEF_RATIO));

    for (i = 0; i < page; i++) {
        diag_input(&st, COMBO_EVENT_BRIGHT_UP, COMBO_BTN_SELECT, 0);
    }
    TEST_ASSERT_EQUAL_UINT8(page, diag_page(&st));

    for (i = 0; i < pattern; i++) {
        diag_input(&st, COMBO_EVENT_NONE, 0, (uint32_t)(i * 10));
        diag_input(&st, COMBO_EVENT_NONE, COMBO_BTN_DOWN,
                   (uint32_t)(i * 10 + 5));
    }

    cv = canvas_over(&fk, &geom);
    diag_draw(&st, &data, &geom, with_ck ? &ck : NULL, now_ms, &cv);
}

static void assert_clean(void)
{
    TEST_ASSERT_EQUAL_UINT(0, fk.violations);
    TEST_ASSERT_EQUAL_UINT(0, fk.null_strings);
    TEST_ASSERT_EQUAL_UINT(0, fk.range_faults);
    TEST_ASSERT_EQUAL_UINT(0, fk.img_w_faults);
    TEST_ASSERT_EQUAL_UINT(0, fk.img_rows_faults);
    TEST_ASSERT_TRUE_MESSAGE(fk.fills + fk.texts + fk.images > 0,
        "the page drew nothing at all");
}

/* ─── geometry ────────────────────────────────────────────────────────────── */

static void test_the_layout_accepts_every_window(void)
{
    TEST_ASSERT_EQUAL_INT(DIAG_OK, diag_layout(GEOM_24_W, GEOM_24_H, &geom));
    TEST_ASSERT_EQUAL_INT16(DIAG_HEADER_H + 2, geom.body_y);
    /* (216 - 20 - 18 - 26) / 10 = 15 rows, well past the eight a page
     * needs. */
    TEST_ASSERT_EQUAL_UINT8(15, geom.rows);
    TEST_ASSERT_EQUAL_INT16(GEOM_24_W / 8, geom.bar_w);
    TEST_ASSERT_EQUAL_INT16(GEOM_24_H - UI_FOOT_H, geom.foot_y);
    TEST_ASSERT_EQUAL_INT16(GEOM_24_H - UI_FOOT_H - UI_HELP_H, geom.help_y);
    TEST_ASSERT_EQUAL_INT16(12 * UI_FONT_SMALL_ADV, geom.label_w);
    TEST_ASSERT_EQUAL_INT16(GEOM_24_W - geom.label_w - 2 * UI_TEXT_X,
                            geom.col_w);

    TEST_ASSERT_EQUAL_INT(DIAG_OK, diag_layout(GEOM_26_W, GEOM_26_H, &geom));
    TEST_ASSERT_EQUAL_INT16(DIAG_HEADER_H + 2, geom.body_y);
    /* (234 - 38 - 26) / 10 = 17. */
    TEST_ASSERT_EQUAL_UINT8(17, geom.rows);
    TEST_ASSERT_EQUAL_INT16(GEOM_26_W / 8, geom.bar_w);

    TEST_ASSERT_EQUAL_INT(DIAG_OK, diag_layout(GEOM_53_W, GEOM_53_H, &geom));
    TEST_ASSERT_EQUAL_INT16(DIAG_HEADER_H + 2, geom.body_y);
    /* (240 - 38 - 26) / 10 = 17, and the footer is inside the window. */
    TEST_ASSERT_EQUAL_UINT8(17, geom.rows);
    TEST_ASSERT_EQUAL_INT16(GEOM_53_W / 8, geom.bar_w);
    TEST_ASSERT_EQUAL_INT16(202, geom.help_y);
    TEST_ASSERT_EQUAL_INT16(220, geom.foot_y);
    TEST_ASSERT_TRUE(geom.foot_y + UI_FOOT_H <= GEOM_53_H);
}

static void test_the_layout_refuses_a_window_with_too_few_rows(void)
{
    /* 60 - 40 leaves less than a row under the 28 px header. */
    TEST_ASSERT_EQUAL_INT(DIAG_ERR_ARGS, diag_layout(100, 60, &geom));
    TEST_ASSERT_EQUAL_INT(DIAG_ERR_ARGS,
        diag_layout(GEOM_24_W, GEOM_24_H, NULL));
    TEST_ASSERT_EQUAL_INT(DIAG_ERR_ARGS, diag_layout(0, GEOM_24_H, &geom));
}

/* ─── every page, both windows ────────────────────────────────────────────── */

static void walk_every_page(int16_t w, int16_t h)
{
    uint8_t page;
    uint8_t pattern;

    for (page = 0; page < DIAG_PAGE_COUNT; page++) {
        for (pattern = 0; pattern < DIAG_PATTERN_COUNT; pattern++) {
            draw_page(w, h, page, pattern, true, 0);
            assert_clean();
        }
    }
}

static void test_every_page_paints_inside_the_240x216_window(void)
{
    fill_data();
    walk_every_page(GEOM_24_W, GEOM_24_H);
}

static void test_every_page_paints_inside_the_260x234_window(void)
{
    fill_data();
    walk_every_page(GEOM_26_W, GEOM_26_H);
}

static void test_every_page_paints_inside_the_266x240_window(void)
{
    fill_data();
    walk_every_page(GEOM_53_W, GEOM_53_H);
}

static void test_every_page_paints_inside_the_window_with_no_data_at_all(void)
{
    /* A zeroed snapshot is the state a page is in before its binding has
     * gathered anything, and it is the case where a placeholder string is
     * most likely to be missing. */
    walk_every_page(GEOM_24_W, GEOM_24_H);
    walk_every_page(GEOM_26_W, GEOM_26_H);
    walk_every_page(GEOM_53_W, GEOM_53_H);
}

/* ─── the display page's three patterns ───────────────────────────────────── */

static void test_the_bars_pattern_paints_eight_bars(void)
{
    fill_data();
    draw_page(GEOM_24_W, GEOM_24_H, DIAG_PAGE_DISPLAY, DIAG_PATTERN_BARS,
              true, 0);
    assert_clean();
    TEST_ASSERT_EQUAL_UINT(8, fk.bar_fills);
    /* 8 x 30 = 240 at this window, so the bars cover it exactly. */
    TEST_ASSERT_TRUE(8 * geom.bar_w <= geom.w);

    draw_page(GEOM_26_W, GEOM_26_H, DIAG_PAGE_DISPLAY, DIAG_PATTERN_BARS,
              true, 0);
    assert_clean();
    TEST_ASSERT_EQUAL_UINT(8, fk.bar_fills);
    /* 8 x 32 = 256, four pixels short of 260 — bars never overrun. */
    TEST_ASSERT_TRUE(8 * geom.bar_w <= geom.w);

    draw_page(GEOM_53_W, GEOM_53_H, DIAG_PAGE_DISPLAY, DIAG_PATTERN_BARS,
              true, 0);
    assert_clean();
    TEST_ASSERT_EQUAL_UINT(8, fk.bar_fills);
    /* 8 x 33 = 264, two pixels short of 266. */
    TEST_ASSERT_TRUE(8 * geom.bar_w <= geom.w);
}

static void test_the_border_pattern_is_four_one_pixel_edges(void)
{
    fill_data();
    draw_page(GEOM_24_W, GEOM_24_H, DIAG_PAGE_DISPLAY, DIAG_PATTERN_BORDER,
              true, 0);
    assert_clean();
    TEST_ASSERT_EQUAL_UINT(1, fk.edge_top);
    TEST_ASSERT_EQUAL_UINT(1, fk.edge_bottom);
    TEST_ASSERT_EQUAL_UINT(1, fk.edge_left);
    TEST_ASSERT_EQUAL_UINT(1, fk.edge_right);
    TEST_ASSERT_EQUAL_UINT(0, fk.thick_edges);

    draw_page(GEOM_26_W, GEOM_26_H, DIAG_PAGE_DISPLAY, DIAG_PATTERN_BORDER,
              true, 0);
    assert_clean();
    TEST_ASSERT_EQUAL_UINT(1, fk.edge_top);
    TEST_ASSERT_EQUAL_UINT(1, fk.edge_bottom);
    TEST_ASSERT_EQUAL_UINT(1, fk.edge_left);
    TEST_ASSERT_EQUAL_UINT(1, fk.edge_right);
    TEST_ASSERT_EQUAL_UINT(0, fk.thick_edges);

    draw_page(GEOM_53_W, GEOM_53_H, DIAG_PAGE_DISPLAY, DIAG_PATTERN_BORDER,
              true, 0);
    assert_clean();
    TEST_ASSERT_EQUAL_UINT(1, fk.edge_top);
    TEST_ASSERT_EQUAL_UINT(1, fk.edge_bottom);
    TEST_ASSERT_EQUAL_UINT(1, fk.edge_left);
    TEST_ASSERT_EQUAL_UINT(1, fk.edge_right);
    TEST_ASSERT_EQUAL_UINT(0, fk.thick_edges);
}

static void test_the_checkerboard_pushes_whole_scaler_blocks(void)
{
    const scaler_geom_info_t* info;

    fill_data();

    draw_page(GEOM_53_W, GEOM_53_H, DIAG_PAGE_DISPLAY, DIAG_PATTERN_CHECKER,
              true, 0);
    assert_clean();
    info = scaler_geom_info(SCALER_GEOM_5_3);
    TEST_ASSERT_EQUAL_UINT16(GEOM_53_W, info->dst_w);
    TEST_ASSERT_EQUAL_UINT(CHECKER_BLOCKS_53, fk.images);
    TEST_ASSERT_EQUAL_INT16(info->dst_rows_per_block, fk.last_img_rows);
    TEST_ASSERT_EQUAL_INT16(GEOM_53_W, fk.last_img_w);
}

static void test_the_checkerboard_without_a_buffer_pushes_no_image(void)
{
    fill_data();
    draw_page(GEOM_53_W, GEOM_53_H, DIAG_PAGE_DISPLAY, DIAG_PATTERN_CHECKER,
              false, 0);
    assert_clean();
    TEST_ASSERT_EQUAL_UINT(0, fk.images);
    /* It still says something, rather than leaving a blank page. */
    TEST_ASSERT_TRUE(fk.texts > 0);
}

static void test_the_checkerboards_first_block_is_the_blend_the_game_uses(void)
{
    diag_checker_t ref;
    uint16_t dark;
    uint16_t light;
    const uint16_t* row0;
    const uint16_t* row1;

    fill_data();
    draw_page(GEOM_53_W, GEOM_53_H, DIAG_PAGE_DISPLAY, DIAG_PATTERN_CHECKER,
              true, 0);
    TEST_ASSERT_TRUE(fk.have_first_block);

    memset(&ref, 0, sizeof(ref));
    diag_checker_build(&ref, data.palette);
    dark = ref.line[0][0];
    light = ref.line[0][1];
    TEST_ASSERT_TRUE_MESSAGE(dark != light,
        "the palette's two background ends are the same colour");

    row0 = fk.first_block;
    row1 = fk.first_block + GEOM_53_W;

    /* 5/3 is 3 source units to 5: copy, blend, copy, copy, blend. Across the
     * first group of a one-pixel chequer — dark, light, dark, then light
     * again — the top output row therefore reads dark, avg, light, dark,
     * avg. */
    TEST_ASSERT_EQUAL_HEX16(dark, row0[0]);
    TEST_ASSERT_EQUAL_HEX16(scaler_avg565(dark, light), row0[1]);
    TEST_ASSERT_EQUAL_HEX16(light, row0[2]);
    TEST_ASSERT_EQUAL_HEX16(dark, row0[3]);
    TEST_ASSERT_EQUAL_HEX16(scaler_avg565(dark, light), row0[4]);

    /* Row 1 is the vertical blend of the block's first two source lines,
     * which are opposite ends of the chequer, so at a one-pixel cell every
     * pixel of it is that same average — sharp and blended pixels alike. */
    TEST_ASSERT_EQUAL_HEX16(scaler_avg565(dark, light), row1[0]);
    TEST_ASSERT_EQUAL_HEX16(scaler_avg565(dark, light), row1[1]);
    TEST_ASSERT_EQUAL_HEX16(scaler_avg565(dark, light), row1[2]);
}

/*
 * The chequer alternates every source line, so a block's source lines are
 * phased on the block's own first line — not on line 0. With an even
 * lines-per-block every block starts even and the distinction is invisible,
 * which is why it survived the two k/16 geometries this one replaced; three
 * source lines is what showed it. Against the old code block 1 repeated
 * block 0's lines and every block's lookahead was line 0's parity.
 */
static void test_the_checkerboard_phases_source_lines_on_the_block_index(void)
{
    diag_checker_t ref;
    uint16_t dark;
    uint16_t light;
    const uint16_t* b0;
    const uint16_t* b1;

    fill_data();
    draw_page(GEOM_53_W, GEOM_53_H, DIAG_PAGE_DISPLAY, DIAG_PATTERN_CHECKER,
              true, 0);
    assert_clean();
    TEST_ASSERT_TRUE(fk.have_first_block);
    TEST_ASSERT_TRUE(fk.have_second_block);

    memset(&ref, 0, sizeof(ref));
    diag_checker_build(&ref, data.palette);
    dark = ref.line[0][0];
    light = ref.line[0][1];
    TEST_ASSERT_TRUE(dark != light);

    b0 = fk.first_block;
    b1 = fk.second_block;

    /* Block 0 starts on source line 0 and block 1 on source line 3, so their
     * top output rows are opposite ends of the chequer. */
    TEST_ASSERT_EQUAL_HEX16(dark, b0[0]);
    TEST_ASSERT_EQUAL_HEX16(light, b1[0]);

    /* pattern_5_3 leaves rows 1 and 4 as vertical blends, and row 4's
     * partner is the NEXT block's first line. At this geometry that is the
     * opposite parity to row 3's source, so row 4 is row 1 again — where the
     * old lookahead of line 0 made it a copy of row 3. */
    TEST_ASSERT_EQUAL_HEX16_ARRAY(b0 + GEOM_53_W, b0 + 4 * GEOM_53_W,
                                  GEOM_53_W);
    TEST_ASSERT_FALSE(b0[3 * GEOM_53_W] == b0[4 * GEOM_53_W]);
}

/* A width the geometry table does not claim has nothing to push, and the page
 * says so rather than drawing a wrong one. 240 and 260 are such widths now,
 * and this case has always used a third that was never anyone's. */
static void test_the_checkerboard_refuses_an_unclaimed_width(void)
{
    fill_data();
    draw_page(250, GEOM_26_H, DIAG_PAGE_DISPLAY, DIAG_PATTERN_CHECKER,
              true, 0);
    assert_clean();
    TEST_ASSERT_EQUAL_UINT(0, fk.images);
    TEST_ASSERT_TRUE(fk.texts > 0);
}

/* ─── the theme's chrome ─────────────────────────────────────────────────── */

static bool drew_text(const char* want);

static void test_every_page_has_a_help_line_and_a_hint_footer(void)
{
    uint8_t page;

    fill_data();
    for (page = 0; page < DIAG_PAGE_COUNT; page++) {
        draw_page(GEOM_53_W, GEOM_53_H, page, 0, true, 0);
        assert_clean();
        TEST_ASSERT_EQUAL_UINT_MESSAGE(1, fk.help_fills, "no help band");
        TEST_ASSERT_EQUAL_UINT_MESSAGE(1, fk.help_texts, "no help text");
        TEST_ASSERT_EQUAL_UINT_MESSAGE(1, fk.foot_fills, "no hint footer");
        /* The outer pill and at least one glyph. */
        TEST_ASSERT_GREATER_OR_EQUAL_UINT(2, fk.round_fills);
    }
}

static void test_every_help_line_fits_one_row(void)
{
    uint8_t page;

    fill_data();
    for (page = 0; page < DIAG_PAGE_COUNT; page++) {
        draw_page(GEOM_53_W, GEOM_53_H, page, 0, true, 0);
        TEST_ASSERT_EQUAL_UINT_MESSAGE(0, fk.help_too_wide,
                                       diag_page_title(page));
    }
}

static void test_the_pages_that_had_a_page_hint_still_name_it(void)
{
    uint8_t page;

    fill_data();
    for (page = 0; page < DIAG_PAGE_COUNT; page++) {
        draw_page(GEOM_53_W, GEOM_53_H, page, 0, true, 0);
        if (page == DIAG_PAGE_NUDGE || page == DIAG_PAGE_TRIM
            || page == DIAG_PAGE_SYSTEM) {
            continue;
        }
        TEST_ASSERT_TRUE_MESSAGE(drew_text("Sel+L/R"), diag_page_title(page));
        TEST_ASSERT_TRUE(drew_text("Page"));
    }
}

/* ─── the tag page's tools ────────────────────────────────────────────────── */

/* Put the machine on the tag page with `word` held from t = 100 to `now`. */
static void hold_on_tag_page(int16_t w, int16_t h, uint8_t word, uint32_t now)
{
    draw_page(w, h, DIAG_PAGE_NFC, 0, true, 0);
    diag_input(&st, COMBO_EVENT_NONE, 0, 50);
    diag_input(&st, COMBO_EVENT_NONE, word, 100);
    if (now > 100) {
        diag_input(&st, COMBO_EVENT_NONE, word, now);
    }
}

static void test_the_tag_page_draws_the_hold_bar_while_held(void)
{
    static const int16_t ws[3] = { GEOM_24_W, GEOM_26_W, GEOM_53_W };
    static const int16_t hs[3] = { GEOM_24_H, GEOM_26_H, GEOM_53_H };
    static const uint32_t at[3] = { 100, 600, 1099 };
    static const uint8_t pct[3] = { 0, 50, 99 };
    ui_canvas_t cv;
    uint8_t gi;
    uint8_t i;
    int16_t track;

    fill_data();
    for (gi = 0; gi < 3; gi++) {
        for (i = 0; i < 3; i++) {
            hold_on_tag_page(ws[gi], hs[gi], COMBO_BTN_B, at[i]);
            TEST_ASSERT_EQUAL_UINT8(pct[i], diag_hold_pct(&st));
            cv = canvas_over(&fk, &geom);
            diag_draw(&st, &data, &geom, &ck, 0, &cv);
            assert_clean();
            track = (int16_t)(geom.w - 2 * UI_PAD);
            /* The track, and the fill over it once there is any. */
            TEST_ASSERT_EQUAL_UINT(pct[i] > 0 ? 2 : 1, fk.hold_rounds);
            TEST_ASSERT_EQUAL_INT16(track, fk.hold_w[0]);
            if (pct[i] > 0) {
                TEST_ASSERT_EQUAL_INT16(track * pct[i] / 100, fk.hold_w[1]);
            }
            TEST_ASSERT_EQUAL_UINT(0, fk.help_texts);
        }
    }

    /* A full bar, painted on its own the way the binding does on the last
     * tick before the tool fires. */
    hold_on_tag_page(GEOM_24_W, GEOM_24_H, COMBO_BTN_B, 1099);
    st.hold_elapsed_ms = 1000;
    cv = canvas_over(&fk, &geom);
    diag_draw_hold(&st, &data, &geom, &cv, false);
    TEST_ASSERT_EQUAL_UINT(0, fk.violations);
    TEST_ASSERT_EQUAL_INT16(geom.w - 2 * UI_PAD, fk.hold_w[1]);

    /* No hold, no bar. */
    draw_page(GEOM_53_W, GEOM_53_H, DIAG_PAGE_NFC, 0, true, 0);
    TEST_ASSERT_EQUAL_UINT(0, fk.hold_rounds);
    TEST_ASSERT_EQUAL_UINT(1, fk.help_texts);
}

static void test_the_hold_bar_grows_without_relaying_the_track(void)
{
    ui_canvas_t cv;

    fill_data();
    hold_on_tag_page(GEOM_53_W, GEOM_53_H, COMBO_BTN_SELECT | COMBO_BTN_A,
                     600);
    cv = canvas_over(&fk, &geom);
    diag_draw_hold(&st, &data, &geom, &cv, true);
    /* The fill alone: no band clear and no track. */
    TEST_ASSERT_EQUAL_UINT(1, fk.hold_rounds);
    TEST_ASSERT_EQUAL_INT16((geom.w - 2 * UI_PAD) / 2, fk.hold_w[0]);
    TEST_ASSERT_EQUAL_UINT(0, fk.help_fills);

    /* Letting go clears the band and puts the help line back. */
    diag_input(&st, COMBO_EVENT_NONE, COMBO_BTN_SELECT, 700);
    cv = canvas_over(&fk, &geom);
    diag_draw_hold(&st, &data, &geom, &cv, true);
    TEST_ASSERT_EQUAL_UINT(0, fk.hold_rounds);
    TEST_ASSERT_EQUAL_UINT(1, fk.help_fills);
    TEST_ASSERT_EQUAL_UINT(1, fk.help_texts);
}

static void test_each_outcome_draws_its_line(void)
{
    static const char* const want[] = {
        NULL, "Blanked", "Wildcard made: Tetris", "MENU cart made",
        "Refused: not our tag", "No tag", "Two tags", "Write failed code -5",
        "Game cart made: Tetris",
    };
    uint8_t out;
    size_t i;

    fill_data();
    snprintf(data.nfc_outcome_title, sizeof(data.nfc_outcome_title),
             "Tetris");
    data.nfc_outcome_rc = -5;
    for (out = DIAG_NFC_OUT_BLANKED; out <= DIAG_NFC_OUT_GAME_MADE; out++) {
        data.nfc_outcome = out;
        draw_page(GEOM_53_W, GEOM_53_H, DIAG_PAGE_NFC, 0, true, 0);
        assert_clean();
        TEST_ASSERT_EQUAL_UINT(1, fk.help_texts);
        TEST_ASSERT_TRUE_MESSAGE(drew_text(want[out]), want[out]);
    }

    /* A 47-character title is cut inside the band at every window. */
    data.nfc_outcome = DIAG_NFC_OUT_WILD_MADE;
    memset(data.nfc_outcome_title, 0, sizeof(data.nfc_outcome_title));
    for (i = 0; i < sizeof(data.nfc_outcome_title) - 1; i++) {
        data.nfc_outcome_title[i] = (char)('a' + i % 26);
    }
    draw_page(GEOM_24_W, GEOM_24_H, DIAG_PAGE_NFC, 0, true, 0);
    assert_clean();
    TEST_ASSERT_EQUAL_UINT(1, fk.help_texts);
    TEST_ASSERT_EQUAL_UINT(0, fk.help_too_wide);
    draw_page(GEOM_26_W, GEOM_26_H, DIAG_PAGE_NFC, 0, true, 0);
    assert_clean();
    TEST_ASSERT_EQUAL_UINT(0, fk.help_too_wide);
    draw_page(GEOM_53_W, GEOM_53_H, DIAG_PAGE_NFC, 0, true, 0);
    assert_clean();
    TEST_ASSERT_EQUAL_UINT(0, fk.help_too_wide);

    /* No outcome is the page's own help line. */
    data.nfc_outcome = DIAG_NFC_OUT_NONE;
    draw_page(GEOM_53_W, GEOM_53_H, DIAG_PAGE_NFC, 0, true, 0);
    TEST_ASSERT_TRUE(drew_text("Sel+A MENU, Sel+B Wild, Start Game"));
}

static void test_the_tag_page_names_its_tools(void)
{
    static const int16_t ws[3] = { GEOM_24_W, GEOM_26_W, GEOM_53_W };
    static const int16_t hs[3] = { GEOM_24_H, GEOM_26_H, GEOM_53_H };
    uint8_t gi;

    fill_data();
    for (gi = 0; gi < 3; gi++) {
        draw_page(ws[gi], hs[gi], DIAG_PAGE_NFC, 0, true, 0);
        assert_clean();
        /* All three footer hints fit: none dropped from the end. */
        TEST_ASSERT_TRUE(drew_text("Scan"));
        TEST_ASSERT_TRUE(drew_text("Blank"));
        TEST_ASSERT_TRUE(drew_text("Page"));
        TEST_ASSERT_TRUE(drew_text("Sel+A MENU, Sel+B Wild, Start Game"));
        TEST_ASSERT_EQUAL_UINT(0, fk.help_too_wide);
    }
}

static void test_the_header_has_no_band_and_no_rule(void)
{
    uint8_t page;

    fill_data();
    for (page = 0; page < DIAG_PAGE_COUNT; page++) {
        draw_page(GEOM_53_W, GEOM_53_H, page, 0, true, 0);
        TEST_ASSERT_EQUAL_UINT(0, fk.header_bands);
        TEST_ASSERT_EQUAL_UINT(0, fk.header_rules);
    }
}

static void test_every_page_starts_its_rows_at_the_title_and_fits_them(void)
{
    uint8_t page;
    uint8_t pattern;

    fill_data();
    for (page = 0; page < DIAG_PAGE_COUNT; page++) {
        for (pattern = 0; pattern < DIAG_PATTERN_COUNT; pattern++) {
            draw_page(GEOM_53_W, GEOM_53_H, page, pattern, true, 0);
            TEST_ASSERT_EQUAL_UINT_MESSAGE(0, fk.small_overflow,
                                           diag_page_title(page));
        }
    }
}

static void test_no_page_runs_into_the_help_line(void)
{
    uint8_t page;

    fill_data();
    for (page = 0; page < DIAG_PAGE_COUNT; page++) {
        draw_page(GEOM_24_W, GEOM_24_H, page, 0, true, 0);
        TEST_ASSERT_EQUAL_UINT_MESSAGE(0, fk.body_overruns,
                                       diag_page_title(page));
        draw_page(GEOM_53_W, GEOM_53_H, page, 0, true, 0);
        TEST_ASSERT_EQUAL_UINT_MESSAGE(0, fk.body_overruns,
                                       diag_page_title(page));
    }
}

/* ─── individual pages ────────────────────────────────────────────────────── */

static void test_the_buttons_page_lights_a_box_per_pressed_button(void)
{
    fill_data();

    data.buttons = 0xFF;
    draw_page(GEOM_24_W, GEOM_24_H, DIAG_PAGE_BUTTONS, 0, true, 0);
    assert_clean();
    TEST_ASSERT_EQUAL_UINT(8, fk.lit_boxes);
    TEST_ASSERT_EQUAL_UINT(0, fk.dark_boxes);

    data.buttons = 0x00;
    draw_page(GEOM_24_W, GEOM_24_H, DIAG_PAGE_BUTTONS, 0, true, 0);
    assert_clean();
    TEST_ASSERT_EQUAL_UINT(0, fk.lit_boxes);
    TEST_ASSERT_EQUAL_UINT(8, fk.dark_boxes);

    /* One button down is one box lit, whichever it is. */
    data.buttons = (uint8_t)COMBO_BTN_START;
    draw_page(GEOM_24_W, GEOM_24_H, DIAG_PAGE_BUTTONS, 0, true, 0);
    assert_clean();
    TEST_ASSERT_EQUAL_UINT(1, fk.lit_boxes);
    TEST_ASSERT_EQUAL_UINT(7, fk.dark_boxes);
}

static void test_the_tag_page_dumps_four_hex_rows(void)
{
    unsigned with_dump;
    unsigned without_dump;

    fill_data();
    draw_page(GEOM_24_W, GEOM_24_H, DIAG_PAGE_NFC, 0, true, 0);
    assert_clean();
    with_dump = fk.texts;

    /* Every row of the dump is one text call, so removing the four of them is
     * the difference between a page with a dump and one without. The other
     * rows are the same either way. */
    without_dump = with_dump - 4u;
    TEST_ASSERT_TRUE_MESSAGE(without_dump > 0, "the tag page drew no rows");

    /* An unreadable tag still paints inside the window and still says why. */
    data.ndef_read_ok = false;
    data.ndef_rc = -3;
    data.cfg_ok = false;
    data.version_ok = false;
    data.nfc_fw = 0;
    data.nfc_state = DIAG_NFC_NONE;
    data.uid_hex[0] = '\0';
    draw_page(GEOM_24_W, GEOM_24_H, DIAG_PAGE_NFC, 0, true, 0);
    assert_clean();
}

static void test_the_nudge_page_shows_saved_only_while_the_toast_is_up(void)
{
    fill_data();

    draw_page(GEOM_24_W, GEOM_24_H, DIAG_PAGE_NUDGE, 0, true, 0);
    assert_clean();
    TEST_ASSERT_EQUAL_UINT(0, fk.saved_texts);

    /* Commit the nudge, then draw inside and outside the toast's window. */
    diag_input(&st, COMBO_EVENT_NONE, COMBO_BTN_A, 1000);
    {
        ui_canvas_t cv = canvas_over(&fk, &geom);
        diag_draw(&st, &data, &geom, &ck, 1000, &cv);
    }
    assert_clean();
    TEST_ASSERT_EQUAL_UINT(1, fk.saved_texts);

    {
        ui_canvas_t cv = canvas_over(&fk, &geom);
        diag_draw(&st, &data, &geom, &ck, 1000 + DIAG_TOAST_MS, &cv);
    }
    assert_clean();
    TEST_ASSERT_EQUAL_UINT(0, fk.saved_texts);
}


/* Did any text box on the page carry this string? */
static bool drew_text(const char* want)
{
    unsigned i;

    for (i = 0; i < fk.seen_n; i++) {
        if (strcmp(fk.seen[i], want) == 0) {
            return true;
        }
    }
    return false;
}

static void test_the_trim_page_shows_the_porch_it_would_store(void)
{
    fill_data();
    draw_page(GEOM_24_W, GEOM_24_H, DIAG_PAGE_TRIM, 0, true, 0);
    assert_clean();

    /* The two numbers a builder writes on the unit, in the form the
     * procedure asks for them. */
    TEST_ASSERT_TRUE(drew_text("Porch"));
    TEST_ASSERT_TRUE(drew_text("20 + 32/64"));
    TEST_ASSERT_TRUE(drew_text("15 + 8/64"));
    TEST_ASSERT_TRUE(drew_text("Stored"));
    /* And nothing measured yet, which has to read as absent rather than as a
     * zero-length crossing. */
    TEST_ASSERT_TRUE(drew_text("not counted yet"));
}

static void test_the_trim_page_follows_the_porch_as_it_is_stepped(void)
{
    ui_canvas_t cv;

    fill_data();
    draw_page(GEOM_24_W, GEOM_24_H, DIAG_PAGE_TRIM, 0, true, 0);

    diag_input(&st, COMBO_EVENT_NONE, COMBO_BTN_RIGHT, 100);

    memset(&fk, 0, sizeof(fk));
    fk.w = geom.w;
    fk.h = geom.h;
    cv = canvas_over(&fk, &geom);
    diag_draw(&st, &data, &geom, &ck, 100, &cv);
    assert_clean();
    TEST_ASSERT_TRUE(drew_text("21 + 32/64"));
    /* Moved and not yet stored, so the page says what a power cycle brings
     * back and how to change that. */
    TEST_ASSERT_TRUE(drew_text("20 + 32/64   A to save"));
    /* The Default row is the compile-time porch, not the stored one, because
     * that is where B leads — and after a correction it is the only value a
     * builder can get back to. */
    TEST_ASSERT_TRUE(drew_text("15 + 8/64"));
}

static void test_the_trim_page_shows_saved_only_while_the_toast_is_up(void)
{
    fill_data();
    draw_page(GEOM_24_W, GEOM_24_H, DIAG_PAGE_TRIM, 0, true, 0);
    assert_clean();
    TEST_ASSERT_EQUAL_UINT(0, fk.saved_texts);

    diag_input(&st, COMBO_EVENT_NONE, COMBO_BTN_A, 1000);
    {
        ui_canvas_t cv = canvas_over(&fk, &geom);
        diag_draw(&st, &data, &geom, &ck, 1000, &cv);
    }
    assert_clean();
    TEST_ASSERT_EQUAL_UINT(1, fk.saved_texts);

    {
        ui_canvas_t cv = canvas_over(&fk, &geom);
        diag_draw(&st, &data, &geom, &ck, 1000 + DIAG_TOAST_MS, &cv);
    }
    assert_clean();
    TEST_ASSERT_EQUAL_UINT(0, fk.saved_texts);
}

/* Where `want` was drawn in the order of text calls, or -1. */
static int seen_at(const char* want)
{
    unsigned i;

    for (i = 0; i < fk.seen_n; i++) {
        if (strcmp(fk.seen[i], want) == 0) {
            return (int)i;
        }
    }
    return -1;
}

/* The System page with the cursor moved `row` rows down from the top. */
static void draw_system(int16_t w, int16_t h, uint8_t row)
{
    draw_page(w, h, DIAG_PAGE_SYSTEM, row, true, 0);
    TEST_ASSERT_EQUAL_UINT8(row, diag_sys_row(&st));
}

static void redraw(void)
{
    ui_canvas_t cv = canvas_over(&fk, &geom);
    diag_draw(&st, &data, &geom, &ck, 1000, &cv);
}

static void test_the_system_page_draws_its_rows_in_order(void)
{
    static const char* const labels[6] = {
        "Frameskip", "Games list", "Boot logo", "Restart setup", "Version",
        "Built",
    };
    uint8_t i;

    fill_data();
    draw_system(GEOM_24_W, GEOM_24_H, 0);
    assert_clean();
    for (i = 0; i < 6; i++) {
        TEST_ASSERT_TRUE_MESSAGE(seen_at(labels[i]) >= 0, labels[i]);
        if (i > 0) {
            TEST_ASSERT_TRUE_MESSAGE(seen_at(labels[i - 1]) < seen_at(labels[i]),
                                     labels[i]);
        }
    }
}

static void test_only_the_selected_system_row_is_highlighted(void)
{
    uint8_t row;

    fill_data();
    for (row = 0; row < DIAG_SYS_ROW_COUNT; row++) {
        draw_system(GEOM_53_W, GEOM_53_H, row);
        assert_clean();
        TEST_ASSERT_EQUAL_UINT(1, fk.row_pills);
        TEST_ASSERT_EQUAL_INT16(geom.body_y + row * DIAG_ROW_H,
                                fk.row_pill_y);
    }
}

static void test_the_system_page_draws_both_switches(void)
{
    fill_data();
    draw_system(GEOM_24_W, GEOM_24_H, 0);
    TEST_ASSERT_TRUE(drew_text("Off"));
    TEST_ASSERT_TRUE(drew_text("On"));
    /* Games list first, off; boot logo after it, on. */
    TEST_ASSERT_TRUE(seen_at("Off") < seen_at("Boot logo"));
    TEST_ASSERT_TRUE(seen_at("On") > seen_at("Boot logo"));

    diag_set_list_mode(&st, true);
    diag_set_boot_logo(&st, false);
    redraw();
    assert_clean();
    TEST_ASSERT_TRUE(seen_at("On") < seen_at("Boot logo"));
    TEST_ASSERT_TRUE(seen_at("Off") > seen_at("Boot logo"));

    diag_set_list_mode(&st, false);
    redraw();
    TEST_ASSERT_TRUE(drew_text("Off"));
    TEST_ASSERT_FALSE(drew_text("On"));
}

static void test_a_restart_hold_draws_the_hold_bar(void)
{
    static const int16_t ws[3] = { GEOM_24_W, GEOM_26_W, GEOM_53_W };
    static const int16_t hs[3] = { GEOM_24_H, GEOM_26_H, GEOM_53_H };
    uint8_t gi;

    fill_data();
    for (gi = 0; gi < 3; gi++) {
        draw_system(ws[gi], hs[gi], DIAG_SYS_SETUP);
        TEST_ASSERT_EQUAL_UINT(0, fk.hold_rounds);
        TEST_ASSERT_EQUAL_UINT(1, fk.help_texts);

        diag_input(&st, COMBO_EVENT_NONE, 0, 50);
        diag_input(&st, COMBO_EVENT_NONE, COMBO_BTN_A, 100);
        diag_input(&st, COMBO_EVENT_NONE, COMBO_BTN_A, 600);
        TEST_ASSERT_EQUAL_UINT8(DIAG_TOOL_SETUP, diag_hold_tool(&st));
        redraw();
        assert_clean();
        TEST_ASSERT_EQUAL_UINT(2, fk.hold_rounds);
        TEST_ASSERT_EQUAL_INT16(geom.w - 2 * UI_PAD, fk.hold_w[0]);
        TEST_ASSERT_EQUAL_UINT(0, fk.help_texts);

        /* Letting go puts the page's help line back. */
        diag_input(&st, COMBO_EVENT_NONE, 0, 700);
        redraw();
        TEST_ASSERT_EQUAL_UINT(0, fk.hold_rounds);
        TEST_ASSERT_EQUAL_UINT(1, fk.help_texts);
        TEST_ASSERT_FALSE(drew_text("Setup restarts at next power-on"));
    }
}

static void test_a_completed_restart_stays_on_the_page(void)
{
    static const int16_t ws[3] = { GEOM_24_W, GEOM_26_W, GEOM_53_W };
    static const int16_t hs[3] = { GEOM_24_H, GEOM_26_H, GEOM_53_H };
    uint8_t gi;

    fill_data();
    for (gi = 0; gi < 3; gi++) {
        draw_system(ws[gi], hs[gi], DIAG_SYS_SETUP);
        diag_input(&st, COMBO_EVENT_NONE, 0, 50);
        diag_input(&st, COMBO_EVENT_NONE, COMBO_BTN_A, 100);
        diag_input(&st, COMBO_EVENT_NONE, COMBO_BTN_A, 1100);
        diag_input(&st, COMBO_EVENT_NONE, 0, 1200);
        diag_tick(&st, 60000);
        redraw();
        assert_clean();
        TEST_ASSERT_EQUAL_UINT(0, fk.body_overruns);
        TEST_ASSERT_TRUE(drew_text("Setup restarts at next power-on"));
    }
}

static void test_the_system_footer_names_its_buttons(void)
{
    static const int16_t ws[3] = { GEOM_24_W, GEOM_26_W, GEOM_53_W };
    static const int16_t hs[3] = { GEOM_24_H, GEOM_26_H, GEOM_53_H };
    uint8_t gi;

    fill_data();
    for (gi = 0; gi < 3; gi++) {
        draw_system(ws[gi], hs[gi], 0);
        assert_clean();
        TEST_ASSERT_TRUE(drew_text("U/D"));
        TEST_ASSERT_TRUE(drew_text("Row"));
        TEST_ASSERT_TRUE(drew_text("L/R"));
        TEST_ASSERT_TRUE(drew_text("Change"));
        TEST_ASSERT_TRUE(drew_text("Select"));
        TEST_ASSERT_TRUE(drew_text(
            "Frameskip, list, logo, setup, build"));
    }
}

static void test_a_missing_build_string_still_paints(void)
{
    fill_data();
    data.fw_version[0] = '\0';
    data.build_time[0] = '\0';
    draw_page(GEOM_24_W, GEOM_24_H, DIAG_PAGE_SYSTEM, 0, true, 0);
    assert_clean();
    TEST_ASSERT_EQUAL_UINT(0, fk.null_strings);
}

static void test_a_missing_card_and_catalog_still_paint(void)
{
    fill_data();
    data.sd_ok = false;
    data.catalog_ok = false;
    data.sd_stats_ok = false;
    data.rom_count = 0;
    draw_page(GEOM_24_W, GEOM_24_H, DIAG_PAGE_SD, 0, true, 0);
    assert_clean();
}

/* ─── NULL handling ───────────────────────────────────────────────────────── */

static void test_a_null_argument_paints_nothing(void)
{
    ui_canvas_t cv;

    fill_data();
    TEST_ASSERT_EQUAL_INT(DIAG_OK, diag_layout(GEOM_24_W, GEOM_24_H, &geom));
    TEST_ASSERT_EQUAL_INT(DIAG_OK,
        diag_init(&st, 320, 240, GEOM_24_W, GEOM_24_H, 40, 12, 40, 12,
                  MIX_VOL_MED, 0, TRIM_FPA, TRIM_RATIO, DEF_FPA, DEF_RATIO));

    cv = canvas_over(&fk, &geom);
    diag_draw(NULL, &data, &geom, &ck, 0, &cv);
    diag_draw(&st, NULL, &geom, &ck, 0, &cv);
    diag_draw(&st, &data, NULL, &ck, 0, &cv);
    diag_draw(&st, &data, &geom, &ck, 0, NULL);

    TEST_ASSERT_EQUAL_UINT(0, fk.fills);
    TEST_ASSERT_EQUAL_UINT(0, fk.texts);
    TEST_ASSERT_EQUAL_UINT(0, fk.images);

    /* And a build with a NULL checkerboard state does nothing rather than
     * writing through it. */
    diag_checker_build(NULL, 0);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_the_layout_accepts_every_window);
    RUN_TEST(test_the_layout_refuses_a_window_with_too_few_rows);
    RUN_TEST(test_every_page_paints_inside_the_240x216_window);
    RUN_TEST(test_every_page_paints_inside_the_260x234_window);
    RUN_TEST(test_every_page_paints_inside_the_266x240_window);
    RUN_TEST(test_every_page_paints_inside_the_window_with_no_data_at_all);
    RUN_TEST(test_the_bars_pattern_paints_eight_bars);
    RUN_TEST(test_the_border_pattern_is_four_one_pixel_edges);
    RUN_TEST(test_the_checkerboard_pushes_whole_scaler_blocks);
    RUN_TEST(test_the_checkerboard_without_a_buffer_pushes_no_image);
    RUN_TEST(test_the_checkerboards_first_block_is_the_blend_the_game_uses);
    RUN_TEST(test_the_checkerboard_phases_source_lines_on_the_block_index);
    RUN_TEST(test_the_checkerboard_refuses_an_unclaimed_width);
    RUN_TEST(test_the_buttons_page_lights_a_box_per_pressed_button);
    RUN_TEST(test_the_tag_page_dumps_four_hex_rows);
    RUN_TEST(test_the_nudge_page_shows_saved_only_while_the_toast_is_up);
    RUN_TEST(test_the_trim_page_shows_the_porch_it_would_store);
    RUN_TEST(test_the_trim_page_follows_the_porch_as_it_is_stepped);
    RUN_TEST(test_the_trim_page_shows_saved_only_while_the_toast_is_up);
    RUN_TEST(test_the_system_page_draws_its_rows_in_order);
    RUN_TEST(test_only_the_selected_system_row_is_highlighted);
    RUN_TEST(test_the_system_page_draws_both_switches);
    RUN_TEST(test_a_restart_hold_draws_the_hold_bar);
    RUN_TEST(test_a_completed_restart_stays_on_the_page);
    RUN_TEST(test_the_system_footer_names_its_buttons);
    RUN_TEST(test_a_missing_build_string_still_paints);
    RUN_TEST(test_a_missing_card_and_catalog_still_paint);
    RUN_TEST(test_a_null_argument_paints_nothing);
    RUN_TEST(test_every_page_has_a_help_line_and_a_hint_footer);
    RUN_TEST(test_every_help_line_fits_one_row);
    RUN_TEST(test_the_pages_that_had_a_page_hint_still_name_it);
    RUN_TEST(test_the_tag_page_draws_the_hold_bar_while_held);
    RUN_TEST(test_the_hold_bar_grows_without_relaying_the_track);
    RUN_TEST(test_each_outcome_draws_its_line);
    RUN_TEST(test_the_tag_page_names_its_tools);
    RUN_TEST(test_the_header_has_no_band_and_no_rule);
    RUN_TEST(test_no_page_runs_into_the_help_line);
    RUN_TEST(test_every_page_starts_its_rows_at_the_title_and_fits_them);
    return UNITY_END();
}
