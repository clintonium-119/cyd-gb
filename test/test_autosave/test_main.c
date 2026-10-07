#include <unity.h>

#include <stddef.h>

#include "save/autosave.h"

/*
 * Autosave tracker suite. Every timestamp is a literal — nothing here reads a
 * clock — so a failure names an exact millisecond rather than a flaky one.
 *
 * The shape of each test follows the module's split: note_write() sets a flag
 * and tick() turns that flag into the dirty state with the tick's own time.
 * So a write only becomes visible after the next tick, and every due
 * assertion is measured from the tick that stamped it, never from the write.
 *
 * The save sizes used are the real ones: 0x2000 is the common 8 KB bank,
 * 0x200 is MBC2's 512 half-bytes, and 0 is a cartridge with no RAM at all.
 */

#define SAVE_8K   0x2000u
#define SAVE_MBC2 0x200u

void setUp(void)
{
}

void tearDown(void)
{
}

/* An initialised state, asserting the init contract on the way through. */
static autosave_state_t fresh(uint32_t save_size)
{
    autosave_state_t s;
    s.save_size = 0xFFFFFFFFu;
    s.wrote = 0xFFu;
    s.dirty = 0xFFu;
    s.last_write_ms = 0xFFFFFFFFu;
    s.dirty_since_ms = 0xFFFFFFFFu;
    s.held = 0xFFu;
    s.hold_until_ms = 0xFFFFFFFFu;
    TEST_ASSERT_EQUAL_INT(AUTOSAVE_OK, autosave_init(&s, save_size));
    return s;
}

/* ─── init and argument checks ────────────────────────────────────────────── */

static void test_init_yields_clean_state(void)
{
    autosave_state_t s = fresh(SAVE_8K);
    TEST_ASSERT_EQUAL_UINT32(SAVE_8K, s.save_size);
    TEST_ASSERT_EQUAL_UINT8(0, s.wrote);
    TEST_ASSERT_EQUAL_UINT8(0, s.dirty);
    TEST_ASSERT_EQUAL_UINT32(0, s.last_write_ms);
    TEST_ASSERT_EQUAL_UINT32(0, s.dirty_since_ms);
    TEST_ASSERT_EQUAL_UINT8(0, s.held);
    TEST_ASSERT_FALSE(autosave_dirty(&s));
    TEST_ASSERT_FALSE(autosave_due(&s, 0));
    TEST_ASSERT_FALSE(autosave_due(&s, 100000));
}

static void test_null_state_is_rejected(void)
{
    TEST_ASSERT_EQUAL_INT(AUTOSAVE_ERR_ARGS, autosave_init(NULL, 1));
    TEST_ASSERT_FALSE(autosave_dirty(NULL));
    TEST_ASSERT_FALSE(autosave_due(NULL, 0));
}

/* ─── the save-size gate ──────────────────────────────────────────────────── */

static void test_write_inside_the_save_size_dirties_on_the_next_tick(void)
{
    autosave_state_t s = fresh(SAVE_8K);
    autosave_note_write(&s, 0x1FFF);
    autosave_tick(&s, 1000);
    TEST_ASSERT_TRUE(autosave_dirty(&s));
    TEST_ASSERT_EQUAL_UINT32(1000, s.last_write_ms);
}

static void test_write_at_the_save_size_never_dirties(void)
{
    autosave_state_t s = fresh(SAVE_8K);
    autosave_note_write(&s, 0x2000);
    autosave_tick(&s, 1000);
    TEST_ASSERT_FALSE(autosave_dirty(&s));
}

static void test_a_cartridge_with_no_ram_never_dirties(void)
{
    autosave_state_t s = fresh(0);
    autosave_note_write(&s, 0);
    autosave_tick(&s, 1000);
    TEST_ASSERT_FALSE(autosave_dirty(&s));
}

static void test_the_mbc2_size_gates_at_0x200(void)
{
    autosave_state_t inside = fresh(SAVE_MBC2);
    autosave_note_write(&inside, 0x1FF);
    autosave_tick(&inside, 1000);
    TEST_ASSERT_TRUE(autosave_dirty(&inside));

    autosave_state_t outside = fresh(SAVE_MBC2);
    autosave_note_write(&outside, 0x200);
    autosave_tick(&outside, 1000);
    TEST_ASSERT_FALSE(autosave_dirty(&outside));
}

/* ─── the flag is not the state ───────────────────────────────────────────── */

static void test_a_write_without_a_tick_is_not_yet_dirty(void)
{
    autosave_state_t s = fresh(SAVE_8K);
    autosave_note_write(&s, 0x100);
    TEST_ASSERT_EQUAL_UINT8(1, s.wrote);
    TEST_ASSERT_FALSE(autosave_dirty(&s));
    TEST_ASSERT_FALSE(autosave_due(&s, 100000));
}

static void test_a_tick_with_no_write_leaves_the_stamp_alone(void)
{
    autosave_state_t s = fresh(SAVE_8K);
    autosave_note_write(&s, 0x100);
    autosave_tick(&s, 1000);
    autosave_tick(&s, 5000);
    TEST_ASSERT_EQUAL_UINT32(1000, s.last_write_ms);
}

/* ─── the quiet arm ───────────────────────────────────────────────────────── */

static void test_a_save_is_due_half_a_second_after_the_last_write(void)
{
    autosave_state_t s = fresh(SAVE_8K);
    autosave_note_write(&s, 0x100);
    autosave_tick(&s, 1000);
    TEST_ASSERT_FALSE(autosave_due(&s, 1499));
    TEST_ASSERT_TRUE(autosave_due(&s, 1500));
}

static void test_a_later_write_moves_the_quiet_deadline(void)
{
    autosave_state_t s = fresh(SAVE_8K);
    autosave_note_write(&s, 0x100);
    autosave_tick(&s, 1000);
    autosave_note_write(&s, 0x101);
    autosave_tick(&s, 1400);
    TEST_ASSERT_FALSE(autosave_due(&s, 1500));
    TEST_ASSERT_FALSE(autosave_due(&s, 1899));
    TEST_ASSERT_TRUE(autosave_due(&s, 1900));
}

/* ─── the max-age arm ─────────────────────────────────────────────────────── */

static void test_ram_that_never_goes_quiet_is_saved_at_ten_seconds(void)
{
    autosave_state_t s = fresh(SAVE_8K);
    uint32_t now;

    /* A write every frame, as Super Mario Land 2 does: the quiet arm never
     * fires, so the age since the RAM first went dirty is what saves it. */
    for (now = 1000; now < 11000; now += 16) {
        autosave_note_write(&s, 0x100);
        autosave_tick(&s, now);
        TEST_ASSERT_FALSE(autosave_due(&s, now));
    }
    autosave_note_write(&s, 0x100);
    autosave_tick(&s, 11000);
    TEST_ASSERT_TRUE(autosave_due(&s, 11000));
    TEST_ASSERT_EQUAL_UINT32(1000, s.dirty_since_ms);
}

static void test_the_age_restarts_from_the_first_write_after_clean(void)
{
    autosave_state_t s = fresh(SAVE_8K);
    autosave_note_write(&s, 0x100);
    autosave_tick(&s, 1000);
    autosave_flushed(&s);
    autosave_note_write(&s, 0x100);
    autosave_tick(&s, 30000);
    TEST_ASSERT_EQUAL_UINT32(30000, s.dirty_since_ms);
    /* A second write while already dirty leaves the age alone. */
    autosave_note_write(&s, 0x100);
    autosave_tick(&s, 30200);
    TEST_ASSERT_EQUAL_UINT32(30000, s.dirty_since_ms);
}

static void test_the_rule_survives_a_timestamp_rollover(void)
{
    autosave_state_t s = fresh(SAVE_8K);
    autosave_note_write(&s, 0x100);
    autosave_tick(&s, 0xFFFFFF00u);
    /* 0x00000100 - 0xFFFFFF00 is 512 ms once the difference is signed. */
    TEST_ASSERT_TRUE(autosave_due(&s, 0x00000100u));
    TEST_ASSERT_FALSE(autosave_due(&s, 0xFFFFFF80u));
}

/* ─── flushed and deferred ────────────────────────────────────────────────── */

static void test_flushed_clears_dirty_and_the_rule(void)
{
    autosave_state_t s = fresh(SAVE_8K);
    autosave_note_write(&s, 0x100);
    autosave_tick(&s, 1000);
    autosave_flushed(&s);
    TEST_ASSERT_FALSE(autosave_dirty(&s));
    TEST_ASSERT_FALSE(autosave_due(&s, 1500));
    TEST_ASSERT_FALSE(autosave_due(&s, 999999));

    /* A following write re-dirties with the new stamp, not the old one. */
    autosave_note_write(&s, 0x100);
    autosave_tick(&s, 20000);
    TEST_ASSERT_TRUE(autosave_dirty(&s));
    TEST_ASSERT_EQUAL_UINT32(20000, s.last_write_ms);
    TEST_ASSERT_FALSE(autosave_due(&s, 20499));
    TEST_ASSERT_TRUE(autosave_due(&s, 20500));
}

static void test_a_failed_save_is_retried_ten_seconds_later(void)
{
    autosave_state_t s = fresh(SAVE_8K);
    autosave_note_write(&s, 0x100);
    autosave_tick(&s, 1000);
    /* The caller took its copy, then the write failed. */
    autosave_flushed(&s);
    autosave_defer(&s, 2000);
    TEST_ASSERT_TRUE(autosave_dirty(&s));
    TEST_ASSERT_FALSE(autosave_due(&s, 2500));
    TEST_ASSERT_FALSE(autosave_due(&s, 11999));
    TEST_ASSERT_TRUE(autosave_due(&s, 12000));
}

static void test_the_hold_outlasts_continuous_writes(void)
{
    autosave_state_t s = fresh(SAVE_8K);
    autosave_defer(&s, 1000);
    autosave_note_write(&s, 0x100);
    autosave_tick(&s, 1100);
    /* Quiet would fire at 1600, but a bad card is not retried before the
     * hold ends. */
    TEST_ASSERT_FALSE(autosave_due(&s, 5000));
    TEST_ASSERT_TRUE(autosave_due(&s, 11000));
}

static void test_flushed_lifts_the_hold(void)
{
    autosave_state_t s = fresh(SAVE_8K);
    autosave_defer(&s, 1000);
    autosave_flushed(&s);
    autosave_note_write(&s, 0x100);
    autosave_tick(&s, 2000);
    TEST_ASSERT_TRUE(autosave_due(&s, 2500));
}

int main(void)
{
    UNITY_BEGIN();

    RUN_TEST(test_init_yields_clean_state);
    RUN_TEST(test_null_state_is_rejected);

    RUN_TEST(test_write_inside_the_save_size_dirties_on_the_next_tick);
    RUN_TEST(test_write_at_the_save_size_never_dirties);
    RUN_TEST(test_a_cartridge_with_no_ram_never_dirties);
    RUN_TEST(test_the_mbc2_size_gates_at_0x200);

    RUN_TEST(test_a_write_without_a_tick_is_not_yet_dirty);
    RUN_TEST(test_a_tick_with_no_write_leaves_the_stamp_alone);

    RUN_TEST(test_a_save_is_due_half_a_second_after_the_last_write);
    RUN_TEST(test_a_later_write_moves_the_quiet_deadline);

    RUN_TEST(test_ram_that_never_goes_quiet_is_saved_at_ten_seconds);
    RUN_TEST(test_the_age_restarts_from_the_first_write_after_clean);
    RUN_TEST(test_the_rule_survives_a_timestamp_rollover);

    RUN_TEST(test_flushed_clears_dirty_and_the_rule);
    RUN_TEST(test_a_failed_save_is_retried_ten_seconds_later);
    RUN_TEST(test_the_hold_outlasts_continuous_writes);
    RUN_TEST(test_flushed_lifts_the_hold);

    return UNITY_END();
}
