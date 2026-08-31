#include <unity.h>

#include <string.h>

#include "cart/ndef.h"

#define CANARY 0xC5
#define GUARD 4
#define OUT_SZ 64

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

static void test_empty_input_returns_not_implemented(void)
{
    TEST_ASSERT_EQUAL_INT(NDEF_ERR_NOT_IMPLEMENTED,
        ndef_parse_text(NULL, 0, g.out, sizeof(g.out)));
    TEST_ASSERT_EQUAL_CHAR('\0', g.out[0]);
    assert_canaries_intact();
}

static void test_tiny_input_returns_not_implemented(void)
{
    const uint8_t buf[] = { 0xD1 };
    TEST_ASSERT_EQUAL_INT(NDEF_ERR_NOT_IMPLEMENTED,
        ndef_parse_text(buf, sizeof(buf), g.out, sizeof(g.out)));
    TEST_ASSERT_EQUAL_CHAR('\0', g.out[0]);
    assert_canaries_intact();
}

static void test_max_size_input_never_overruns_output(void)
{
    static uint8_t buf[512];
    memset(buf, 0xAB, sizeof(buf));
    TEST_ASSERT_EQUAL_INT(NDEF_ERR_NOT_IMPLEMENTED,
        ndef_parse_text(buf, sizeof(buf), g.out, sizeof(g.out)));
    TEST_ASSERT_EQUAL_CHAR('\0', g.out[0]);
    assert_canaries_intact();
}

static void test_zero_sized_output_is_never_written(void)
{
    const uint8_t buf[] = { 0xD1, 0x01, 0x00 };
    TEST_ASSERT_EQUAL_INT(NDEF_ERR_NOT_IMPLEMENTED,
        ndef_parse_text(buf, sizeof(buf), g.out, 0));
    TEST_ASSERT_EQUAL_HEX8(0x7F, (unsigned char)g.out[0]);
    assert_canaries_intact();
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_empty_input_returns_not_implemented);
    RUN_TEST(test_tiny_input_returns_not_implemented);
    RUN_TEST(test_max_size_input_never_overruns_output);
    RUN_TEST(test_zero_sized_output_is_never_written);
    return UNITY_END();
}
