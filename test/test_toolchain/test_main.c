#include <unity.h>

#include "peanut_gb.h"

void setUp(void)
{
}

void tearDown(void)
{
}

static void test_endian_autodetects_little(void)
{
    TEST_ASSERT_EQUAL_INT(1, PEANUT_GB_IS_LITTLE_ENDIAN);
}

static void test_gb_s_struct_is_nonzero(void)
{
    TEST_ASSERT_GREATER_THAN(0, sizeof(struct gb_s));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_endian_autodetects_little);
    RUN_TEST(test_gb_s_struct_is_nonzero);
    return UNITY_END();
}
