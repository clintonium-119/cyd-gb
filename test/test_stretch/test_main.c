#include <unity.h>

#include "audio/stretch.h"

/*
 * Frame stretch. The speaker pushes 548 samples a frame and pads by up to
 * SPEAKER_PAD_MAX when the DMA queue starves, so the cases that matter are
 * the identity (no pad, the common path) and a small stretch.
 */

#define FRAME 548u
#define PAD_MAX 56u

static uint8_t in_buf[FRAME];
static uint8_t out_buf[FRAME + PAD_MAX];

void setUp(void)
{
    unsigned i;

    /* A ramp: every interpolated value is then predictable by hand. */
    for (i = 0; i < FRAME; i++) {
        in_buf[i] = (uint8_t)(i & 0xFF);
    }
}

void tearDown(void)
{
}

static void test_rejects_bad_arguments(void)
{
    TEST_ASSERT_EQUAL_INT(STRETCH_ERR_ARGS,
                          stretch_mono(NULL, FRAME, out_buf, FRAME));
    TEST_ASSERT_EQUAL_INT(STRETCH_ERR_ARGS,
                          stretch_mono(in_buf, FRAME, NULL, FRAME));
    TEST_ASSERT_EQUAL_INT(STRETCH_ERR_ARGS,
                          stretch_mono(in_buf, 1, out_buf, FRAME));
    TEST_ASSERT_EQUAL_INT(STRETCH_ERR_ARGS,
                          stretch_mono(in_buf, FRAME, out_buf, 1));
}

/* The common path: no pad means no resampling cost and no drift. */
static void test_equal_lengths_copy_exactly(void)
{
    unsigned i;

    TEST_ASSERT_EQUAL_INT(STRETCH_OK,
                          stretch_mono(in_buf, FRAME, out_buf, FRAME));
    for (i = 0; i < FRAME; i++) {
        TEST_ASSERT_EQUAL_UINT8(in_buf[i], out_buf[i]);
    }
}

static void test_endpoints_are_preserved_when_stretched(void)
{
    TEST_ASSERT_EQUAL_INT(STRETCH_OK,
                          stretch_mono(in_buf, FRAME, out_buf,
                                       FRAME + PAD_MAX));
    TEST_ASSERT_EQUAL_UINT8(in_buf[0], out_buf[0]);
    TEST_ASSERT_EQUAL_UINT8(in_buf[FRAME - 1], out_buf[FRAME + PAD_MAX - 1]);
}

/*
 * A stretched ramp stays monotone and never steps by more than one input
 * sample's worth. A duplication-based stretch would hold a value and then
 * jump, which is the periodic artefact this module exists to avoid.
 */
static void test_stretched_ramp_stays_monotone(void)
{
    unsigned i;

    TEST_ASSERT_EQUAL_INT(STRETCH_OK,
                          stretch_mono(in_buf, 256, out_buf, 256 + 32));
    for (i = 1; i < 256 + 32; i++) {
        TEST_ASSERT_TRUE(out_buf[i] >= out_buf[i - 1]);
        TEST_ASSERT_TRUE(out_buf[i] - out_buf[i - 1] <= 1);
    }
}

/* Halfway between two samples of a two-point frame is their average. */
static void test_midpoint_interpolates(void)
{
    static const uint8_t two[2] = { 100, 200 };
    uint8_t out[3];

    TEST_ASSERT_EQUAL_INT(STRETCH_OK, stretch_mono(two, 2, out, 3));
    TEST_ASSERT_EQUAL_UINT8(100, out[0]);
    TEST_ASSERT_EQUAL_UINT8(150, out[1]);
    TEST_ASSERT_EQUAL_UINT8(200, out[2]);
}

/* Silence in, silence out: a stretched pause must not click. */
static void test_midscale_frame_stays_midscale(void)
{
    unsigned i;

    for (i = 0; i < FRAME; i++) {
        in_buf[i] = 128;
    }
    TEST_ASSERT_EQUAL_INT(STRETCH_OK,
                          stretch_mono(in_buf, FRAME, out_buf,
                                       FRAME + PAD_MAX));
    for (i = 0; i < FRAME + PAD_MAX; i++) {
        TEST_ASSERT_EQUAL_UINT8(128, out_buf[i]);
    }
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_rejects_bad_arguments);
    RUN_TEST(test_equal_lengths_copy_exactly);
    RUN_TEST(test_endpoints_are_preserved_when_stretched);
    RUN_TEST(test_stretched_ramp_stays_monotone);
    RUN_TEST(test_midpoint_interpolates);
    RUN_TEST(test_midscale_frame_stays_midscale);
    return UNITY_END();
}
