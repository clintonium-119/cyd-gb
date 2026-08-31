#include <unity.h>

#include "render/scaler.h"

#define CANARY 0xC5C5u
#define GUARD 4

/* Output rows wrapped in canary guards so out-of-bounds writes are caught. */
static struct {
    uint16_t pre[GUARD];
    uint16_t row[SCALER_DST_W];
    uint16_t post[GUARD];
} dst0, dst1, dst2;

static uint16_t src0[SCALER_SRC_W];
static uint16_t src1[SCALER_SRC_W];

void setUp(void)
{
    size_t i;
    for (i = 0; i < SCALER_SRC_W; i++) {
        src0[i] = (uint16_t)i;
        src1[i] = (uint16_t)(0x8000u | i);
    }
    for (i = 0; i < GUARD; i++) {
        dst0.pre[i] = dst0.post[i] = CANARY;
        dst1.pre[i] = dst1.post[i] = CANARY;
        dst2.pre[i] = dst2.post[i] = CANARY;
    }
}

void tearDown(void)
{
}

static void assert_canaries_intact(void)
{
    size_t i;
    for (i = 0; i < GUARD; i++) {
        TEST_ASSERT_EQUAL_HEX16(CANARY, dst0.pre[i]);
        TEST_ASSERT_EQUAL_HEX16(CANARY, dst0.post[i]);
        TEST_ASSERT_EQUAL_HEX16(CANARY, dst1.pre[i]);
        TEST_ASSERT_EQUAL_HEX16(CANARY, dst1.post[i]);
        TEST_ASSERT_EQUAL_HEX16(CANARY, dst2.pre[i]);
        TEST_ASSERT_EQUAL_HEX16(CANARY, dst2.post[i]);
    }
}

static void test_pair_scales_to_expected_nearest_neighbour_triple(void)
{
    size_t k;
    TEST_ASSERT_EQUAL_INT(SCALER_OK,
        scaler_scale_pair(src0, src1, dst0.row, dst1.row, dst2.row));

    /* Each source pair (a, b) becomes (a, a, b); the middle output row
     * duplicates the top one under the nearest-neighbour placeholder. */
    for (k = 0; k * 2 + 1 < SCALER_SRC_W; k++) {
        uint16_t a0 = src0[k * 2];
        uint16_t b0 = src0[k * 2 + 1];
        uint16_t a1 = src1[k * 2];
        uint16_t b1 = src1[k * 2 + 1];
        TEST_ASSERT_EQUAL_HEX16(a0, dst0.row[k * 3]);
        TEST_ASSERT_EQUAL_HEX16(a0, dst0.row[k * 3 + 1]);
        TEST_ASSERT_EQUAL_HEX16(b0, dst0.row[k * 3 + 2]);
        TEST_ASSERT_EQUAL_HEX16(dst0.row[k * 3], dst1.row[k * 3]);
        TEST_ASSERT_EQUAL_HEX16(dst0.row[k * 3 + 1], dst1.row[k * 3 + 1]);
        TEST_ASSERT_EQUAL_HEX16(dst0.row[k * 3 + 2], dst1.row[k * 3 + 2]);
        TEST_ASSERT_EQUAL_HEX16(a1, dst2.row[k * 3]);
        TEST_ASSERT_EQUAL_HEX16(a1, dst2.row[k * 3 + 1]);
        TEST_ASSERT_EQUAL_HEX16(b1, dst2.row[k * 3 + 2]);
    }
    assert_canaries_intact();
}

static void test_null_buffers_are_rejected(void)
{
    TEST_ASSERT_EQUAL_INT(SCALER_ERR_ARGS,
        scaler_scale_pair(NULL, src1, dst0.row, dst1.row, dst2.row));
    TEST_ASSERT_EQUAL_INT(SCALER_ERR_ARGS,
        scaler_scale_pair(src0, src1, dst0.row, NULL, dst2.row));
    assert_canaries_intact();
}

static void test_avg565_placeholder_returns_first_argument(void)
{
    TEST_ASSERT_EQUAL_HEX16(0x1234u, scaler_avg565(0x1234u, 0xFFFFu));
    TEST_ASSERT_EQUAL_HEX16(0x0000u, scaler_avg565(0x0000u, 0xF7DEu));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_pair_scales_to_expected_nearest_neighbour_triple);
    RUN_TEST(test_null_buffers_are_rejected);
    RUN_TEST(test_avg565_placeholder_returns_first_argument);
    return UNITY_END();
}
