#include <unity.h>

#include <math.h>

#include "render/panel_rate.h"

/*
 * Panel rate trim suite.
 *
 * Nothing here pins a frequency. The anchor is one board's measurement — the
 * null a human placed by eye on one panel — and a test that asserted its value
 * would fail on the next unit calibrated, which is the whole point of the
 * trim being per unit. So every assertion is about a ratio, an average or a
 * round trip, and the anchor below is deliberately NOT either of the numbers
 * in circulation on the bench: a suite that passes only at 11 + 20/64 would be
 * pinning that board by accident.
 */
static const panel_anchor_t anchor = { 9, 32, 60.0 };

#define ANCHOR_LINES (PANEL_RATE_BASE_LINES + 9.0 + 32.0 / 64.0)

/* Comfortably finer than the 1/64 of a line the register can express, so a
 * rounding difference passes and an arithmetic one does not. */
#define HZ_EPS 1e-9

static void test_the_anchors_own_porch_gives_the_anchors_own_rate(void)
{
    TEST_ASSERT_DOUBLE_WITHIN(HZ_EPS, anchor.hz,
                              panel_rate_hz(&anchor, anchor.fpa, anchor.ratio));
}

static void test_rate_scales_inversely_with_the_total_line_count(void)
{
    uint8_t fpa;

    /* The relation the whole scheme rests on: a frame is BASE + fpa +
     * ratio/64 lines long, and the refresh rate is the anchor's scaled by the
     * ratio of the two totals. Checked against the arithmetic written out
     * longhand, at every whole porch across the usable range. */
    for (fpa = 1; fpa <= 126; fpa++) {
        double lines = PANEL_RATE_BASE_LINES + (double)fpa;
        double want = anchor.hz * ANCHOR_LINES / lines;

        TEST_ASSERT_DOUBLE_WITHIN(HZ_EPS, want,
                                  panel_rate_hz(&anchor, fpa, 0));
    }
}

static void test_a_longer_porch_is_a_slower_panel(void)
{
    /* The direction matters more than the magnitude: get it backwards and a
     * calibration walks away from the null instead of towards it. */
    TEST_ASSERT_TRUE(panel_rate_hz(&anchor, 11, 0)
                     > panel_rate_hz(&anchor, 12, 0));
    TEST_ASSERT_TRUE(panel_rate_hz(&anchor, 11, 0)
                     > panel_rate_hz(&anchor, 11, 1));
    /* And one 64th of a line is a real step, not one lost to rounding. */
    TEST_ASSERT_TRUE(panel_rate_hz(&anchor, 11, 0)
                     - panel_rate_hz(&anchor, 11, 1) > 0.0);
}

static void test_one_line_of_porch_is_about_a_sixth_of_a_hertz(void)
{
    /* The number that decides the whole design: a whole line is coarser than
     * the error being cancelled, which is why the fractional divider exists
     * at all. Bounded loosely — this is a sanity rail on the base line count,
     * not a measurement. */
    double step = panel_rate_hz(&anchor, 11, 0) - panel_rate_hz(&anchor, 12, 0);

    TEST_ASSERT_TRUE(step > 0.10);
    TEST_ASSERT_TRUE(step < 0.25);
}

static void test_solving_for_a_porch_round_trips(void)
{
    uint8_t fpa;
    uint8_t ratio;

    /* Every expressible setting, out to a rate and back again. The solver
     * rounds to 64ths, so an exact return is the assertion: a half-step of
     * drift here would put a calibrated unit one dither step off its own
     * measured null. */
    for (fpa = 1; fpa <= 126; fpa++) {
        for (ratio = 0; ratio < PANEL_RATE_RATIO_DEN; ratio++) {
            double hz = panel_rate_hz(&anchor, fpa, ratio);
            uint8_t got_fpa = 0xFF;
            uint8_t got_ratio = 0xFF;

            TEST_ASSERT_TRUE(panel_rate_porch_for(&anchor, hz, &got_fpa,
                                                  &got_ratio));
            TEST_ASSERT_EQUAL_UINT8(fpa, got_fpa);
            TEST_ASSERT_EQUAL_UINT8(ratio, got_ratio);
        }
    }
}

static void test_a_cadence_outside_porctrls_reach_is_refused(void)
{
    uint8_t fpa = 7;
    uint8_t ratio = 7;

    /* Far too fast: the porch would have to go negative. */
    TEST_ASSERT_FALSE(panel_rate_porch_for(&anchor, 200.0, &fpa, &ratio));
    /* Far too slow: past the register's range. */
    TEST_ASSERT_FALSE(panel_rate_porch_for(&anchor, 30.0, &fpa, &ratio));
    /* Nonsense in, refused rather than divided by. */
    TEST_ASSERT_FALSE(panel_rate_porch_for(&anchor, 0.0, &fpa, &ratio));
    TEST_ASSERT_FALSE(panel_rate_porch_for(&anchor, -60.0, &fpa, &ratio));
    /* Refused means untouched: the caller keeps the trim it had. */
    TEST_ASSERT_EQUAL_UINT8(7, fpa);
    TEST_ASSERT_EQUAL_UINT8(7, ratio);
}

static void test_the_divider_spends_exactly_ratio_frames_in_64_long(void)
{
    static const uint8_t ratios[] = { 0, 1, 2, 15, 20, 32, 63 };
    unsigned r;

    for (r = 0; r < sizeof ratios / sizeof ratios[0]; r++) {
        uint8_t acc = 0;
        unsigned lng = 0;
        unsigned i;

        for (i = 0; i < PANEL_RATE_RATIO_DEN; i++) {
            uint8_t porch = panel_rate_next_porch(&acc, 11, ratios[r]);

            TEST_ASSERT_TRUE(porch == 11 || porch == 12);
            if (porch == 12) {
                lng++;
            }
        }
        TEST_ASSERT_EQUAL_UINT(ratios[r], lng);
        /* And it comes back to where it started, so the second 64 frames are
         * the same as the first rather than sliding. */
        TEST_ASSERT_EQUAL_UINT8(0, acc);
    }
}

static void test_the_long_run_average_is_the_fraction_asked_for(void)
{
    static const uint8_t ratios[] = { 0, 1, 7, 20, 45, 63 };
    unsigned r;

    /* The assertion that catches an off-by-one in the accumulator: an error
     * of one frame in 64 is about 0.003 Hz, which is invisible on a bench and
     * moves a parked seam across the screen over minutes. Four thousand
     * frames is a little over a minute of play. */
    for (r = 0; r < sizeof ratios / sizeof ratios[0]; r++) {
        uint8_t acc = 0;
        unsigned long lines = 0;
        unsigned i;
        double want = 11.0 + (double)ratios[r] / (double)PANEL_RATE_RATIO_DEN;

        for (i = 0; i < 4096u; i++) {
            lines += panel_rate_next_porch(&acc, 11, ratios[r]);
        }
        TEST_ASSERT_DOUBLE_WITHIN(1e-9, want, (double)lines / 4096.0);
    }
}

static void test_each_callers_divider_keeps_its_own_phase(void)
{
    uint8_t game = 0;
    uint8_t page = 0;
    unsigned i;

    /* Two accumulators, one of them a few frames ahead. Neither may consume
     * the other's long frames — the diagnostic page runs the divider on a
     * cadence of its own while the frame path is not running, and a shared
     * accumulator would make each spend the other's. */
    for (i = 0; i < 5u; i++) {
        panel_rate_next_porch(&page, 11, 20);
    }
    for (i = 0; i < PANEL_RATE_RATIO_DEN; i++) {
        panel_rate_next_porch(&game, 11, 20);
    }
    /* A full 64 frames returns any accumulator to 0 whatever its phase. */
    TEST_ASSERT_EQUAL_UINT8(0, game);
    TEST_ASSERT_EQUAL_UINT8(20u * 5u % PANEL_RATE_RATIO_DEN, page);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_the_anchors_own_porch_gives_the_anchors_own_rate);
    RUN_TEST(test_rate_scales_inversely_with_the_total_line_count);
    RUN_TEST(test_a_longer_porch_is_a_slower_panel);
    RUN_TEST(test_one_line_of_porch_is_about_a_sixth_of_a_hertz);
    RUN_TEST(test_solving_for_a_porch_round_trips);
    RUN_TEST(test_a_cadence_outside_porctrls_reach_is_refused);
    RUN_TEST(test_the_divider_spends_exactly_ratio_frames_in_64_long);
    RUN_TEST(test_the_long_run_average_is_the_fraction_asked_for);
    RUN_TEST(test_each_callers_divider_keeps_its_own_phase);
    return UNITY_END();
}
