#include <unity.h>

#include <stddef.h>
#include <stdint.h>
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
 * the driver below walks every screen the writer has at each render-geometry
 * window.
 *
 * The detail page scrolls, so a screen is not one picture but one per scroll
 * offset — and an image clipped at the band's edge is drawn as a row range,
 * which is the case most likely to walk off either the window or the source
 * buffer. Both are checked.
 *
 * Every expected number is the arithmetic from the layout module written out,
 * not a literal copied from a run.
 */

/* The window render_config.h's one legal geometry gives. The 4/3-ish 240x216
 * and 260x234 windows were retired with their geometries, and the split list
 * and Cart-Info detail page are laid out for this one. */
#define GEOM_53_W 266
#define GEOM_53_H 240

#define FB_MAX (GEOM_53_W * GEOM_53_H)

/* The catalog shape: the library's 132 titles, every tenth one a starter. */
#define LIB_COUNT 132
#define LIB_STARTER_EVERY 10
#define LIB_STARTERS 14

#define B_NONE  0x00u
#define B_A     ((uint8_t)COMBO_BTN_A)
#define B_B     ((uint8_t)COMBO_BTN_B)
#define B_UP    ((uint8_t)COMBO_BTN_UP)
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
    unsigned round_fills;
    int16_t last_round_r;
    int16_t last_img_x, last_img_y, last_img_w, last_img_rows;
    unsigned art_fills;   /* fills exactly PICKER_ART_W wide            */
    unsigned bar_fills;   /* fills in the hold-bar colour               */
    int16_t bar_w;
    int16_t bar_y;
    unsigned range_faults; /* image row ranges outside the source image */
    unsigned null_strings;
    int16_t widest_desc;
    /* The offscreen row between begin and end, and anything drawn outside
     * it while it is open. */
    bool off;
    int16_t off_x, off_y, off_w, off_h;
    unsigned off_violations, begins;
    /* Every text call, in order, for the tests that ask what was said. */
    struct {
        char s[CATALOG_TITLE_MAX + 4];
        int16_t x, y;
        uint16_t fg, bg;
    } log[64];
    unsigned logged;
    /* The bounding box of everything painted since paint_reset(), and fills
     * in the bar's track colour at the bar's height. */
    int16_t px0, py0, px1, py1;
    unsigned track_fills;
} fake_t;

/* The second is the reference a partial draw is compared against. */
static fake_t fk;
static fake_t fk_ref;

/* How a primitive lands in the buffer: a fill or opaque text sets its pixels,
 * transparent text marks over whatever is there. Each value is a function of
 * the call alone, so two draws that make the same calls in the same place
 * leave the same pixels — which is what "partial equals full" compares. */
enum { PAINT_SET, PAINT_OVER, PAINT_NONE };

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

static void paint_reset(fake_t* f)
{
    f->px0 = f->py0 = 0x7FFF;
    f->px1 = f->py1 = -1;
    f->track_fills = 0;
}

/* Paint the rect, or count it as a violation. One rule, every caller. */
static void put_rect(fake_t* f, int16_t x, int16_t y, int16_t w, int16_t h,
                     int mode, uint16_t v)
{
    int16_t iy, ix;

    if (x < 0 || y < 0 || w < 0 || h < 0 || x + w > f->w || y + h > f->h) {
        f->violations++;
        return;
    }
    if (f->off && (x < f->off_x || y < f->off_y ||
                   x + w > f->off_x + f->off_w ||
                   y + h > f->off_y + f->off_h)) {
        f->off_violations++;
        return;
    }
    if (mode == PAINT_NONE || w == 0 || h == 0) {
        return;
    }
    if (x < f->px0) f->px0 = x;
    if (y < f->py0) f->py0 = y;
    if (x + w > f->px1) f->px1 = (int16_t)(x + w);
    if (y + h > f->py1) f->py1 = (int16_t)(y + h);
    for (iy = y; iy < y + h; iy++) {
        for (ix = x; ix < x + w; ix++) {
            uint16_t* px = &f->fb[iy * f->w + ix];

            *px = (mode == PAINT_SET) ? v : (uint16_t)(*px ^ (v | 1u));
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
    /* COL_BAR, the hold bar's white. The list highlight is white too, so the
     * bar is told apart by its height. */
    if (color == 0xFFFF && h == PICKER_BAR_H) {
        f->bar_fills++;
        f->bar_w = w;
        f->bar_y = y;
    }
    if (color == 0x1082 && h == PICKER_BAR_H) {
        f->track_fills++;
    }
    put_rect(f, x, y, w, h, PAINT_SET, color);
}

/* The corners are the driver's business; the fake paints the whole
 * rectangle and remembers the radius it was asked for. */
static void fk_round_fill(void* ctx, int16_t x, int16_t y, int16_t w,
                          int16_t h, int16_t r, uint16_t color, uint16_t bg)
{
    fake_t* f = (fake_t*)ctx;

    (void)bg;
    f->round_fills++;
    f->last_round_r = r;
    put_rect(f, x, y, w, h, PAINT_SET, color);
}

static void fk_text(void* ctx, const char* s, int16_t x, int16_t y, int16_t w,
                    uint8_t rows, uint8_t font, uint8_t align, uint16_t fg,
                    uint16_t bg)
{
    fake_t* f = (fake_t*)ctx;
    int16_t h;

    (void)align;
    f->texts++;
    if (s == NULL) {
        f->null_strings++;
        return;
    }
    if (f->logged < sizeof(f->log) / sizeof(f->log[0])) {
        snprintf(f->log[f->logged].s, sizeof(f->log[0].s), "%s", s);
        f->log[f->logged].x = x;
        f->log[f->logged].y = y;
        f->log[f->logged].fg = fg;
        f->log[f->logged].bg = bg;
        f->logged++;
    }
    if (font == UI_FONT_SMALL && w > f->widest_desc) {
        f->widest_desc = w;
    }
    /* The box the driver will clip into: rows lines at the font's pitch. */
    h = (int16_t)(rows * UI_ROW_PITCH(ui_font_height(font)));
    /* Offscreen, the sprite clips a label sideways, which is how a marquee
     * scrolls one through a fixed box. It never clips a fill, an image, or a
     * text row taller than itself: those are still layout faults. */
    if (f->off) {
        int16_t l = (x > f->off_x) ? x : f->off_x;
        int16_t r = (x + w < f->off_x + f->off_w) ? (int16_t)(x + w)
                                                  : (int16_t)(f->off_x + f->off_w);

        x = l;
        w = (int16_t)((r > l) ? r - l : 0);
    }
    put_rect(f, x, y, w, h, (fg == bg) ? PAINT_OVER : PAINT_SET,
             hash_call(s, fg, bg, x, y));
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
    put_rect(f, x, y, w, h, PAINT_SET,
             hash_call(NULL, (uint16_t)(uintptr_t)px, (uint16_t)row0, x, y));
}

/* A fixed advance per glyph, 8 px at font 2 and 6 at font 1: close enough to
 * the panel's proportional font 2 for layout arithmetic, and exact for font 1. */
static int16_t fk_advance(uint8_t font)
{
    return (font == UI_FONT_SMALL) ? UI_FONT_SMALL_ADV : 8;
}

static int16_t fk_measure(void* ctx, const char* s, uint8_t font)
{
    (void)ctx;
    return (s == NULL) ? 0 : (int16_t)(strlen(s) * fk_advance(font));
}

static bool fk_begin(void* ctx, int16_t x, int16_t y, int16_t w, int16_t h)
{
    fake_t* f = (fake_t*)ctx;

    f->begins++;
    /* The buffer itself has to land inside the window. */
    put_rect(f, x, y, w, h, PAINT_NONE, 0);
    f->off = true;
    f->off_x = x;
    f->off_y = y;
    f->off_w = w;
    f->off_h = h;
    return true;
}

static void fk_end(void* ctx)
{
    ((fake_t*)ctx)->off = false;
}

static ui_canvas_t canvas_over(fake_t* f, int16_t w, int16_t h)
{
    ui_canvas_t cv;

    memset(&cv, 0, sizeof(cv));
    memset(f, 0, sizeof(*f));
    f->w = w;
    f->h = h;
    paint_reset(f);
    cv.ctx = f;
    cv.fill = fk_fill;
    cv.round_fill = fk_round_fill;
    cv.text = fk_text;
    cv.image = fk_image;
    cv.measure = fk_measure;
    cv.begin = fk_begin;
    cv.end = fk_end;
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

    if (picker_media_due(&p, t, &idx)) {
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
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0, fk.off_violations,
                                   "a primitive left its offscreen row");
    TEST_ASSERT_GREATER_THAN_UINT(0, fk.fills);
    TEST_ASSERT_GREATER_THAN_UINT(0, fk.texts);
}

/* ─── the layout table ───────────────────────────────────────────────────── */

/* The writer logs "cannot happen" on a refused layout, so the one geometry
 * has to compose. */
static void test_the_layout_at_266_by_240(void)
{
    picker_layout_t g;

    TEST_ASSERT_EQUAL_INT(PICKER_OK, picker_layout(GEOM_53_W, GEOM_53_H, &g));
    TEST_ASSERT_EQUAL_UINT8(12, g.rows);         /* (240 - 18) / 18      */
    TEST_ASSERT_EQUAL_INT16(166, g.list_art_x);  /* 266 - 4 - 96         */
    TEST_ASSERT_EQUAL_INT16(158, g.list_w);      /* 166 - 4 - 4          */
    TEST_ASSERT_EQUAL_INT16(18, g.list_art_y);   /* under the header     */
    TEST_ASSERT_EQUAL_INT16(118, g.list_shot_y); /* 18 + 96 + 4          */
    /* Both images stacked, inside the window. */
    TEST_ASSERT_LESS_OR_EQUAL_INT16(GEOM_53_H, g.list_shot_y + PICKER_ART_H);
    TEST_ASSERT_GREATER_OR_EQUAL_INT16(150, g.list_w);

    /* The detail page, Cart Info's numbers, for a two-row title. */
    TEST_ASSERT_EQUAL_INT16(8, g.detail_x);
    TEST_ASSERT_EQUAL_INT16(250, g.detail_w);    /* 266 - 16             */
    TEST_ASSERT_EQUAL_INT16(8, g.title_y);
    TEST_ASSERT_EQUAL_INT16(46, g.media_y);      /* 8 + 2 * 18 + 2       */
    TEST_ASSERT_EQUAL_INT16(112, g.shot_x);      /* 8 + 96 + 8           */
    TEST_ASSERT_EQUAL_INT16(150, g.band_y);      /* 46 + 96 + 8          */
    TEST_ASSERT_EQUAL_INT16(42, g.band_h);       /* 240 - 46 - 2 - 150   */
    TEST_ASSERT_EQUAL_UINT8(4, g.band_rows);     /* 42 / 10              */
    TEST_ASSERT_EQUAL_UINT8(41, g.desc_cols);    /* 250 / 6              */
    TEST_ASSERT_GREATER_OR_EQUAL_UINT8(3, g.band_rows);
    /* The band ends above the footer's target line. */
    TEST_ASSERT_LESS_OR_EQUAL_INT16(GEOM_53_H - PICKER_DETAIL_FOOT_H,
                                    g.band_y + g.band_h);
}

static void test_the_layout_refuses_a_window_it_cannot_compose(void)
{
    picker_layout_t g;

    /* 100 leaves the band nothing. */
    TEST_ASSERT_EQUAL_INT(PICKER_ERR_ARGS, picker_layout(200, 100, &g));
    /* 213 + ... the list's images fit but 150 + 46 + 2 leaves no band line. */
    TEST_ASSERT_EQUAL_INT(PICKER_ERR_ARGS, picker_layout(266, 205, &g));
    /* 150 < 96 + 16 + 48: an image and no usable column beside it. */
    TEST_ASSERT_EQUAL_INT(PICKER_ERR_ARGS, picker_layout(150, 216, &g));
    /* 213 < 18 + 96 + 4 + 96: the list's images do not stack. */
    TEST_ASSERT_EQUAL_INT(PICKER_ERR_ARGS, picker_layout(266, 213, &g));
    TEST_ASSERT_EQUAL_INT(PICKER_ERR_ARGS, picker_layout(240, 216, NULL));
}

/* ─── the list ────────────────────────────────────────────────────────────── */

static void test_the_list_stays_inside_the_window_in_every_mode(void)
{
    struct { int16_t w, h; } geo[] = { { GEOM_53_W, GEOM_53_H } };
    size_t i;

    fill_library(LIB_COUNT);
    for (i = 0; i < sizeof(geo) / sizeof(geo[0]); i++) {
        picker_layout_t g;

        TEST_ASSERT_EQUAL_INT(PICKER_OK,
                              picker_layout(geo[i].w, geo[i].h, &g));

        run_list(geo[i].w, geo[i].h, PICKER_MODE_PENDING, true, false, NULL);
        assert_sane();
        /* Still loading: two plain slots, no image yet. */
        TEST_ASSERT_EQUAL_UINT(0, fk.images);
        TEST_ASSERT_EQUAL_UINT(2, fk.art_fills);
        /* One label per visible row, plus the header. */
        TEST_ASSERT_GREATER_OR_EQUAL_UINT(g.rows + 1u, fk.texts);

        /* Cancel pending write is highlighted: no images for an action. */
        run_list(geo[i].w, geo[i].h, PICKER_MODE_PENDING, true, true, NULL);
        assert_sane();
        TEST_ASSERT_EQUAL_UINT(0, fk.images);
        TEST_ASSERT_EQUAL_UINT(0, fk.art_fills);

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

    run_list(GEOM_53_W, GEOM_53_H, PICKER_MODE_IMMEDIATE, false, false, &made);
    assert_sane();
}

/*
 * The list with the first title's media in the given states and its label
 * scrolled `px` of a `span`-pixel overflow.
 */
static void run_list_media(uint8_t art_state, uint8_t shot_state, int16_t span,
                           int16_t px)
{
    picker_t p;
    picker_layout_t g;
    ui_canvas_t cv = canvas_over(&fk, GEOM_53_W, GEOM_53_H);
    uint16_t idx = 0;

    TEST_ASSERT_EQUAL_INT(PICKER_OK, picker_layout(GEOM_53_W, GEOM_53_H, &g));
    TEST_ASSERT_EQUAL_INT(PICKER_OK,
                          picker_init(&p, PICKER_MODE_PENDING, &lib, true,
                                      false, NULL, g.rows));
    TEST_ASSERT_TRUE(picker_media_due(&p, 100, &idx));
    picker_media_loaded(&p, idx, art_state == PICKER_MEDIA_READY,
                        shot_state == PICKER_MEDIA_READY);
    if (art_state == PICKER_MEDIA_LOADING) {
        p.art_state = PICKER_MEDIA_LOADING;
    }
    if (shot_state == PICKER_MEDIA_LOADING) {
        p.shot_state = PICKER_MEDIA_LOADING;
    }
    picker_set_marquee_span(&p, span);
    p.marquee_px = px;
    picker_draw(&p, &g, NULL, (const uint16_t*)&lib, (const uint16_t*)&lib,
                &cv);
}

static void test_the_list_stays_inside_the_window_in_every_media_state(void)
{
    uint8_t states[][2] = {
        { PICKER_MEDIA_READY, PICKER_MEDIA_READY },
        { PICKER_MEDIA_READY, PICKER_MEDIA_MISSING },
        { PICKER_MEDIA_MISSING, PICKER_MEDIA_MISSING },
        { PICKER_MEDIA_LOADING, PICKER_MEDIA_LOADING },
    };
    size_t i;

    fill_library(LIB_COUNT);
    for (i = 0; i < sizeof(states) / sizeof(states[0]); i++) {
        run_list_media(states[i][0], states[i][1], 0, 0);
        assert_sane();
        TEST_ASSERT_EQUAL_UINT((states[i][0] == PICKER_MEDIA_READY) +
                                   (states[i][1] == PICKER_MEDIA_READY),
                               fk.images);
    }
}

static void test_the_images_stack_in_the_right_column(void)
{
    picker_layout_t g;

    fill_library(LIB_COUNT);
    picker_layout(GEOM_53_W, GEOM_53_H, &g);
    run_list_media(PICKER_MEDIA_READY, PICKER_MEDIA_READY, 0, 0);
    assert_sane();
    TEST_ASSERT_EQUAL_UINT(2, fk.images);
    /* The last push is the snapshot, under the cover. */
    TEST_ASSERT_EQUAL_INT16(g.list_art_x, fk.last_img_x);
    TEST_ASSERT_EQUAL_INT16(g.list_shot_y, fk.last_img_y);
    TEST_ASSERT_GREATER_OR_EQUAL_INT16(4 + g.list_w + 4, g.list_art_x);
}

static void test_the_marquee_row_stays_inside_the_title_column(void)
{
    picker_layout_t g;
    const int16_t span = 60;
    int16_t pxs[] = { 0, span / 2, span };
    size_t i;

    fill_library(LIB_COUNT);
    picker_layout(GEOM_53_W, GEOM_53_H, &g);
    for (i = 0; i < sizeof(pxs) / sizeof(pxs[0]); i++) {
        run_list_media(PICKER_MEDIA_READY, PICKER_MEDIA_READY, span, pxs[i]);
        assert_sane();
        /* The highlighted row, built offscreen, and only it. */
        TEST_ASSERT_EQUAL_UINT(1, fk.begins);
        TEST_ASSERT_LESS_OR_EQUAL_INT16(g.list_art_x,
                                        fk.off_x + fk.off_w);
        TEST_ASSERT_EQUAL_INT16(PICKER_HEADER_H, fk.off_y);
        TEST_ASSERT_EQUAL_INT16(PICKER_ROW_H, fk.off_h);
    }
}

static void test_a_long_title_overflows_its_row_and_a_short_one_does_not(void)
{
    picker_t p;
    picker_layout_t g;
    ui_canvas_t cv = canvas_over(&fk, GEOM_53_W, GEOM_53_H);

    fill_library(LIB_COUNT);
    picker_layout(GEOM_53_W, GEOM_53_H, &g);
    picker_init(&p, PICKER_MODE_PENDING, &lib, true, false, NULL, g.rows);

    snprintf(lib.e[0].title, sizeof(lib.e[0].title), "Tetris");
    TEST_ASSERT_EQUAL_INT16(0, picker_row_overflow(&p, &g, &cv));

    /* 47 characters, the catalog's longest: 47 * 8 + 1 - 158. */
    memset(lib.e[0].title, 'W', CATALOG_TITLE_MAX - 1);
    lib.e[0].title[CATALOG_TITLE_MAX - 1] = '\0';
    TEST_ASSERT_EQUAL_INT16((CATALOG_TITLE_MAX - 1) * 8 + 1 - g.list_w,
                            picker_row_overflow(&p, &g, &cv));
    TEST_ASSERT_GREATER_THAN_INT16(0, picker_row_overflow(&p, &g, &cv));
}

/* ─── the detail page ─────────────────────────────────────────────────────── */

static void test_every_detail_combination_stays_inside_the_window(void)
{
    struct { int16_t w, h; } geo[] = { { GEOM_53_W, GEOM_53_H } };
    uint8_t states[][2] = {
        { PICKER_MEDIA_READY, PICKER_MEDIA_READY },
        { PICKER_MEDIA_READY, PICKER_MEDIA_MISSING },
        { PICKER_MEDIA_MISSING, PICKER_MEDIA_MISSING },
        { PICKER_MEDIA_LOADING, PICKER_MEDIA_LOADING },
    };
    uint8_t pcts[] = { 0, 50, 100 };
    size_t gi, si, pi, sc;

    fill_library(LIB_COUNT);
    for (gi = 0; gi < sizeof(geo) / sizeof(geo[0]); gi++) {
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
                    /* And a one-row title, which lifts everything below. */
                    snprintf(lib.e[0].title, sizeof(lib.e[0].title), "Tetris");
                    run_detail(geo[gi].w, geo[gi].h, PICKER_MODE_PENDING, true,
                               false, 0, states[si][0], states[si][1],
                               pcts[pi], scrolls[sc], DESC_200);
                    assert_sane();
                    fill_library(LIB_COUNT);
                }
            }
        }
    }
}

static void test_both_images_sit_side_by_side_under_the_title(void)
{
    picker_layout_t g;

    fill_library(LIB_COUNT);
    picker_layout(GEOM_53_W, GEOM_53_H, &g);

    run_detail(GEOM_53_W, GEOM_53_H, PICKER_MODE_PENDING, true, false, 0,
               PICKER_MEDIA_READY, PICKER_MEDIA_READY, 0, 0, DESC_200);
    assert_sane();

    /* The cover and the snapshot: two whole images, one push each. The last
     * is the snapshot, 8 px right of the cover at the same height. */
    TEST_ASSERT_EQUAL_UINT(2, fk.images);
    TEST_ASSERT_EQUAL_INT16(PICKER_ART_W, fk.last_img_w);
    TEST_ASSERT_EQUAL_INT16(PICKER_ART_H, fk.last_img_rows);
    TEST_ASSERT_EQUAL_INT16(8 + PICKER_ART_W + 8, fk.last_img_x);
    TEST_ASSERT_EQUAL_INT16(g.media_y, fk.last_img_y);
}

static void test_a_one_row_title_lifts_the_images_a_row(void)
{
    picker_layout_t g;

    fill_library(LIB_COUNT);
    picker_layout(GEOM_53_W, GEOM_53_H, &g);
    snprintf(lib.e[0].title, sizeof(lib.e[0].title), "Tetris");

    run_detail(GEOM_53_W, GEOM_53_H, PICKER_MODE_PENDING, true, false, 0,
               PICKER_MEDIA_READY, PICKER_MEDIA_READY, 0, 0, DESC_200);
    assert_sane();
    TEST_ASSERT_EQUAL_INT16(g.media_y - UI_ROW_PITCH(ui_font_height(UI_FONT_ROW)),
                            fk.last_img_y);
}

static void test_a_one_row_title_gives_its_row_to_the_band(void)
{
    picker_t p;
    picker_layout_t g;
    ui_canvas_t cv = canvas_over(&fk, GEOM_53_W, GEOM_53_H);

    fill_library(LIB_COUNT);
    picker_layout(GEOM_53_W, GEOM_53_H, &g);
    picker_init(&p, PICKER_MODE_PENDING, &lib, true, false, NULL, g.rows);
    /* No page open: the layout's own figure. */
    TEST_ASSERT_EQUAL_UINT8(g.band_rows, picker_band_rows(&p, &g, &cv));

    picker_input(&p, B_A, 0);
    TEST_ASSERT_EQUAL_UINT8(g.band_rows, picker_band_rows(&p, &g, &cv));
    snprintf(lib.e[0].title, sizeof(lib.e[0].title), "Tetris");
    /* (42 + 18) / 10 */
    TEST_ASSERT_EQUAL_UINT8(6, picker_band_rows(&p, &g, &cv));
}

static void test_the_images_stay_put_while_the_band_scrolls(void)
{
    picker_layout_t g;
    uint16_t span;
    int16_t x0, y0;

    fill_library(LIB_COUNT);
    picker_layout(GEOM_53_W, GEOM_53_H, &g);
    span = (uint16_t)(picker_page_lines(&g, DESC_200) - g.band_rows);
    TEST_ASSERT_GREATER_THAN_UINT(0, span);

    run_detail(GEOM_53_W, GEOM_53_H, PICKER_MODE_PENDING, true, false, 0,
               PICKER_MEDIA_READY, PICKER_MEDIA_READY, 0, 0, DESC_200);
    x0 = fk.last_img_x;
    y0 = fk.last_img_y;
    run_detail(GEOM_53_W, GEOM_53_H, PICKER_MODE_PENDING, true, false, 0,
               PICKER_MEDIA_READY, PICKER_MEDIA_READY, 0, span, DESC_200);
    assert_sane();
    TEST_ASSERT_EQUAL_UINT(2, fk.images);
    TEST_ASSERT_EQUAL_INT16(x0, fk.last_img_x);
    TEST_ASSERT_EQUAL_INT16(y0, fk.last_img_y);
    TEST_ASSERT_EQUAL_INT16(PICKER_ART_H, fk.last_img_rows);
}

static void test_no_detail_page_draws_the_filename(void)
{
    struct { enum picker_mode_e mode; bool wild, pending; } pages[] = {
        { PICKER_MODE_PENDING, true, false },   /* a game          */
        { PICKER_MODE_PENDING, true, true },    /* Cancel pending  */
        { PICKER_MODE_IMMEDIATE, true, false }, /* Finish setup    */
        { PICKER_MODE_IMMEDIATE, false, false },/* a starter game  */
    };
    size_t i;
    unsigned t;

    fill_library(LIB_COUNT);
    for (i = 0; i < sizeof(pages) / sizeof(pages[0]); i++) {
        run_detail(GEOM_53_W, GEOM_53_H, pages[i].mode, pages[i].wild,
                   pages[i].pending, 0, PICKER_MEDIA_READY,
                   PICKER_MEDIA_READY, 0, 0, DESC_200);
        assert_sane();
        for (t = 0; t < fk.logged; t++) {
            TEST_ASSERT_NULL(strstr(fk.log[t].s, ".gb"));
        }
    }
}

static void test_missing_media_draws_two_placeholders_and_no_image(void)
{
    fill_library(LIB_COUNT);

    run_detail(GEOM_53_W, GEOM_53_H, PICKER_MODE_PENDING, true, false, 0,
               PICKER_MEDIA_MISSING, PICKER_MEDIA_MISSING, 0, 0, DESC_200);
    assert_sane();

    TEST_ASSERT_EQUAL_UINT(0, fk.images);
    /* One slot-width fill each for the cover and the snapshot. */
    TEST_ASSERT_GREATER_OR_EQUAL_UINT(2, fk.art_fills);
}

static void test_the_hold_bar_appears_only_once_the_hold_starts(void)
{
    fill_library(LIB_COUNT);

    run_detail(GEOM_53_W, GEOM_53_H, PICKER_MODE_PENDING, true, false, 0,
               PICKER_MEDIA_READY, PICKER_MEDIA_READY, 0, 0, DESC_200);
    assert_sane();
    TEST_ASSERT_EQUAL_UINT(0, fk.bar_fills);

    run_detail(GEOM_53_W, GEOM_53_H, PICKER_MODE_PENDING, true, false, 0,
               PICKER_MEDIA_READY, PICKER_MEDIA_READY, 100, 0, DESC_200);
    assert_sane();
    TEST_ASSERT_EQUAL_UINT(1, fk.bar_fills);
    /* A hold one tick short of done is a bar one pixel-step short of full. */
    TEST_ASSERT_GREATER_THAN_INT16((GEOM_53_W - 16) * 9 / 10, fk.bar_w);
    TEST_ASSERT_EQUAL_INT16(GEOM_53_H - 14, fk.bar_y);
}

/* ─── the description ─────────────────────────────────────────────────────── */

static void test_the_page_is_the_description_and_never_less_than_the_band(void)
{
    static char text[2001];
    picker_layout_t g;
    size_t i;

    TEST_ASSERT_EQUAL_INT(PICKER_OK, picker_layout(GEOM_53_W, GEOM_53_H, &g));
    TEST_ASSERT_EQUAL_UINT16(g.band_rows, picker_page_lines(&g, NULL));
    TEST_ASSERT_EQUAL_UINT16(g.band_rows, picker_page_lines(&g, "Short."));

    /* 2,000 characters in three paragraphs of short words: the images play no
     * part, only the text's own lines. */
    for (i = 0; i < 2000; i++) {
        text[i] = (i % 6 == 5) ? ' ' : 'w';
    }
    text[666] = '\n';
    text[667] = '\n';
    text[1333] = '\n';
    text[1334] = '\n';
    text[2000] = '\0';
    TEST_ASSERT_EQUAL_UINT16(picker_desc_lines(text, g.desc_cols),
                             picker_page_lines(&g, text));
    TEST_ASSERT_EQUAL_UINT16(picker_desc_lines(DESC_200, g.desc_cols),
                             picker_page_lines(&g, DESC_200));
}

static void test_every_wrapped_line_fits_the_column_in_pixels(void)
{
    picker_layout_t g;
    uint16_t lines;
    uint16_t i;
    char line[PICKER_DESC_LINE_MAX];

    TEST_ASSERT_EQUAL_INT(PICKER_OK, picker_layout(GEOM_53_W, GEOM_53_H, &g));
    lines = picker_desc_lines(DESC_200, g.desc_cols);
    TEST_ASSERT_GREATER_THAN_UINT16(0, lines);

    for (i = 0; i < lines; i++) {
        TEST_ASSERT_TRUE(
            picker_desc_line(DESC_200, g.desc_cols, i, line, sizeof(line)));
        /* The property that makes this module's break points and
         * display_draw_wrapped's pixel breaking the same: font 1's advance is
         * fixed, so a line of n characters is exactly n * 6 pixels wide. */
        TEST_ASSERT_LESS_OR_EQUAL_INT16(
            g.detail_w, (int16_t)(strlen(line) * UI_FONT_SMALL_ADV));
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

static void test_a_newline_ends_the_line_and_a_blank_one_is_empty(void)
{
    char line[PICKER_DESC_LINE_MAX];

    TEST_ASSERT_EQUAL_UINT16(2, picker_desc_lines("ab\ncd", 10));
    TEST_ASSERT_TRUE(picker_desc_line("ab\ncd", 10, 0, line, sizeof(line)));
    TEST_ASSERT_EQUAL_STRING("ab", line);
    TEST_ASSERT_TRUE(picker_desc_line("ab\ncd", 10, 1, line, sizeof(line)));
    TEST_ASSERT_EQUAL_STRING("cd", line);

    /* The second newline of a paragraph break is a line of its own. */
    TEST_ASSERT_EQUAL_UINT16(3, picker_desc_lines("ab\n\ncd", 10));
    TEST_ASSERT_TRUE(picker_desc_line("ab\n\ncd", 10, 1, line, sizeof(line)));
    TEST_ASSERT_EQUAL_STRING("", line);
    TEST_ASSERT_TRUE(picker_desc_line("ab\n\ncd", 10, 2, line, sizeof(line)));
    TEST_ASSERT_EQUAL_STRING("cd", line);

    /* A newline just past a full-width line breaks there, not at the earlier
     * space the space rule would pick. */
    TEST_ASSERT_TRUE(
        picker_desc_line("alpha beta\ngamma", 10, 0, line, sizeof(line)));
    TEST_ASSERT_EQUAL_STRING("alpha beta", line);
    TEST_ASSERT_EQUAL_UINT16(2, picker_desc_lines("alpha beta\ngamma", 10));
}

static void test_three_paragraphs_wrap_with_a_blank_line_between(void)
{
    char line[PICKER_DESC_LINE_MAX];
    const char* text =
        "The hero sets out from a small town to find the lost crown of the "
        "kingdom.\n\n"
        "Along the way four friends join the party, each with a skill of "
        "their own.\n\n"
        "Only together can they reach the tower.";

    /* Two lines, blank, two lines, blank, one line. */
    TEST_ASSERT_EQUAL_UINT16(7, picker_desc_lines(text, 41));
    TEST_ASSERT_TRUE(picker_desc_line(text, 41, 2, line, sizeof(line)));
    TEST_ASSERT_EQUAL_STRING("", line);
    TEST_ASSERT_TRUE(picker_desc_line(text, 41, 6, line, sizeof(line)));
    TEST_ASSERT_EQUAL_STRING("Only together can they reach the tower.", line);
    TEST_ASSERT_FALSE(picker_desc_line(text, 41, 7, line, sizeof(line)));
}

/* ─── argument safety ─────────────────────────────────────────────────────── */

static void test_draw_paints_nothing_when_it_is_handed_nothing(void)
{
    picker_t p;
    picker_layout_t g;
    ui_canvas_t cv = canvas_over(&fk, GEOM_53_W, GEOM_53_H);
    ui_canvas_t broken = cv;

    fill_library(LIB_COUNT);
    picker_layout(GEOM_53_W, GEOM_53_H, &g);
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

/* ─── colours and wording ─────────────────────────────────────────────────── */

static void test_the_pending_list_header_reads_choose_a_game(void)
{
    fill_library(LIB_COUNT);
    run_list(GEOM_53_W, GEOM_53_H, PICKER_MODE_PENDING, true, false, NULL);
    assert_sane();
    TEST_ASSERT_EQUAL_STRING("Choose a Game", fk.log[0].s);
}

static void test_the_cursor_row_is_bold_black_on_a_white_bar(void)
{
    unsigned i;
    unsigned black = 0;
    int16_t x0 = 0;

    fill_library(LIB_COUNT);
    run_list(GEOM_53_W, GEOM_53_H, PICKER_MODE_PENDING, true, false, NULL);
    assert_sane();
    /* The cursor is on the first title: struck twice, transparent black, one
     * pixel apart. Every other row is white on black. */
    for (i = 1; i < fk.logged; i++) {
        if (fk.log[i].fg == 0x0000) {
            TEST_ASSERT_EQUAL_HEX16(0x0000, fk.log[i].bg);
            TEST_ASSERT_EQUAL_STRING(lib.e[0].title, fk.log[i].s);
            if (black == 0) {
                x0 = fk.log[i].x;
            } else {
                TEST_ASSERT_EQUAL_INT16(x0 + 1, fk.log[i].x);
            }
            black++;
        } else {
            TEST_ASSERT_EQUAL_HEX16(0xFFFF, fk.log[i].fg);
        }
    }
    TEST_ASSERT_EQUAL_UINT(2, black);
    /* The bar itself: the row's left edge, which no label box reaches. */
    TEST_ASSERT_EQUAL_HEX16(0xFFFF, fk.fb[PICKER_HEADER_H * GEOM_53_W]);
    TEST_ASSERT_EQUAL_HEX16(0x0000,
                            fk.fb[(PICKER_HEADER_H + PICKER_ROW_H) * GEOM_53_W]);
}

/* ─── partial redraws ─────────────────────────────────────────────────────── */

static picker_t sp;
static picker_layout_t sg;
static ui_canvas_t scv;
static const char* sdesc;

#define ART ((const uint16_t*)&lib)

/* A list on screen, drawn in full, the way the binding starts. */
static void seq_begin(void)
{
    fill_library(LIB_COUNT);
    TEST_ASSERT_EQUAL_INT(PICKER_OK, picker_layout(GEOM_53_W, GEOM_53_H, &sg));
    TEST_ASSERT_EQUAL_INT(PICKER_OK,
                          picker_init(&sp, PICKER_MODE_PENDING, &lib, true,
                                      false, NULL, sg.rows));
    sdesc = NULL;
    scv = canvas_over(&fk, GEOM_53_W, GEOM_53_H);
    picker_draw(&sp, &sg, sdesc, ART, ART, &scv);
}

/* Paint `ev` over what is on screen, and check that it leaves exactly what a
 * fresh full draw of the same state would. */
static void seq_step(uint8_t ev)
{
    ui_canvas_t ref;

    TEST_ASSERT_NOT_EQUAL_UINT8(PICKER_EVENT_NONE, ev);
    paint_reset(&fk);
    picker_draw_events(&sp, &sg, ev, sdesc, ART, ART, &scv);
    assert_sane();

    ref = canvas_over(&fk_ref, GEOM_53_W, GEOM_53_H);
    picker_draw(&sp, &sg, sdesc, ART, ART, &ref);
    TEST_ASSERT_EQUAL_MEMORY_MESSAGE(fk_ref.fb, fk.fb,
                                     sizeof(uint16_t) * GEOM_53_W * GEOM_53_H,
                                     "a partial draw left a different screen");
}

/* The painted box sits inside x0..x1, y0..y1. */
static void assert_painted_within(int16_t x0, int16_t y0, int16_t x1,
                                  int16_t y1)
{
    TEST_ASSERT_GREATER_THAN_INT16_MESSAGE(fk.px0, fk.px1, "nothing was painted");
    TEST_ASSERT_GREATER_OR_EQUAL_INT16(x0, fk.px0);
    TEST_ASSERT_GREATER_OR_EQUAL_INT16(y0, fk.py0);
    TEST_ASSERT_LESS_OR_EQUAL_INT16(x1, fk.px1);
    TEST_ASSERT_LESS_OR_EQUAL_INT16(y1, fk.py1);
}

/* Open the first title with its images and description in. */
static void seq_open(void)
{
    uint16_t idx = 0;

    TEST_ASSERT_TRUE(picker_media_due(&sp, 100, &idx));
    seq_step(picker_media_loaded(&sp, idx, true, true));
    seq_step(picker_input(&sp, B_A, 200));
    picker_input(&sp, B_NONE, 210);
    TEST_ASSERT_TRUE(picker_desc_due(&sp, &idx));
    sdesc = DESC_200;
    picker_set_scroll_span(&sp, picker_page_lines(&sg, sdesc),
                           picker_band_rows(&sp, &sg, &scv));
    seq_step(PICKER_EVENT_BAND);
}

static void test_a_hold_tick_paints_only_the_bar_and_never_its_track(void)
{
    int16_t bar_y = (int16_t)(GEOM_53_H - 4 - PICKER_BAR_H);

    seq_begin();
    seq_open();
    seq_step(picker_input(&sp, B_A, 300));
    seq_step(picker_input(&sp, B_A, 500));
    assert_painted_within(sg.detail_x, bar_y, sg.detail_x + sg.detail_w,
                          bar_y + PICKER_BAR_H);
    TEST_ASSERT_EQUAL_UINT(0, fk.track_fills);
    TEST_ASSERT_EQUAL_UINT(1, fk.bar_fills);

    /* Letting go lays the track back down, and only the track. */
    seq_step(picker_input(&sp, B_NONE, 600));
    assert_painted_within(sg.detail_x, bar_y, sg.detail_x + sg.detail_w,
                          bar_y + PICKER_BAR_H);
    TEST_ASSERT_EQUAL_UINT(1, fk.track_fills);
}

static void test_a_scroll_paints_only_the_band(void)
{
    seq_begin();
    seq_open();
    TEST_ASSERT_GREATER_THAN_UINT16(0, sp.scroll_max);
    TEST_ASSERT_EQUAL_UINT8(PICKER_EVENT_BAND,
                            picker_input(&sp, B_DOWN, 300));
    seq_step(PICKER_EVENT_BAND);
    assert_painted_within(0, sg.band_y, GEOM_53_W, sg.band_y + sg.band_h);
}

static void test_a_move_paints_only_its_two_rows_and_the_image_column(void)
{
    uint8_t ev;

    seq_begin();
    ev = picker_input(&sp, B_DOWN, 100);
    TEST_ASSERT_EQUAL_UINT8(PICKER_EVENT_ROWS | PICKER_EVENT_MEDIA, ev);

    seq_step(PICKER_EVENT_ROWS);
    assert_painted_within(0, PICKER_HEADER_H, sg.list_art_x - 4,
                          PICKER_HEADER_H + 2 * PICKER_ROW_H);
    seq_step(PICKER_EVENT_MEDIA);
    assert_painted_within(sg.list_art_x - 4, PICKER_HEADER_H, GEOM_53_W,
                          GEOM_53_H);
}

static void test_a_marquee_step_paints_only_the_highlighted_row_offscreen(void)
{
    uint32_t t;
    uint8_t ev = PICKER_EVENT_NONE;

    seq_begin();
    picker_set_marquee_span(&sp, picker_row_overflow(&sp, &sg, &scv));
    TEST_ASSERT_GREATER_THAN_INT16(0, sp.marquee_span);
    for (t = 0; t < 5000 && ev == PICKER_EVENT_NONE; t += 16) {
        ev = picker_input(&sp, B_NONE, t);
    }
    TEST_ASSERT_EQUAL_UINT8(PICKER_EVENT_MARQUEE, ev);

    fk.begins = 0;
    seq_step(ev);
    TEST_ASSERT_EQUAL_UINT(1, fk.begins);
    assert_painted_within(0, PICKER_HEADER_H, sg.list_art_x - 4,
                          PICKER_HEADER_H + PICKER_ROW_H);
}

static void test_images_landing_on_a_detail_page_paint_only_the_images(void)
{
    uint16_t idx = 0;

    seq_begin();
    /* Opened before the settle ran out, so the images arrive on the page. */
    seq_step(picker_input(&sp, B_A, 10));
    picker_input(&sp, B_NONE, 20);
    TEST_ASSERT_TRUE(picker_media_due(&sp, 30, &idx));
    seq_step(picker_media_loaded(&sp, idx, true, true));
    assert_painted_within(sg.detail_x, sg.media_y, sg.shot_x + PICKER_ART_W,
                          sg.media_y + PICKER_ART_H);
}

static void test_partial_draws_add_up_to_a_full_draw(void)
{
    uint16_t idx = 0;
    uint32_t t;
    uint8_t ev;

    seq_begin();
    /* Moves, and the images landing. */
    seq_step(picker_input(&sp, B_DOWN, 100));
    picker_input(&sp, B_NONE, 110);
    TEST_ASSERT_TRUE(picker_media_due(&sp, 200, &idx));
    seq_step(picker_media_loaded(&sp, idx, true, false));
    seq_step(picker_input(&sp, B_DOWN, 300));
    picker_input(&sp, B_NONE, 310);
    seq_step(picker_input(&sp, B_UP, 400));
    picker_input(&sp, B_NONE, 410);
    /* Each move dropped the images; back on this title they load again. */
    TEST_ASSERT_TRUE(picker_media_due(&sp, 460, &idx));
    seq_step(picker_media_loaded(&sp, idx, false, true));

    /* The marquee, a few steps in. */
    picker_set_marquee_span(&sp, picker_row_overflow(&sp, &sg, &scv));
    for (t = 420; t < 420 + PICKER_MARQUEE_DELAY_MS + 200; t += 16) {
        ev = picker_input(&sp, B_NONE, t);
        if (ev != PICKER_EVENT_NONE) {
            seq_step(ev);
        }
    }
    TEST_ASSERT_GREATER_THAN_INT16(0, sp.marquee_px);

    /* Open, the images already in; the description; a scroll. */
    seq_step(picker_input(&sp, B_A, 2000));
    picker_input(&sp, B_NONE, 2010);
    TEST_ASSERT_FALSE(picker_media_due(&sp, 2020, &idx));
    TEST_ASSERT_TRUE(picker_desc_due(&sp, &idx));
    sdesc = DESC_200;
    picker_set_scroll_span(&sp, picker_page_lines(&sg, sdesc),
                           picker_band_rows(&sp, &sg, &scv));
    seq_step(PICKER_EVENT_BAND);
    seq_step(picker_input(&sp, B_DOWN, 2100));
    picker_input(&sp, B_NONE, 2110);

    /* A hold, part way, and let go. */
    seq_step(picker_input(&sp, B_A, 2200));
    for (t = 2216; t < 2700; t += 16) {
        seq_step(picker_input(&sp, B_A, t));
    }
    seq_step(picker_input(&sp, B_NONE, 2700));

    /* Back to the list, and one more move. */
    seq_step(picker_input(&sp, B_B, 2800));
    picker_input(&sp, B_NONE, 2810);
    seq_step(picker_input(&sp, B_DOWN, 2900));
}

/* ─── the fake itself ────────────────────────────────────────────────────── */

static void test_the_fake_measures_a_fixed_advance_per_glyph(void)
{
    ui_canvas_t cv = canvas_over(&fk, GEOM_53_W, GEOM_53_H);

    TEST_ASSERT_EQUAL_INT16(6 * 8, cv.measure(cv.ctx, "Tetris", UI_FONT_ROW));
    TEST_ASSERT_EQUAL_INT16(6 * UI_FONT_SMALL_ADV,
                            cv.measure(cv.ctx, "Tetris", UI_FONT_SMALL));
}

static void test_the_fake_paints_a_round_fill_inside_its_bounds(void)
{
    ui_canvas_t cv = canvas_over(&fk, GEOM_53_W, GEOM_53_H);

    cv.round_fill(cv.ctx, 10, 20, 60, 18, 9, 0xFFFF, 0x0000);
    TEST_ASSERT_EQUAL_UINT(1, fk.round_fills);
    TEST_ASSERT_EQUAL_INT16(9, fk.last_round_r);
    TEST_ASSERT_EQUAL_UINT(0, fk.violations);
    TEST_ASSERT_EQUAL_HEX16(0xFFFF, fk.fb[20 * GEOM_53_W + 10]);
    TEST_ASSERT_EQUAL_HEX16(0xFFFF, fk.fb[37 * GEOM_53_W + 69]);
    TEST_ASSERT_EQUAL_HEX16(0x0000, fk.fb[38 * GEOM_53_W + 70]);
    /* A pixel past the window is a violation, as it is for fill. */
    cv.round_fill(cv.ctx, GEOM_53_W - 10, 0, 11, 18, 9, 0xFFFF, 0x0000);
    TEST_ASSERT_EQUAL_UINT(1, fk.violations);
}

static void test_the_fake_rejects_a_draw_outside_its_offscreen_row(void)
{
    ui_canvas_t cv = canvas_over(&fk, GEOM_53_W, GEOM_53_H);

    TEST_ASSERT_TRUE(cv.begin(cv.ctx, 4, 36, 158, PICKER_ROW_H));
    cv.fill(cv.ctx, 4, 36, 158, PICKER_ROW_H, 0);
    TEST_ASSERT_EQUAL_UINT(0, fk.off_violations);
    /* A label wider than the row is clipped, as the sprite clips it... */
    cv.text(cv.ctx, "x", -20, 36, 300, 1, UI_FONT_ROW, UI_ALIGN_LEFT, 0, 0);
    TEST_ASSERT_EQUAL_UINT(0, fk.off_violations);
    /* ...but a fill a pixel too wide, or a text box a row too tall, is not. */
    cv.fill(cv.ctx, 4, 36, 159, PICKER_ROW_H, 0);
    TEST_ASSERT_EQUAL_UINT(1, fk.off_violations);
    cv.text(cv.ctx, "x", 4, 36, 158, 2, UI_FONT_ROW, UI_ALIGN_LEFT, 0, 0);
    TEST_ASSERT_EQUAL_UINT(2, fk.off_violations);
    cv.end(cv.ctx);
    cv.fill(cv.ctx, 0, 0, 1, 1, 0);
    TEST_ASSERT_EQUAL_UINT(2, fk.off_violations);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_the_layout_at_266_by_240);
    RUN_TEST(test_the_layout_refuses_a_window_it_cannot_compose);
    RUN_TEST(test_the_list_stays_inside_the_window_in_every_mode);
    RUN_TEST(test_a_marked_row_still_fits_its_box);
    RUN_TEST(test_the_list_stays_inside_the_window_in_every_media_state);
    RUN_TEST(test_the_images_stack_in_the_right_column);
    RUN_TEST(test_the_marquee_row_stays_inside_the_title_column);
    RUN_TEST(test_a_long_title_overflows_its_row_and_a_short_one_does_not);
    RUN_TEST(test_every_detail_combination_stays_inside_the_window);
    RUN_TEST(test_both_images_sit_side_by_side_under_the_title);
    RUN_TEST(test_a_one_row_title_lifts_the_images_a_row);
    RUN_TEST(test_a_one_row_title_gives_its_row_to_the_band);
    RUN_TEST(test_the_images_stay_put_while_the_band_scrolls);
    RUN_TEST(test_no_detail_page_draws_the_filename);
    RUN_TEST(test_the_page_is_the_description_and_never_less_than_the_band);
    RUN_TEST(test_missing_media_draws_two_placeholders_and_no_image);
    RUN_TEST(test_the_hold_bar_appears_only_once_the_hold_starts);
    RUN_TEST(test_every_wrapped_line_fits_the_column_in_pixels);
    RUN_TEST(test_the_wrap_breaks_at_spaces_and_hard_breaks_long_words);
    RUN_TEST(test_a_newline_ends_the_line_and_a_blank_one_is_empty);
    RUN_TEST(test_three_paragraphs_wrap_with_a_blank_line_between);
    RUN_TEST(test_draw_paints_nothing_when_it_is_handed_nothing);
    RUN_TEST(test_the_pending_list_header_reads_choose_a_game);
    RUN_TEST(test_the_cursor_row_is_bold_black_on_a_white_bar);
    RUN_TEST(test_a_hold_tick_paints_only_the_bar_and_never_its_track);
    RUN_TEST(test_a_scroll_paints_only_the_band);
    RUN_TEST(test_a_move_paints_only_its_two_rows_and_the_image_column);
    RUN_TEST(test_a_marquee_step_paints_only_the_highlighted_row_offscreen);
    RUN_TEST(test_images_landing_on_a_detail_page_paint_only_the_images);
    RUN_TEST(test_partial_draws_add_up_to_a_full_draw);
    RUN_TEST(test_the_fake_measures_a_fixed_advance_per_glyph);
    RUN_TEST(test_the_fake_paints_a_round_fill_inside_its_bounds);
    RUN_TEST(test_the_fake_rejects_a_draw_outside_its_offscreen_row);
    return UNITY_END();
}
