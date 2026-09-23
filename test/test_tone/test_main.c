#include <unity.h>

#include <string.h>

#include "audio/mix.h"
#include "audio/tone.h"

/*
 * Test tone: a square wave the diagnostic audio page pushes through the same
 * mixer the emulator uses.
 *
 * Two properties matter enough to pin exactly. The wave's own shape — one
 * cycle per 1/440 s, every sample at full amplitude, both channels the same,
 * no seam where one frame ends and the next begins. And what the mixer does
 * with it: off is mid-scale exactly, the three live steps stay ordered, and
 * this amplitude does not reach either rail at the loudest one.
 *
 * The numbers here are traced from the generator's own arithmetic at
 * TONE_RATE, not measured from a run: at 32768 Hz a 440 Hz step is
 * 440 * 65536 / 32768 = 880 exactly, so 32768 frames is 440 whole cycles and
 * the period is 65536 / 880 = 74.47 samples.
 */

#define TONE_RATE     32768u
#define FRAME_FRAMES    548     /* one Game Boy frame at that rate */
#define N_FRAMES       32768    /* exactly one second */
#define GUARD              8

static struct {
    int16_t pre[GUARD];
    int16_t stereo[N_FRAMES * 2];
    int16_t post[GUARD];
} g;

static uint8_t out[N_FRAMES];
static tone_state_t t;

void setUp(void)
{
    memset(g.pre, 0x5C, sizeof(g.pre));
    memset(g.post, 0x5C, sizeof(g.post));
    memset(g.stereo, 0, sizeof(g.stereo));
    memset(out, 0x7E, sizeof(out));
    memset(&t, 0, sizeof(t));
}

void tearDown(void)
{
}

static void assert_guards_intact(void)
{
    size_t i;
    for (i = 0; i < GUARD; i++) {
        TEST_ASSERT_EQUAL_HEX16(0x5C5C, (uint16_t)g.pre[i]);
        TEST_ASSERT_EQUAL_HEX16(0x5C5C, (uint16_t)g.post[i]);
    }
}

/* ─── argument checking ───────────────────────────────────────────────────── */

static void test_init_rejects_null_zero_and_past_nyquist(void)
{
    TEST_ASSERT_EQUAL_INT(TONE_ERR_ARGS, tone_init(NULL, TONE_HZ, TONE_RATE));
    TEST_ASSERT_EQUAL_INT(TONE_ERR_ARGS, tone_init(&t, TONE_HZ, 0));
    TEST_ASSERT_EQUAL_INT(TONE_ERR_ARGS, tone_init(&t, 0, TONE_RATE));
    /* One past Nyquist is refused; Nyquist itself is not, and its step is
     * exactly the half-cycle, which alternates the sign every frame. */
    TEST_ASSERT_EQUAL_INT(TONE_ERR_ARGS,
        tone_init(&t, (uint16_t)(TONE_RATE / 2 + 1), TONE_RATE));

    TEST_ASSERT_EQUAL_INT(TONE_OK,
        tone_init(&t, (uint16_t)(TONE_RATE / 2), TONE_RATE));
    TEST_ASSERT_EQUAL_UINT32(0x8000u, t.step);
}

static void test_fill_rejects_null_state_or_buffer_and_writes_nothing(void)
{
    size_t i;

    TEST_ASSERT_EQUAL_INT(TONE_OK, tone_init(&t, TONE_HZ, TONE_RATE));

    TEST_ASSERT_EQUAL_INT(TONE_ERR_ARGS,
        tone_fill(NULL, TONE_AMPLITUDE, g.stereo, FRAME_FRAMES));
    TEST_ASSERT_EQUAL_INT(TONE_ERR_ARGS,
        tone_fill(&t, TONE_AMPLITUDE, NULL, FRAME_FRAMES));

    for (i = 0; i < FRAME_FRAMES * 2; i++) {
        TEST_ASSERT_EQUAL_INT16(0, g.stereo[i]);
    }
    /* A refused fill also leaves the accumulator alone, so the caller can
     * retry with a real buffer and still be at cycle zero. */
    TEST_ASSERT_EQUAL_UINT32(0, t.phase);
    assert_guards_intact();
}

/* ─── the wave itself ─────────────────────────────────────────────────────── */

static void test_step_is_the_traced_value_and_phase_starts_at_zero(void)
{
    TEST_ASSERT_EQUAL_INT(TONE_OK, tone_init(&t, TONE_HZ, TONE_RATE));
    TEST_ASSERT_EQUAL_UINT32(880, t.step);
    TEST_ASSERT_EQUAL_UINT32(0, t.phase);
}

static void test_one_second_at_440_hz_has_440_rising_edges(void)
{
    size_t i;
    int edges = 0;

    TEST_ASSERT_EQUAL_INT(TONE_OK, tone_init(&t, TONE_HZ, TONE_RATE));
    TEST_ASSERT_EQUAL_INT(TONE_OK,
        tone_fill(&t, TONE_AMPLITUDE, g.stereo, N_FRAMES));

    for (i = 1; i < N_FRAMES; i++) {
        if (g.stereo[(i - 1) * 2] < 0 && g.stereo[i * 2] > 0) {
            edges++;
        }
    }

    /* 440 whole cycles fit in the second, and the 440th edge lands on the
     * frame just past the buffer — so 439 transitions are visible inside it.
     * The window allows the boundary either way. */
    TEST_ASSERT_INT_WITHIN(1, 440, edges);
    assert_guards_intact();
}

static void test_every_sample_is_exactly_plus_or_minus_the_amplitude(void)
{
    size_t i;
    int saw_high = 0;
    int saw_low = 0;

    TEST_ASSERT_EQUAL_INT(TONE_OK, tone_init(&t, TONE_HZ, TONE_RATE));
    TEST_ASSERT_EQUAL_INT(TONE_OK,
        tone_fill(&t, TONE_AMPLITUDE, g.stereo, N_FRAMES));

    for (i = 0; i < N_FRAMES * 2; i++) {
        if (g.stereo[i] == TONE_AMPLITUDE) {
            saw_high = 1;
        } else if (g.stereo[i] == -TONE_AMPLITUDE) {
            saw_low = 1;
        } else {
            TEST_FAIL_MESSAGE("a sample was neither +TONE_AMPLITUDE nor "
                              "-TONE_AMPLITUDE");
        }
    }

    TEST_ASSERT_TRUE_MESSAGE(saw_high && saw_low,
        "the wave never left one half of the cycle");
    assert_guards_intact();
}

static void test_left_equals_right_for_every_frame(void)
{
    size_t i;

    TEST_ASSERT_EQUAL_INT(TONE_OK, tone_init(&t, TONE_HZ, TONE_RATE));
    TEST_ASSERT_EQUAL_INT(TONE_OK,
        tone_fill(&t, TONE_AMPLITUDE, g.stereo, N_FRAMES));

    for (i = 0; i < N_FRAMES; i++) {
        TEST_ASSERT_EQUAL_INT16(g.stereo[i * 2 + 0], g.stereo[i * 2 + 1]);
    }
    assert_guards_intact();
}

static void test_the_duty_cycle_is_half_over_a_whole_number_of_cycles(void)
{
    size_t i;
    int positive = 0;

    TEST_ASSERT_EQUAL_INT(TONE_OK, tone_init(&t, TONE_HZ, TONE_RATE));
    TEST_ASSERT_EQUAL_INT(TONE_OK,
        tone_fill(&t, TONE_AMPLITUDE, g.stereo, N_FRAMES));

    for (i = 0; i < N_FRAMES; i++) {
        if (g.stereo[i * 2] > 0) {
            positive++;
        }
    }

    /* Exactly half. The step is 880 = 16 * 55 and gcd(55, 4096) is 1, so
     * over 32768 frames the accumulator visits each of its 4096 reachable
     * positions eight times — half of them in the cycle's first half. */
    TEST_ASSERT_EQUAL_INT(N_FRAMES / 2, positive);
    assert_guards_intact();
}

static void test_two_frames_equal_one_frame_of_twice_the_length(void)
{
    static int16_t split[FRAME_FRAMES * 2 * 2];
    tone_state_t a;
    tone_state_t b;

    TEST_ASSERT_EQUAL_INT(TONE_OK, tone_init(&a, TONE_HZ, TONE_RATE));
    TEST_ASSERT_EQUAL_INT(TONE_OK,
        tone_fill(&a, TONE_AMPLITUDE, split, FRAME_FRAMES));
    TEST_ASSERT_EQUAL_INT(TONE_OK,
        tone_fill(&a, TONE_AMPLITUDE, split + FRAME_FRAMES * 2,
                  FRAME_FRAMES));

    TEST_ASSERT_EQUAL_INT(TONE_OK, tone_init(&b, TONE_HZ, TONE_RATE));
    TEST_ASSERT_EQUAL_INT(TONE_OK,
        tone_fill(&b, TONE_AMPLITUDE, g.stereo, FRAME_FRAMES * 2));

    TEST_ASSERT_EQUAL_INT16_ARRAY(g.stereo, split, FRAME_FRAMES * 2 * 2);
    TEST_ASSERT_EQUAL_UINT32(b.phase, a.phase);
    assert_guards_intact();
}

/* ─── through the mixer ───────────────────────────────────────────────────── */

static void mix_a_frame(uint8_t vol_index)
{
    TEST_ASSERT_EQUAL_INT(TONE_OK, tone_init(&t, TONE_HZ, TONE_RATE));
    TEST_ASSERT_EQUAL_INT(TONE_OK,
        tone_fill(&t, TONE_AMPLITUDE, g.stereo, FRAME_FRAMES));
    TEST_ASSERT_EQUAL_INT(MIX_OK,
        mix_mono(g.stereo, FRAME_FRAMES, vol_index, out));
}

static void span_of_frame(uint8_t vol_index, int* lo, int* hi)
{
    size_t i;

    mix_a_frame(vol_index);

    *lo = 255;
    *hi = 0;
    for (i = 0; i < FRAME_FRAMES; i++) {
        if (out[i] < *lo) {
            *lo = out[i];
        }
        if (out[i] > *hi) {
            *hi = out[i];
        }
    }
}

static void test_off_is_mid_scale_for_every_sample_of_the_tone(void)
{
    size_t i;

    mix_a_frame(MIX_VOL_OFF);

    for (i = 0; i < FRAME_FRAMES; i++) {
        TEST_ASSERT_EQUAL_UINT8(128, out[i]);
    }
    assert_guards_intact();
}

static void test_the_three_live_steps_stay_ordered(void)
{
    int high_lo;
    int high_hi;
    int med_lo;
    int med_hi;
    int low_lo;
    int low_hi;

    span_of_frame(MIX_VOL_HIGH, &high_lo, &high_hi);
    span_of_frame(MIX_VOL_MED, &med_lo, &med_hi);
    span_of_frame(MIX_VOL_LOW, &low_lo, &low_hi);

    /* Traced from the mixer's arithmetic on a 6000 mono sample: the scaled
     * value is 10125, 2390 and 562 at High, Med and Low, which round to the
     * biased bytes 168, 137 and 130 (88, 119 and 126 on the negative half). */
    TEST_ASSERT_GREATER_THAN_INT(med_hi, high_hi);
    TEST_ASSERT_GREATER_THAN_INT(low_hi, med_hi);
    TEST_ASSERT_LESS_THAN_INT(med_lo, high_lo);
    TEST_ASSERT_LESS_THAN_INT(low_lo, med_lo);

    /* Every step sits either side of mid-scale — a step that had collapsed
     * into silence would pass the ordering above on one side only. */
    TEST_ASSERT_GREATER_THAN_INT(128, low_hi);
    TEST_ASSERT_LESS_THAN_INT(128, low_lo);
}

static void test_this_amplitude_does_not_reach_a_rail_at_full_volume(void)
{
    int lo;
    int hi;

    span_of_frame(MIX_VOL_HIGH, &lo, &hi);

    /* 6000 scaled by 432/256 is 10125, which rounds to 168 as a biased high
     * byte; -10125 rounds to 88. Both rails are far off, so a clipped test
     * tone would be a real fault and not the signal's own level. */
    TEST_ASSERT_EQUAL_INT(168, hi);
    TEST_ASSERT_EQUAL_INT(88, lo);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_init_rejects_null_zero_and_past_nyquist);
    RUN_TEST(test_fill_rejects_null_state_or_buffer_and_writes_nothing);
    RUN_TEST(test_step_is_the_traced_value_and_phase_starts_at_zero);
    RUN_TEST(test_one_second_at_440_hz_has_440_rising_edges);
    RUN_TEST(test_every_sample_is_exactly_plus_or_minus_the_amplitude);
    RUN_TEST(test_left_equals_right_for_every_frame);
    RUN_TEST(test_the_duty_cycle_is_half_over_a_whole_number_of_cycles);
    RUN_TEST(test_two_frames_equal_one_frame_of_twice_the_length);
    RUN_TEST(test_off_is_mid_scale_for_every_sample_of_the_tone);
    RUN_TEST(test_the_three_live_steps_stay_ordered);
    RUN_TEST(test_this_amplitude_does_not_reach_a_rail_at_full_volume);
    return UNITY_END();
}
