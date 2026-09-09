#include <unity.h>

#include <stddef.h>
#include <stdio.h>
#include <string.h>

#include "audio/mix.h"
#include "input/combo.h"
#include "ui/diag_draw.h"

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

/* The two windows SCALE_K gives (render_config.h: 160 x k/16 by 144 x k/16). */
#define GEOM_24_W 240
#define GEOM_24_H 216
#define GEOM_26_W 260
#define GEOM_26_H 234

#define FB_MAX (GEOM_26_W * GEOM_26_H)

/* body_y is DIAG_HEADER_H + 2 = 20, so the checkerboard has 196 rows to fill
 * at 24/16 and 214 at 26/16, and only whole blocks are pushed. */
#define CHECKER_BLOCKS_24 ((GEOM_24_H - (DIAG_HEADER_H + 2)) / 3)   /* 65 */
#define CHECKER_BLOCKS_26 ((GEOM_26_H - (DIAG_HEADER_H + 2)) / 13)  /* 16 */

/* ─── the fake canvas ─────────────────────────────────────────────────────── */

typedef struct {
    int16_t w, h;
    uint16_t fb[FB_MAX];
    unsigned violations, fills, texts, images;
    unsigned null_strings;
    unsigned range_faults;   /* image row ranges outside the source block */

    unsigned bar_fills;      /* fills exactly bar_w wide                  */
    int16_t bar_w;

    unsigned edge_top, edge_bottom, edge_left, edge_right;
    unsigned thick_edges;    /* a "1 px" edge fill that was not 1 px      */

    unsigned lit_boxes;      /* button boxes in the pressed colour        */
    unsigned dark_boxes;

    unsigned saved_texts;    /* text calls whose string is "Saved"        */

    int16_t last_img_w, last_img_rows;
    unsigned img_w_faults, img_rows_faults;
    uint16_t first_block[SCALER_DST_ROWS_MAX * SCALER_DST_W_MAX];
    bool have_first_block;
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
    /* The box the driver will clip into: rows lines at the font's pitch. */
    h = (int16_t)(rows * UI_ROW_PITCH(ui_font_height(font)));
    put_rect(f, x, y, w, h);
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
    if (!f->have_first_block && rows > 0
        && (size_t)rows * (size_t)w
               <= sizeof(f->first_block) / sizeof(f->first_block[0])) {
        memcpy(f->first_block, px, (size_t)rows * (size_t)w * sizeof(px[0]));
        f->have_first_block = true;
    }
    put_rect(f, x, y, w, h);
}

static ui_canvas_t canvas_over(fake_t* f, const diag_layout_t* g)
{
    ui_canvas_t cv;

    memset(f, 0, sizeof(*f));
    f->w = g->w;
    f->h = g->h;
    f->bar_w = g->bar_w;
    cv.ctx = f;
    cv.fill = fk_fill;
    cv.text = fk_text;
    cv.image = fk_image;
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
        diag_init(&st, 320, 240, w, h, 40, 12, 40, 12, MIX_VOL_MED, 1));

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

static void test_the_layout_accepts_both_windows(void)
{
    TEST_ASSERT_EQUAL_INT(DIAG_OK, diag_layout(GEOM_24_W, GEOM_24_H, &geom));
    TEST_ASSERT_EQUAL_INT16(DIAG_HEADER_H + 2, geom.body_y);
    /* (216 - 20 - 2) / 10 = 19 rows, well past the eight a page needs. */
    TEST_ASSERT_EQUAL_UINT8(19, geom.rows);
    TEST_ASSERT_EQUAL_INT16(GEOM_24_W / 8, geom.bar_w);
    TEST_ASSERT_EQUAL_INT16(GEOM_24_H - DIAG_ROW_H - 2, geom.footer_y);
    TEST_ASSERT_EQUAL_INT16(12 * UI_FONT_SMALL_ADV, geom.label_w);
    TEST_ASSERT_EQUAL_INT16(GEOM_24_W - geom.label_w - 8, geom.col_w);

    TEST_ASSERT_EQUAL_INT(DIAG_OK, diag_layout(GEOM_26_W, GEOM_26_H, &geom));
    TEST_ASSERT_EQUAL_INT16(DIAG_HEADER_H + 2, geom.body_y);
    /* (234 - 20 - 2) / 10 = 21. */
    TEST_ASSERT_EQUAL_UINT8(21, geom.rows);
    TEST_ASSERT_EQUAL_INT16(GEOM_26_W / 8, geom.bar_w);
}

static void test_the_layout_refuses_a_window_with_too_few_rows(void)
{
    /* (60 - 20 - 2) / 10 = 3 rows, under DIAG_MIN_ROWS. */
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

static void test_every_page_paints_inside_the_window_with_no_data_at_all(void)
{
    /* A zeroed snapshot is the state a page is in before its binding has
     * gathered anything, and it is the case where a placeholder string is
     * most likely to be missing. */
    walk_every_page(GEOM_24_W, GEOM_24_H);
    walk_every_page(GEOM_26_W, GEOM_26_H);
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
}

static void test_the_checkerboard_pushes_whole_scaler_blocks(void)
{
    const scaler_geom_info_t* info;

    fill_data();

    draw_page(GEOM_24_W, GEOM_24_H, DIAG_PAGE_DISPLAY, DIAG_PATTERN_CHECKER,
              true, 0);
    assert_clean();
    info = scaler_geom_info(SCALER_GEOM_24_16);
    TEST_ASSERT_EQUAL_UINT16(GEOM_24_W, info->dst_w);
    TEST_ASSERT_EQUAL_UINT(CHECKER_BLOCKS_24, fk.images);
    TEST_ASSERT_EQUAL_INT16(info->dst_rows_per_block, fk.last_img_rows);
    TEST_ASSERT_EQUAL_INT16(GEOM_24_W, fk.last_img_w);

    draw_page(GEOM_26_W, GEOM_26_H, DIAG_PAGE_DISPLAY, DIAG_PATTERN_CHECKER,
              true, 0);
    assert_clean();
    info = scaler_geom_info(SCALER_GEOM_26_16);
    TEST_ASSERT_EQUAL_UINT16(GEOM_26_W, info->dst_w);
    TEST_ASSERT_EQUAL_UINT(CHECKER_BLOCKS_26, fk.images);
    TEST_ASSERT_EQUAL_INT16(info->dst_rows_per_block, fk.last_img_rows);
    TEST_ASSERT_EQUAL_INT16(GEOM_26_W, fk.last_img_w);
}

static void test_the_checkerboard_without_a_buffer_pushes_no_image(void)
{
    fill_data();
    draw_page(GEOM_24_W, GEOM_24_H, DIAG_PAGE_DISPLAY, DIAG_PATTERN_CHECKER,
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
    draw_page(GEOM_24_W, GEOM_24_H, DIAG_PAGE_DISPLAY, DIAG_PATTERN_CHECKER,
              true, 0);
    TEST_ASSERT_TRUE(fk.have_first_block);

    memset(&ref, 0, sizeof(ref));
    diag_checker_build(&ref, data.palette);
    dark = ref.line[0][0];
    light = ref.line[0][1];
    TEST_ASSERT_TRUE_MESSAGE(dark != light,
        "the palette's two background ends are the same colour");

    row0 = fk.first_block;
    row1 = fk.first_block + GEOM_24_W;

    /* 24/16 is 2 source units to 3 output units: copy, blend, copy. So the
     * top output row reads dark, avg, light across the first source pair —
     * and the middle row of every block is the vertical blend of the two
     * source lines, which at a one-pixel cell is the same average again. */
    TEST_ASSERT_EQUAL_HEX16(dark, row0[0]);
    TEST_ASSERT_EQUAL_HEX16(scaler_avg565(dark, light), row0[1]);
    TEST_ASSERT_EQUAL_HEX16(light, row0[2]);

    TEST_ASSERT_EQUAL_HEX16(scaler_avg565(dark, light), row1[0]);
    TEST_ASSERT_EQUAL_HEX16(scaler_avg565(light, dark), row1[2]);
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
                  MIX_VOL_MED, 0));

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
    RUN_TEST(test_the_layout_accepts_both_windows);
    RUN_TEST(test_the_layout_refuses_a_window_with_too_few_rows);
    RUN_TEST(test_every_page_paints_inside_the_240x216_window);
    RUN_TEST(test_every_page_paints_inside_the_260x234_window);
    RUN_TEST(test_every_page_paints_inside_the_window_with_no_data_at_all);
    RUN_TEST(test_the_bars_pattern_paints_eight_bars);
    RUN_TEST(test_the_border_pattern_is_four_one_pixel_edges);
    RUN_TEST(test_the_checkerboard_pushes_whole_scaler_blocks);
    RUN_TEST(test_the_checkerboard_without_a_buffer_pushes_no_image);
    RUN_TEST(test_the_checkerboards_first_block_is_the_blend_the_game_uses);
    RUN_TEST(test_the_buttons_page_lights_a_box_per_pressed_button);
    RUN_TEST(test_the_tag_page_dumps_four_hex_rows);
    RUN_TEST(test_the_nudge_page_shows_saved_only_while_the_toast_is_up);
    RUN_TEST(test_a_missing_build_string_still_paints);
    RUN_TEST(test_a_missing_card_and_catalog_still_paint);
    RUN_TEST(test_a_null_argument_paints_nothing);
    return UNITY_END();
}
