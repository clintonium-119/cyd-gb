#include <unity.h>

#include <stdlib.h>
#include <string.h>

#include "audio/mix.h"

/*
 * Mixer: interleaved stereo int16 in, unsigned 8-bit mono out.
 *
 * The reference below repeats the production arithmetic on purpose — these
 * cases pin the shape of the result (silence is exact, rounding is to
 * nearest, the steps keep their ratios), not the expression that produces it.
 */

#define GUARD 8
#define N_FRAMES 4096

static struct {
    uint8_t pre[GUARD];
    uint8_t out[N_FRAMES];
    uint8_t post[GUARD];
} g;

static int16_t stereo[N_FRAMES * 2];

static const uint16_t lut[MIX_VOL_HIGH + 1] = { 0, 24, 102, 432 };

/* Test-side generator, deliberately not libc's. */
static uint32_t rng_state;

static uint32_t rng_next(void)
{
    rng_state ^= rng_state << 7;
    rng_state ^= rng_state >> 9;
    rng_state ^= rng_state << 8;
    return rng_state;
}

void setUp(void)
{
    memset(g.pre, 0x5C, sizeof(g.pre));
    memset(g.post, 0x5C, sizeof(g.post));
    memset(g.out, 0x7E, sizeof(g.out));
    memset(stereo, 0, sizeof(stereo));
    rng_state = 0xC0FFEEu;
}

void tearDown(void)
{
}

static void assert_guards_intact(void)
{
    size_t i;
    for (i = 0; i < GUARD; i++) {
        TEST_ASSERT_EQUAL_HEX8(0x5C, g.pre[i]);
        TEST_ASSERT_EQUAL_HEX8(0x5C, g.post[i]);
    }
}

static uint8_t reference(int16_t left, int16_t right, uint8_t vol_index)
{
    int32_t mono = ((int32_t)left + (int32_t)right) / 2;
    int32_t scaled = (mono * (int32_t)lut[vol_index]) / 256;
    uint32_t biased;

    if (scaled > 32767) {
        scaled = 32767;
    } else if (scaled < -32768) {
        scaled = -32768;
    }
    biased = ((uint32_t)(scaled + 32768) + 128u) >> 8;
    return (uint8_t)(biased > 255u ? 255u : biased);
}

static void fill_constant(int16_t left, int16_t right, size_t n_frames)
{
    size_t i;
    for (i = 0; i < n_frames; i++) {
        stereo[i * 2 + 0] = left;
        stereo[i * 2 + 1] = right;
    }
}

/* Total deviation from mid-scale over the buffer. Integer, because this
 * build of Unity has its floating-point assertions compiled out. */
static long long deviation_sum(size_t n_frames)
{
    size_t i;
    long long sum = 0;
    for (i = 0; i < n_frames; i++) {
        sum += (long long)g.out[i] - (long long)MIX_SILENCE;
    }
    return sum;
}

/* |sum / reference_sum - num / den| <= 0.02, cross-multiplied. */
static void assert_ratio_within_2_percent(long long sum, long long reference_sum,
                                          long long num, long long den)
{
    long long lhs = llabs(sum * den - reference_sum * num);
    long long tolerance = (reference_sum * den * 2) / 100;
    TEST_ASSERT_TRUE_MESSAGE(lhs <= tolerance,
        "volume step is not within 2% of its design ratio");
}

/* ─── silence and off ─────────────────────────────────────────────────────── */

static void test_zero_input_is_exactly_mid_scale_at_every_volume(void)
{
    uint8_t vol;
    size_t i;

    for (vol = MIX_VOL_OFF; vol <= MIX_VOL_HIGH; vol++) {
        memset(g.out, 0x7E, sizeof(g.out));
        TEST_ASSERT_EQUAL_INT(MIX_OK, mix_mono(stereo, N_FRAMES, vol, g.out));
        for (i = 0; i < N_FRAMES; i++) {
            TEST_ASSERT_EQUAL_HEX8(MIX_SILENCE, g.out[i]);
        }
    }
    assert_guards_intact();
}

static void test_off_is_mid_scale_for_a_loud_signal(void)
{
    size_t i;

    fill_constant(-32768, 32767, N_FRAMES);
    fill_constant(32767, -32767, N_FRAMES / 2);

    TEST_ASSERT_EQUAL_INT(MIX_OK,
        mix_mono(stereo, N_FRAMES, MIX_VOL_OFF, g.out));
    for (i = 0; i < N_FRAMES; i++) {
        TEST_ASSERT_EQUAL_HEX8(MIX_SILENCE, g.out[i]);
    }
    assert_guards_intact();
}

/* ─── clamping ────────────────────────────────────────────────────────────── */

static void test_full_scale_input_clamps_at_high_without_wrapping(void)
{
    size_t i;

    /* High's gain is past unity, so this input is well over the clamp. */
    fill_constant(32767, 32767, N_FRAMES);
    TEST_ASSERT_EQUAL_INT(MIX_OK,
        mix_mono(stereo, N_FRAMES, MIX_VOL_HIGH, g.out));
    for (i = 0; i < N_FRAMES; i++) {
        TEST_ASSERT_EQUAL_UINT8(255, g.out[i]);
    }

    fill_constant(-32768, -32768, N_FRAMES);
    TEST_ASSERT_EQUAL_INT(MIX_OK,
        mix_mono(stereo, N_FRAMES, MIX_VOL_HIGH, g.out));
    for (i = 0; i < N_FRAMES; i++) {
        TEST_ASSERT_EQUAL_UINT8(0, g.out[i]);
    }
    assert_guards_intact();
}

/* ─── rounding ────────────────────────────────────────────────────────────── */

static void test_output_matches_the_reference_at_every_volume(void)
{
    uint8_t vol;
    size_t i;

    for (i = 0; i < N_FRAMES; i++) {
        stereo[i * 2 + 0] = (int16_t)(rng_next() & 0xFFFFu);
        stereo[i * 2 + 1] = (int16_t)(rng_next() & 0xFFFFu);
    }

    for (vol = MIX_VOL_LOW; vol <= MIX_VOL_HIGH; vol++) {
        TEST_ASSERT_EQUAL_INT(MIX_OK, mix_mono(stereo, N_FRAMES, vol, g.out));
        for (i = 0; i < N_FRAMES; i++) {
            TEST_ASSERT_EQUAL_UINT8(
                reference(stereo[i * 2 + 0], stereo[i * 2 + 1], vol), g.out[i]);
        }
    }
    assert_guards_intact();
}

static void test_half_an_output_step_rounds_up_and_less_rounds_down(void)
{
    /* At Low, a mono sample of 1344 scales to exactly 126 and 1376 to 129:
     * one either side of the half-step at 128. */
    stereo[0] = 1344;
    stereo[1] = 1344;
    stereo[2] = 1376;
    stereo[3] = 1376;

    TEST_ASSERT_EQUAL_INT(MIX_OK, mix_mono(stereo, 2, MIX_VOL_LOW, g.out));
    TEST_ASSERT_EQUAL_UINT8(128, g.out[0]);
    TEST_ASSERT_EQUAL_UINT8(129, g.out[1]);
}

/* ─── volume ratios ───────────────────────────────────────────────────────── */

static void test_volume_steps_keep_the_design_ratios(void)
{
    long long low, med, high;

    /* Loud enough that Low still spans many output steps, quiet enough that
     * High stays inside the clamp. */
    fill_constant(16384, 16384, N_FRAMES);

    TEST_ASSERT_EQUAL_INT(MIX_OK,
        mix_mono(stereo, N_FRAMES, MIX_VOL_LOW, g.out));
    low = deviation_sum(N_FRAMES);
    TEST_ASSERT_EQUAL_INT(MIX_OK,
        mix_mono(stereo, N_FRAMES, MIX_VOL_MED, g.out));
    med = deviation_sum(N_FRAMES);
    TEST_ASSERT_EQUAL_INT(MIX_OK,
        mix_mono(stereo, N_FRAMES, MIX_VOL_HIGH, g.out));
    high = deviation_sum(N_FRAMES);

    TEST_ASSERT_TRUE(high > med);
    TEST_ASSERT_TRUE(med > low);
    assert_ratio_within_2_percent(med, high, lut[MIX_VOL_MED], lut[MIX_VOL_HIGH]);
    assert_ratio_within_2_percent(low, high, lut[MIX_VOL_LOW], lut[MIX_VOL_HIGH]);
    assert_guards_intact();
}

/* ─── mono sum, not a channel pick ────────────────────────────────────────── */

static void test_left_only_and_right_only_give_the_same_output(void)
{
    static uint8_t left_out[N_FRAMES];

    fill_constant(12000, 0, N_FRAMES);
    TEST_ASSERT_EQUAL_INT(MIX_OK,
        mix_mono(stereo, N_FRAMES, MIX_VOL_HIGH, left_out));

    fill_constant(0, 12000, N_FRAMES);
    TEST_ASSERT_EQUAL_INT(MIX_OK,
        mix_mono(stereo, N_FRAMES, MIX_VOL_HIGH, g.out));

    TEST_ASSERT_EQUAL_HEX8_ARRAY(left_out, g.out, N_FRAMES);
    assert_guards_intact();
}

static void test_opposite_full_scale_channels_cancel_to_mid_scale(void)
{
    size_t i;

    for (i = 0; i < N_FRAMES; i++) {
        /* Both orderings, and the asymmetric int16 range, so a sum of -1
         * has to floor to zero rather than to -1. */
        stereo[i * 2 + 0] = (i & 1u) ? (int16_t)32767 : (int16_t)-32768;
        stereo[i * 2 + 1] = (i & 1u) ? (int16_t)-32768 : (int16_t)32767;
    }
    TEST_ASSERT_EQUAL_INT(MIX_OK,
        mix_mono(stereo, N_FRAMES, MIX_VOL_HIGH, g.out));
    for (i = 0; i < N_FRAMES; i++) {
        TEST_ASSERT_EQUAL_HEX8(MIX_SILENCE, g.out[i]);
    }
    assert_guards_intact();
}

/* ─── half-rate mix (fast-forward) ────────────────────────────────────────── */

static void assert_half_writes_exactly(size_t n_frames, size_t expect_out)
{
    size_t i;
    memset(g.out, 0x7E, sizeof(g.out));
    fill_constant(20000, 20000, n_frames);
    TEST_ASSERT_EQUAL_INT(MIX_OK,
        mix_mono_half(stereo, n_frames, MIX_VOL_HIGH, g.out));
    for (i = 0; i < expect_out; i++) {
        TEST_ASSERT_NOT_EQUAL(0x7E, g.out[i]);
    }
    for (i = expect_out; i < n_frames; i++) {
        TEST_ASSERT_EQUAL_HEX8(0x7E, g.out[i]);
    }
    assert_guards_intact();
}

static void test_half_writes_half_the_frames_and_drops_an_odd_one(void)
{
    assert_half_writes_exactly(8, 4);
    assert_half_writes_exactly(7, 3);
    assert_half_writes_exactly(1, 0);
}

static void test_half_matches_mix_mono_on_a_constant_input(void)
{
    static uint8_t full[64];
    static const int16_t levels[][2] = {
        { 0, 0 }, { 1, 0 }, { -1, 2 }, { 1234, -567 }, { 20000, 20000 },
        { -32768, -32768 }, { 32767, 32767 }, { 32767, -32768 },
    };
    size_t l;
    uint8_t v;

    for (l = 0; l < sizeof(levels) / sizeof(levels[0]); l++) {
        fill_constant(levels[l][0], levels[l][1], 64);
        for (v = 0; v <= MIX_VOL_HIGH; v++) {
            TEST_ASSERT_EQUAL_INT(MIX_OK, mix_mono(stereo, 64, v, full));
            TEST_ASSERT_EQUAL_INT(MIX_OK, mix_mono_half(stereo, 64, v, g.out));
            TEST_ASSERT_EQUAL_HEX8_ARRAY(full, g.out, 32);
        }
    }
    assert_guards_intact();
}

static void test_half_averages_each_pair_of_frames(void)
{
    stereo[0] = 1000;
    stereo[1] = 1000;
    stereo[2] = 3000;
    stereo[3] = 3000;
    TEST_ASSERT_EQUAL_INT(MIX_OK, mix_mono_half(stereo, 2, MIX_VOL_HIGH, g.out));
    TEST_ASSERT_EQUAL_HEX8(reference(2000, 2000, MIX_VOL_HIGH), g.out[0]);
}

static void test_half_zero_input_is_exactly_mid_scale_at_every_volume(void)
{
    size_t i;
    uint8_t v;
    for (v = 0; v <= MIX_VOL_HIGH; v++) {
        TEST_ASSERT_EQUAL_INT(MIX_OK,
            mix_mono_half(stereo, N_FRAMES, v, g.out));
        for (i = 0; i < N_FRAMES / 2; i++) {
            TEST_ASSERT_EQUAL_HEX8(MIX_SILENCE, g.out[i]);
        }
    }
    assert_guards_intact();
}

static void test_half_bad_arguments_return_err_args_and_write_nothing(void)
{
    size_t i;

    fill_constant(20000, 20000, N_FRAMES);

    TEST_ASSERT_EQUAL_INT(MIX_ERR_ARGS,
        mix_mono_half(NULL, N_FRAMES, MIX_VOL_HIGH, g.out));
    TEST_ASSERT_EQUAL_INT(MIX_ERR_ARGS,
        mix_mono_half(stereo, N_FRAMES, MIX_VOL_HIGH, NULL));
    TEST_ASSERT_EQUAL_INT(MIX_ERR_ARGS,
        mix_mono_half(stereo, N_FRAMES, MIX_VOL_HIGH + 1, g.out));

    for (i = 0; i < N_FRAMES; i++) {
        TEST_ASSERT_EQUAL_HEX8(0x7E, g.out[i]);
    }
    assert_guards_intact();
}

/* ─── argument checking ───────────────────────────────────────────────────── */

static void test_bad_arguments_return_err_args_and_write_nothing(void)
{
    size_t i;

    fill_constant(20000, 20000, N_FRAMES);

    TEST_ASSERT_EQUAL_INT(MIX_ERR_ARGS,
        mix_mono(NULL, N_FRAMES, MIX_VOL_HIGH, g.out));
    TEST_ASSERT_EQUAL_INT(MIX_ERR_ARGS,
        mix_mono(stereo, N_FRAMES, MIX_VOL_HIGH, NULL));
    TEST_ASSERT_EQUAL_INT(MIX_ERR_ARGS,
        mix_mono(stereo, N_FRAMES, MIX_VOL_HIGH + 1, g.out));

    for (i = 0; i < N_FRAMES; i++) {
        TEST_ASSERT_EQUAL_HEX8(0x7E, g.out[i]);
    }
    assert_guards_intact();
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_zero_input_is_exactly_mid_scale_at_every_volume);
    RUN_TEST(test_off_is_mid_scale_for_a_loud_signal);
    RUN_TEST(test_full_scale_input_clamps_at_high_without_wrapping);
    RUN_TEST(test_output_matches_the_reference_at_every_volume);
    RUN_TEST(test_half_an_output_step_rounds_up_and_less_rounds_down);
    RUN_TEST(test_volume_steps_keep_the_design_ratios);
    RUN_TEST(test_left_only_and_right_only_give_the_same_output);
    RUN_TEST(test_opposite_full_scale_channels_cancel_to_mid_scale);
    RUN_TEST(test_bad_arguments_return_err_args_and_write_nothing);
    RUN_TEST(test_half_writes_half_the_frames_and_drops_an_odd_one);
    RUN_TEST(test_half_matches_mix_mono_on_a_constant_input);
    RUN_TEST(test_half_averages_each_pair_of_frames);
    RUN_TEST(test_half_zero_input_is_exactly_mid_scale_at_every_volume);
    RUN_TEST(test_half_bad_arguments_return_err_args_and_write_nothing);
    return UNITY_END();
}
