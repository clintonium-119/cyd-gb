#include <unity.h>

#include <stdio.h>
#include <string.h>

#include "cart/catalog.h"

#define DESC_SZ 256

/* ---- injected readers ------------------------------------------------- */

typedef struct {
    const char* data;
    size_t len;
    int fail;
} mem_src_t;

static int mem_read(void* ctx, uint32_t off, void* dst, size_t cap,
                    size_t* got)
{
    mem_src_t* s = (mem_src_t*)ctx;
    size_t n;

    if (s->fail) {
        return -1;
    }
    if (off >= s->len) {
        *got = 0;
        return 0;
    }
    n = s->len - off;
    if (n > cap) {
        n = cap;
    }
    memcpy(dst, s->data + off, n);
    *got = n;
    return 0;
}

static int file_read(void* ctx, uint32_t off, void* dst, size_t cap,
                     size_t* got)
{
    FILE* f = (FILE*)ctx;

    if (fseek(f, (long)off, SEEK_SET) != 0) {
        return -1;
    }
    *got = fread(dst, 1, cap, f);
    if (*got == 0 && ferror(f)) {
        return -1;
    }
    return 0;
}

static catalog_reader_t mem_reader(mem_src_t* s, const char* data)
{
    catalog_reader_t rd;
    s->data = data;
    s->len = strlen(data);
    s->fail = 0;
    rd.ctx = s;
    rd.read = mem_read;
    return rd;
}

/* The whole-file index is ~19 KB — static, never on the stack. */
static catalog_index_t idx;
static char gen[16384];
static char desc[DESC_SZ];

void setUp(void)
{
    memset(&idx, 0, sizeof(idx));
    memset(desc, 0x7F, sizeof(desc));
}

void tearDown(void)
{
}

/* ---- line parsing ------------------------------------------------------ */

static void test_parse_well_formed_line(void)
{
    static const char line[] = "Tetris.gb\tTetris\tstarter\tFit the blocks.";
    catalog_entry_t e;
    const char* d = NULL;
    size_t dlen = 0;

    TEST_ASSERT_EQUAL_INT(CATALOG_OK,
        catalog_parse_line(line, strlen(line), &e, &d, &dlen));
    TEST_ASSERT_EQUAL_STRING("Tetris.gb", e.filename);
    TEST_ASSERT_EQUAL_STRING("Tetris", e.title);
    TEST_ASSERT_EQUAL_HEX8(CATALOG_FLAG_STARTER, e.flags);
    TEST_ASSERT_EQUAL_size_t(strlen("Fit the blocks."), dlen);
    TEST_ASSERT_EQUAL_MEMORY("Fit the blocks.", d, dlen);
    /* parse_line does not know where the line came from. */
    TEST_ASSERT_EQUAL_UINT32(0, e.offset);
}

static void test_parse_line_with_empty_flags(void)
{
    static const char line[] = "Alleyway.gb\tAlleyway\t\tBreak the bricks.";
    catalog_entry_t e;
    const char* d = NULL;
    size_t dlen = 0;

    TEST_ASSERT_EQUAL_INT(CATALOG_OK,
        catalog_parse_line(line, strlen(line), &e, &d, &dlen));
    TEST_ASSERT_EQUAL_HEX8(0, e.flags);
    TEST_ASSERT_EQUAL_size_t(strlen("Break the bricks."), dlen);
    TEST_ASSERT_EQUAL_MEMORY("Break the bricks.", d, dlen);
}

static void test_parse_line_with_empty_description(void)
{
    static const char line[] = "Alleyway.gb\tAlleyway\t\t";
    catalog_entry_t e;
    const char* d = NULL;
    size_t dlen = 0;

    TEST_ASSERT_EQUAL_INT(CATALOG_OK,
        catalog_parse_line(line, strlen(line), &e, &d, &dlen));
    TEST_ASSERT_EQUAL_STRING("Alleyway.gb", e.filename);
    TEST_ASSERT_EQUAL_size_t(0, dlen);
}

static void test_unknown_flag_tokens_are_ignored(void)
{
    static const char line[] = "T.gb\tT\tfuture,starter,other\tD.";
    catalog_entry_t e;

    TEST_ASSERT_EQUAL_INT(CATALOG_OK,
        catalog_parse_line(line, strlen(line), &e, NULL, NULL));
    TEST_ASSERT_EQUAL_HEX8(CATALOG_FLAG_STARTER, e.flags);
}

static void test_too_few_tabs_is_a_line_error(void)
{
    static const char line[] = "Tetris.gb\tTetris\tstarter";
    catalog_entry_t e;

    TEST_ASSERT_EQUAL_INT(CATALOG_ERR_LINE,
        catalog_parse_line(line, strlen(line), &e, NULL, NULL));
}

static void test_over_long_filename_is_a_line_error(void)
{
    char line[256];
    catalog_entry_t e;
    size_t i;

    for (i = 0; i < ROM_STORE_NAME_MAX; i++) {
        line[i] = 'a';
    }
    memcpy(line + ROM_STORE_NAME_MAX, "\tT\t\tD.", 7);
    TEST_ASSERT_EQUAL_INT(CATALOG_ERR_LINE,
        catalog_parse_line(line, strlen(line), &e, NULL, NULL));
}

static void test_over_long_title_is_a_line_error(void)
{
    char line[256];
    catalog_entry_t e;
    size_t n;

    memcpy(line, "T.gb\t", 5);
    n = 5;
    while (n < 5 + CATALOG_TITLE_MAX) {
        line[n] = 'a';
        n++;
    }
    memcpy(line + n, "\t\tD.", 5);
    TEST_ASSERT_EQUAL_INT(CATALOG_ERR_LINE,
        catalog_parse_line(line, strlen(line), &e, NULL, NULL));
}

static void test_parse_line_rejects_null_arguments(void)
{
    catalog_entry_t e;

    TEST_ASSERT_EQUAL_INT(CATALOG_ERR_ARGS,
        catalog_parse_line(NULL, 0, &e, NULL, NULL));
    TEST_ASSERT_EQUAL_INT(CATALOG_ERR_ARGS,
        catalog_parse_line("a\tb\tc\td", 7, NULL, NULL, NULL));
}

/* ---- index building ---------------------------------------------------- */

static const char two_line_lf[] =
    "Tetris.gb\tTetris\tstarter\tBlocks.\n"
    "Dr. Mario.gb\tDr. Mario\t\tPills.\n";

static const char two_line_crlf[] =
    "Tetris.gb\tTetris\tstarter\tBlocks.\r\n"
    "Dr. Mario.gb\tDr. Mario\t\tPills.\r\n";

static void test_crlf_parses_identically_to_lf(void)
{
    static catalog_index_t crlf;
    mem_src_t s;
    catalog_reader_t rd;

    rd = mem_reader(&s, two_line_lf);
    TEST_ASSERT_EQUAL_INT(CATALOG_OK, catalog_index_build(&rd, &idx));
    rd = mem_reader(&s, two_line_crlf);
    TEST_ASSERT_EQUAL_INT(CATALOG_OK, catalog_index_build(&rd, &crlf));

    TEST_ASSERT_EQUAL_size_t(2, idx.count);
    TEST_ASSERT_EQUAL_size_t(2, crlf.count);
    TEST_ASSERT_EQUAL_STRING(idx.e[0].filename, crlf.e[0].filename);
    TEST_ASSERT_EQUAL_STRING(idx.e[0].title, crlf.e[0].title);
    TEST_ASSERT_EQUAL_HEX8(idx.e[0].flags, crlf.e[0].flags);
    TEST_ASSERT_EQUAL_STRING(idx.e[1].filename, crlf.e[1].filename);
    TEST_ASSERT_EQUAL_STRING(idx.e[1].title, crlf.e[1].title);
}

static void test_index_offsets_survive_chunk_boundaries(void)
{
    static uint32_t expect[40];
    mem_src_t s;
    catalog_reader_t rd;
    size_t n = 0;
    size_t i;
    int written;

    /* Lines of varying length so entries land either side of every
     * CATALOG_LINE_MAX-sized read the reader is asked for. */
    for (i = 0; i < 40; i++) {
        expect[i] = (uint32_t)n;
        written = snprintf(gen + n, sizeof(gen) - n,
                           "game%02u.gb\tGame %02u\t%s\t%.*s\n",
                           (unsigned)i, (unsigned)i,
                           (i % 3 == 0) ? "starter" : "",
                           (int)(20 + (i * 7) % 160),
                           "description text that is repeated to pad the line "
                           "out to a varying length so that lines straddle "
                           "every read boundary the reader is given, again and "
                           "again until the catalog is several chunks long.");
        TEST_ASSERT_TRUE(written > 0);
        n += (size_t)written;
    }
    TEST_ASSERT_TRUE(n > 3 * CATALOG_LINE_MAX);
    gen[n] = '\0';

    rd = mem_reader(&s, gen);
    TEST_ASSERT_EQUAL_INT(CATALOG_OK, catalog_index_build(&rd, &idx));
    TEST_ASSERT_EQUAL_size_t(40, idx.count);
    for (i = 0; i < 40; i++) {
        char want[16];
        snprintf(want, sizeof(want), "game%02u.gb", (unsigned)i);
        TEST_ASSERT_EQUAL_UINT32(expect[i], idx.e[i].offset);
        TEST_ASSERT_EQUAL_STRING(want, idx.e[i].filename);
    }
}

static void test_index_build_past_capacity_is_full(void)
{
    mem_src_t s;
    catalog_reader_t rd;
    size_t n = 0;
    size_t i;

    for (i = 0; i < CATALOG_MAX + 1; i++) {
        n += (size_t)snprintf(gen + n, sizeof(gen) - n,
                              "g%03u.gb\tG%03u\t\td\n", (unsigned)i,
                              (unsigned)i);
    }
    gen[n] = '\0';

    rd = mem_reader(&s, gen);
    TEST_ASSERT_EQUAL_INT(CATALOG_ERR_FULL, catalog_index_build(&rd, &idx));
    TEST_ASSERT_EQUAL_size_t(CATALOG_MAX, idx.count);
    TEST_ASSERT_EQUAL_STRING("g000.gb", idx.e[0].filename);
    TEST_ASSERT_EQUAL_STRING("g159.gb", idx.e[CATALOG_MAX - 1].filename);
}

static void test_over_long_line_is_rejected_and_indexes_nothing(void)
{
    mem_src_t s;
    catalog_reader_t rd;
    size_t i;

    /* A line that reaches CATALOG_LINE_MAX without a newline is malformed. */
    for (i = 0; i < CATALOG_LINE_MAX + 16; i++) {
        gen[i] = 'a';
    }
    gen[i] = '\0';

    rd = mem_reader(&s, gen);
    TEST_ASSERT_EQUAL_INT(CATALOG_ERR_LINE, catalog_index_build(&rd, &idx));
    TEST_ASSERT_EQUAL_size_t(0, idx.count);
}

static void test_reader_errors_propagate(void)
{
    mem_src_t s;
    catalog_reader_t rd;
    catalog_entry_t e;

    rd = mem_reader(&s, two_line_lf);
    s.fail = 1;
    TEST_ASSERT_EQUAL_INT(CATALOG_ERR_IO, catalog_index_build(&rd, &idx));
    TEST_ASSERT_EQUAL_INT(CATALOG_ERR_IO,
        catalog_find(&rd, "Tetris.gb", &e));
    TEST_ASSERT_EQUAL_INT(CATALOG_ERR_IO,
        catalog_first_flagged(&rd, CATALOG_FLAG_STARTER, &e));
    TEST_ASSERT_EQUAL_INT(CATALOG_ERR_IO,
        catalog_read_desc(&rd, 0, desc, sizeof(desc)));
}

static void test_null_arguments_are_rejected(void)
{
    mem_src_t s;
    catalog_reader_t rd;
    catalog_reader_t no_fn;
    catalog_entry_t e;

    rd = mem_reader(&s, two_line_lf);
    no_fn.ctx = &s;
    no_fn.read = NULL;

    TEST_ASSERT_EQUAL_INT(CATALOG_ERR_ARGS, catalog_index_build(NULL, &idx));
    TEST_ASSERT_EQUAL_INT(CATALOG_ERR_ARGS, catalog_index_build(&no_fn, &idx));
    TEST_ASSERT_EQUAL_INT(CATALOG_ERR_ARGS, catalog_index_build(&rd, NULL));
    TEST_ASSERT_EQUAL_INT(CATALOG_ERR_ARGS, catalog_find(&rd, NULL, &e));
    TEST_ASSERT_EQUAL_INT(CATALOG_ERR_ARGS, catalog_find(&rd, "a", NULL));
    TEST_ASSERT_EQUAL_INT(CATALOG_ERR_ARGS,
        catalog_first_flagged(&rd, CATALOG_FLAG_STARTER, NULL));
    TEST_ASSERT_EQUAL_INT(CATALOG_ERR_ARGS,
        catalog_read_desc(&rd, 0, desc, 0));
    TEST_ASSERT_EQUAL_INT(CATALOG_ERR_ARGS,
        catalog_read_desc(&rd, 0, NULL, sizeof(desc)));
}

/* ---- lookups ----------------------------------------------------------- */

static void test_find_is_exact_and_case_sensitive(void)
{
    mem_src_t s;
    catalog_reader_t rd = mem_reader(&s, two_line_lf);
    catalog_entry_t e;

    TEST_ASSERT_EQUAL_INT(CATALOG_OK, catalog_find(&rd, "Dr. Mario.gb", &e));
    TEST_ASSERT_EQUAL_STRING("Dr. Mario", e.title);
    TEST_ASSERT_EQUAL_UINT32(strlen("Tetris.gb\tTetris\tstarter\tBlocks.\n"),
                             e.offset);

    TEST_ASSERT_EQUAL_INT(CATALOG_NOT_FOUND, catalog_find(&rd, "Nope.gb", &e));
    /* Byte-exact: a lowercased name is a different entry. */
    TEST_ASSERT_EQUAL_INT(CATALOG_NOT_FOUND,
        catalog_find(&rd, "dr. mario.gb", &e));
}

static void test_first_flagged(void)
{
    mem_src_t s;
    catalog_reader_t rd = mem_reader(&s, two_line_lf);
    catalog_entry_t e;

    TEST_ASSERT_EQUAL_INT(CATALOG_OK,
        catalog_first_flagged(&rd, CATALOG_FLAG_STARTER, &e));
    TEST_ASSERT_EQUAL_STRING("Tetris.gb", e.filename);
    TEST_ASSERT_EQUAL_UINT32(0, e.offset);

    TEST_ASSERT_EQUAL_INT(CATALOG_NOT_FOUND, catalog_first_flagged(&rd, 0x80, &e));
}

static void test_read_desc_truncates_and_terminates(void)
{
    mem_src_t s;
    catalog_reader_t rd = mem_reader(&s, two_line_lf);
    char small[5];

    TEST_ASSERT_EQUAL_INT(CATALOG_OK,
        catalog_read_desc(&rd, 0, desc, sizeof(desc)));
    TEST_ASSERT_EQUAL_STRING("Blocks.", desc);

    memset(small, 0x7F, sizeof(small));
    TEST_ASSERT_EQUAL_INT(CATALOG_OK,
        catalog_read_desc(&rd, 0, small, sizeof(small)));
    TEST_ASSERT_EQUAL_STRING("Bloc", small);
}

/* ---- the fixture on disk ------------------------------------------------ */

/* Which path opened the fixture, so the device-side suite can reuse it. */
static const char* fixture_path_used = NULL;

static FILE* open_fixture(void)
{
    FILE* f;

    f = fopen("test/fixtures/catalog.txt", "rb");
    if (f != NULL) {
        fixture_path_used = "working directory";
        return f;
    }
#ifdef PROJECT_DIR
    f = fopen(PROJECT_DIR "/test/fixtures/catalog.txt", "rb");
    if (f != NULL) {
        fixture_path_used = "PROJECT_DIR";
        return f;
    }
#endif
    return NULL;
}

static void test_fixture_file_builds_six_entries(void)
{
    catalog_reader_t rd;
    catalog_entry_t e;
    FILE* f = open_fixture();

    TEST_ASSERT_NOT_NULL_MESSAGE(f, "test/fixtures/catalog.txt not found");
    TEST_ASSERT_NOT_NULL(fixture_path_used);
    rd.ctx = f;
    rd.read = file_read;

    TEST_ASSERT_EQUAL_INT(CATALOG_OK, catalog_index_build(&rd, &idx));
    TEST_ASSERT_EQUAL_size_t(6, idx.count);
    TEST_ASSERT_EQUAL_STRING("Tetris.gb", idx.e[0].filename);
    TEST_ASSERT_EQUAL_STRING("Snow Bros. Jr..gb", idx.e[2].filename);
    TEST_ASSERT_EQUAL_STRING("Alleyway.gb", idx.e[5].filename);
    TEST_ASSERT_EQUAL_UINT32(0, idx.e[0].offset);

    /* Starters are the first two entries and nothing else. */
    TEST_ASSERT_EQUAL_HEX8(CATALOG_FLAG_STARTER, idx.e[0].flags);
    TEST_ASSERT_EQUAL_HEX8(CATALOG_FLAG_STARTER, idx.e[1].flags);
    TEST_ASSERT_EQUAL_HEX8(0, idx.e[2].flags);

    TEST_ASSERT_EQUAL_INT(CATALOG_OK,
        catalog_first_flagged(&rd, CATALOG_FLAG_STARTER, &e));
    TEST_ASSERT_EQUAL_STRING("Tetris.gb", e.filename);

    TEST_ASSERT_EQUAL_INT(CATALOG_OK, catalog_find(&rd, "Dr. Mario.gb", &e));
    TEST_ASSERT_EQUAL_STRING("Dr. Mario", e.title);
    TEST_ASSERT_EQUAL_HEX8(CATALOG_FLAG_STARTER, e.flags);
    TEST_ASSERT_EQUAL_INT(CATALOG_NOT_FOUND, catalog_find(&rd, "Nope.gb", &e));

    /* Every indexed offset reads back that line's own description, and the
     * last entry's is empty. */
    TEST_ASSERT_EQUAL_INT(CATALOG_OK,
        catalog_read_desc(&rd, idx.e[0].offset, desc, sizeof(desc)));
    TEST_ASSERT_EQUAL_MEMORY("Fit the falling blocks", desc, 22);
    TEST_ASSERT_EQUAL_INT(CATALOG_OK,
        catalog_read_desc(&rd, idx.e[4].offset, desc, sizeof(desc)));
    TEST_ASSERT_EQUAL_MEMORY("Dig tunnels", desc, 11);
    TEST_ASSERT_EQUAL_INT(CATALOG_OK,
        catalog_read_desc(&rd, idx.e[5].offset, desc, sizeof(desc)));
    TEST_ASSERT_EQUAL_STRING("", desc);

    fclose(f);
}

static void test_index_size_is_recorded(void)
{
    /* Around 19 KB: a static in the writer's translation unit, never in one
     * the emulator links at game time. */
    printf("sizeof(catalog_entry_t) = %u\n",
           (unsigned)sizeof(catalog_entry_t));
    printf("sizeof(catalog_index_t) = %u\n",
           (unsigned)sizeof(catalog_index_t));
    printf("fixture opened via: %s\n",
           fixture_path_used ? fixture_path_used : "(not opened)");
    TEST_ASSERT_TRUE(sizeof(catalog_index_t) < 24u * 1024u);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_parse_well_formed_line);
    RUN_TEST(test_parse_line_with_empty_flags);
    RUN_TEST(test_parse_line_with_empty_description);
    RUN_TEST(test_unknown_flag_tokens_are_ignored);
    RUN_TEST(test_too_few_tabs_is_a_line_error);
    RUN_TEST(test_over_long_filename_is_a_line_error);
    RUN_TEST(test_over_long_title_is_a_line_error);
    RUN_TEST(test_parse_line_rejects_null_arguments);
    RUN_TEST(test_crlf_parses_identically_to_lf);
    RUN_TEST(test_index_offsets_survive_chunk_boundaries);
    RUN_TEST(test_index_build_past_capacity_is_full);
    RUN_TEST(test_over_long_line_is_rejected_and_indexes_nothing);
    RUN_TEST(test_reader_errors_propagate);
    RUN_TEST(test_null_arguments_are_rejected);
    RUN_TEST(test_find_is_exact_and_case_sensitive);
    RUN_TEST(test_first_flagged);
    RUN_TEST(test_read_desc_truncates_and_terminates);
    RUN_TEST(test_fixture_file_builds_six_entries);
    RUN_TEST(test_index_size_is_recorded);
    return UNITY_END();
}
