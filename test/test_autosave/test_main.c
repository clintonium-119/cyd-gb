#include <unity.h>

#include <stddef.h>

#include "save/autosave.h"

/*
 * Autosave tracker suite. Every timestamp is a literal — nothing here reads a
 * clock — so a failure names an exact millisecond rather than a flaky one.
 *
 * The shape of each test follows the module's split: note_write() sets a flag
 * and tick() turns that flag into the dirty state with the tick's own time.
 * So a write only becomes visible after the next tick, and every idle
 * assertion is measured from the tick that stamped it, never from the write.
 *
 * The save sizes used are the real ones: 0x2000 is the common 8 KB bank,
 * 0x200 is MBC2's 512 half-bytes, and 0 is a cartridge with no RAM at all.
 */

#define SAVE_8K   0x2000u
#define SAVE_MBC2 0x200u

/* Battery thresholds match the firmware's placeholders so the regions in the
 * latch tests read the way the hardware will. */
#define LOW_MV  3500u
#define HYST_MV 100u

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
    s.batt_low = 0xFFu;
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
    TEST_ASSERT_EQUAL_UINT8(0, s.batt_low);
    TEST_ASSERT_FALSE(autosave_dirty(&s));
    TEST_ASSERT_FALSE(autosave_idle_due(&s, 0));
    TEST_ASSERT_FALSE(autosave_idle_due(&s, 100000));
}

static void test_null_state_is_rejected(void)
{
    TEST_ASSERT_EQUAL_INT(AUTOSAVE_ERR_ARGS, autosave_init(NULL, 1));
    TEST_ASSERT_FALSE(autosave_dirty(NULL));
    TEST_ASSERT_FALSE(autosave_idle_due(NULL, 0));
    TEST_ASSERT_FALSE(autosave_battery(NULL, 0, LOW_MV, HYST_MV));
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
    TEST_ASSERT_FALSE(autosave_idle_due(&s, 100000));
}

static void test_a_tick_with_no_write_leaves_the_stamp_alone(void)
{
    autosave_state_t s = fresh(SAVE_8K);
    autosave_note_write(&s, 0x100);
    autosave_tick(&s, 1000);
    autosave_tick(&s, 5000);
    TEST_ASSERT_EQUAL_UINT32(1000, s.last_write_ms);
}

/* ─── the idle rule ───────────────────────────────────────────────────────── */

static void test_the_idle_rule_fires_at_exactly_ten_seconds(void)
{
    autosave_state_t s = fresh(SAVE_8K);
    autosave_note_write(&s, 0x100);
    autosave_tick(&s, 1000);
    TEST_ASSERT_FALSE(autosave_idle_due(&s, 10999));
    TEST_ASSERT_TRUE(autosave_idle_due(&s, 11000));
}

static void test_a_later_write_moves_the_deadline(void)
{
    autosave_state_t s = fresh(SAVE_8K);
    autosave_note_write(&s, 0x100);
    autosave_tick(&s, 1000);
    autosave_note_write(&s, 0x101);
    autosave_tick(&s, 5000);
    TEST_ASSERT_FALSE(autosave_idle_due(&s, 11000));
    TEST_ASSERT_TRUE(autosave_idle_due(&s, 15000));
}

static void test_the_idle_rule_survives_a_timestamp_rollover(void)
{
    autosave_state_t s = fresh(SAVE_8K);
    autosave_note_write(&s, 0x100);
    autosave_tick(&s, 0xFFFFF000u);
    /* 0x00002000 - 0xFFFFF000 is 12 288 ms once the difference is signed. */
    TEST_ASSERT_TRUE(autosave_idle_due(&s, 0x00002000u));
}

/* ─── flushed and deferred ────────────────────────────────────────────────── */

static void test_flushed_clears_dirty_and_the_rule(void)
{
    autosave_state_t s = fresh(SAVE_8K);
    autosave_note_write(&s, 0x100);
    autosave_tick(&s, 1000);
    autosave_flushed(&s);
    TEST_ASSERT_FALSE(autosave_dirty(&s));
    TEST_ASSERT_FALSE(autosave_idle_due(&s, 11000));
    TEST_ASSERT_FALSE(autosave_idle_due(&s, 999999));

    /* A following write re-dirties with the new stamp, not the old one. */
    autosave_note_write(&s, 0x100);
    autosave_tick(&s, 20000);
    TEST_ASSERT_TRUE(autosave_dirty(&s));
    TEST_ASSERT_EQUAL_UINT32(20000, s.last_write_ms);
    TEST_ASSERT_FALSE(autosave_idle_due(&s, 29999));
    TEST_ASSERT_TRUE(autosave_idle_due(&s, 30000));
}

static void test_defer_keeps_dirty_and_restarts_the_idle_clock(void)
{
    autosave_state_t s = fresh(SAVE_8K);
    autosave_note_write(&s, 0x100);
    autosave_tick(&s, 1000);
    autosave_defer(&s, 20000);
    TEST_ASSERT_TRUE(autosave_dirty(&s));
    TEST_ASSERT_FALSE(autosave_idle_due(&s, 29999));
    TEST_ASSERT_TRUE(autosave_idle_due(&s, 30000));
}

/* ─── the low-battery latch ───────────────────────────────────────────────── */

static void test_the_battery_latch_fires_once_per_crossing(void)
{
    autosave_state_t s = fresh(SAVE_8K);
    /* Above the threshold: nothing to do. */
    TEST_ASSERT_FALSE(autosave_battery(&s, 3600, LOW_MV, HYST_MV));
    /* The crossing itself. */
    TEST_ASSERT_TRUE(autosave_battery(&s, 3450, LOW_MV, HYST_MV));
    /* Still low, still latched. */
    TEST_ASSERT_FALSE(autosave_battery(&s, 3440, LOW_MV, HYST_MV));
    /* Recovered past low_mv but not past low_mv + hyst_mv: still latched. */
    TEST_ASSERT_FALSE(autosave_battery(&s, 3590, LOW_MV, HYST_MV));
    /* Clear of the hysteresis band: re-armed, and re-arming is not a flush. */
    TEST_ASSERT_FALSE(autosave_battery(&s, 3600, LOW_MV, HYST_MV));
    /* So the next crossing fires again. */
    TEST_ASSERT_TRUE(autosave_battery(&s, 3450, LOW_MV, HYST_MV));
}

static void test_the_battery_latch_is_independent_of_dirty(void)
{
    autosave_state_t s = fresh(SAVE_8K);
    TEST_ASSERT_FALSE(autosave_dirty(&s));
    TEST_ASSERT_TRUE(autosave_battery(&s, 3450, LOW_MV, HYST_MV));
    TEST_ASSERT_FALSE(autosave_dirty(&s));
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

    RUN_TEST(test_the_idle_rule_fires_at_exactly_ten_seconds);
    RUN_TEST(test_a_later_write_moves_the_deadline);
    RUN_TEST(test_the_idle_rule_survives_a_timestamp_rollover);

    RUN_TEST(test_flushed_clears_dirty_and_the_rule);
    RUN_TEST(test_defer_keeps_dirty_and_restarts_the_idle_clock);

    RUN_TEST(test_the_battery_latch_fires_once_per_crossing);
    RUN_TEST(test_the_battery_latch_is_independent_of_dirty);

    return UNITY_END();
}
