#include <unity.h>

#include <string.h>

#include "cart/match.h"

#define CANARY 0xC5
#define GUARD 4
#define OUT_SZ 32

static const char* const listing[] = {
    "TETRIS.GB",
    "Super Mario Land (World).gb",
    "pokemon_red.gb",
};

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

static void test_find_stub_returns_not_found_over_listing(void)
{
    size_t idx = (size_t)-1;
    TEST_ASSERT_EQUAL_INT(MATCH_NOT_FOUND,
        match_find("TETRIS", listing,
                   sizeof(listing) / sizeof(listing[0]), &idx));
    /* Stub never matches, so it must not have written a winning index. */
    TEST_ASSERT_EQUAL_size_t((size_t)-1, idx);
}

static void test_normalise_stub_respects_output_bound(void)
{
    TEST_ASSERT_EQUAL_INT(MATCH_ERR_NOT_IMPLEMENTED,
        match_normalise("Super Mario Land (World).gb", g.out, sizeof(g.out)));
    TEST_ASSERT_EQUAL_CHAR('\0', g.out[0]);
    assert_canaries_intact();
}

static void test_normalise_zero_sized_output_is_never_written(void)
{
    TEST_ASSERT_EQUAL_INT(MATCH_ERR_NOT_IMPLEMENTED,
        match_normalise("TETRIS", g.out, 0));
    TEST_ASSERT_EQUAL_HEX8(0x7F, (unsigned char)g.out[0]);
    assert_canaries_intact();
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_find_stub_returns_not_found_over_listing);
    RUN_TEST(test_normalise_stub_respects_output_bound);
    RUN_TEST(test_normalise_zero_sized_output_is_never_written);
    return UNITY_END();
}
