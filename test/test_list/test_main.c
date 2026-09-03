#include <unity.h>

#include <stddef.h>

#include "ui/list.h"

/*
 * List state machine suite. Every timestamp is a literal — nothing here reads
 * a clock — so a repeat failure names an exact millisecond rather than a
 * flaky one.
 *
 * Two shapes of test, because the module has two callers at two sizes. The
 * menu shape is six entries in a six-row window, where nothing ever scrolls
 * and the only interesting move is the wrap. The catalog shape is 132 entries
 * in an eight-row window, where the window has to follow the cursor, snap on
 * a wrap and step a page at a time. The same code has to be right at both.
 *
 * The repeat timeline tests drive press() with a scripted sequence of
 * (direction, now_ms) samples and assert what each individual call returned,
 * because which call produced the step is the whole property. The rest call
 * list_move() and list_page() directly, where the cursor and window are the
 * only things worth reading.
 */

/* The menu shape: six entries, all of them on screen. */
#define MENU_COUNT 6
#define MENU_ROWS 6

/* The catalog shape: the library's 132 titles, eight rows of them visible. */
#define LIB_COUNT 132
#define LIB_ROWS 8

/* The last window position that still shows a full page of the library. */
#define LIB_LAST_FIRST (LIB_COUNT - LIB_ROWS)

/* Direction words, spelled out once. */
#define D_NONE  0x00u
#define D_UP    ((uint8_t)COMBO_BTN_UP)
#define D_DOWN  ((uint8_t)COMBO_BTN_DOWN)
#define D_LEFT  ((uint8_t)COMBO_BTN_LEFT)
#define D_RIGHT ((uint8_t)COMBO_BTN_RIGHT)

void setUp(void)
{
}

void tearDown(void)
{
}

/* A machine at rest over count entries with rows of them visible. */
static list_state_t fresh(uint16_t count, uint8_t rows)
{
    list_state_t s;

    TEST_ASSERT_EQUAL_INT(LIST_OK, list_init(&s, count, rows));
    return s;
}

/* One D-pad sample in, the event it produced out. */
static uint8_t press(list_state_t* s, uint8_t dpad, uint32_t at_ms)
{
    return list_input(s, dpad, at_ms);
}

/* ─── init and argument checks ────────────────────────────────────────────── */

static void test_init_yields_a_clean_state(void)
{
    list_state_t s;

    s.cursor = 0xFFFFu;
    s.first = 0xFFFFu;
    s.held_dir = 0xFFu;
    s.repeat_due_ms = 0xFFFFFFFFu;
    TEST_ASSERT_EQUAL_INT(LIST_OK, list_init(&s, LIB_COUNT, LIB_ROWS));
    TEST_ASSERT_EQUAL_UINT16(LIB_COUNT, s.count);
    TEST_ASSERT_EQUAL_UINT8(LIB_ROWS, s.rows);
    TEST_ASSERT_EQUAL_UINT16(0, s.cursor);
    TEST_ASSERT_EQUAL_UINT16(0, s.first);
    TEST_ASSERT_EQUAL_UINT8(0, s.held_dir);
    TEST_ASSERT_EQUAL_UINT32(0, s.repeat_due_ms);
}

static void test_init_rejects_null_and_a_zero_row_window(void)
{
    list_state_t s;

    TEST_ASSERT_EQUAL_INT(LIST_ERR_ARGS, list_init(NULL, MENU_COUNT, MENU_ROWS));
    TEST_ASSERT_EQUAL_INT(LIST_ERR_ARGS, list_init(&s, MENU_COUNT, 0));
}

static void test_the_readers_and_input_reject_null(void)
{
    TEST_ASSERT_EQUAL_INT(LIST_ERR_ARGS, list_set_count(NULL, MENU_COUNT));
    TEST_ASSERT_EQUAL_UINT8(LIST_EVENT_NONE, list_input(NULL, D_DOWN, 0));
    TEST_ASSERT_EQUAL_UINT16(0, list_cursor(NULL));
    TEST_ASSERT_EQUAL_UINT16(0, list_first(NULL));
    TEST_ASSERT_FALSE(list_visible(NULL, 0));
}

/* ─── the menu shape: six entries, one window ─────────────────────────────── */

static void test_the_six_entry_menu_wraps_at_both_ends(void)
{
    list_state_t s = fresh(MENU_COUNT, MENU_ROWS);

    TEST_ASSERT_EQUAL_UINT16(0, list_cursor(&s));
    TEST_ASSERT_EQUAL_UINT16(0, list_first(&s));

    /* Up from the top lands on the last entry, and a list that fits its
     * window has nowhere to scroll to. */
    list_move(&s, -1);
    TEST_ASSERT_EQUAL_UINT16(MENU_COUNT - 1, list_cursor(&s));
    TEST_ASSERT_EQUAL_UINT16(0, list_first(&s));

    list_move(&s, +1);
    TEST_ASSERT_EQUAL_UINT16(0, list_cursor(&s));
    TEST_ASSERT_EQUAL_UINT16(0, list_first(&s));
}

/* ─── the catalog shape: the window follows the cursor ────────────────────── */

static void test_the_window_follows_the_cursor_down_and_back_up(void)
{
    list_state_t s = fresh(LIB_COUNT, LIB_ROWS);
    int i;

    /* Eight steps down: the first seven stay inside the opening window, and
     * the eighth is the one that scrolls it by a row. */
    for (i = 0; i < 8; i++) {
        list_move(&s, +1);
    }
    TEST_ASSERT_EQUAL_UINT16(8, list_cursor(&s));
    TEST_ASSERT_EQUAL_UINT16(1, list_first(&s));

    for (i = 0; i < 8; i++) {
        list_move(&s, -1);
    }
    TEST_ASSERT_EQUAL_UINT16(0, list_cursor(&s));
    TEST_ASSERT_EQUAL_UINT16(0, list_first(&s));
}

static void test_a_wrap_snaps_the_window_to_the_far_end(void)
{
    list_state_t s = fresh(LIB_COUNT, LIB_ROWS);

    list_move(&s, -1);
    TEST_ASSERT_EQUAL_UINT16(LIB_COUNT - 1, list_cursor(&s));
    TEST_ASSERT_EQUAL_UINT16(LIB_LAST_FIRST, list_first(&s));

    list_move(&s, +1);
    TEST_ASSERT_EQUAL_UINT16(0, list_cursor(&s));
    TEST_ASSERT_EQUAL_UINT16(0, list_first(&s));
}

static void test_a_page_jump_moves_a_page_and_clamps_at_both_ends(void)
{
    list_state_t s = fresh(LIB_COUNT, LIB_ROWS);

    /* Cursor and window step together, so the highlighted row stays put on
     * screen and the page under it changes. */
    list_page(&s, +1);
    TEST_ASSERT_EQUAL_UINT16(8, list_cursor(&s));
    TEST_ASSERT_EQUAL_UINT16(8, list_first(&s));

    /* A page down from near the bottom clamps rather than wrapping. */
    s.cursor = 128;
    s.first = LIB_LAST_FIRST;
    list_page(&s, +1);
    TEST_ASSERT_EQUAL_UINT16(LIB_COUNT - 1, list_cursor(&s));
    TEST_ASSERT_EQUAL_UINT16(LIB_LAST_FIRST, list_first(&s));

    list_page(&s, -1);
    TEST_ASSERT_EQUAL_UINT16(123, list_cursor(&s));
    TEST_ASSERT_EQUAL_UINT16(116, list_first(&s));

    /* And a page up from inside the first page clamps at the top. */
    s.cursor = 3;
    s.first = 0;
    list_page(&s, -1);
    TEST_ASSERT_EQUAL_UINT16(0, list_cursor(&s));
    TEST_ASSERT_EQUAL_UINT16(0, list_first(&s));
}

static void test_visible_covers_exactly_the_window(void)
{
    list_state_t s = fresh(LIB_COUNT, LIB_ROWS);

    s.first = 8;
    TEST_ASSERT_FALSE(list_visible(&s, 7));
    TEST_ASSERT_TRUE(list_visible(&s, 8));
    TEST_ASSERT_TRUE(list_visible(&s, 15));
    TEST_ASSERT_FALSE(list_visible(&s, 16));
}

/* ─── key repeat ──────────────────────────────────────────────────────────── */

static void test_a_held_direction_repeats_at_the_combo_cadence(void)
{
    list_state_t s = fresh(LIB_COUNT, LIB_ROWS);

    /* The press itself steps once, immediately. */
    TEST_ASSERT_EQUAL_UINT8(LIST_EVENT_MOVED, press(&s, D_DOWN, 0));
    TEST_ASSERT_EQUAL_UINT16(1, list_cursor(&s));

    /* Then nothing at all until the delay is up. */
    TEST_ASSERT_EQUAL_UINT8(LIST_EVENT_NONE, press(&s, D_DOWN, 100));
    TEST_ASSERT_EQUAL_UINT8(LIST_EVENT_NONE, press(&s, D_DOWN, 399));
    TEST_ASSERT_EQUAL_UINT8(LIST_EVENT_MOVED, press(&s, D_DOWN, 400));
    TEST_ASSERT_EQUAL_UINT16(2, list_cursor(&s));

    /* And one step per repeat period after that. */
    TEST_ASSERT_EQUAL_UINT8(LIST_EVENT_NONE, press(&s, D_DOWN, 599));
    TEST_ASSERT_EQUAL_UINT8(LIST_EVENT_MOVED, press(&s, D_DOWN, 600));
    TEST_ASSERT_EQUAL_UINT16(3, list_cursor(&s));

    /* Releasing re-arms the immediate step, and restarts the long delay. */
    TEST_ASSERT_EQUAL_UINT8(LIST_EVENT_NONE, press(&s, D_NONE, 650));
    TEST_ASSERT_EQUAL_UINT8(LIST_EVENT_MOVED, press(&s, D_DOWN, 700));
    TEST_ASSERT_EQUAL_UINT16(4, list_cursor(&s));
    TEST_ASSERT_EQUAL_UINT8(LIST_EVENT_NONE, press(&s, D_DOWN, 900));
    TEST_ASSERT_EQUAL_UINT8(LIST_EVENT_MOVED, press(&s, D_DOWN, 1100));
    TEST_ASSERT_EQUAL_UINT16(5, list_cursor(&s));
}

static void test_a_new_direction_acts_at_once_and_restarts_the_delay(void)
{
    list_state_t s = fresh(LIB_COUNT, LIB_ROWS);

    TEST_ASSERT_EQUAL_UINT8(LIST_EVENT_MOVED, press(&s, D_DOWN, 0));
    TEST_ASSERT_EQUAL_UINT16(1, list_cursor(&s));

    /* A different bit is a fresh press, not a repeat of the old one, so it
     * steps on the call it arrives on. */
    TEST_ASSERT_EQUAL_UINT8(LIST_EVENT_MOVED, press(&s, D_UP, 100));
    TEST_ASSERT_EQUAL_UINT16(0, list_cursor(&s));

    /* Held from there, its first repeat is a full delay later — and it is the
     * repeat that walks off the top and wraps. */
    TEST_ASSERT_EQUAL_UINT8(LIST_EVENT_NONE, press(&s, D_UP, 499));
    TEST_ASSERT_EQUAL_UINT8(LIST_EVENT_MOVED, press(&s, D_UP, 500));
    TEST_ASSERT_EQUAL_UINT16(LIB_COUNT - 1, list_cursor(&s));
    TEST_ASSERT_EQUAL_UINT16(LIB_LAST_FIRST, list_first(&s));
}

static void test_two_directions_at_once_move_nothing(void)
{
    list_state_t s = fresh(LIB_COUNT, LIB_ROWS);

    TEST_ASSERT_EQUAL_UINT8(LIST_EVENT_NONE,
                            press(&s, (uint8_t)(D_UP | D_DOWN), 0));
    TEST_ASSERT_EQUAL_UINT16(0, list_cursor(&s));
    TEST_ASSERT_EQUAL_UINT16(0, list_first(&s));
}

static void test_left_and_right_page_through_input(void)
{
    list_state_t s = fresh(LIB_COUNT, LIB_ROWS);

    TEST_ASSERT_EQUAL_UINT8(LIST_EVENT_MOVED, press(&s, D_RIGHT, 0));
    TEST_ASSERT_EQUAL_UINT16(8, list_cursor(&s));
    TEST_ASSERT_EQUAL_UINT16(8, list_first(&s));

    TEST_ASSERT_EQUAL_UINT8(LIST_EVENT_MOVED, press(&s, D_LEFT, 500));
    TEST_ASSERT_EQUAL_UINT16(0, list_cursor(&s));
    TEST_ASSERT_EQUAL_UINT16(0, list_first(&s));
}

/* ─── a list that changes length ──────────────────────────────────────────── */

static void test_a_shorter_count_clamps_the_cursor_and_the_window(void)
{
    list_state_t s = fresh(LIB_COUNT, LIB_ROWS);

    s.cursor = 100;
    s.first = 93;
    TEST_ASSERT_EQUAL_INT(LIST_OK, list_set_count(&s, 5));
    TEST_ASSERT_EQUAL_UINT16(4, list_cursor(&s));
    TEST_ASSERT_EQUAL_UINT16(0, list_first(&s));
}

static void test_an_empty_list_moves_nothing(void)
{
    list_state_t s = fresh(LIB_COUNT, LIB_ROWS);

    TEST_ASSERT_EQUAL_INT(LIST_OK, list_set_count(&s, 0));
    TEST_ASSERT_EQUAL_UINT16(0, list_cursor(&s));
    TEST_ASSERT_EQUAL_UINT16(0, list_first(&s));

    list_move(&s, +1);
    list_move(&s, -1);
    list_page(&s, +1);
    list_page(&s, -1);
    TEST_ASSERT_EQUAL_UINT16(0, list_cursor(&s));
    TEST_ASSERT_EQUAL_UINT16(0, list_first(&s));
    TEST_ASSERT_EQUAL_UINT8(LIST_EVENT_NONE, press(&s, D_DOWN, 0));
    TEST_ASSERT_FALSE(list_visible(&s, 0));
}

static void test_a_single_entry_wraps_onto_itself(void)
{
    list_state_t s = fresh(1, MENU_ROWS);

    list_move(&s, +1);
    TEST_ASSERT_EQUAL_UINT16(0, list_cursor(&s));
    TEST_ASSERT_EQUAL_UINT16(0, list_first(&s));

    list_move(&s, -1);
    TEST_ASSERT_EQUAL_UINT16(0, list_cursor(&s));
    TEST_ASSERT_EQUAL_UINT16(0, list_first(&s));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_init_yields_a_clean_state);
    RUN_TEST(test_init_rejects_null_and_a_zero_row_window);
    RUN_TEST(test_the_readers_and_input_reject_null);
    RUN_TEST(test_the_six_entry_menu_wraps_at_both_ends);
    RUN_TEST(test_the_window_follows_the_cursor_down_and_back_up);
    RUN_TEST(test_a_wrap_snaps_the_window_to_the_far_end);
    RUN_TEST(test_a_page_jump_moves_a_page_and_clamps_at_both_ends);
    RUN_TEST(test_visible_covers_exactly_the_window);
    RUN_TEST(test_a_held_direction_repeats_at_the_combo_cadence);
    RUN_TEST(test_a_new_direction_acts_at_once_and_restarts_the_delay);
    RUN_TEST(test_two_directions_at_once_move_nothing);
    RUN_TEST(test_left_and_right_page_through_input);
    RUN_TEST(test_a_shorter_count_clamps_the_cursor_and_the_window);
    RUN_TEST(test_an_empty_list_moves_nothing);
    RUN_TEST(test_a_single_entry_wraps_onto_itself);
    return UNITY_END();
}
