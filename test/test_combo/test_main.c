#include <unity.h>

#include "input/combo.h"

void setUp(void)
{
}

void tearDown(void)
{
}

static void test_init_yields_clean_state(void)
{
    combo_state_t s;
    s.stable_word = 0xFFFFu;
    s.raw_word = 0xFFFFu;
    s.last_change_ms = 0xFFFFFFFFu;
    TEST_ASSERT_EQUAL_INT(COMBO_OK, combo_init(&s));
    TEST_ASSERT_EQUAL_HEX16(0, s.stable_word);
    TEST_ASSERT_EQUAL_HEX16(0, s.raw_word);
    TEST_ASSERT_EQUAL_UINT32(0, s.last_change_ms);
}

static void test_init_rejects_null(void)
{
    TEST_ASSERT_EQUAL_INT(COMBO_ERR_ARGS, combo_init(NULL));
}

static void test_update_stub_reports_no_events_for_any_word(void)
{
    combo_state_t s;
    uint16_t words[] = { 0x0000u, 0x0001u, 0x8000u, 0xFFFFu };
    size_t i;
    TEST_ASSERT_EQUAL_INT(COMBO_OK, combo_init(&s));
    for (i = 0; i < sizeof(words) / sizeof(words[0]); i++) {
        uint8_t event = 0xFFu;
        TEST_ASSERT_EQUAL_INT(COMBO_ERR_NOT_IMPLEMENTED,
            combo_update(&s, words[i], (uint32_t)(i * 10), &event));
        TEST_ASSERT_EQUAL_UINT8(COMBO_EVENT_NONE, event);
    }
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_init_yields_clean_state);
    RUN_TEST(test_init_rejects_null);
    RUN_TEST(test_update_stub_reports_no_events_for_any_word);
    return UNITY_END();
}
