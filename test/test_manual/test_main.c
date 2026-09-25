#include <unity.h>

#include <stdio.h>
#include <string.h>

#include "ui/manual.h"

/*
 * Manual reader suite: the .2bp format, band decoding, the LZ4 decoder, the
 * tile geometry, the navigation machine and the two row operations. Every
 * timestamp is a literal — nothing here reads a clock.
 *
 * test/fixtures/manual_two_pages.2bp is written by the Python encoder, so the
 * two sides of the format are checked against each other; the tool's suite
 * re-encodes the same pages and asserts the bytes still match. Its pages are
 * FIXTURE_MANUAL_PAGES in tools/tests/test_image_sd.py, which fixture_level()
 * below repeats. It was made from the project root with:
 *
 *   cd tools/tests && python -c "import sys; sys.path[:0] = ['..', '.']; import test_image_sd as t, image_sd; open('../../test/fixtures/manual_two_pages.2bp', 'wb').write(image_sd.encode_manual(t.FIXTURE_MANUAL_PAGES))"
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
    unsigned reads; /* calls made, to pin how often the card is seeked */
} mem_t;

static int mem_read(void* ctx, uint32_t off, void* dst, size_t cap,
                    size_t* got)
{
    mem_t* m = (mem_t*)ctx;
    size_t n;

    m->reads++;
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
    m->reads = 0;
    rd.ctx = m;
    rd.read = mem_read;
    return rd;
}

static void put16(uint8_t* p, uint16_t v)
{
    p[0] = (uint8_t)(v & 0xFF);
    p[1] = (uint8_t)(v >> 8);
}

static void put32(uint8_t* p, uint32_t v)
{
    put16(p, (uint16_t)(v & 0xFFFF));
    put16(p + 2, (uint16_t)(v >> 16));
}

static uint32_t get32(const uint8_t* p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

static uint16_t bands_of(uint16_t h)
{
    return (uint16_t)((h + MANUAL_BAND_ROWS - 1) / MANUAL_BAND_ROWS);
}

/* A literal-only LZ4 block of n zero bytes. Returns its length. */
static size_t zero_block(uint8_t* p, size_t n)
{
    size_t len = 0;
    size_t more;

    p[len++] = (uint8_t)((n < 15 ? n : 15) << 4);
    if (n >= 15) {
        for (more = n - 15; more >= 255; more -= 255) {
            p[len++] = 255;
        }
        p[len++] = (uint8_t)more;
    }
    memset(p + len, 0, n);
    return len + n;
}

/*
 * A header, a size table of `n` pages, their band tables and a zero-filled
 * literal block per band, all consistent. Returns the length.
 */
static size_t build(uint8_t* buf, uint16_t version, uint16_t count,
                    const uint16_t (*sizes)[2], uint16_t n)
{
    size_t table = MANUAL_HEADER_BYTES + (size_t)n * MANUAL_ENTRY_BYTES;
    size_t len = table;
    uint16_t i;
    uint16_t b;

    memcpy(buf, MANUAL_MAGIC, 4);
    put16(buf + 4, version);
    put16(buf + 6, count);
    for (i = 0; i < n; i++) {
        put16(buf + MANUAL_HEADER_BYTES + i * MANUAL_ENTRY_BYTES, sizes[i][0]);
        put16(buf + MANUAL_HEADER_BYTES + i * MANUAL_ENTRY_BYTES + 2,
              sizes[i][1]);
        len += (size_t)(bands_of(sizes[i][1]) + 1) * MANUAL_OFFSET_BYTES;
    }
    for (i = 0; i < n; i++) {
        uint16_t w = sizes[i][0];
        uint16_t h = sizes[i][1];

        for (b = 0; b < bands_of(h); b++) {
            size_t rows = (size_t)(h - b * MANUAL_BAND_ROWS);

            if (rows > MANUAL_BAND_ROWS) {
                rows = MANUAL_BAND_ROWS;
            }
            put32(buf + table, (uint32_t)len);
            table += MANUAL_OFFSET_BYTES;
            len += zero_block(buf + len, rows * ((w + 3u) / 4u));
        }
        put32(buf + table, (uint32_t)len);
        table += MANUAL_OFFSET_BYTES;
    }
    return len;
}

/* Room for two full-size pages: 2 x 30 bands of 16 x 133 bytes and their
 * literal-length bytes, plus the header and tables. */
static uint8_t big[2 * 30 * (16 * 133 + 16) + 512];

/* ─── the fixture on disk ─────────────────────────────────────────────────── */

static size_t load_fixture(uint8_t* buf, size_t cap)
{
    FILE* f = fopen("test/fixtures/manual_two_pages.2bp", "rb");
    size_t n;

#ifdef PROJECT_DIR
    if (f == NULL) {
        f = fopen(PROJECT_DIR "/test/fixtures/manual_two_pages.2bp", "rb");
    }
#endif
    TEST_ASSERT_NOT_NULL_MESSAGE(f, "manual_two_pages.2bp not found");
    n = fread(buf, 1, cap, f);
    fclose(f);
    return n;
}

/* The fixture's pixel (x, y) on page `page`, by the Python test's formulas. */
static uint8_t fixture_level(uint16_t page, uint16_t x, uint16_t y)
{
    return (uint8_t)((page == 0 ? x + y : 3 * x + y) % 4);
}

static void test_the_encoders_fixture_parses(void)
{
    uint8_t buf[128];
    size_t len = load_fixture(buf, sizeof buf);
    mem_t m;
    manual_reader_t rd = reader_over(&m, buf, len);
    manual_page_t pages[2];
    uint16_t count = 0;

    /* Three bytes a read, so every field straddles a short read. */
    m.chunk = 3;
    TEST_ASSERT_EQUAL_INT(MANUAL_OK, manual_header(&rd, &count));
    TEST_ASSERT_EQUAL_UINT16(2, count);
    TEST_ASSERT_EQUAL_INT(MANUAL_OK, manual_table(&rd, (uint32_t)len, WIN_W,
                                                  WIN_H, pages, count));
    TEST_ASSERT_EQUAL_UINT16(10, pages[0].w);
    TEST_ASSERT_EQUAL_UINT16(20, pages[0].h);
    TEST_ASSERT_EQUAL_UINT32(8 + 4 * 2, pages[0].offset);
    TEST_ASSERT_EQUAL_UINT16(5, pages[1].w);
    TEST_ASSERT_EQUAL_UINT16(3, pages[1].h);
    /* The first page is two bands, so three offsets. */
    TEST_ASSERT_EQUAL_UINT32(8 + 4 * 2 + 4 * 3, pages[1].offset);
    /* The blocks start right after the second page's two offsets. */
    TEST_ASSERT_EQUAL_UINT32(pages[1].offset + 4 * 2, get32(buf + pages[0].offset));
    TEST_ASSERT_EQUAL_UINT32(len, get32(buf + pages[1].offset + 4));
}

static void test_every_band_of_the_fixture_decodes_to_the_encoders_pixels(void)
{
    uint8_t buf[128];
    size_t len = load_fixture(buf, sizeof buf);
    mem_t m;
    manual_reader_t rd = reader_over(&m, buf, len);
    manual_page_t pages[2];
    uint16_t count = 0;
    uint16_t p;

    m.chunk = 3;
    TEST_ASSERT_EQUAL_INT(MANUAL_OK, manual_header(&rd, &count));
    TEST_ASSERT_EQUAL_INT(MANUAL_OK, manual_table(&rd, (uint32_t)len, WIN_W,
                                                  WIN_H, pages, count));
    for (p = 0; p < count; p++) {
        uint16_t stride = (uint16_t)((pages[p].w + 3) / 4);
        uint32_t offsets[3];
        uint16_t b;

        TEST_ASSERT_EQUAL_INT(MANUAL_OK,
            manual_band_offsets(&rd, (uint32_t)len, &pages[p], offsets, 3));
        for (b = 0; b < bands_of(pages[p].h); b++) {
            uint8_t scratch[64];
            uint8_t out[16 * 3 + 1];
            uint8_t want[16 * 3];
            uint16_t rows = (uint16_t)(pages[p].h - b * MANUAL_BAND_ROWS);
            uint16_t y;
            uint16_t x;

            if (rows > MANUAL_BAND_ROWS) {
                rows = MANUAL_BAND_ROWS;
            }
            memset(want, 0, sizeof want);
            for (y = 0; y < rows; y++) {
                for (x = 0; x < pages[p].w; x++) {
                    uint8_t v = fixture_level(
                        p, x, (uint16_t)(b * MANUAL_BAND_ROWS + y));
                    want[y * stride + x / 4] |=
                        (uint8_t)(v << (6 - 2 * (x % 4)));
                }
            }
            /* One spare byte, which a band of exactly its size leaves. */
            memset(out, 0xEE, sizeof out);
            TEST_ASSERT_EQUAL_INT(MANUAL_OK,
                manual_band(&rd, &pages[p], offsets, b, scratch,
                            sizeof scratch, out, sizeof out));
            TEST_ASSERT_EQUAL_HEX8_ARRAY(want, out, rows * stride);
            TEST_ASSERT_EQUAL_HEX8(0xEE, out[rows * stride]);
        }
    }
    /* The first page's second band is the short one. */
    TEST_ASSERT_EQUAL_UINT16(2, bands_of(pages[0].h));
}

/* The fixture, parsed, for the band refusals to damage. */
static uint8_t fx[128];
static size_t fx_len;
static manual_page_t fx_pages[2];

static manual_reader_t parsed_fixture(mem_t* m)
{
    manual_reader_t rd;
    uint16_t count = 0;

    fx_len = load_fixture(fx, sizeof fx);
    rd = reader_over(m, fx, fx_len);
    TEST_ASSERT_EQUAL_INT(MANUAL_OK, manual_header(&rd, &count));
    TEST_ASSERT_EQUAL_INT(MANUAL_OK, manual_table(&rd, (uint32_t)fx_len,
                                                  WIN_W, WIN_H, fx_pages,
                                                  count));
    return rd;
}

/* The page's offsets, then the band: the first refusal of the two. */
static int band_of_fixture(const manual_reader_t* rd, uint16_t page,
                           uint16_t band, size_t scratch_cap)
{
    uint32_t offsets[3];
    uint8_t scratch[64];
    uint8_t out[64];
    int rc = manual_band_offsets(rd, (uint32_t)fx_len, &fx_pages[page],
                                 offsets, 3);

    if (rc != MANUAL_OK) {
        return rc;
    }
    return manual_band(rd, &fx_pages[page], offsets, band, scratch,
                       scratch_cap, out, sizeof out);
}

static void test_band_offsets_that_decrease_are_a_format_error(void)
{
    mem_t m;
    manual_reader_t rd = parsed_fixture(&m);

    /* The first band's end before its start. */
    put32(fx + fx_pages[0].offset + 4, get32(fx + fx_pages[0].offset) - 1);
    TEST_ASSERT_EQUAL_INT(MANUAL_ERR_FORMAT, band_of_fixture(&rd, 0, 0, 64));
}

static void test_a_band_offset_past_the_file_is_a_format_error(void)
{
    mem_t m;
    manual_reader_t rd = parsed_fixture(&m);

    put32(fx + fx_pages[1].offset + 4, (uint32_t)fx_len + 1);
    TEST_ASSERT_EQUAL_INT(MANUAL_ERR_FORMAT, band_of_fixture(&rd, 1, 0, 64));
}

static void test_a_block_bigger_than_the_scratch_is_refused_unwritten(void)
{
    mem_t m;
    manual_reader_t rd = parsed_fixture(&m);
    uint32_t start = get32(fx + fx_pages[0].offset);
    uint32_t end = get32(fx + fx_pages[0].offset + 4);
    uint32_t offsets[3];
    uint8_t scratch[64];
    uint8_t out[64];
    uint8_t untouched[64];

    TEST_ASSERT_EQUAL_INT(MANUAL_OK,
        manual_band_offsets(&rd, (uint32_t)fx_len, &fx_pages[0], offsets, 3));
    memset(scratch, 0xEE, sizeof scratch);
    memset(out, 0xEE, sizeof out);
    memset(untouched, 0xEE, sizeof untouched);
    TEST_ASSERT_EQUAL_INT(MANUAL_ERR_FORMAT,
        manual_band(&rd, &fx_pages[0], offsets, 0, scratch,
                    end - start - 1, out, sizeof out));
    TEST_ASSERT_EQUAL_HEX8_ARRAY(untouched, scratch, sizeof scratch);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(untouched, out, sizeof out);
    /* One byte more and it fits. */
    TEST_ASSERT_EQUAL_INT(MANUAL_OK, band_of_fixture(&rd, 0, 0, end - start));
}

static void test_a_block_that_decodes_short_is_a_format_error(void)
{
    mem_t m;
    manual_reader_t rd = parsed_fixture(&m);
    uint32_t start = get32(fx + fx_pages[1].offset);

    /* The small page's one band as a 1-byte literal block: valid LZ4, but
     * not the band's 6 bytes. */
    fx[start] = 0x10;
    put32(fx + fx_pages[1].offset + 4, start + 2);
    TEST_ASSERT_EQUAL_INT(MANUAL_ERR_FORMAT, band_of_fixture(&rd, 1, 0, 64));
}

static void test_a_band_past_the_page_or_an_out_too_small_is_refused(void)
{
    mem_t m;
    manual_reader_t rd = parsed_fixture(&m);
    uint32_t offsets[3];
    uint8_t scratch[64];
    uint8_t out[64];

    TEST_ASSERT_EQUAL_INT(MANUAL_ERR_ARGS, band_of_fixture(&rd, 0, 2, 64));
    TEST_ASSERT_EQUAL_INT(MANUAL_OK,
        manual_band_offsets(&rd, (uint32_t)fx_len, &fx_pages[1], offsets, 3));
    /* The small page's band is 3 rows of 2 bytes. */
    TEST_ASSERT_EQUAL_INT(MANUAL_ERR_ARGS,
        manual_band(&rd, &fx_pages[1], offsets, 0, scratch, sizeof scratch,
                    out, 5));
}

static void test_offsets_with_no_room_for_the_whole_table_are_refused(void)
{
    mem_t m;
    manual_reader_t rd = parsed_fixture(&m);
    uint32_t offsets[3];

    /* The first page is two bands, so three offsets. */
    TEST_ASSERT_EQUAL_UINT16(2, manual_bands(&fx_pages[0]));
    TEST_ASSERT_EQUAL_INT(MANUAL_ERR_ARGS,
        manual_band_offsets(&rd, (uint32_t)fx_len, &fx_pages[0], offsets, 2));
}

static void test_a_table_is_one_read_and_a_band_one_more(void)
{
    /* A seek on the card costs as much as a block, so the table must not be
     * re-read per band. */
    static const uint16_t s[1][2] = { { 2 * WIN_W, 2 * WIN_H } };
    static uint8_t scratch[16 * 133 + 16];
    static uint8_t out[16 * 133];
    uint32_t offsets[31];
    mem_t m;
    size_t len = build(big, MANUAL_VERSION, 1, s, 1);
    manual_reader_t rd = reader_over(&m, big, len);
    manual_page_t pages[1];
    uint16_t b;

    TEST_ASSERT_EQUAL_INT(MANUAL_OK,
        manual_table(&rd, (uint32_t)len, WIN_W, WIN_H, pages, 1));
    m.reads = 0;
    TEST_ASSERT_EQUAL_INT(MANUAL_OK,
        manual_band_offsets(&rd, (uint32_t)len, &pages[0], offsets, 31));
    TEST_ASSERT_EQUAL_UINT(1, m.reads);
    for (b = 0; b < manual_bands(&pages[0]); b++) {
        TEST_ASSERT_EQUAL_INT(MANUAL_OK,
            manual_band(&rd, &pages[0], offsets, b, scratch, sizeof scratch,
                        out, sizeof out));
    }
    TEST_ASSERT_EQUAL_UINT(1 + 30, m.reads);
}

static void test_a_reader_that_fails_mid_band_is_an_io_error(void)
{
    mem_t m;
    manual_reader_t rd = parsed_fixture(&m);

    /* The file ends before the last block does. */
    m.len = fx_len - 1;
    TEST_ASSERT_EQUAL_INT(MANUAL_ERR_IO, band_of_fixture(&rd, 1, 0, 64));
}

/* ─── the LZ4 decoder ─────────────────────────────────────────────────────── */

static void test_lz4_decodes_a_literal_only_block(void)
{
    static const uint8_t block[4] = { 0x30, 'a', 'b', 'c' };
    uint8_t out[8];

    TEST_ASSERT_EQUAL_INT(3, manual_lz4_decode(block, sizeof block, out,
                                               sizeof out));
    TEST_ASSERT_EQUAL_MEMORY("abc", out, 3);
}

static void test_lz4_reads_a_literal_length_extension(void)
{
    /* 15 + 1 literals. */
    uint8_t block[2 + 16];
    uint8_t out[16];
    uint8_t i;

    block[0] = 0xF0;
    block[1] = 0x01;
    for (i = 0; i < 16; i++) {
        block[2 + i] = i;
    }
    TEST_ASSERT_EQUAL_INT(16, manual_lz4_decode(block, sizeof block, out,
                                                sizeof out));
    TEST_ASSERT_EQUAL_MEMORY(block + 2, out, 16);
}

/* One literal x, a match of 8 at offset 1 — a run — then a last y. */
static const uint8_t RUN[6] = { 0x14, 'x', 0x01, 0x00, 0x10, 'y' };

static void test_lz4_copies_an_overlapping_match_byte_by_byte(void)
{
    uint8_t out[16];

    TEST_ASSERT_EQUAL_INT(10, manual_lz4_decode(RUN, sizeof RUN, out,
                                                sizeof out));
    TEST_ASSERT_EQUAL_MEMORY("xxxxxxxxxy", out, 10);
}

static void test_lz4_reads_a_match_length_extension(void)
{
    /* A match of 15 + 5 + 4 = 24, then an empty last literal run. */
    static const uint8_t block[6] = { 0x1F, 'z', 0x01, 0x00, 0x05, 0x00 };
    uint8_t out[32];
    uint8_t want[25];

    memset(want, 'z', sizeof want);
    TEST_ASSERT_EQUAL_INT(25, manual_lz4_decode(block, sizeof block, out,
                                                sizeof out));
    TEST_ASSERT_EQUAL_MEMORY(want, out, sizeof want);
}

static void test_lz4_refuses_a_zero_offset(void)
{
    static const uint8_t block[6] = { 0x14, 'x', 0x00, 0x00, 0x10, 'y' };
    uint8_t out[16];

    TEST_ASSERT_EQUAL_INT(-1, manual_lz4_decode(block, sizeof block, out,
                                                sizeof out));
}

static void test_lz4_refuses_an_offset_before_the_start(void)
{
    static const uint8_t block[6] = { 0x14, 'x', 0x02, 0x00, 0x10, 'y' };
    uint8_t out[16];

    TEST_ASSERT_EQUAL_INT(-1, manual_lz4_decode(block, sizeof block, out,
                                                sizeof out));
}

static void test_lz4_refuses_output_past_cap_and_writes_nothing_past_it(void)
{
    uint8_t out[16];

    memset(out, 0xEE, sizeof out);
    TEST_ASSERT_EQUAL_INT(-1, manual_lz4_decode(RUN, sizeof RUN, out, 9));
    TEST_ASSERT_EQUAL_HEX8(0xEE, out[9]);
    /* A literal run past cap, too. */
    TEST_ASSERT_EQUAL_INT(-1, manual_lz4_decode(RUN + 4, 2, out, 0));
}

static void test_lz4_refuses_truncated_input(void)
{
    static const uint8_t short_literals[3] = { 0x30, 'a', 'b' };
    static const uint8_t short_offset[3] = { 0x14, 'x', 0x01 };
    static const uint8_t short_length[1] = { 0xF0 };
    uint8_t out[16];

    TEST_ASSERT_EQUAL_INT(-1, manual_lz4_decode(short_literals, 3, out, 16));
    TEST_ASSERT_EQUAL_INT(-1, manual_lz4_decode(short_offset, 3, out, 16));
    TEST_ASSERT_EQUAL_INT(-1, manual_lz4_decode(short_length, 1, out, 16));
    TEST_ASSERT_EQUAL_INT(-1, manual_lz4_decode(RUN, 0, out, 16));
}

static void test_lz4_refuses_a_block_that_ends_on_a_match(void)
{
    /* The last sequence has to be literals alone. */
    TEST_ASSERT_EQUAL_INT(-1, manual_lz4_decode(RUN, 4, (uint8_t[16]){ 0 },
                                                16));
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

static void test_a_version_1_header_is_a_format_error(void)
{
    static const uint16_t one[1][2] = { { 8, 1 } };
    mem_t m;
    size_t len = build(big, 1, 1, one, 1);
    manual_reader_t rd = reader_over(&m, big, len);
    uint16_t count;

    TEST_ASSERT_EQUAL_INT(MANUAL_ERR_FORMAT, manual_header(&rd, &count));
}

static void test_a_later_version_is_a_format_error(void)
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

static void test_a_file_one_byte_short_of_its_last_offset_is_a_size_error(void)
{
    static const uint16_t s[2][2] = { { 10, 20 }, { 5, 3 } };

    TEST_ASSERT_EQUAL_INT(MANUAL_ERR_SIZE, table_for(s, 2, -1));
}

static void test_a_file_one_byte_past_its_last_offset_is_a_size_error(void)
{
    static const uint16_t s[2][2] = { { 10, 20 }, { 5, 3 } };

    TEST_ASSERT_EQUAL_INT(MANUAL_ERR_SIZE, table_for(s, 2, +1));
}

static void test_a_file_shorter_than_its_size_table_is_a_size_error(void)
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

static void test_a_file_shorter_than_its_band_tables_is_a_size_error(void)
{
    /* The size table fits, but a full-height page's 31 offsets do not. */
    static const uint16_t s[1][2] = { { 8, 2 * WIN_H } };
    mem_t m;
    manual_reader_t rd;
    manual_page_t pages[1];

    build(big, MANUAL_VERSION, 1, s, 1);
    rd = reader_over(&m, big, sizeof big);
    TEST_ASSERT_EQUAL_INT(MANUAL_ERR_SIZE,
        manual_table(&rd, MANUAL_HEADER_BYTES + MANUAL_ENTRY_BYTES + 30 * 4,
                     WIN_W, WIN_H, pages, 1));
}

static void test_every_band_of_a_full_size_page_decodes(void)
{
    static const uint16_t s[1][2] = { { 2 * WIN_W, 2 * WIN_H } };
    static uint8_t scratch[16 * 133 + 16];
    static uint8_t out[16 * 133];
    mem_t m;
    size_t len = build(big, MANUAL_VERSION, 1, s, 1);
    manual_reader_t rd = reader_over(&m, big, len);
    manual_page_t pages[1];
    uint16_t b;

    uint32_t offsets[31];

    TEST_ASSERT_EQUAL_INT(MANUAL_OK,
        manual_table(&rd, (uint32_t)len, WIN_W, WIN_H, pages, 1));
    TEST_ASSERT_EQUAL_INT(MANUAL_OK,
        manual_band_offsets(&rd, (uint32_t)len, &pages[0], offsets, 31));
    for (b = 0; b < bands_of(2 * WIN_H); b++) {
        TEST_ASSERT_EQUAL_INT(MANUAL_OK,
            manual_band(&rd, &pages[0], offsets, b, scratch,
                        sizeof scratch, out, sizeof out));
    }
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

/* A machine reading page 0 at tile 0: opened, the buttons that opened it
 * released, and zoomed in from the overview it starts on. */
static manual_nav_t fresh(void)
{
    manual_nav_t nav;

    manual_nav_init(&nav, 4);
    manual_nav_input(&nav, NAV_PAGES, WIN_W, WIN_H, B_NONE, 0);
    manual_nav_input(&nav, NAV_PAGES, WIN_W, WIN_H, B_A, 0);
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

static void test_the_reader_opens_on_the_first_pages_overview(void)
{
    manual_nav_t nav;

    manual_nav_init(&nav, 4);
    TEST_ASSERT_TRUE(nav.overview);
    assert_at(&nav, 0, 0, 0);
}

static void test_an_a_held_at_init_waits_for_release_and_a_new_press(void)
{
    manual_nav_t nav;

    manual_nav_init(&nav, 4);
    TEST_ASSERT_EQUAL_UINT8(MANUAL_EVENT_NONE, in(&nav, B_A, 0));
    TEST_ASSERT_EQUAL_UINT8(MANUAL_EVENT_NONE, in(&nav, B_A, 50));
    TEST_ASSERT_TRUE(nav.overview);
    TEST_ASSERT_EQUAL_UINT8(MANUAL_EVENT_NONE, in(&nav, B_NONE, 60));
    TEST_ASSERT_EQUAL_UINT8(MANUAL_EVENT_REDRAW, in(&nav, B_A, 70));
    TEST_ASSERT_FALSE(nav.overview);
    assert_at(&nav, 0, 0, 0);
}

/* ─── row operations ──────────────────────────────────────────────────────── */

/* Four distinct colours, none of them symmetric, so a swap would show. */
static const uint16_t LEVELS[4] = { 0xFFFFu, 0xAD55u, 0x52AAu, 0x0000u };

static void test_expand_row_maps_each_level_from_an_unaligned_pixel(void)
{
    /* 00 01 10 11 | 11 10 01 00: pixels 3 .. 7 are 3 3 2 1 0. */
    static const uint8_t row[2] = { 0x1B, 0xE4 };
    const uint16_t want[5] = { LEVELS[3], LEVELS[3], LEVELS[2], LEVELS[1],
                               LEVELS[0] };
    uint16_t out[6];

    out[5] = 0x1234;
    manual_expand_row(row, 3, 5, LEVELS, out);
    TEST_ASSERT_EQUAL_HEX16_ARRAY(want, out, 5);
    TEST_ASSERT_EQUAL_HEX16(0x1234, out[5]);
}

static void test_decimate_takes_the_rounded_mean_of_each_cell(void)
{
    /*
     * Five pixels wide, so three cells, the last one only half on the page.
     * a: 1 0 | 3 3 | 0 (3 in the padding)    b: 0 2 | 0 0 | 1 (3 in the padding)
     * Means 0.75, 1.5 and 0.5 round to 1, 2 and 1: halves round up, and the
     * padding is not page, or the last cell would be 1.75 and round to 2.
     */
    static const uint8_t a[2] = { 0x4F, 0x30 };
    static const uint8_t b[2] = { 0x20, 0x70 };
    uint8_t out[1];

    manual_decimate_row(a, b, 5, out);
    TEST_ASSERT_EQUAL_HEX8(0x64, out[0]);
}

static void test_decimate_keeps_solid_black_and_white_cells(void)
{
    static const uint8_t a[1] = { 0xF0 };
    uint8_t out[1];

    /* 3 3 | 0 0 over the same row: black and white stay themselves. */
    manual_decimate_row(a, a, 4, out);
    TEST_ASSERT_EQUAL_HEX8(0xC0, out[0]);
}

static void test_decimate_takes_a_null_second_row_for_an_odd_last_row(void)
{
    static const uint8_t a[2] = { 0x42, 0x30 };
    uint8_t out[1];

    /* 1 0 | 0 2 | 0: means 0.5, 1 and 0. */
    manual_decimate_row(a, NULL, 5, out);
    TEST_ASSERT_EQUAL_HEX8(0x50, out[0]);
}

static void test_decimate_packs_a_wide_row_across_bytes(void)
{
    /* 18 pixels, black only at 16: output pixel 8, the first of byte 2,
     * is the mean of black and white, 1.5, rounded to dark grey. */
    static const uint8_t a[5] = { 0x00, 0x00, 0x00, 0x00, 0xC0 };
    uint8_t out[3];

    manual_decimate_row(a, NULL, 18, out);
    TEST_ASSERT_EQUAL_HEX8(0x00, out[0]);
    TEST_ASSERT_EQUAL_HEX8(0x00, out[1]);
    TEST_ASSERT_EQUAL_HEX8(0x80, out[2]);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_the_encoders_fixture_parses);
    RUN_TEST(test_every_band_of_the_fixture_decodes_to_the_encoders_pixels);
    RUN_TEST(test_band_offsets_that_decrease_are_a_format_error);
    RUN_TEST(test_a_band_offset_past_the_file_is_a_format_error);
    RUN_TEST(test_a_block_bigger_than_the_scratch_is_refused_unwritten);
    RUN_TEST(test_a_block_that_decodes_short_is_a_format_error);
    RUN_TEST(test_a_band_past_the_page_or_an_out_too_small_is_refused);
    RUN_TEST(test_a_reader_that_fails_mid_band_is_an_io_error);
    RUN_TEST(test_offsets_with_no_room_for_the_whole_table_are_refused);
    RUN_TEST(test_a_table_is_one_read_and_a_band_one_more);
    RUN_TEST(test_lz4_decodes_a_literal_only_block);
    RUN_TEST(test_lz4_reads_a_literal_length_extension);
    RUN_TEST(test_lz4_copies_an_overlapping_match_byte_by_byte);
    RUN_TEST(test_lz4_reads_a_match_length_extension);
    RUN_TEST(test_lz4_refuses_a_zero_offset);
    RUN_TEST(test_lz4_refuses_an_offset_before_the_start);
    RUN_TEST(test_lz4_refuses_output_past_cap_and_writes_nothing_past_it);
    RUN_TEST(test_lz4_refuses_truncated_input);
    RUN_TEST(test_lz4_refuses_a_block_that_ends_on_a_match);
    RUN_TEST(test_a_bad_magic_is_a_format_error);
    RUN_TEST(test_a_version_1_header_is_a_format_error);
    RUN_TEST(test_a_later_version_is_a_format_error);
    RUN_TEST(test_zero_pages_is_a_format_error);
    RUN_TEST(test_too_many_pages_is_a_format_error);
    RUN_TEST(test_the_largest_page_count_is_accepted);
    RUN_TEST(test_a_page_of_exactly_twice_the_window_is_accepted);
    RUN_TEST(test_a_page_wider_than_twice_the_window_is_a_format_error);
    RUN_TEST(test_a_page_taller_than_twice_the_window_is_a_format_error);
    RUN_TEST(test_a_page_of_zero_size_is_a_format_error);
    RUN_TEST(test_a_file_one_byte_short_of_its_last_offset_is_a_size_error);
    RUN_TEST(test_a_file_one_byte_past_its_last_offset_is_a_size_error);
    RUN_TEST(test_a_file_shorter_than_its_size_table_is_a_size_error);
    RUN_TEST(test_a_file_shorter_than_its_band_tables_is_a_size_error);
    RUN_TEST(test_every_band_of_a_full_size_page_decodes);
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
    RUN_TEST(test_the_reader_opens_on_the_first_pages_overview);
    RUN_TEST(test_an_a_held_at_init_waits_for_release_and_a_new_press);
    RUN_TEST(test_expand_row_maps_each_level_from_an_unaligned_pixel);
    RUN_TEST(test_decimate_takes_the_rounded_mean_of_each_cell);
    RUN_TEST(test_decimate_keeps_solid_black_and_white_cells);
    RUN_TEST(test_decimate_takes_a_null_second_row_for_an_odd_last_row);
    RUN_TEST(test_decimate_packs_a_wide_row_across_bytes);
    return UNITY_END();
}
