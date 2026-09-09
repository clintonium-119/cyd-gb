#include <unity.h>

#include <stdint.h>

/* With ENABLE_SOUND on, peanut_gb.h calls these and expects the including
 * translation unit to provide them. This suite only inspects the header's
 * compile-time behaviour and never runs the emulator, so the definitions are
 * stubs; the real APU wiring lives in the headless runner. */
uint8_t audio_read(uint16_t addr);
void audio_write(uint16_t addr, uint8_t val);

#include "peanut_gb.h"

uint8_t audio_read(uint16_t addr)
{
    (void)addr;
    return 0xFF;
}

void audio_write(uint16_t addr, uint8_t val)
{
    (void)addr;
    (void)val;
}

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
