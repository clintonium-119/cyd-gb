#include <unity.h>

#include <string.h>

#include "cart/match.h"

#define CANARY 0xC5
#define GUARD 4
#define OUT_SZ 64

/* "Super Tetris.gb" comes first so a legacy substring candidate always sits
 * ahead of the exact one — the ordering that makes "exact first" observable. */
static const char* const listing[] = {
    "Super Tetris.gb",
    "Tetris.gb",
    "Dr. Mario.gb",
};
#define LISTING_N (sizeof(listing) / sizeof(listing[0]))

static struct {
    unsigned char pre[GUARD];
    char out[OUT_SZ];
    unsigned char post[GUARD];
} g;

void setUp(void)
{
    memset(g.pre, CANARY, sizeof(g.pre));
    memset(g.post, CANARY, sizeof(g.post));
    memset(g.out, 0x7F, sizeof(g.out));
}

void tearDown(void)
{
}

static void assert_canaries_intact(void)
{
    size_t i;
    for (i = 0; i < GUARD; i++) {
        TEST_ASSERT_EQUAL_HEX8(CANARY, g.pre[i]);
        TEST_ASSERT_EQUAL_HEX8(CANARY, g.post[i]);
    }
}

static void assert_normalises_to(const char* in, const char* expected)
{
    TEST_ASSERT_EQUAL_INT(MATCH_OK, match_normalise(in, g.out, sizeof(g.out)));
    TEST_ASSERT_EQUAL_STRING(expected, g.out);
    assert_canaries_intact();
}

static void test_exact_match_beats_an_earlier_substring_candidate(void)
{
    size_t idx = (size_t)-1;

    TEST_ASSERT_EQUAL_INT(MATCH_OK,
        match_find("Tetris.gb", listing, LISTING_N, &idx));
    TEST_ASSERT_EQUAL_size_t(1, idx);
}

static void test_exact_pass_is_case_sensitive_and_falls_to_legacy(void)
{
    size_t idx = (size_t)-1;

    /* "tetris.gb" is not byte-equal to "Tetris.gb", so the exact pass misses
     * and the legacy pass takes the first entry that contains it. */
    TEST_ASSERT_EQUAL_INT(MATCH_OK,
        match_find("tetris.gb", listing, LISTING_N, &idx));
    TEST_ASSERT_EQUAL_size_t(0, idx);
}

static void test_legacy_hit_returns_the_first_index_in_listing_order(void)
{
    size_t idx = (size_t)-1;

    TEST_ASSERT_EQUAL_INT(MATCH_OK,
        match_find("tetris", listing, LISTING_N, &idx));
    TEST_ASSERT_EQUAL_size_t(0, idx);
}

static void test_find_over_an_empty_listing_leaves_the_index_untouched(void)
{
    size_t idx = (size_t)-1;

    TEST_ASSERT_EQUAL_INT(MATCH_NOT_FOUND,
        match_find("Tetris.gb", listing, 0, &idx));
    TEST_ASSERT_EQUAL_size_t((size_t)-1, idx);
}

static void test_find_with_no_match_at_all(void)
{
    size_t idx = (size_t)-1;

    TEST_ASSERT_EQUAL_INT(MATCH_NOT_FOUND,
        match_find("Zelda", listing, LISTING_N, &idx));
    TEST_ASSERT_EQUAL_size_t((size_t)-1, idx);
}

static void test_normalise_trims_and_lowercases(void)
{
    assert_normalises_to(" TETRIS ", "tetris.gb");
}

static void test_normalise_does_not_double_the_suffix(void)
{
    assert_normalises_to("tetris.GB", "tetris.gb");
}

static void test_normalise_keeps_the_dotted_library_names_intact(void)
{
    assert_normalises_to("Snow Bros. Jr..gb", "snow bros. jr..gb");
    assert_normalises_to("Dr. Mario", "dr. mario.gb");
    assert_normalises_to("Super R.C. Pro-Am.gb", "super r.c. pro-am.gb");
    assert_normalises_to("Mr. Do!.gb", "mr. do!.gb");
}

static void test_normalise_rejects_empty_and_whitespace_only(void)
{
    TEST_ASSERT_EQUAL_INT(MATCH_ERR_ARGS,
        match_normalise("", g.out, sizeof(g.out)));
    TEST_ASSERT_EQUAL_CHAR('\0', g.out[0]);
    TEST_ASSERT_EQUAL_INT(MATCH_ERR_ARGS,
        match_normalise("   \t\r\n ", g.out, sizeof(g.out)));
    TEST_ASSERT_EQUAL_CHAR('\0', g.out[0]);
    assert_canaries_intact();
}

static void test_normalise_into_a_too_small_buffer_is_rejected(void)
{
    /* "tetris.gb" needs ten bytes with its NUL; nine is one short. */
    TEST_ASSERT_EQUAL_INT(MATCH_ERR_ARGS, match_normalise("Tetris", g.out, 9));
    TEST_ASSERT_EQUAL_CHAR('\0', g.out[0]);

    /* A zero-sized output is never written, not even the NUL — so re-fill the
     * buffer first, or the NUL from the call above would be read as proof. */
    memset(g.out, 0x7F, sizeof(g.out));
    TEST_ASSERT_EQUAL_INT(MATCH_ERR_ARGS, match_normalise("Tetris", g.out, 0));
    TEST_ASSERT_EQUAL_HEX8(0x7F, (unsigned char)g.out[0]);
    assert_canaries_intact();
}

static void test_normalise_rejects_null_arguments(void)
{
    TEST_ASSERT_EQUAL_INT(MATCH_ERR_ARGS,
        match_normalise(NULL, g.out, sizeof(g.out)));
    TEST_ASSERT_EQUAL_INT(MATCH_ERR_ARGS, match_normalise("Tetris", NULL, 8));
    assert_canaries_intact();
}

static void test_legacy_predicate(void)
{
    TEST_ASSERT_TRUE(match_legacy("mario.gb", "Dr. Mario.gb"));
    TEST_ASSERT_FALSE(match_legacy("mario.gb", "Tetris.gb"));
    TEST_ASSERT_FALSE(match_legacy("mario.gb", NULL));
    TEST_ASSERT_FALSE(match_legacy(NULL, "Dr. Mario.gb"));
    TEST_ASSERT_FALSE(match_legacy("", "Dr. Mario.gb"));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_exact_match_beats_an_earlier_substring_candidate);
    RUN_TEST(test_exact_pass_is_case_sensitive_and_falls_to_legacy);
    RUN_TEST(test_legacy_hit_returns_the_first_index_in_listing_order);
    RUN_TEST(test_find_over_an_empty_listing_leaves_the_index_untouched);
    RUN_TEST(test_find_with_no_match_at_all);
    RUN_TEST(test_normalise_trims_and_lowercases);
    RUN_TEST(test_normalise_does_not_double_the_suffix);
    RUN_TEST(test_normalise_keeps_the_dotted_library_names_intact);
    RUN_TEST(test_normalise_rejects_empty_and_whitespace_only);
    RUN_TEST(test_normalise_into_a_too_small_buffer_is_rejected);
    RUN_TEST(test_normalise_rejects_null_arguments);
    RUN_TEST(test_legacy_predicate);
    return UNITY_END();
}
