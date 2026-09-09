#include <unity.h>

#include <stdlib.h>
#include <string.h>

#include "audio/mix.h"

/*
 * Mixer: interleaved stereo int16 in, unsigned 8-bit mono out.
 *
 * The reference below repeats the production arithmetic on purpose — these
 * cases pin the shape of the result (silence is exact, dither is bounded,
 * the three volume steps keep their ratios), not the expression that
 * produces it.
 */

#define GUARD 8
#define N_FRAMES 4096

static struct {
    uint8_t pre[GUARD];
    uint8_t out[N_FRAMES];
    uint8_t post[GUARD];
} g;

static int16_t stereo[N_FRAMES * 2];
static mix_state_t st;

/* Test-side generator, deliberately not the one under test and not libc's. */
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
    mix_init(&st, 0x1234ABCDu);
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

/* What the mixer would emit with the dither term removed. */
static uint8_t undithered(int16_t left, int16_t right, uint8_t vol_index)
{
    static const uint16_t lut[3] = { 256, 176, 128 };
    int32_t mono = ((int32_t)left + (int32_t)right) / 2;
    int32_t scaled = (mono * (int32_t)lut[vol_index]) / 256;

    if (scaled > 32767) {
        scaled = 32767;
    } else if (scaled < -32768) {
        scaled = -32768;
    }
    return (uint8_t)(((uint32_t)(scaled + 32768)) >> 8);
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

/* |sum / reference_sum - num / 256| <= 0.02, cross-multiplied. */
static void assert_ratio_within_2_percent(long long sum, long long reference_sum,
                                          long long num)
{
    long long lhs = llabs(sum * 256 - reference_sum * num);
    long long tolerance = (reference_sum * 256 * 2) / 100;
    TEST_ASSERT_TRUE_MESSAGE(lhs <= tolerance,
        "volume step is not within 2% of its design ratio");
}

/* ─── silence and off ─────────────────────────────────────────────────────── */

static void test_zero_input_is_exactly_mid_scale_at_every_volume(void)
{
    uint8_t vol;
    size_t i;

    for (vol = MIX_VOL_HIGH; vol <= MIX_VOL_OFF; vol++) {
        memset(g.out, 0x7E, sizeof(g.out));
        TEST_ASSERT_EQUAL_INT(MIX_OK,
            mix_mono(&st, stereo, N_FRAMES, vol, g.out));
        for (i = 0; i < N_FRAMES; i++) {
            TEST_ASSERT_EQUAL_HEX8(MIX_SILENCE, g.out[i]);
        }
    }
    assert_guards_intact();
}

static void test_off_is_mid_scale_for_a_loud_signal_and_leaves_state_alone(void)
{
    mix_state_t before;
    size_t i;

    fill_constant(32767, -32767, N_FRAMES / 2);
    fill_constant(-32768, 32767, N_FRAMES);
    before = st;

    TEST_ASSERT_EQUAL_INT(MIX_OK,
        mix_mono(&st, stereo, N_FRAMES, MIX_VOL_OFF, g.out));

    for (i = 0; i < N_FRAMES; i++) {
        TEST_ASSERT_EQUAL_HEX8(MIX_SILENCE, g.out[i]);
    }
    TEST_ASSERT_EQUAL_HEX32(before.lfsr, st.lfsr);
    assert_guards_intact();
}

/* ─── clamping ────────────────────────────────────────────────────────────── */

static void test_full_scale_input_clamps_without_wrapping(void)
{
    size_t i;

    fill_constant(32767, 32767, N_FRAMES);
    TEST_ASSERT_EQUAL_INT(MIX_OK,
        mix_mono(&st, stereo, N_FRAMES, MIX_VOL_HIGH, g.out));
    for (i = 0; i < N_FRAMES; i++) {
        TEST_ASSERT_GREATER_OR_EQUAL_UINT8(254, g.out[i]);
    }

    fill_constant(-32768, -32768, N_FRAMES);
    TEST_ASSERT_EQUAL_INT(MIX_OK,
        mix_mono(&st, stereo, N_FRAMES, MIX_VOL_HIGH, g.out));
    for (i = 0; i < N_FRAMES; i++) {
        TEST_ASSERT_LESS_OR_EQUAL_UINT8(1, g.out[i]);
    }
    assert_guards_intact();
}

/* ─── dither bound ────────────────────────────────────────────────────────── */

static void test_dither_never_moves_a_sample_by_more_than_one(void)
{
    uint8_t vol;
    size_t i;

    for (i = 0; i < N_FRAMES; i++) {
        stereo[i * 2 + 0] = (int16_t)(rng_next() & 0xFFFFu);
        stereo[i * 2 + 1] = (int16_t)(rng_next() & 0xFFFFu);
    }

    for (vol = MIX_VOL_HIGH; vol <= MIX_VOL_LOW; vol++) {
        TEST_ASSERT_EQUAL_INT(MIX_OK,
            mix_mono(&st, stereo, N_FRAMES, vol, g.out));
        for (i = 0; i < N_FRAMES; i++) {
            int got = (int)g.out[i];
            int ref = (int)undithered(stereo[i * 2 + 0], stereo[i * 2 + 1], vol);
            TEST_ASSERT_LESS_OR_EQUAL_INT(1, abs(got - ref));
        }
    }
    assert_guards_intact();
}

/* ─── volume ratios ───────────────────────────────────────────────────────── */

static void test_volume_steps_keep_the_design_ratios(void)
{
    long long high, med, low;

    fill_constant(16384, 16384, N_FRAMES);

    TEST_ASSERT_EQUAL_INT(MIX_OK,
        mix_mono(&st, stereo, N_FRAMES, MIX_VOL_HIGH, g.out));
    high = deviation_sum(N_FRAMES);

    TEST_ASSERT_EQUAL_INT(MIX_OK,
        mix_mono(&st, stereo, N_FRAMES, MIX_VOL_MED, g.out));
    med = deviation_sum(N_FRAMES);

    TEST_ASSERT_EQUAL_INT(MIX_OK,
        mix_mono(&st, stereo, N_FRAMES, MIX_VOL_LOW, g.out));
    low = deviation_sum(N_FRAMES);

    TEST_ASSERT_TRUE(high > med);
    TEST_ASSERT_TRUE(med > low);
    assert_ratio_within_2_percent(med, high, 176);
    assert_ratio_within_2_percent(low, high, 128);
    assert_guards_intact();
}

/* ─── mono sum, not a channel pick ────────────────────────────────────────── */

static void test_left_only_and_right_only_give_the_same_output(void)
{
    static uint8_t left_out[N_FRAMES];
    mix_state_t a, b;

    mix_init(&a, 0x5EED5EEDu);
    mix_init(&b, 0x5EED5EEDu);

    fill_constant(12000, 0, N_FRAMES);
    TEST_ASSERT_EQUAL_INT(MIX_OK,
        mix_mono(&a, stereo, N_FRAMES, MIX_VOL_HIGH, left_out));

    fill_constant(0, 12000, N_FRAMES);
    TEST_ASSERT_EQUAL_INT(MIX_OK,
        mix_mono(&b, stereo, N_FRAMES, MIX_VOL_HIGH, g.out));

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
        mix_mono(&st, stereo, N_FRAMES, MIX_VOL_HIGH, g.out));
    for (i = 0; i < N_FRAMES; i++) {
        TEST_ASSERT_EQUAL_HEX8(MIX_SILENCE, g.out[i]);
    }
    assert_guards_intact();
}

/* ─── dither state ────────────────────────────────────────────────────────── */

static void test_equal_seeds_produce_identical_output(void)
{
    static uint8_t first[N_FRAMES];
    mix_state_t a, b;

    for (size_t i = 0; i < N_FRAMES; i++) {
        stereo[i * 2 + 0] = (int16_t)(rng_next() & 0x3FFFu);
        stereo[i * 2 + 1] = (int16_t)(rng_next() & 0x3FFFu);
    }

    mix_init(&a, 0xABCD1234u);
    mix_init(&b, 0xABCD1234u);
    TEST_ASSERT_EQUAL_INT(MIX_OK,
        mix_mono(&a, stereo, N_FRAMES, MIX_VOL_MED, first));
    TEST_ASSERT_EQUAL_INT(MIX_OK,
        mix_mono(&b, stereo, N_FRAMES, MIX_VOL_MED, g.out));

    TEST_ASSERT_EQUAL_HEX8_ARRAY(first, g.out, N_FRAMES);
    assert_guards_intact();
}

static void test_seed_zero_still_dithers(void)
{
    mix_state_t zero_seeded;
    size_t i;
    int saw_other = 0;

    mix_init(&zero_seeded, 0);
    TEST_ASSERT_NOT_EQUAL_UINT32(0, zero_seeded.lfsr);

    /* 256 lands exactly on an output-byte boundary, so the sample reads 128
     * or 129 purely according to the dither term. A generator stuck at zero
     * would give -256 every time, and every sample would be 128. */
    fill_constant(256, 256, N_FRAMES);
    TEST_ASSERT_EQUAL_INT(MIX_OK,
        mix_mono(&zero_seeded, stereo, N_FRAMES, MIX_VOL_HIGH, g.out));
    for (i = 0; i < N_FRAMES; i++) {
        TEST_ASSERT_TRUE(g.out[i] == 128 || g.out[i] == 129);
        if (g.out[i] != 128) {
            saw_other = 1;
        }
    }
    TEST_ASSERT_TRUE_MESSAGE(saw_other,
        "every sample was 128 — the dither generator is stuck at zero");
    assert_guards_intact();
}

/* ─── argument checking ───────────────────────────────────────────────────── */

static void test_bad_arguments_return_err_args_and_write_nothing(void)
{
    size_t i;

    fill_constant(20000, 20000, N_FRAMES);

    TEST_ASSERT_EQUAL_INT(MIX_ERR_ARGS,
        mix_mono(&st, NULL, N_FRAMES, MIX_VOL_HIGH, g.out));
    TEST_ASSERT_EQUAL_INT(MIX_ERR_ARGS,
        mix_mono(&st, stereo, N_FRAMES, MIX_VOL_HIGH, NULL));
    TEST_ASSERT_EQUAL_INT(MIX_ERR_ARGS,
        mix_mono(NULL, stereo, N_FRAMES, MIX_VOL_HIGH, g.out));
    TEST_ASSERT_EQUAL_INT(MIX_ERR_ARGS,
        mix_mono(&st, stereo, N_FRAMES, MIX_VOL_OFF + 1, g.out));

    for (i = 0; i < N_FRAMES; i++) {
        TEST_ASSERT_EQUAL_HEX8(0x7E, g.out[i]);
    }
    assert_guards_intact();
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_zero_input_is_exactly_mid_scale_at_every_volume);
    RUN_TEST(test_off_is_mid_scale_for_a_loud_signal_and_leaves_state_alone);
    RUN_TEST(test_full_scale_input_clamps_without_wrapping);
    RUN_TEST(test_dither_never_moves_a_sample_by_more_than_one);
    RUN_TEST(test_volume_steps_keep_the_design_ratios);
    RUN_TEST(test_left_only_and_right_only_give_the_same_output);
    RUN_TEST(test_opposite_full_scale_channels_cancel_to_mid_scale);
    RUN_TEST(test_equal_seeds_produce_identical_output);
    RUN_TEST(test_seed_zero_still_dithers);
    RUN_TEST(test_bad_arguments_return_err_args_and_write_nothing);
    return UNITY_END();
}
