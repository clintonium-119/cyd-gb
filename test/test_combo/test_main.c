#include <unity.h>

#include <stddef.h>

#include "input/combo.h"

/*
 * Combo state machine suite. Every timestamp is a literal — nothing here
 * reads a clock — so a failure names an exact millisecond rather than a
 * flaky one.
 *
 * Two shapes of test. The timeline tests script a sequence of (word, now_ms)
 * samples through run_timeline() and assert the event stream that came out,
 * which is how debounce, one-shot and repeat behaviour is read. The pointwise
 * tests call feed() directly when the interesting thing is which single call
 * produced an event, and read combo_joypad() between calls.
 *
 * Debounce shows up in every timeline: a word is only committed once it has
 * been sampled unchanged for COMBO_DEBOUNCE_MS, so a press always needs two
 * samples before it can produce anything.
 */

/* Words used repeatedly, spelled out once. */
#define W_NONE   0x0000u
#define W_MENU   ((uint16_t)(COMBO_BTN_START | COMBO_BTN_SELECT))
#define W_VOL_UP ((uint16_t)(COMBO_BTN_SELECT | COMBO_BTN_UP))
#define W_VOL_DN ((uint16_t)(COMBO_BTN_SELECT | COMBO_BTN_DOWN))
#define W_BRT_UP ((uint16_t)(COMBO_BTN_SELECT | COMBO_BTN_RIGHT))
#define W_BRT_DN ((uint16_t)(COMBO_BTN_SELECT | COMBO_BTN_LEFT))

/* Volume is indexed 0 = high .. 3 = off; brightness steps by 32 off a floor
 * of 32 so a shell-mounted unit can never look dead. */
#define VOL_MIN 0
#define VOL_MAX 3
#define VOL_STEP 1
#define BRT_MIN 32
#define BRT_MAX 255
#define BRT_STEP 32

#define MAX_EVENTS 32

typedef struct {
    uint16_t word;
    uint32_t at_ms;
} sample_t;

static uint8_t events[MAX_EVENTS];
static size_t event_count;

void setUp(void)
{
}

void tearDown(void)
{
}

/* One sample in, the event it produced out. */
static uint8_t feed(combo_state_t* s, uint16_t word, uint32_t at_ms)
{
    uint8_t event = 0xFFu;
    TEST_ASSERT_EQUAL_INT(COMBO_OK, combo_update(s, word, at_ms, &event));
    return event;
}

/* Feed a scripted timeline, recording the calls that produced an event. */
static void run_timeline(combo_state_t* s, const sample_t* seq, size_t n)
{
    size_t i;
    event_count = 0;
    for (i = 0; i < n; i++) {
        uint8_t event = feed(s, seq[i].word, seq[i].at_ms);
        if (event != (uint8_t)COMBO_EVENT_NONE) {
            TEST_ASSERT_TRUE(event_count < MAX_EVENTS);
            events[event_count++] = event;
        }
    }
}

/* Hold word from at_ms until it commits; returns the committing call's event. */
static uint8_t settle(combo_state_t* s, uint16_t word, uint32_t at_ms)
{
    feed(s, word, at_ms);
    return feed(s, word, at_ms + COMBO_DEBOUNCE_MS);
}

/* ─── init and argument checks ────────────────────────────────────────────── */

static void test_init_yields_clean_state(void)
{
    combo_state_t s;
    s.stable_word = 0xFFFFu;
    s.raw_word = 0xFFFFu;
    s.last_change_ms = 0xFFFFFFFFu;
    s.repeat_due_ms = 0xFFFFFFFFu;
    s.active_dir = 0xFFu;
    s.menu_latch = 0xFFu;
    TEST_ASSERT_EQUAL_INT(COMBO_OK, combo_init(&s));
    TEST_ASSERT_EQUAL_HEX16(0, s.stable_word);
    TEST_ASSERT_EQUAL_HEX16(0, s.raw_word);
    TEST_ASSERT_EQUAL_UINT32(0, s.last_change_ms);
    TEST_ASSERT_EQUAL_UINT32(0, s.repeat_due_ms);
    TEST_ASSERT_EQUAL_UINT8(0, s.active_dir);
    TEST_ASSERT_EQUAL_UINT8(0, s.menu_latch);
    TEST_ASSERT_EQUAL_HEX8(0, combo_joypad(&s));
}

static void test_init_rejects_null(void)
{
    TEST_ASSERT_EQUAL_INT(COMBO_ERR_ARGS, combo_init(NULL));
}

static void test_update_rejects_null_state(void)
{
    uint8_t event = 0xA5u;
    TEST_ASSERT_EQUAL_INT(COMBO_ERR_ARGS, combo_update(NULL, 0, 0, &event));
    /* The header promises *out_event is untouched on a rejected call. */
    TEST_ASSERT_EQUAL_UINT8(0xA5u, event);
}

static void test_update_rejects_null_event(void)
{
    combo_state_t s;
    TEST_ASSERT_EQUAL_INT(COMBO_OK, combo_init(&s));
    TEST_ASSERT_EQUAL_INT(COMBO_ERR_ARGS, combo_update(&s, W_MENU, 0, NULL));
}

static void test_joypad_rejects_null(void)
{
    TEST_ASSERT_EQUAL_HEX8(0, combo_joypad(NULL));
}

/* ─── debounce ────────────────────────────────────────────────────────────── */

static void test_debounce_rejects_glitch_shorter_than_8ms(void)
{
    /* A 3 ms bounce on A: pressed at 2, gone by 5. It is replaced before it
     * can settle, so it never reaches the debounced word at all. */
    static const sample_t seq[] = {
        { W_NONE, 0 },
        { COMBO_BTN_A, 2 },
        { COMBO_BTN_A, 4 },
        { W_NONE, 5 },
        { W_NONE, 100 },
    };
    combo_state_t s;
    size_t i;
    TEST_ASSERT_EQUAL_INT(COMBO_OK, combo_init(&s));
    for (i = 0; i < sizeof(seq) / sizeof(seq[0]); i++) {
        TEST_ASSERT_EQUAL_UINT8((uint8_t)COMBO_EVENT_NONE,
            feed(&s, seq[i].word, seq[i].at_ms));
        TEST_ASSERT_EQUAL_HEX8(0, combo_joypad(&s));
    }
}

static void test_debounce_accepts_press_stable_for_8ms(void)
{
    combo_state_t s;
    TEST_ASSERT_EQUAL_INT(COMBO_OK, combo_init(&s));

    /* The first sample of a new word only arms the clock. */
    feed(&s, COMBO_BTN_A, 0);
    TEST_ASSERT_EQUAL_HEX8(0, combo_joypad(&s));

    /* 7 ms is one short of the gate. */
    feed(&s, COMBO_BTN_A, 7);
    TEST_ASSERT_EQUAL_HEX8(0, combo_joypad(&s));

    /* 8 ms commits. */
    feed(&s, COMBO_BTN_A, 8);
    TEST_ASSERT_EQUAL_HEX8(COMBO_BTN_A, combo_joypad(&s));
}

static void test_debounce_gates_release_too(void)
{
    combo_state_t s;
    TEST_ASSERT_EQUAL_INT(COMBO_OK, combo_init(&s));
    settle(&s, COMBO_BTN_A, 0);
    TEST_ASSERT_EQUAL_HEX8(COMBO_BTN_A, combo_joypad(&s));

    feed(&s, W_NONE, 100);
    TEST_ASSERT_EQUAL_HEX8(COMBO_BTN_A, combo_joypad(&s));
    feed(&s, W_NONE, 108);
    TEST_ASSERT_EQUAL_HEX8(0, combo_joypad(&s));
}

/* ─── menu combo ──────────────────────────────────────────────────────────── */

static void test_menu_fires_once_on_edge_and_stays_silent_while_held(void)
{
    static const sample_t seq[] = {
        { W_MENU, 0 },      /* arms the clock                */
        { W_MENU, 8 },      /* commits -> MENU               */
        { W_MENU, 16 },
        { W_MENU, 500 },
        { W_MENU, 5000 },   /* well past any repeat interval */
    };
    combo_state_t s;
    TEST_ASSERT_EQUAL_INT(COMBO_OK, combo_init(&s));
    run_timeline(&s, seq, sizeof(seq) / sizeof(seq[0]));
    TEST_ASSERT_EQUAL_size_t(1, event_count);
    TEST_ASSERT_EQUAL_UINT8((uint8_t)COMBO_EVENT_MENU, events[0]);
}

static void test_menu_rearms_after_the_pair_releases(void)
{
    static const sample_t seq[] = {
        { W_MENU, 0 },
        { W_MENU, 8 },      /* MENU                       */
        { W_MENU, 100 },
        { W_NONE, 200 },
        { W_NONE, 208 },    /* release commits, latch off */
        { W_MENU, 300 },
        { W_MENU, 308 },    /* MENU again                 */
    };
    combo_state_t s;
    TEST_ASSERT_EQUAL_INT(COMBO_OK, combo_init(&s));
    run_timeline(&s, seq, sizeof(seq) / sizeof(seq[0]));
    TEST_ASSERT_EQUAL_size_t(2, event_count);
    TEST_ASSERT_EQUAL_UINT8((uint8_t)COMBO_EVENT_MENU, events[0]);
    TEST_ASSERT_EQUAL_UINT8((uint8_t)COMBO_EVENT_MENU, events[1]);
}

static void test_bare_start_and_bare_select_bind_nothing(void)
{
    static const sample_t seq[] = {
        { COMBO_BTN_START, 0 },
        { COMBO_BTN_START, 8 },
        { COMBO_BTN_START, 1000 },
        { COMBO_BTN_SELECT, 1100 },
        { COMBO_BTN_SELECT, 1108 },
        { COMBO_BTN_SELECT, 2000 },
    };
    combo_state_t s;
    TEST_ASSERT_EQUAL_INT(COMBO_OK, combo_init(&s));
    run_timeline(&s, seq, sizeof(seq) / sizeof(seq[0]));
    TEST_ASSERT_EQUAL_size_t(0, event_count);
}

/* ─── adjustment combos ───────────────────────────────────────────────────── */

static void assert_combo_edge_fires(uint16_t word, uint8_t expect)
{
    combo_state_t s;
    TEST_ASSERT_EQUAL_INT(COMBO_OK, combo_init(&s));
    TEST_ASSERT_EQUAL_UINT8((uint8_t)COMBO_EVENT_NONE, feed(&s, word, 0));
    TEST_ASSERT_EQUAL_UINT8(expect, feed(&s, word, 8));
    /* Edge-triggered: nothing more until the repeat delay is up. */
    TEST_ASSERT_EQUAL_UINT8((uint8_t)COMBO_EVENT_NONE, feed(&s, word, 16));
    TEST_ASSERT_EQUAL_UINT8((uint8_t)COMBO_EVENT_NONE, feed(&s, word, 400));
}

static void test_select_up_steps_volume_up(void)
{
    assert_combo_edge_fires(W_VOL_UP, (uint8_t)COMBO_EVENT_VOL_UP);
}

static void test_select_down_steps_volume_down(void)
{
    assert_combo_edge_fires(W_VOL_DN, (uint8_t)COMBO_EVENT_VOL_DOWN);
}

static void test_select_right_steps_brightness_up(void)
{
    assert_combo_edge_fires(W_BRT_UP, (uint8_t)COMBO_EVENT_BRIGHT_UP);
}

static void test_select_left_steps_brightness_down(void)
{
    assert_combo_edge_fires(W_BRT_DN, (uint8_t)COMBO_EVENT_BRIGHT_DOWN);
}

static void test_direction_without_select_fires_nothing(void)
{
    static const sample_t seq[] = {
        { COMBO_BTN_UP, 0 },
        { COMBO_BTN_UP, 8 },
        { COMBO_BTN_UP, 1000 },
        { COMBO_BTN_UP, 2000 },
    };
    combo_state_t s;
    TEST_ASSERT_EQUAL_INT(COMBO_OK, combo_init(&s));
    run_timeline(&s, seq, sizeof(seq) / sizeof(seq[0]));
    TEST_ASSERT_EQUAL_size_t(0, event_count);
}

/* ─── key repeat ──────────────────────────────────────────────────────────── */

static void test_repeat_waits_400ms_then_steps_every_200ms(void)
{
    combo_state_t s;
    TEST_ASSERT_EQUAL_INT(COMBO_OK, combo_init(&s));

    feed(&s, W_VOL_UP, 0);
    /* Edge at 8, so the first repeat is due at 408. */
    TEST_ASSERT_EQUAL_UINT8((uint8_t)COMBO_EVENT_VOL_UP, feed(&s, W_VOL_UP, 8));
    TEST_ASSERT_EQUAL_UINT8((uint8_t)COMBO_EVENT_NONE, feed(&s, W_VOL_UP, 200));
    TEST_ASSERT_EQUAL_UINT8((uint8_t)COMBO_EVENT_NONE, feed(&s, W_VOL_UP, 407));
    TEST_ASSERT_EQUAL_UINT8((uint8_t)COMBO_EVENT_VOL_UP, feed(&s, W_VOL_UP, 408));

    TEST_ASSERT_EQUAL_UINT8((uint8_t)COMBO_EVENT_NONE, feed(&s, W_VOL_UP, 607));
    TEST_ASSERT_EQUAL_UINT8((uint8_t)COMBO_EVENT_VOL_UP, feed(&s, W_VOL_UP, 608));
    TEST_ASSERT_EQUAL_UINT8((uint8_t)COMBO_EVENT_NONE, feed(&s, W_VOL_UP, 807));
    TEST_ASSERT_EQUAL_UINT8((uint8_t)COMBO_EVENT_VOL_UP, feed(&s, W_VOL_UP, 808));
}

static void test_releasing_the_direction_resets_the_repeat_timer(void)
{
    combo_state_t s;
    TEST_ASSERT_EQUAL_INT(COMBO_OK, combo_init(&s));

    feed(&s, W_VOL_UP, 0);
    TEST_ASSERT_EQUAL_UINT8((uint8_t)COMBO_EVENT_VOL_UP, feed(&s, W_VOL_UP, 8));
    TEST_ASSERT_EQUAL_UINT8((uint8_t)COMBO_EVENT_VOL_UP, feed(&s, W_VOL_UP, 408));

    /* Let go of Up but keep Select held. */
    feed(&s, COMBO_BTN_SELECT, 500);
    TEST_ASSERT_EQUAL_UINT8((uint8_t)COMBO_EVENT_NONE,
        feed(&s, COMBO_BTN_SELECT, 508));

    /* Press it again: this is a fresh edge, so the 400 ms delay starts over
     * rather than the 200 ms cadence continuing. */
    feed(&s, W_VOL_UP, 600);
    TEST_ASSERT_EQUAL_UINT8((uint8_t)COMBO_EVENT_VOL_UP, feed(&s, W_VOL_UP, 608));
    TEST_ASSERT_EQUAL_UINT8((uint8_t)COMBO_EVENT_NONE, feed(&s, W_VOL_UP, 808));
    TEST_ASSERT_EQUAL_UINT8((uint8_t)COMBO_EVENT_NONE, feed(&s, W_VOL_UP, 1007));
    TEST_ASSERT_EQUAL_UINT8((uint8_t)COMBO_EVENT_VOL_UP, feed(&s, W_VOL_UP, 1008));
}

static void test_a_long_stall_costs_one_late_step_not_a_burst(void)
{
    /* The menu was open for two seconds. At most one event comes out per
     * call, and the deadline re-arms from now, so the held combo does not
     * fire a backlog of catch-up steps. */
    combo_state_t s;
    TEST_ASSERT_EQUAL_INT(COMBO_OK, combo_init(&s));

    feed(&s, W_VOL_UP, 0);
    TEST_ASSERT_EQUAL_UINT8((uint8_t)COMBO_EVENT_VOL_UP, feed(&s, W_VOL_UP, 8));
    TEST_ASSERT_EQUAL_UINT8((uint8_t)COMBO_EVENT_VOL_UP, feed(&s, W_VOL_UP, 2408));
    TEST_ASSERT_EQUAL_UINT8((uint8_t)COMBO_EVENT_NONE, feed(&s, W_VOL_UP, 2500));
    TEST_ASSERT_EQUAL_UINT8((uint8_t)COMBO_EVENT_VOL_UP, feed(&s, W_VOL_UP, 2608));
}

static void test_menu_wins_when_a_repeat_is_due_on_the_same_call(void)
{
    combo_state_t s;
    uint16_t both = (uint16_t)(W_MENU | COMBO_BTN_UP);
    TEST_ASSERT_EQUAL_INT(COMBO_OK, combo_init(&s));

    feed(&s, W_VOL_UP, 0);
    TEST_ASSERT_EQUAL_UINT8((uint8_t)COMBO_EVENT_VOL_UP, feed(&s, W_VOL_UP, 8));

    /* Start goes down as well; the new word commits exactly when the volume
     * repeat falls due at 408. */
    feed(&s, both, 396);
    TEST_ASSERT_EQUAL_UINT8((uint8_t)COMBO_EVENT_MENU, feed(&s, both, 408));

    /* The repeat is deferred, not starved: it reports on the next call. */
    TEST_ASSERT_EQUAL_UINT8((uint8_t)COMBO_EVENT_VOL_UP, feed(&s, both, 416));
}

/* ─── joypad word ─────────────────────────────────────────────────────────── */

static void test_joypad_masks_the_dpad_while_select_is_held(void)
{
    combo_state_t s;
    uint16_t word = (uint16_t)(COMBO_BTN_SELECT | COMBO_BTN_UP |
                               COMBO_BTN_LEFT | COMBO_BTN_A);
    TEST_ASSERT_EQUAL_INT(COMBO_OK, combo_init(&s));
    settle(&s, word, 0);
    /* Select and A survive; Up and Left do not, so the character does not
     * walk around during an adjustment. */
    TEST_ASSERT_EQUAL_HEX8(COMBO_BTN_SELECT | COMBO_BTN_A, combo_joypad(&s));
}

static void test_joypad_masks_start_and_select_while_the_menu_is_latched(void)
{
    combo_state_t s;
    uint16_t word = (uint16_t)(W_MENU | COMBO_BTN_A);
    TEST_ASSERT_EQUAL_INT(COMBO_OK, combo_init(&s));
    TEST_ASSERT_EQUAL_UINT8((uint8_t)COMBO_EVENT_MENU, settle(&s, word, 0));
    TEST_ASSERT_EQUAL_HEX8(COMBO_BTN_A, combo_joypad(&s));

    /* Releasing Start clears the latch, so Select is passed through again
     * (its D-pad mask still applies). */
    settle(&s, (uint16_t)(COMBO_BTN_SELECT | COMBO_BTN_A), 100);
    TEST_ASSERT_EQUAL_HEX8(COMBO_BTN_SELECT | COMBO_BTN_A, combo_joypad(&s));
}

static void test_joypad_passes_unrelated_bits_through_debounced(void)
{
    combo_state_t s;
    uint16_t word = (uint16_t)(COMBO_BTN_A | COMBO_BTN_B |
                               COMBO_BTN_START | COMBO_BTN_DOWN);
    TEST_ASSERT_EQUAL_INT(COMBO_OK, combo_init(&s));
    settle(&s, word, 0);
    TEST_ASSERT_EQUAL_HEX8((uint8_t)word, combo_joypad(&s));
}

/* ─── clamp-step helper ───────────────────────────────────────────────────── */

static void test_step_u8_volume_never_wraps(void)
{
    /* 0 = high .. 3 = off. */
    TEST_ASSERT_EQUAL_UINT8(1, combo_step_u8(0, 1, VOL_MIN, VOL_MAX, VOL_STEP));
    TEST_ASSERT_EQUAL_UINT8(2, combo_step_u8(3, -1, VOL_MIN, VOL_MAX, VOL_STEP));
    TEST_ASSERT_EQUAL_UINT8(0, combo_step_u8(0, -1, VOL_MIN, VOL_MAX, VOL_STEP));
    TEST_ASSERT_EQUAL_UINT8(3, combo_step_u8(3, 1, VOL_MIN, VOL_MAX, VOL_STEP));
    TEST_ASSERT_EQUAL_UINT8(2, combo_step_u8(2, 0, VOL_MIN, VOL_MAX, VOL_STEP));
    /* A stale stored value above the range repairs itself on first use. */
    TEST_ASSERT_EQUAL_UINT8(2, combo_step_u8(9, -1, VOL_MIN, VOL_MAX, VOL_STEP));
    TEST_ASSERT_EQUAL_UINT8(3, combo_step_u8(9, 1, VOL_MIN, VOL_MAX, VOL_STEP));
}

static void test_step_u8_brightness_never_wraps(void)
{
    TEST_ASSERT_EQUAL_UINT8(64, combo_step_u8(32, 1, BRT_MIN, BRT_MAX, BRT_STEP));
    TEST_ASSERT_EQUAL_UINT8(32, combo_step_u8(32, -1, BRT_MIN, BRT_MAX, BRT_STEP));
    TEST_ASSERT_EQUAL_UINT8(255, combo_step_u8(255, 1, BRT_MIN, BRT_MAX, BRT_STEP));
    /* 224 + 32 would be 256: clamped, not wrapped to 0. */
    TEST_ASSERT_EQUAL_UINT8(255, combo_step_u8(224, 1, BRT_MIN, BRT_MAX, BRT_STEP));
    /* 48 - 32 would be 16, below the floor. */
    TEST_ASSERT_EQUAL_UINT8(32, combo_step_u8(48, -1, BRT_MIN, BRT_MAX, BRT_STEP));
    /* A value stored below the floor is lifted to it. */
    TEST_ASSERT_EQUAL_UINT8(32, combo_step_u8(0, -1, BRT_MIN, BRT_MAX, BRT_STEP));
    TEST_ASSERT_EQUAL_UINT8(64, combo_step_u8(0, 1, BRT_MIN, BRT_MAX, BRT_STEP));
}

static void test_step_u8_sweeps_the_brightness_ladder_and_sits_at_the_top(void)
{
    static const uint8_t ladder[] = { 64, 96, 128, 160, 192, 224, 255, 255 };
    uint8_t v = BRT_MIN;
    size_t i;
    for (i = 0; i < sizeof(ladder) / sizeof(ladder[0]); i++) {
        v = combo_step_u8(v, 1, BRT_MIN, BRT_MAX, BRT_STEP);
        TEST_ASSERT_EQUAL_UINT8(ladder[i], v);
    }
    /* And down from the floor it sits there rather than rolling over. */
    v = BRT_MIN;
    for (i = 0; i < 3; i++) {
        v = combo_step_u8(v, -1, BRT_MIN, BRT_MAX, BRT_STEP);
        TEST_ASSERT_EQUAL_UINT8(BRT_MIN, v);
    }
}

static void test_step_u8_leaves_a_nonsense_range_alone(void)
{
    TEST_ASSERT_EQUAL_UINT8(77, combo_step_u8(77, 1, 200, 100, 1));
}

int main(void)
{
    UNITY_BEGIN();

    RUN_TEST(test_init_yields_clean_state);
    RUN_TEST(test_init_rejects_null);
    RUN_TEST(test_update_rejects_null_state);
    RUN_TEST(test_update_rejects_null_event);
    RUN_TEST(test_joypad_rejects_null);

    RUN_TEST(test_debounce_rejects_glitch_shorter_than_8ms);
    RUN_TEST(test_debounce_accepts_press_stable_for_8ms);
    RUN_TEST(test_debounce_gates_release_too);

    RUN_TEST(test_menu_fires_once_on_edge_and_stays_silent_while_held);
    RUN_TEST(test_menu_rearms_after_the_pair_releases);
    RUN_TEST(test_bare_start_and_bare_select_bind_nothing);

    RUN_TEST(test_select_up_steps_volume_up);
    RUN_TEST(test_select_down_steps_volume_down);
    RUN_TEST(test_select_right_steps_brightness_up);
    RUN_TEST(test_select_left_steps_brightness_down);
    RUN_TEST(test_direction_without_select_fires_nothing);

    RUN_TEST(test_repeat_waits_400ms_then_steps_every_200ms);
    RUN_TEST(test_releasing_the_direction_resets_the_repeat_timer);
    RUN_TEST(test_a_long_stall_costs_one_late_step_not_a_burst);
    RUN_TEST(test_menu_wins_when_a_repeat_is_due_on_the_same_call);

    RUN_TEST(test_joypad_masks_the_dpad_while_select_is_held);
    RUN_TEST(test_joypad_masks_start_and_select_while_the_menu_is_latched);
    RUN_TEST(test_joypad_passes_unrelated_bits_through_debounced);

    RUN_TEST(test_step_u8_volume_never_wraps);
    RUN_TEST(test_step_u8_brightness_never_wraps);
    RUN_TEST(test_step_u8_sweeps_the_brightness_ladder_and_sits_at_the_top);
    RUN_TEST(test_step_u8_leaves_a_nonsense_range_alone);

    return UNITY_END();
}
