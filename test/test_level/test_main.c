#include <unity.h>

#include "audio/level.h"

/*
 * Output-queue level estimate.
 *
 * Rate and capacity mirror what the speaker uses: 32768 Hz, four frames of
 * 548 samples. One frame lasts 548 / 32768 s = 16723.63 us, so the estimate
 * — which floors the elapsed sample count — reaches 0 at the first whole
 * microsecond past that, 16724. FRAME_US below is the truncated 16723, the
 * period a caller pacing on whole microseconds would actually use.
 */

#define RATE_HZ 32768u
#define FRAME 548u
#define DEPTH 4u
#define CAPACITY (FRAME * DEPTH) /* 2192 */
#define FRAME_US 16723           /* floor(548 * 1000000 / 32768) */
#define FRAME_DRAINED_US 16724   /* first instant the frame has fully drained */

static level_t lvl;

void setUp(void)
{
    level_init(&lvl, RATE_HZ, CAPACITY);
}

void tearDown(void)
{
}

static void test_fresh_state_is_stopped_and_empty(void)
{
    TEST_ASSERT_FALSE(level_running(&lvl));
    TEST_ASSERT_EQUAL_UINT32(0, level_queued(&lvl, 0));
    TEST_ASSERT_EQUAL_UINT32(0, level_queued(&lvl, 1000000));
}

static void test_one_frame_drains_over_exactly_its_own_duration(void)
{
    level_note_write(&lvl, FRAME, 0);

    TEST_ASSERT_TRUE(level_running(&lvl));
    TEST_ASSERT_EQUAL_UINT32(FRAME, level_queued(&lvl, 0));
    TEST_ASSERT_EQUAL_UINT32(0, level_queued(&lvl, FRAME_DRAINED_US));
    TEST_ASSERT_EQUAL_UINT32(0, level_queued(&lvl, 20000));
}

static void test_half_a_frame_has_drained_at_half_its_duration(void)
{
    /* 8361 us is half of 16723; floor(8361 * 32768 / 1e6) = 273 consumed. */
    level_note_write(&lvl, FRAME, 0);
    TEST_ASSERT_UINT32_WITHIN(1, 274, level_queued(&lvl, 8361));
}

static void test_queued_is_clamped_to_capacity(void)
{
    unsigned i;
    for (i = 0; i < DEPTH + 1u; i++) {
        level_note_write(&lvl, FRAME, 0);
    }
    TEST_ASSERT_EQUAL_UINT32(CAPACITY, level_queued(&lvl, 0));
}

static void test_steady_state_never_runs_dry(void)
{
    int64_t now = 0;
    unsigned i;

    /* Priming fills the queue the way speaker_init() does. */
    level_note_write(&lvl, CAPACITY, now);

    for (i = 0; i < 1000u; i++) {
        uint32_t before;
        now += FRAME_US;
        before = level_queued(&lvl, now);
        TEST_ASSERT_GREATER_THAN_UINT32(0, before);
        TEST_ASSERT_GREATER_OR_EQUAL_UINT32(CAPACITY - FRAME, before);
        level_note_write(&lvl, FRAME, now);
        TEST_ASSERT_LESS_OR_EQUAL_UINT32(CAPACITY, level_queued(&lvl, now));
    }
}

static void test_a_starved_queue_reads_zero(void)
{
    /* One frame written, three frames' worth of time gone by: the speaker's
     * underflow condition is running && queued == 0. */
    level_note_write(&lvl, FRAME, 0);
    TEST_ASSERT_TRUE(level_running(&lvl));
    TEST_ASSERT_EQUAL_UINT32(0, level_queued(&lvl, 50000));
}

static void test_reset_stops_the_clock_and_the_next_write_restarts_it(void)
{
    const int64_t much_later = 100 * 1000000LL;

    level_note_write(&lvl, FRAME, 0);
    level_reset(&lvl);

    TEST_ASSERT_FALSE(level_running(&lvl));
    TEST_ASSERT_EQUAL_UINT32(0, level_queued(&lvl, much_later));

    level_note_write(&lvl, FRAME, much_later);
    TEST_ASSERT_TRUE(level_running(&lvl));
    /* The pause did not drain anything: the clock restarted at this write. */
    TEST_ASSERT_EQUAL_UINT32(FRAME, level_queued(&lvl, much_later));
}

static void test_a_timestamp_before_the_start_counts_as_no_elapsed_time(void)
{
    level_note_write(&lvl, FRAME, 1000000);
    TEST_ASSERT_EQUAL_UINT32(FRAME, level_queued(&lvl, 999000));
    TEST_ASSERT_EQUAL_UINT32(FRAME, level_queued(&lvl, 0));
}

static void test_four_hours_of_writes_do_not_overflow_the_estimate(void)
{
    const int64_t four_hours_us = 4LL * 3600LL * 1000000LL;
    int64_t now = 0;

    level_note_write(&lvl, CAPACITY, now);
    while (now < four_hours_us) {
        now += FRAME_US;
        level_note_write(&lvl, FRAME, now);
        TEST_ASSERT_LESS_OR_EQUAL_UINT32(CAPACITY, level_queued(&lvl, now));
    }

    /* ~4.7e8 samples written across ~1.44e10 us; the estimate is still sane. */
    TEST_ASSERT_GREATER_THAN_UINT64(400000000ULL, lvl.written);
    TEST_ASSERT_LESS_OR_EQUAL_UINT32(CAPACITY, level_queued(&lvl, now));
    TEST_ASSERT_EQUAL_UINT32(0, level_queued(&lvl, now + four_hours_us));
}

/*
 * The speaker's underflow rule, run over a write sequence: one count per write
 * that finds the estimate empty.
 */
static unsigned count_underflow(int64_t now)
{
    return (level_running(&lvl) && level_queued(&lvl, now) == 0) ? 1u : 0u;
}

static void test_one_shortfall_is_counted_once_not_every_frame_after(void)
{
    int64_t now = 0;
    unsigned under = 0;
    unsigned i;

    level_note_write(&lvl, CAPACITY, now);

    /* A 0.4 s stall — the board between flashing and play — then 90 s of a
     * core that runs a little ahead of the DAC, 549 samples every 16,723 us.
     * Before the balance was bounded, the stall's 13,000-sample deficit took
     * minutes to repay and every one of these writes counted (BUG-0016). */
    now += 400000;
    under += count_underflow(now);
    level_note_write(&lvl, 549u, now);
    for (i = 0; i < 90u * 60u; i++) {
        now += FRAME_US;
        under += count_underflow(now);
        level_note_write(&lvl, 549u, now);
    }
    TEST_ASSERT_EQUAL_UINT(1u, under);
    TEST_ASSERT_GREATER_THAN_UINT32(0, level_queued(&lvl, now));
}

static void test_a_core_that_keeps_starving_counts_every_starved_write(void)
{
    int64_t now = 0;
    unsigned under = 0;
    unsigned i;

    /* Half a frame per frame: the queue is genuinely dry at every write, and
     * forgiving the deficit must not hide that. */
    level_note_write(&lvl, FRAME / 2u, now);
    for (i = 0; i < 100u; i++) {
        now += FRAME_US;
        under += count_underflow(now);
        level_note_write(&lvl, FRAME / 2u, now);
    }
    TEST_ASSERT_EQUAL_UINT(100u, under);
}

static void test_a_blocked_write_pins_the_estimate_full(void)
{
    level_note_full(&lvl, 0);
    TEST_ASSERT_FALSE(level_running(&lvl));

    level_note_write(&lvl, FRAME, 0);
    level_note_full(&lvl, 5000);
    TEST_ASSERT_EQUAL_UINT32(CAPACITY, level_queued(&lvl, 5000));
    TEST_ASSERT_EQUAL_UINT32(CAPACITY - FRAME,
                             level_queued(&lvl, 5000 + FRAME_DRAINED_US));
}

static void test_a_dac_slower_than_nominal_never_reads_dry(void)
{
    int64_t now = 0;
    unsigned under = 0;
    unsigned i;

    /* The estimate drains at 32,768 Hz; this DAC takes a frame every
     * 17,000 us, so the writer blocks on every frame. Each block pins the
     * estimate full, and ten minutes of it never reads empty. */
    level_note_write(&lvl, CAPACITY, now);
    for (i = 0; i < 10u * 60u * 59u; i++) {
        now += 17000;
        under += count_underflow(now);
        level_note_write(&lvl, FRAME, now);
        level_note_full(&lvl, now + 300);
    }
    TEST_ASSERT_EQUAL_UINT(0u, under);
}

static void test_null_state_is_inert(void)
{
    level_init(NULL, RATE_HZ, CAPACITY);
    level_reset(NULL);
    level_note_write(NULL, FRAME, 0);
    level_note_full(NULL, 0);
    TEST_ASSERT_FALSE(level_running(NULL));
    TEST_ASSERT_EQUAL_UINT32(0, level_queued(NULL, 0));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_fresh_state_is_stopped_and_empty);
    RUN_TEST(test_one_frame_drains_over_exactly_its_own_duration);
    RUN_TEST(test_half_a_frame_has_drained_at_half_its_duration);
    RUN_TEST(test_queued_is_clamped_to_capacity);
    RUN_TEST(test_steady_state_never_runs_dry);
    RUN_TEST(test_a_starved_queue_reads_zero);
    RUN_TEST(test_reset_stops_the_clock_and_the_next_write_restarts_it);
    RUN_TEST(test_a_timestamp_before_the_start_counts_as_no_elapsed_time);
    RUN_TEST(test_four_hours_of_writes_do_not_overflow_the_estimate);
    RUN_TEST(test_one_shortfall_is_counted_once_not_every_frame_after);
    RUN_TEST(test_a_core_that_keeps_starving_counts_every_starved_write);
    RUN_TEST(test_a_blocked_write_pins_the_estimate_full);
    RUN_TEST(test_a_dac_slower_than_nominal_never_reads_dry);
    RUN_TEST(test_null_state_is_inert);
    return UNITY_END();
}
