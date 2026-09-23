#include <unity.h>

#include <stdio.h>
#include <string.h>

#include "ui/manual.h"

/*
 * Manual reader suite: the .1bp format, the tile geometry, the navigation
 * machine and the two row operations. Every timestamp is a literal — nothing
 * here reads a clock.
 *
 * test/fixtures/manual_two_pages.1bp is written by the Python encoder, so the
 * two sides of the format are checked against each other; the tool's suite
 * re-encodes the same pages and asserts the bytes still match. It was made
 * from the project root with:
 *
 *   cd tools && python -c "import image_sd; open('../test/fixtures/manual_two_pages.1bp','wb').write(image_sd.encode_manual([(10, 3, bytes.fromhex('aa80aa80aa80')), (8, 2, bytes.fromhex('a53c'))]))"
 */

/* The game window the reader draws in. */
#define WIN_W 266
#define WIN_H 240

#define B_NONE  0x00u
#define B_UP    ((uint8_t)COMBO_BTN_UP)
#define B_DOWN  ((uint8_t)COMBO_BTN_DOWN)
#define B_LEFT  ((uint8_t)COMBO_BTN_LEFT)
#define B_RIGHT ((uint8_t)COMBO_BTN_RIGHT)
#define B_A     ((uint8_t)COMBO_BTN_A)
#define B_B     ((uint8_t)COMBO_BTN_B)

void setUp(void)
{
}

void tearDown(void)
{
}

/* ─── a memory-backed reader ──────────────────────────────────────────────── */

typedef struct mem_s {
    const uint8_t* data;
    size_t len;
    size_t chunk; /* the most one read hands back, to force short reads */
} mem_t;

static int mem_read(void* ctx, uint32_t off, void* dst, size_t cap,
                    size_t* got)
{
    mem_t* m = (mem_t*)ctx;
    size_t n;

    if (off >= m->len) {
        *got = 0;
        return 0;
    }
    n = m->len - off;
    if (n > cap) {
        n = cap;
    }
    if (n > m->chunk) {
        n = m->chunk;
    }
    memcpy(dst, m->data + off, n);
    *got = n;
    return 0;
}

static manual_reader_t reader_over(mem_t* m, const uint8_t* data, size_t len)
{
    manual_reader_t rd;

    m->data = data;
    m->len = len;
    m->chunk = len;
    rd.ctx = m;
    rd.read = mem_read;
    return rd;
}

static void put16(uint8_t* p, uint16_t v)
{
    p[0] = (uint8_t)(v & 0xFF);
    p[1] = (uint8_t)(v >> 8);
}

/* A header, a table of `n` pages and zeroed rasters. Returns the length. */
static size_t build(uint8_t* buf, uint16_t version, uint16_t count,
                    const uint16_t (*sizes)[2], uint16_t n)
{
    size_t len = MANUAL_HEADER_BYTES + (size_t)n * MANUAL_ENTRY_BYTES;
    uint16_t i;

    memcpy(buf, MANUAL_MAGIC, 4);
    put16(buf + 4, version);
    put16(buf + 6, count);
    for (i = 0; i < n; i++) {
        put16(buf + MANUAL_HEADER_BYTES + i * MANUAL_ENTRY_BYTES, sizes[i][0]);
        put16(buf + MANUAL_HEADER_BYTES + i * MANUAL_ENTRY_BYTES + 2,
              sizes[i][1]);
        len += (size_t)((sizes[i][0] + 7u) / 8u) * sizes[i][1];
    }
    return len;
}

/* Room for two full-size pages: 2 x (67 x 480) plus the header and table. */
static uint8_t big[2 * 67 * 480 + 64];

/* ─── the fixture on disk ─────────────────────────────────────────────────── */

static size_t load_fixture(uint8_t* buf, size_t cap)
{
    FILE* f = fopen("test/fixtures/manual_two_pages.1bp", "rb");
    size_t n;

#ifdef PROJECT_DIR
    if (f == NULL) {
        f = fopen(PROJECT_DIR "/test/fixtures/manual_two_pages.1bp", "rb");
    }
#endif
    TEST_ASSERT_NOT_NULL_MESSAGE(f, "manual_two_pages.1bp not found");
    n = fread(buf, 1, cap, f);
    fclose(f);
    return n;
}

static void test_the_encoders_fixture_parses(void)
{
    uint8_t buf[64];
    size_t len = load_fixture(buf, sizeof buf);
    mem_t m;
    manual_reader_t rd = reader_over(&m, buf, len);
    manual_page_t pages[2];
    uint16_t count = 0;

    /* Three bytes a read, so every field straddles a short read. */
    m.chunk = 3;
    TEST_ASSERT_EQUAL_size_t(8 + 4 * 2 + 2 * 3 + 1 * 2, len);
    TEST_ASSERT_EQUAL_INT(MANUAL_OK, manual_header(&rd, &count));
    TEST_ASSERT_EQUAL_UINT16(2, count);
    TEST_ASSERT_EQUAL_INT(MANUAL_OK, manual_table(&rd, (uint32_t)len, WIN_W,
                                                  WIN_H, pages, count));
    TEST_ASSERT_EQUAL_UINT16(10, pages[0].w);
    TEST_ASSERT_EQUAL_UINT16(3, pages[0].h);
    TEST_ASSERT_EQUAL_UINT32(8 + 4 * 2, pages[0].offset);
    TEST_ASSERT_EQUAL_UINT16(8, pages[1].w);
    TEST_ASSERT_EQUAL_UINT16(2, pages[1].h);
    /* The first page is 10 wide, so 2 bytes a row, 3 rows. */
    TEST_ASSERT_EQUAL_UINT32(8 + 4 * 2 + 2 * 3, pages[1].offset);
    TEST_ASSERT_EQUAL_HEX8(0xAA, buf[pages[0].offset]);
    TEST_ASSERT_EQUAL_HEX8(0xA5, buf[pages[1].offset]);
}

/* ─── refusals ────────────────────────────────────────────────────────────── */

static void test_a_bad_magic_is_a_format_error(void)
{
    static const uint16_t one[1][2] = { { 8, 1 } };
    mem_t m;
    size_t len = build(big, MANUAL_VERSION, 1, one, 1);
    manual_reader_t rd = reader_over(&m, big, len);
    uint16_t count;

    big[0] = 'X';
    TEST_ASSERT_EQUAL_INT(MANUAL_ERR_FORMAT, manual_header(&rd, &count));
}

static void test_a_wrong_version_is_a_format_error(void)
{
    static const uint16_t one[1][2] = { { 8, 1 } };
    mem_t m;
    size_t len = build(big, MANUAL_VERSION + 1, 1, one, 1);
    manual_reader_t rd = reader_over(&m, big, len);
    uint16_t count;

    TEST_ASSERT_EQUAL_INT(MANUAL_ERR_FORMAT, manual_header(&rd, &count));
}

static void test_zero_pages_is_a_format_error(void)
{
    mem_t m;
    size_t len = build(big, MANUAL_VERSION, 0, NULL, 0);
    manual_reader_t rd = reader_over(&m, big, len);
    uint16_t count;

    TEST_ASSERT_EQUAL_INT(MANUAL_ERR_FORMAT, manual_header(&rd, &count));
}

static void test_too_many_pages_is_a_format_error(void)
{
    mem_t m;
    size_t len = build(big, MANUAL_VERSION, MANUAL_MAX_PAGES + 1, NULL, 0);
    manual_reader_t rd = reader_over(&m, big, len);
    uint16_t count;

    TEST_ASSERT_EQUAL_INT(MANUAL_ERR_FORMAT, manual_header(&rd, &count));
}

static void test_the_largest_page_count_is_accepted(void)
{
    mem_t m;
    size_t len = build(big, MANUAL_VERSION, MANUAL_MAX_PAGES, NULL, 0);
    manual_reader_t rd = reader_over(&m, big, len);
    uint16_t count = 0;

    TEST_ASSERT_EQUAL_INT(MANUAL_OK, manual_header(&rd, &count));
    TEST_ASSERT_EQUAL_UINT16(MANUAL_MAX_PAGES, count);
}

static int table_for(const uint16_t (*sizes)[2], uint16_t n, long slack)
{
    mem_t m;
    size_t len = build(big, MANUAL_VERSION, n, sizes, n);
    manual_reader_t rd = reader_over(&m, big, sizeof big);
    manual_page_t pages[2];

    return manual_table(&rd, (uint32_t)((long)len + slack), WIN_W, WIN_H,
                        pages, n);
}

static void test_a_page_of_exactly_twice_the_window_is_accepted(void)
{
    static const uint16_t s[2][2] = { { 2 * WIN_W, 2 * WIN_H }, { 1, 1 } };

    TEST_ASSERT_EQUAL_INT(MANUAL_OK, table_for(s, 2, 0));
}

static void test_a_page_wider_than_twice_the_window_is_a_format_error(void)
{
    static const uint16_t s[2][2] = { { 8, 8 }, { 2 * WIN_W + 1, 8 } };

    TEST_ASSERT_EQUAL_INT(MANUAL_ERR_FORMAT, table_for(s, 2, 0));
}

static void test_a_page_taller_than_twice_the_window_is_a_format_error(void)
{
    static const uint16_t s[1][2] = { { 8, 2 * WIN_H + 1 } };

    TEST_ASSERT_EQUAL_INT(MANUAL_ERR_FORMAT, table_for(s, 1, 0));
}

static void test_a_page_of_zero_size_is_a_format_error(void)
{
    static const uint16_t s[1][2] = { { 0, 8 } };

    TEST_ASSERT_EQUAL_INT(MANUAL_ERR_FORMAT, table_for(s, 1, 0));
}

static void test_a_file_one_byte_short_is_a_size_error(void)
{
    static const uint16_t s[2][2] = { { 10, 3 }, { 8, 2 } };

    TEST_ASSERT_EQUAL_INT(MANUAL_ERR_SIZE, table_for(s, 2, -1));
}

static void test_a_file_one_byte_long_is_a_size_error(void)
{
    static const uint16_t s[2][2] = { { 10, 3 }, { 8, 2 } };

    TEST_ASSERT_EQUAL_INT(MANUAL_ERR_SIZE, table_for(s, 2, +1));
}

static void test_a_file_shorter_than_its_table_is_a_size_error(void)
{
    uint8_t buf[MANUAL_HEADER_BYTES + MANUAL_ENTRY_BYTES];
    static const uint16_t s[2][2] = { { 8, 1 }, { 8, 1 } };
    mem_t m;
    manual_reader_t rd;
    manual_page_t pages[2];

    build(big, MANUAL_VERSION, 2, s, 2);
    memcpy(buf, big, sizeof buf);
    rd = reader_over(&m, buf, sizeof buf);
    TEST_ASSERT_EQUAL_INT(MANUAL_ERR_SIZE,
        manual_table(&rd, sizeof buf, WIN_W, WIN_H, pages, 2));
}

/* ─── tile geometry ───────────────────────────────────────────────────────── */

static void test_a_full_width_page_is_two_by_two_with_clamped_origins(void)
{
    manual_page_t p = { 532, 394, 0 };
    uint8_t nx;
    uint8_t ny;
    uint16_t x0;
    uint16_t y0;

    manual_tiles(&p, WIN_W, WIN_H, &nx, &ny);
    TEST_ASSERT_EQUAL_UINT8(2, nx);
    TEST_ASSERT_EQUAL_UINT8(2, ny);
    manual_tile_origin(&p, WIN_W, WIN_H, 0, 0, &x0, &y0);
    TEST_ASSERT_EQUAL_UINT16(0, x0);
    TEST_ASSERT_EQUAL_UINT16(0, y0);
    /* 394 - 240 = 154: the bottom row is clamped to the page's edge. */
    manual_tile_origin(&p, WIN_W, WIN_H, 1, 1, &x0, &y0);
    TEST_ASSERT_EQUAL_UINT16(266, x0);
    TEST_ASSERT_EQUAL_UINT16(154, y0);
}

static void test_a_480_square_clamps_its_second_column(void)
{
    manual_page_t p = { 480, 480, 0 };
    uint8_t nx;
    uint8_t ny;
    uint16_t x0;
    uint16_t y0;

    manual_tiles(&p, WIN_W, WIN_H, &nx, &ny);
    TEST_ASSERT_EQUAL_UINT8(2, nx);
    TEST_ASSERT_EQUAL_UINT8(2, ny);
    /* 480 - 266 = 214; 480 - 240 = 240 is not a clamp. */
    manual_tile_origin(&p, WIN_W, WIN_H, 1, 1, &x0, &y0);
    TEST_ASSERT_EQUAL_UINT16(214, x0);
    TEST_ASSERT_EQUAL_UINT16(240, y0);
}

static void test_a_page_smaller_than_the_window_is_one_tile_at_zero(void)
{
    manual_page_t p = { 200, 150, 0 };
    uint8_t nx;
    uint8_t ny;
    uint16_t x0;
    uint16_t y0;

    manual_tiles(&p, WIN_W, WIN_H, &nx, &ny);
    TEST_ASSERT_EQUAL_UINT8(1, nx);
    TEST_ASSERT_EQUAL_UINT8(1, ny);
    manual_tile_origin(&p, WIN_W, WIN_H, 0, 0, &x0, &y0);
    TEST_ASSERT_EQUAL_UINT16(0, x0);
    TEST_ASSERT_EQUAL_UINT16(0, y0);
}

/* ─── navigation ──────────────────────────────────────────────────────────── */

/*
 * Four pages: two of 2 x 2 tiles, a 1 x 1, and a last page of 2 x 1, so every
 * turn lands on a page whose tile count differs from the one it left.
 */
static const manual_page_t NAV_PAGES[4] = {
    { 532, 394, 0 },
    { 532, 394, 0 },
    { 200, 200, 0 },
    { 500, 200, 0 },
};

/* A machine on page 0, tile 0, with the buttons that opened it released. */
static manual_nav_t fresh(void)
{
    manual_nav_t nav;

    manual_nav_init(&nav, 4);
    manual_nav_input(&nav, NAV_PAGES, WIN_W, WIN_H, B_NONE, 0);
    return nav;
}

static uint8_t in(manual_nav_t* nav, uint8_t buttons, uint32_t at_ms)
{
    return manual_nav_input(nav, NAV_PAGES, WIN_W, WIN_H, buttons, at_ms);
}

/* Press and release; the event the press produced. */
static uint8_t tap(manual_nav_t* nav, uint8_t buttons)
{
    uint8_t ev = in(nav, buttons, 0);

    in(nav, B_NONE, 0);
    return ev;
}

static void assert_at(const manual_nav_t* nav, uint16_t page, uint8_t tx,
                      uint8_t ty)
{
    TEST_ASSERT_EQUAL_UINT16(page, nav->page);
    TEST_ASSERT_EQUAL_UINT8(tx, nav->tx);
    TEST_ASSERT_EQUAL_UINT8(ty, nav->ty);
}

static void test_right_walks_the_tiles_then_turns_the_page(void)
{
    manual_nav_t nav = fresh();

    TEST_ASSERT_EQUAL_UINT8(MANUAL_EVENT_REDRAW, tap(&nav, B_RIGHT));
    assert_at(&nav, 0, 1, 0);
    TEST_ASSERT_EQUAL_UINT8(MANUAL_EVENT_REDRAW, tap(&nav, B_RIGHT));
    assert_at(&nav, 0, 0, 1);
    TEST_ASSERT_EQUAL_UINT8(MANUAL_EVENT_REDRAW, tap(&nav, B_RIGHT));
    assert_at(&nav, 0, 1, 1);
    TEST_ASSERT_EQUAL_UINT8(MANUAL_EVENT_REDRAW, tap(&nav, B_RIGHT));
    assert_at(&nav, 1, 0, 0);
}

static void test_left_walks_back_to_the_previous_pages_last_tile(void)
{
    manual_nav_t nav = fresh();

    nav.page = 1;
    TEST_ASSERT_EQUAL_UINT8(MANUAL_EVENT_REDRAW, tap(&nav, B_LEFT));
    assert_at(&nav, 0, 1, 1);
    TEST_ASSERT_EQUAL_UINT8(MANUAL_EVENT_REDRAW, tap(&nav, B_LEFT));
    assert_at(&nav, 0, 0, 1);
    TEST_ASSERT_EQUAL_UINT8(MANUAL_EVENT_REDRAW, tap(&nav, B_LEFT));
    assert_at(&nav, 0, 1, 0);
    TEST_ASSERT_EQUAL_UINT8(MANUAL_EVENT_REDRAW, tap(&nav, B_LEFT));
    assert_at(&nav, 0, 0, 0);
}

static void test_down_from_the_bottom_row_turns_to_the_first_tile(void)
{
    manual_nav_t nav = fresh();

    nav.tx = 1;
    TEST_ASSERT_EQUAL_UINT8(MANUAL_EVENT_REDRAW, tap(&nav, B_DOWN));
    assert_at(&nav, 0, 1, 1);
    TEST_ASSERT_EQUAL_UINT8(MANUAL_EVENT_REDRAW, tap(&nav, B_DOWN));
    assert_at(&nav, 1, 0, 0);
}

static void test_up_from_the_top_row_turns_to_the_last_tile(void)
{
    manual_nav_t nav = fresh();

    nav.page = 1;
    nav.ty = 1;
    TEST_ASSERT_EQUAL_UINT8(MANUAL_EVENT_REDRAW, tap(&nav, B_UP));
    assert_at(&nav, 1, 0, 0);
    TEST_ASSERT_EQUAL_UINT8(MANUAL_EVENT_REDRAW, tap(&nav, B_UP));
    assert_at(&nav, 0, 1, 1);
}

static void test_every_direction_turns_a_one_tile_page(void)
{
    manual_nav_t nav = fresh();

    nav.page = 2;
    tap(&nav, B_RIGHT);
    assert_at(&nav, 3, 0, 0);
    nav.page = 2;
    tap(&nav, B_DOWN);
    assert_at(&nav, 3, 0, 0);
    nav.page = 2;
    tap(&nav, B_LEFT);
    assert_at(&nav, 1, 1, 1);
    nav.page = 2;
    nav.tx = 0;
    nav.ty = 0;
    tap(&nav, B_UP);
    assert_at(&nav, 1, 1, 1);
}

static void test_the_first_page_clamps_on_left_and_up(void)
{
    manual_nav_t nav = fresh();

    TEST_ASSERT_EQUAL_UINT8(MANUAL_EVENT_NONE, tap(&nav, B_LEFT));
    assert_at(&nav, 0, 0, 0);
    TEST_ASSERT_EQUAL_UINT8(MANUAL_EVENT_NONE, tap(&nav, B_UP));
    assert_at(&nav, 0, 0, 0);
}

static void test_the_last_page_clamps_on_right_and_down(void)
{
    manual_nav_t nav = fresh();

    /* The last page is 2 x 1: its last tile is (1, 0). */
    nav.page = 3;
    nav.tx = 1;
    TEST_ASSERT_EQUAL_UINT8(MANUAL_EVENT_NONE, tap(&nav, B_RIGHT));
    assert_at(&nav, 3, 1, 0);
    TEST_ASSERT_EQUAL_UINT8(MANUAL_EVENT_NONE, tap(&nav, B_DOWN));
    assert_at(&nav, 3, 1, 0);
}

static void test_two_directions_at_once_do_nothing(void)
{
    manual_nav_t nav = fresh();

    TEST_ASSERT_EQUAL_UINT8(MANUAL_EVENT_NONE,
                            tap(&nav, (uint8_t)(B_RIGHT | B_DOWN)));
    assert_at(&nav, 0, 0, 0);
}

static void test_a_held_direction_steps_once_while_reading(void)
{
    manual_nav_t nav = fresh();

    TEST_ASSERT_EQUAL_UINT8(MANUAL_EVENT_REDRAW, in(&nav, B_RIGHT, 1000));
    TEST_ASSERT_EQUAL_UINT8(MANUAL_EVENT_NONE, in(&nav, B_RIGHT, 1400));
    TEST_ASSERT_EQUAL_UINT8(MANUAL_EVENT_NONE, in(&nav, B_RIGHT, 1600));
    assert_at(&nav, 0, 1, 0);
}

static void test_a_returns_from_the_overview_to_the_tile_it_left(void)
{
    manual_nav_t nav = fresh();

    nav.tx = 1;
    nav.ty = 1;
    TEST_ASSERT_EQUAL_UINT8(MANUAL_EVENT_REDRAW, tap(&nav, B_A));
    TEST_ASSERT_TRUE(nav.overview);
    TEST_ASSERT_EQUAL_UINT8(MANUAL_EVENT_REDRAW, tap(&nav, B_A));
    TEST_ASSERT_FALSE(nav.overview);
    assert_at(&nav, 0, 1, 1);
}

static void test_a_page_turned_in_the_overview_returns_to_its_first_tile(void)
{
    manual_nav_t nav = fresh();

    nav.tx = 1;
    nav.ty = 1;
    tap(&nav, B_A);
    TEST_ASSERT_EQUAL_UINT8(MANUAL_EVENT_REDRAW, tap(&nav, B_RIGHT));
    TEST_ASSERT_EQUAL_UINT16(1, nav.page);
    tap(&nav, B_A);
    TEST_ASSERT_FALSE(nav.overview);
    assert_at(&nav, 1, 0, 0);
}

static void test_up_and_down_do_nothing_in_the_overview(void)
{
    manual_nav_t nav = fresh();

    tap(&nav, B_A);
    TEST_ASSERT_EQUAL_UINT8(MANUAL_EVENT_NONE, tap(&nav, B_DOWN));
    TEST_ASSERT_EQUAL_UINT8(MANUAL_EVENT_NONE, tap(&nav, B_UP));
    assert_at(&nav, 0, 0, 0);
}

static void test_a_held_right_in_the_overview_repeats_at_the_combo_cadence(void)
{
    manual_nav_t nav = fresh();

    tap(&nav, B_A);
    TEST_ASSERT_EQUAL_UINT8(MANUAL_EVENT_REDRAW, in(&nav, B_RIGHT, 1000));
    TEST_ASSERT_EQUAL_UINT16(1, nav.page);
    /* 1000 + COMBO_REPEAT_DELAY_MS (400) = 1400 */
    TEST_ASSERT_EQUAL_UINT8(MANUAL_EVENT_NONE, in(&nav, B_RIGHT, 1399));
    TEST_ASSERT_EQUAL_UINT8(MANUAL_EVENT_REDRAW, in(&nav, B_RIGHT, 1400));
    TEST_ASSERT_EQUAL_UINT16(2, nav.page);
    /* 1400 + COMBO_REPEAT_MS (200) = 1600 */
    TEST_ASSERT_EQUAL_UINT8(MANUAL_EVENT_NONE, in(&nav, B_RIGHT, 1599));
    TEST_ASSERT_EQUAL_UINT8(MANUAL_EVENT_REDRAW, in(&nav, B_RIGHT, 1600));
    TEST_ASSERT_EQUAL_UINT16(3, nav.page);
    /* The last page clamps: the repeat comes due and turns nothing. */
    TEST_ASSERT_EQUAL_UINT8(MANUAL_EVENT_NONE, in(&nav, B_RIGHT, 1800));
    TEST_ASSERT_EQUAL_UINT16(3, nav.page);
}

static void test_b_exits_from_reading(void)
{
    manual_nav_t nav = fresh();

    TEST_ASSERT_EQUAL_UINT8(MANUAL_EVENT_EXIT, tap(&nav, B_B));
}

static void test_b_exits_from_the_overview(void)
{
    manual_nav_t nav = fresh();

    tap(&nav, B_A);
    TEST_ASSERT_EQUAL_UINT8(MANUAL_EVENT_EXIT, tap(&nav, B_B));
}

static void test_an_a_held_at_init_waits_for_release_and_a_new_press(void)
{
    manual_nav_t nav;

    manual_nav_init(&nav, 4);
    TEST_ASSERT_EQUAL_UINT8(MANUAL_EVENT_NONE, in(&nav, B_A, 0));
    TEST_ASSERT_EQUAL_UINT8(MANUAL_EVENT_NONE, in(&nav, B_A, 50));
    TEST_ASSERT_FALSE(nav.overview);
    TEST_ASSERT_EQUAL_UINT8(MANUAL_EVENT_NONE, in(&nav, B_NONE, 60));
    TEST_ASSERT_EQUAL_UINT8(MANUAL_EVENT_REDRAW, in(&nav, B_A, 70));
    TEST_ASSERT_TRUE(nav.overview);
}

/* ─── row operations ──────────────────────────────────────────────────────── */

#define INK 0x0000u
#define PAPER 0xFFFFu

static void test_expand_row_starts_at_an_unaligned_bit(void)
{
    /* Bits 3 .. 9 of 1011 0000 | 0100 0000 are 1 0 0 0 0 0 1. */
    static const uint8_t bits[2] = { 0xB0, 0x40 };
    static const uint16_t want[7] = { INK, PAPER, PAPER, PAPER,
                                      PAPER, PAPER, INK };
    uint16_t out[8];

    out[7] = 0x1234;
    manual_expand_row(bits, 3, 7, INK, PAPER, out);
    TEST_ASSERT_EQUAL_HEX16_ARRAY(want, out, 7);
    TEST_ASSERT_EQUAL_HEX16(0x1234, out[7]);
}

static void test_decimate_blackens_a_cell_with_any_black_pixel(void)
{
    /*
     * Five pixels wide, so three cells, the last one only half on the page.
     * a: 1 0 | 0 0 | 0 (1 in the padding)    b: 0 0 | 0 1 | 0
     * Cell 0 has a's first pixel, cell 1 has b's second, and cell 2 has
     * only the padding bit, which is not page.
     */
    static const uint8_t a[1] = { 0x84 };
    static const uint8_t b[1] = { 0x10 };
    uint8_t out[1];

    manual_decimate_row(a, b, 5, out);
    TEST_ASSERT_EQUAL_HEX8(0xC0, out[0]);
}

static void test_decimate_takes_a_null_second_row_for_an_odd_last_row(void)
{
    static const uint8_t a[1] = { 0x84 };
    uint8_t out[1];

    manual_decimate_row(a, NULL, 5, out);
    TEST_ASSERT_EQUAL_HEX8(0x80, out[0]);
}

static void test_decimate_packs_a_wide_row_across_bytes(void)
{
    /* 18 pixels, black only at 16: output pixel 8, the first of byte 1. */
    static const uint8_t a[3] = { 0x00, 0x00, 0x80 };
    uint8_t out[2];

    manual_decimate_row(a, NULL, 18, out);
    TEST_ASSERT_EQUAL_HEX8(0x00, out[0]);
    TEST_ASSERT_EQUAL_HEX8(0x80, out[1]);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_the_encoders_fixture_parses);
    RUN_TEST(test_a_bad_magic_is_a_format_error);
    RUN_TEST(test_a_wrong_version_is_a_format_error);
    RUN_TEST(test_zero_pages_is_a_format_error);
    RUN_TEST(test_too_many_pages_is_a_format_error);
    RUN_TEST(test_the_largest_page_count_is_accepted);
    RUN_TEST(test_a_page_of_exactly_twice_the_window_is_accepted);
    RUN_TEST(test_a_page_wider_than_twice_the_window_is_a_format_error);
    RUN_TEST(test_a_page_taller_than_twice_the_window_is_a_format_error);
    RUN_TEST(test_a_page_of_zero_size_is_a_format_error);
    RUN_TEST(test_a_file_one_byte_short_is_a_size_error);
    RUN_TEST(test_a_file_one_byte_long_is_a_size_error);
    RUN_TEST(test_a_file_shorter_than_its_table_is_a_size_error);
    RUN_TEST(test_a_full_width_page_is_two_by_two_with_clamped_origins);
    RUN_TEST(test_a_480_square_clamps_its_second_column);
    RUN_TEST(test_a_page_smaller_than_the_window_is_one_tile_at_zero);
    RUN_TEST(test_right_walks_the_tiles_then_turns_the_page);
    RUN_TEST(test_left_walks_back_to_the_previous_pages_last_tile);
    RUN_TEST(test_down_from_the_bottom_row_turns_to_the_first_tile);
    RUN_TEST(test_up_from_the_top_row_turns_to_the_last_tile);
    RUN_TEST(test_every_direction_turns_a_one_tile_page);
    RUN_TEST(test_the_first_page_clamps_on_left_and_up);
    RUN_TEST(test_the_last_page_clamps_on_right_and_down);
    RUN_TEST(test_two_directions_at_once_do_nothing);
    RUN_TEST(test_a_held_direction_steps_once_while_reading);
    RUN_TEST(test_a_returns_from_the_overview_to_the_tile_it_left);
    RUN_TEST(test_a_page_turned_in_the_overview_returns_to_its_first_tile);
    RUN_TEST(test_up_and_down_do_nothing_in_the_overview);
    RUN_TEST(test_a_held_right_in_the_overview_repeats_at_the_combo_cadence);
    RUN_TEST(test_b_exits_from_reading);
    RUN_TEST(test_b_exits_from_the_overview);
    RUN_TEST(test_an_a_held_at_init_waits_for_release_and_a_new_press);
    RUN_TEST(test_expand_row_starts_at_an_unaligned_bit);
    RUN_TEST(test_decimate_blackens_a_cell_with_any_black_pixel);
    RUN_TEST(test_decimate_takes_a_null_second_row_for_an_odd_last_row);
    RUN_TEST(test_decimate_packs_a_wide_row_across_bytes);
    return UNITY_END();
}
