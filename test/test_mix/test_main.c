#include <unity.h>

#include <string.h>

#include "audio/mix.h"

#define GUARD 4
#define N_SAMPLES 32

static struct {
    int16_t pre[GUARD];
    int16_t out[N_SAMPLES];
    int16_t post[GUARD];
} g;

static int16_t left[N_SAMPLES];
static int16_t right[N_SAMPLES];

void setUp(void)
{
    size_t i;
    for (i = 0; i < GUARD; i++) {
        g.pre[i] = g.post[i] = (int16_t)0x5C5C;
    }
    for (i = 0; i < N_SAMPLES; i++) {
        g.out[i] = 0x7EED;
        left[i] = (int16_t)(i * 100);
        right[i] = (int16_t)(-(int)i * 100);
    }
}

void tearDown(void)
{
}

static void test_stub_returns_not_implemented(void)
{
    TEST_ASSERT_EQUAL_INT(MIX_ERR_NOT_IMPLEMENTED,
        mix_mono(left, right, N_SAMPLES, 128, g.out));
}

static void test_stub_never_writes_output_buffer(void)
{
    size_t i;
    TEST_ASSERT_EQUAL_INT(MIX_ERR_NOT_IMPLEMENTED,
        mix_mono(left, right, N_SAMPLES, 255, g.out));
    for (i = 0; i < N_SAMPLES; i++) {
        TEST_ASSERT_EQUAL_INT16(0x7EED, g.out[i]);
    }
    for (i = 0; i < GUARD; i++) {
        TEST_ASSERT_EQUAL_INT16((int16_t)0x5C5C, g.pre[i]);
        TEST_ASSERT_EQUAL_INT16((int16_t)0x5C5C, g.post[i]);
    }
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_stub_returns_not_implemented);
    RUN_TEST(test_stub_never_writes_output_buffer);
    return UNITY_END();
}
