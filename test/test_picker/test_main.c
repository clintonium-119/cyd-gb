#include <unity.h>

#include <stddef.h>
#include <stdio.h>
#include <string.h>

#include "ui/picker.h"

/*
 * Writer state machine suite. Every timestamp is a literal — nothing here
 * reads a clock — so a hold or repeat failure names an exact millisecond
 * rather than a flaky one.
 *
 * The shape under test is the real one: the 132-title library in an 11-row
 * window, which is what picker_layout gives at 240x216. Two modes, two
 * screens: the list, where the D-pad moves and A opens a title, and the
 * detail page, where Up and Down scroll a band, B backs out and A must be
 * held.
 *
 * The repeat cases read COMBO_REPEAT_DELAY_MS and COMBO_REPEAT_MS rather than
 * copying them, so a change to the one shared cadence moves this suite with
 * it instead of breaking it.
 */

/* The catalog shape: the library's 132 titles, 11 rows of them visible. */
#define LIB_COUNT 132
#define LIB_ROWS 11

/* Every tenth entry is a starter: indices 0, 10, ... 130 — 14 of them. */
#define LIB_STARTER_EVERY 10
#define LIB_STARTERS 14

/* Button words, spelled out once. */
#define B_NONE  0x00u
#define B_A     ((uint8_t)COMBO_BTN_A)
#define B_B     ((uint8_t)COMBO_BTN_B)
#define B_UP    ((uint8_t)COMBO_BTN_UP)
#define B_DOWN  ((uint8_t)COMBO_BTN_DOWN)
#define B_LEFT  ((uint8_t)COMBO_BTN_LEFT)
#define B_RIGHT ((uint8_t)COMBO_BTN_RIGHT)

static catalog_index_t lib;

void setUp(void)
{
}

void tearDown(void)
{
}

/* ─── helpers ─────────────────────────────────────────────────────────────── */

/* `n` entries named "Game NNN.gb", every tenth one a starter. */
static void fill_library(size_t n)
{
    size_t i;

    memset(&lib, 0, sizeof(lib));
    for (i = 0; i < n; i++) {
        lib.e[i].offset = (uint32_t)(i * 100);
        snprintf(lib.e[i].filename, sizeof(lib.e[i].filename),
                 "Game %03zu.gb", i);
        snprintf(lib.e[i].title, sizeof(lib.e[i].title), "Game %03zu", i);
        lib.e[i].flags =
            (i % LIB_STARTER_EVERY == 0) ? CATALOG_FLAG_STARTER : 0;
    }
    lib.count = n;
}

/* The same, with nothing flagged — the wizard's empty case. */
static void fill_library_without_starters(size_t n)
{
    size_t i;

    fill_library(n);
    for (i = 0; i < n; i++) {
        lib.e[i].flags = 0;
    }
}

static picker_t fresh(enum picker_mode_e mode, bool wild_done,
                      bool pending_set, const boot_made_t* made)
{
    picker_t p;

    TEST_ASSERT_EQUAL_INT(PICKER_OK,
                          picker_init(&p, mode, &lib, wild_done, pending_set,
                                      made, LIB_ROWS));
    return p;
}

/* One button sample in, the event it produced out. */
static uint8_t press(picker_t* p, uint8_t buttons, uint32_t at_ms)
{
    return picker_input(p, buttons, at_ms);
}

/*
 * Walk the cursor to `row`, press A, and release — so a hold case that
 * follows starts from a clean edge rather than from the A that opened the
 * page.
 */
static void open_row(picker_t* p, uint16_t row, uint32_t at_ms)
{
    uint32_t t = at_ms;

    while (list_cursor(&p->list) < row) {
        press(p, B_DOWN, t++);
        press(p, B_NONE, t++);
    }
    press(p, B_A, t++);
    TEST_ASSERT_EQUAL_UINT8(PICKER_SCREEN_DETAIL, p->screen);
    press(p, B_NONE, t);
}

/* ─── row composition ─────────────────────────────────────────────────────── */

static void test_pending_without_a_pending_write_is_the_whole_catalog(void)
{
    fill_library(LIB_COUNT);
    picker_t p = fresh(PICKER_MODE_PENDING, true, false, NULL);
    uint16_t i;

    TEST_ASSERT_EQUAL_UINT16(LIB_COUNT, p.row_count);
    for (i = 0; i < p.row_count; i++) {
        TEST_ASSERT_EQUAL_UINT8(PICKER_ROW_GAME, p.rows[i].kind);
    }
    /* File order, one row per entry. */
    TEST_ASSERT_EQUAL_UINT16(0, p.rows[0].cat);
    TEST_ASSERT_EQUAL_UINT16(LIB_COUNT - 1, p.rows[LIB_COUNT - 1].cat);
}

static void test_a_pending_write_adds_a_cancel_row_at_the_top(void)
{
    fill_library(LIB_COUNT);
    picker_t p = fresh(PICKER_MODE_PENDING, true, true, NULL);

    TEST_ASSERT_EQUAL_UINT8(PICKER_ROW_CANCEL_PENDING, p.rows[0].kind);
    TEST_ASSERT_EQUAL_UINT16(LIB_COUNT + 1, p.row_count);
    TEST_ASSERT_EQUAL_UINT8(PICKER_ROW_GAME, p.rows[1].kind);
    TEST_ASSERT_EQUAL_UINT16(0, p.rows[1].cat);
}

static void test_the_wizard_shows_starters_only(void)
{
    fill_library(LIB_COUNT);
    picker_t p = fresh(PICKER_MODE_IMMEDIATE, false, false, NULL);
    uint16_t i;

    TEST_ASSERT_EQUAL_UINT16(LIB_STARTERS, p.row_count);
    for (i = 0; i < p.row_count; i++) {
        TEST_ASSERT_EQUAL_UINT8(PICKER_ROW_GAME, p.rows[i].kind);
        TEST_ASSERT_TRUE(lib.e[p.rows[i].cat].flags & CATALOG_FLAG_STARTER);
    }
}

static void test_finish_setup_appears_only_once_the_wildcard_is_written(void)
{
    fill_library(LIB_COUNT);
    picker_t before = fresh(PICKER_MODE_IMMEDIATE, false, false, NULL);
    picker_t after = fresh(PICKER_MODE_IMMEDIATE, true, false, NULL);

    /* Before the wildcard the boot flow treats a finish as invalid, so the
     * row is not offered rather than offered and refused. */
    TEST_ASSERT_EQUAL_UINT8(PICKER_ROW_GAME, before.rows[0].kind);
    TEST_ASSERT_EQUAL_UINT8(PICKER_ROW_FINISH, after.rows[0].kind);
    TEST_ASSERT_EQUAL_UINT16(LIB_STARTERS + 1, after.row_count);
}

static void test_a_made_filename_marks_exactly_its_row(void)
{
    boot_made_t made;
    uint16_t i;
    unsigned marked = 0;

    fill_library(LIB_COUNT);
    boot_made_clear(&made);
    TEST_ASSERT_EQUAL_INT(BOOT_MADE_OK, boot_made_add(&made, "Game 010.gb"));

    picker_t p = fresh(PICKER_MODE_IMMEDIATE, false, false, &made);
    for (i = 0; i < p.row_count; i++) {
        if (picker_row_marked(&p, i)) {
            marked++;
            TEST_ASSERT_EQUAL_UINT16(10, p.rows[i].cat);
        }
    }
    TEST_ASSERT_EQUAL_UINT(1, marked);

    /* "Game 000.gb" is row 0 and was never made. */
    TEST_ASSERT_FALSE(picker_row_marked(&p, 0));

    /* No record at all marks nothing. */
    picker_t bare = fresh(PICKER_MODE_IMMEDIATE, false, false, NULL);
    for (i = 0; i < bare.row_count; i++) {
        TEST_ASSERT_FALSE(picker_row_marked(&bare, i));
    }
}

/* ─── opening and leaving a detail page ───────────────────────────────────── */

static void test_a_opens_the_highlighted_titles_detail_page(void)
{
    fill_library(LIB_COUNT);
    picker_t p = fresh(PICKER_MODE_PENDING, true, false, NULL);

    TEST_ASSERT_EQUAL_UINT8(PICKER_SCREEN_LIST, p.screen);
    TEST_ASSERT_EQUAL_UINT8(PICKER_EVENT_REDRAW, press(&p, B_A, 0));
    TEST_ASSERT_EQUAL_UINT8(PICKER_SCREEN_DETAIL, p.screen);
    TEST_ASSERT_EQUAL_UINT16(list_cursor(&p.list), p.detail_row);
}

static void test_b_returns_to_the_list_with_the_cursor_unchanged(void)
{
    fill_library(LIB_COUNT);
    picker_t p = fresh(PICKER_MODE_PENDING, true, false, NULL);

    open_row(&p, 3, 0);
    TEST_ASSERT_EQUAL_UINT16(3, list_cursor(&p.list));
    TEST_ASSERT_EQUAL_UINT8(PICKER_EVENT_REDRAW, press(&p, B_B, 100));
    TEST_ASSERT_EQUAL_UINT8(PICKER_SCREEN_LIST, p.screen);
    TEST_ASSERT_EQUAL_UINT16(3, list_cursor(&p.list));
    TEST_ASSERT_EQUAL_UINT8(0, picker_hold_pct(&p));
}

/* ─── the hold ────────────────────────────────────────────────────────────── */

static void test_the_a_that_opened_the_page_cannot_confirm_it(void)
{
    fill_library(LIB_COUNT);
    picker_t p = fresh(PICKER_MODE_PENDING, true, false, NULL);

    /* A is pressed once on the list and never released. */
    TEST_ASSERT_EQUAL_UINT8(PICKER_EVENT_REDRAW, press(&p, B_A, 0));
    TEST_ASSERT_EQUAL_UINT8(PICKER_EVENT_NONE, press(&p, B_A, 500));
    TEST_ASSERT_EQUAL_UINT8(PICKER_EVENT_NONE, press(&p, B_A, 1500));
    TEST_ASSERT_EQUAL_UINT8(PICKER_SCREEN_DETAIL, p.screen);
    TEST_ASSERT_EQUAL_UINT8(0, picker_hold_pct(&p));
}

static void test_a_fresh_hold_completes_at_the_hold_figure(void)
{
    fill_library(LIB_COUNT);
    picker_t p = fresh(PICKER_MODE_PENDING, true, false, NULL);

    open_row(&p, 0, 0);
    press(&p, B_NONE, 100);
    TEST_ASSERT_EQUAL_UINT8(PICKER_EVENT_BAR, press(&p, B_A, 200));

    /* One millisecond short: 999 * 100 / 1000 = 99 %. */
    TEST_ASSERT_EQUAL_UINT8(PICKER_EVENT_BAR, press(&p, B_A, 1199));
    TEST_ASSERT_EQUAL_UINT8(99, picker_hold_pct(&p));
    TEST_ASSERT_EQUAL_UINT8(PICKER_SCREEN_DETAIL, p.screen);

    TEST_ASSERT_EQUAL_UINT8(PICKER_EVENT_DONE, press(&p, B_A, 1200));
    TEST_ASSERT_EQUAL_UINT8(PICKER_SCREEN_DONE, p.screen);
}

static void test_letting_go_early_resets_the_hold(void)
{
    fill_library(LIB_COUNT);
    picker_t p = fresh(PICKER_MODE_PENDING, true, false, NULL);

    open_row(&p, 0, 0);
    press(&p, B_NONE, 100);
    press(&p, B_A, 200);
    TEST_ASSERT_EQUAL_UINT8(PICKER_EVENT_BAR, press(&p, B_NONE, 1199));
    TEST_ASSERT_EQUAL_UINT8(0, picker_hold_pct(&p));
    TEST_ASSERT_EQUAL_UINT8(PICKER_SCREEN_DETAIL, p.screen);

    /* A new edge starts again from the time it was pressed. */
    press(&p, B_A, 1300);
    TEST_ASSERT_EQUAL_UINT8(PICKER_EVENT_BAR, press(&p, B_A, 2299));
    TEST_ASSERT_EQUAL_UINT8(PICKER_SCREEN_DETAIL, p.screen);
    TEST_ASSERT_EQUAL_UINT8(PICKER_EVENT_DONE, press(&p, B_A, 2300));
}

static void test_a_done_machine_ignores_every_further_sample(void)
{
    fill_library(LIB_COUNT);
    picker_t p = fresh(PICKER_MODE_PENDING, true, false, NULL);

    open_row(&p, 0, 0);
    press(&p, B_NONE, 100);
    press(&p, B_A, 200);
    TEST_ASSERT_EQUAL_UINT8(PICKER_EVENT_DONE, press(&p, B_A, 1200));

    TEST_ASSERT_EQUAL_UINT8(PICKER_EVENT_NONE, press(&p, B_NONE, 1300));
    TEST_ASSERT_EQUAL_UINT8(PICKER_EVENT_NONE, press(&p, B_B, 1400));
    TEST_ASSERT_EQUAL_UINT8(PICKER_EVENT_NONE, press(&p, B_DOWN, 1500));
    TEST_ASSERT_EQUAL_UINT8(PICKER_SCREEN_DONE, p.screen);
}

static void test_launch_mode_is_every_game_and_no_action_row(void)
{
    fill_library(LIB_COUNT);
    /* pending_set and wild_done are both true: neither may add a row. */
    picker_t p = fresh(PICKER_MODE_LAUNCH, true, true, NULL);
    uint16_t i;

    TEST_ASSERT_EQUAL_UINT16(LIB_COUNT, p.row_count);
    for (i = 0; i < p.row_count; i++) {
        TEST_ASSERT_EQUAL_UINT8(PICKER_ROW_GAME, p.rows[i].kind);
        TEST_ASSERT_EQUAL_UINT16(i, p.rows[i].cat);
    }
}

/* ─── the selection ───────────────────────────────────────────────────────── */

static void test_a_held_launch_pick_returns_its_filename(void)
{
    boot_selection_t out;

    fill_library(LIB_COUNT);
    picker_t p = fresh(PICKER_MODE_LAUNCH, false, false, NULL);

    /* Row 1 is not a starter: launch mode offers it anyway. */
    open_row(&p, 1, 0);
    press(&p, B_NONE, 100);
    press(&p, B_A, 200);
    TEST_ASSERT_EQUAL_UINT8(PICKER_EVENT_DONE, press(&p, B_A, 1200));
    TEST_ASSERT_EQUAL_INT(BOOT_PICK_ROM, picker_result(&p, &out));
    TEST_ASSERT_EQUAL_STRING("Game 001.gb", out.rom);
}

static void test_an_early_release_in_launch_mode_picks_nothing(void)
{
    boot_selection_t out;

    fill_library(LIB_COUNT);
    picker_t p = fresh(PICKER_MODE_LAUNCH, false, false, NULL);

    open_row(&p, 0, 0);
    press(&p, B_NONE, 100);
    press(&p, B_A, 200);
    press(&p, B_A, 1100);
    press(&p, B_NONE, 1150);
    TEST_ASSERT_EQUAL_UINT8(PICKER_SCREEN_DETAIL, p.screen);
    TEST_ASSERT_EQUAL_INT(BOOT_PICK_NONE, picker_result(&p, &out));
}

static void test_a_confirmed_game_row_returns_its_filename(void)
{
    boot_selection_t out;

    fill_library(LIB_COUNT);
    picker_t p = fresh(PICKER_MODE_PENDING, true, false, NULL);

    /* Untouched until the machine is done. */
    memset(&out, 0, sizeof(out));
    snprintf(out.rom, sizeof(out.rom), "%s", "sentinel");
    out.target = 0xEE;
    TEST_ASSERT_EQUAL_INT(BOOT_PICK_NONE, picker_result(&p, &out));
    TEST_ASSERT_EQUAL_STRING("sentinel", out.rom);
    TEST_ASSERT_EQUAL_UINT8(0xEE, out.target);

    open_row(&p, 0, 0);
    press(&p, B_NONE, 100);
    press(&p, B_A, 200);
    TEST_ASSERT_EQUAL_UINT8(PICKER_EVENT_DONE, press(&p, B_A, 1200));

    TEST_ASSERT_EQUAL_INT(BOOT_PICK_ROM, picker_result(&p, &out));
    TEST_ASSERT_EQUAL_STRING("Game 000.gb", out.rom);
    TEST_ASSERT_EQUAL_UINT8(BOOT_TARGET_WILDCARD, out.target);
}

static void test_every_mode_returns_the_wildcard_target(void)
{
    boot_selection_t out;
    enum picker_mode_e modes[] = { PICKER_MODE_PENDING,
                                   PICKER_MODE_IMMEDIATE };
    size_t m;

    fill_library(LIB_COUNT);
    for (m = 0; m < sizeof(modes) / sizeof(modes[0]); m++) {
        /* wild_done true in immediate mode puts Finish setup at row 0, so
         * open row 1 there; pending mode has games from row 0. */
        picker_t p = fresh(modes[m], false, false, NULL);

        open_row(&p, 0, 0);
        press(&p, B_NONE, 100);
        press(&p, B_A, 200);
        TEST_ASSERT_EQUAL_UINT8(PICKER_EVENT_DONE, press(&p, B_A, 1200));
        TEST_ASSERT_EQUAL_INT(BOOT_PICK_ROM, picker_result(&p, &out));
        TEST_ASSERT_EQUAL_UINT8(BOOT_TARGET_WILDCARD, out.target);
    }
}

static void test_the_cancel_row_returns_cancel_pending(void)
{
    boot_selection_t out;

    fill_library(LIB_COUNT);
    picker_t p = fresh(PICKER_MODE_PENDING, true, true, NULL);

    open_row(&p, 0, 0);
    press(&p, B_NONE, 100);
    press(&p, B_A, 200);
    TEST_ASSERT_EQUAL_UINT8(PICKER_EVENT_DONE, press(&p, B_A, 1200));
    TEST_ASSERT_EQUAL_INT(BOOT_PICK_CANCEL_PENDING, picker_result(&p, &out));
}

static void test_the_finish_row_returns_finish(void)
{
    boot_selection_t out;

    fill_library(LIB_COUNT);
    picker_t p = fresh(PICKER_MODE_IMMEDIATE, true, false, NULL);

    open_row(&p, 0, 0);
    press(&p, B_NONE, 100);
    press(&p, B_A, 200);
    TEST_ASSERT_EQUAL_UINT8(PICKER_EVENT_DONE, press(&p, B_A, 1200));
    TEST_ASSERT_EQUAL_INT(BOOT_PICK_FINISH, picker_result(&p, &out));
}

/* ─── the detail band's scroll ────────────────────────────────────────────── */

static void test_the_band_scrolls_a_line_at_the_shared_cadence(void)
{
    fill_library(LIB_COUNT);
    picker_t p = fresh(PICKER_MODE_PENDING, true, false, NULL);

    open_row(&p, 0, 0);
    /* A 19-line page in an 11-row band: eight lines of travel. */
    TEST_ASSERT_EQUAL_INT(PICKER_OK, picker_set_scroll_span(&p, 19, LIB_ROWS));
    TEST_ASSERT_EQUAL_UINT16(8, p.scroll_max);

    TEST_ASSERT_EQUAL_UINT8(PICKER_EVENT_BAND, press(&p, B_DOWN, 300));
    TEST_ASSERT_EQUAL_UINT16(1, p.scroll);

    /* Held, but one millisecond before the first repeat is due. */
    press(&p, B_DOWN, 300 + COMBO_REPEAT_DELAY_MS - 1);
    TEST_ASSERT_EQUAL_UINT16(1, p.scroll);

    press(&p, B_DOWN, 300 + COMBO_REPEAT_DELAY_MS);
    TEST_ASSERT_EQUAL_UINT16(2, p.scroll);

    press(&p, B_DOWN, 300 + COMBO_REPEAT_DELAY_MS + COMBO_REPEAT_MS);
    TEST_ASSERT_EQUAL_UINT16(3, p.scroll);
}

static void test_the_band_clamps_at_both_ends_rather_than_wrapping(void)
{
    uint32_t t;

    fill_library(LIB_COUNT);
    picker_t p = fresh(PICKER_MODE_PENDING, true, false, NULL);

    open_row(&p, 0, 0);
    picker_set_scroll_span(&p, 19, LIB_ROWS);

    /* Walk to the bottom with fresh presses, each of which steps at once. */
    for (t = 300; p.scroll < p.scroll_max; t += 2) {
        press(&p, B_DOWN, t);
        press(&p, B_NONE, t + 1);
    }
    TEST_ASSERT_EQUAL_UINT16(8, p.scroll);

    /* A wrap here would jump the description back to its first line. */
    TEST_ASSERT_EQUAL_UINT8(PICKER_EVENT_NONE, press(&p, B_DOWN, t));
    TEST_ASSERT_EQUAL_UINT16(8, p.scroll);

    for (t += 2; p.scroll > 0; t += 2) {
        press(&p, B_UP, t);
        press(&p, B_NONE, t + 1);
    }
    TEST_ASSERT_EQUAL_UINT16(0, p.scroll);
    TEST_ASSERT_EQUAL_UINT8(PICKER_EVENT_NONE, press(&p, B_UP, t));
    TEST_ASSERT_EQUAL_UINT16(0, p.scroll);
}

static void test_a_page_with_nothing_to_scroll_ignores_up_and_down(void)
{
    fill_library(LIB_COUNT);
    picker_t p = fresh(PICKER_MODE_PENDING, true, false, NULL);

    open_row(&p, 0, 0);

    /* Exactly a bandful, and less than a bandful, are both a span of zero. */
    picker_set_scroll_span(&p, LIB_ROWS, LIB_ROWS);
    TEST_ASSERT_EQUAL_UINT16(0, p.scroll_max);
    TEST_ASSERT_EQUAL_UINT8(PICKER_EVENT_NONE, press(&p, B_DOWN, 300));
    TEST_ASSERT_EQUAL_UINT16(0, p.scroll);

    picker_set_scroll_span(&p, 4, LIB_ROWS);
    TEST_ASSERT_EQUAL_UINT16(0, p.scroll_max);
    press(&p, B_NONE, 350);
    TEST_ASSERT_EQUAL_UINT8(PICKER_EVENT_NONE, press(&p, B_UP, 400));
    TEST_ASSERT_EQUAL_UINT16(0, p.scroll);
}

static void test_a_shorter_span_clamps_the_offset_it_already_had(void)
{
    uint32_t t;

    fill_library(LIB_COUNT);
    picker_t p = fresh(PICKER_MODE_PENDING, true, false, NULL);

    open_row(&p, 0, 0);
    picker_set_scroll_span(&p, 19, LIB_ROWS);
    for (t = 300; p.scroll < 8; t += 2) {
        press(&p, B_DOWN, t);
        press(&p, B_NONE, t + 1);
    }
    TEST_ASSERT_EQUAL_UINT16(8, p.scroll);

    /* A shorter description arrives; the offset follows it down. */
    picker_set_scroll_span(&p, 14, LIB_ROWS);
    TEST_ASSERT_EQUAL_UINT16(3, p.scroll_max);
    TEST_ASSERT_EQUAL_UINT16(3, p.scroll);
}

static void test_left_and_right_do_nothing_on_the_detail_page(void)
{
    fill_library(LIB_COUNT);
    picker_t p = fresh(PICKER_MODE_PENDING, true, false, NULL);

    open_row(&p, 0, 0);
    picker_set_scroll_span(&p, 19, LIB_ROWS);

    TEST_ASSERT_EQUAL_UINT8(PICKER_EVENT_NONE, press(&p, B_LEFT, 400));
    TEST_ASSERT_EQUAL_UINT16(0, p.scroll);
    press(&p, B_NONE, 410);
    TEST_ASSERT_EQUAL_UINT8(PICKER_EVENT_NONE, press(&p, B_RIGHT, 420));
    TEST_ASSERT_EQUAL_UINT16(0, p.scroll);
    TEST_ASSERT_EQUAL_UINT8(PICKER_SCREEN_DETAIL, p.screen);
}

static void test_opening_a_row_starts_its_page_at_the_top(void)
{
    uint32_t t;

    fill_library(LIB_COUNT);
    picker_t p = fresh(PICKER_MODE_PENDING, true, false, NULL);

    open_row(&p, 0, 0);
    picker_set_scroll_span(&p, 19, LIB_ROWS);
    for (t = 300; p.scroll < 4; t += 2) {
        press(&p, B_DOWN, t);
        press(&p, B_NONE, t + 1);
    }
    TEST_ASSERT_EQUAL_UINT16(4, p.scroll);

    press(&p, B_B, t);
    press(&p, B_NONE, t + 1);
    TEST_ASSERT_EQUAL_UINT8(PICKER_SCREEN_LIST, p.screen);

    open_row(&p, 1, t + 2);
    TEST_ASSERT_EQUAL_UINT16(0, p.scroll);
}

/* ─── list navigation ─────────────────────────────────────────────────────── */

static void test_left_and_right_page_the_list(void)
{
    fill_library(LIB_COUNT);
    picker_t p = fresh(PICKER_MODE_PENDING, true, false, NULL);
    uint32_t t;
    int n;

    TEST_ASSERT_EQUAL_UINT8(PICKER_EVENT_REDRAW,
                            press(&p, B_RIGHT, 0));
    TEST_ASSERT_EQUAL_UINT16(LIB_ROWS, list_cursor(&p.list));
    TEST_ASSERT_EQUAL_UINT16(LIB_ROWS, list_first(&p.list));

    press(&p, B_NONE, 10);
    press(&p, B_LEFT, 20);
    TEST_ASSERT_EQUAL_UINT16(0, list_cursor(&p.list));
    TEST_ASSERT_EQUAL_UINT16(0, list_first(&p.list));

    /* 132 rows, 11 per page: 11 pages reach 121, and the 12th clamps at the
     * last row rather than wrapping. */
    t = 100;
    for (n = 0; n < 12; n++) {
        press(&p, B_NONE, t++);
        press(&p, B_RIGHT, t++);
    }
    TEST_ASSERT_EQUAL_UINT16(LIB_COUNT - 1, list_cursor(&p.list));
    TEST_ASSERT_EQUAL_UINT16(LIB_COUNT - LIB_ROWS, list_first(&p.list));
}

/* ─── media ───────────────────────────────────────────────────────────────── */

static void test_opening_a_title_requests_its_media_exactly_once(void)
{
    uint16_t idx = 0xFFFF;

    fill_library(LIB_COUNT);
    picker_t p = fresh(PICKER_MODE_PENDING, true, false, NULL);

    /* Opened before the settle ran out, so the page asks straight away. */
    open_row(&p, 2, 0);
    TEST_ASSERT_TRUE(picker_media_due(&p, 10, &idx));
    TEST_ASSERT_EQUAL_UINT16(2, idx);

    /* Asked once and not again. */
    TEST_ASSERT_FALSE(picker_media_due(&p, 1000, &idx));
}

static void test_a_media_report_sets_each_image_state(void)
{
    uint16_t idx = 0;

    fill_library(LIB_COUNT);
    picker_t p = fresh(PICKER_MODE_PENDING, true, false, NULL);

    open_row(&p, 0, 0);
    TEST_ASSERT_TRUE(picker_media_due(&p, 10, &idx));

    /* A report for a title the highlight has left is stale and changes
     * nothing. */
    TEST_ASSERT_EQUAL_UINT8(PICKER_EVENT_NONE,
                            picker_media_loaded(&p, 99, true, true));
    TEST_ASSERT_EQUAL_UINT8(PICKER_MEDIA_LOADING, p.art_state);
    TEST_ASSERT_EQUAL_UINT8(PICKER_MEDIA_LOADING, p.shot_state);

    /* A cover with no snapshot is an ordinary case, not a failure. */
    TEST_ASSERT_EQUAL_UINT8(PICKER_EVENT_MEDIA,
                            picker_media_loaded(&p, idx, true, false));
    TEST_ASSERT_EQUAL_UINT8(PICKER_MEDIA_READY, p.art_state);
    TEST_ASSERT_EQUAL_UINT8(PICKER_MEDIA_MISSING, p.shot_state);
}

static void test_an_action_row_never_requests_media_or_a_description(void)
{
    uint16_t idx = 0;

    fill_library(LIB_COUNT);
    picker_t finish = fresh(PICKER_MODE_IMMEDIATE, true, false, NULL);
    picker_t cancel = fresh(PICKER_MODE_PENDING, true, true, NULL);

    /* Highlighted long past the settle, on the list. */
    TEST_ASSERT_FALSE(picker_media_due(&finish, 5000, &idx));
    TEST_ASSERT_FALSE(picker_media_due(&cancel, 5000, &idx));

    open_row(&finish, 0, 0);
    TEST_ASSERT_EQUAL_UINT8(PICKER_ROW_FINISH, finish.rows[0].kind);
    TEST_ASSERT_FALSE(picker_media_due(&finish, 5000, &idx));
    TEST_ASSERT_FALSE(picker_desc_due(&finish, &idx));

    open_row(&cancel, 0, 0);
    TEST_ASSERT_FALSE(picker_media_due(&cancel, 5000, &idx));
    TEST_ASSERT_FALSE(picker_desc_due(&cancel, &idx));
}

static void test_hovered_media_is_due_once_after_the_settle(void)
{
    uint16_t idx = 0xFFFF;

    fill_library(LIB_COUNT);
    picker_t p = fresh(PICKER_MODE_PENDING, true, false, NULL);

    press(&p, B_DOWN, 1000);
    press(&p, B_NONE, 1001);
    TEST_ASSERT_FALSE(picker_media_due(&p, 1000 + PICKER_MEDIA_SETTLE_MS - 1,
                                       &idx));
    TEST_ASSERT_TRUE(picker_media_due(&p, 1000 + PICKER_MEDIA_SETTLE_MS,
                                      &idx));
    TEST_ASSERT_EQUAL_UINT16(1, idx);
    TEST_ASSERT_FALSE(picker_media_due(&p, 5000, &idx));
}

static void test_a_move_inside_the_settle_restarts_it(void)
{
    uint16_t idx = 0xFFFF;

    fill_library(LIB_COUNT);
    picker_t p = fresh(PICKER_MODE_PENDING, true, false, NULL);

    press(&p, B_DOWN, 1000);
    press(&p, B_NONE, 1001);
    press(&p, B_DOWN, 1040);
    press(&p, B_NONE, 1041);
    TEST_ASSERT_FALSE(picker_media_due(&p, 1000 + PICKER_MEDIA_SETTLE_MS,
                                       &idx));
    TEST_ASSERT_TRUE(picker_media_due(&p, 1040 + PICKER_MEDIA_SETTLE_MS,
                                      &idx));
    TEST_ASSERT_EQUAL_UINT16(2, idx);
}

static void test_a_move_resets_both_images_and_rearms_the_request(void)
{
    uint16_t idx = 0;

    fill_library(LIB_COUNT);
    picker_t p = fresh(PICKER_MODE_PENDING, true, false, NULL);

    TEST_ASSERT_TRUE(picker_media_due(&p, 100, &idx));
    picker_media_loaded(&p, idx, true, true);
    TEST_ASSERT_EQUAL_UINT8(PICKER_MEDIA_READY, p.art_state);

    press(&p, B_DOWN, 200);
    TEST_ASSERT_EQUAL_UINT8(PICKER_MEDIA_LOADING, p.art_state);
    TEST_ASSERT_EQUAL_UINT8(PICKER_MEDIA_LOADING, p.shot_state);
    TEST_ASSERT_TRUE(picker_media_due(&p, 200 + PICKER_MEDIA_SETTLE_MS,
                                      &idx));
    TEST_ASSERT_EQUAL_UINT16(1, idx);
}

static void test_a_report_for_a_title_no_longer_highlighted_is_ignored(void)
{
    uint16_t idx = 0;

    fill_library(LIB_COUNT);
    picker_t p = fresh(PICKER_MODE_PENDING, true, false, NULL);

    TEST_ASSERT_TRUE(picker_media_due(&p, 100, &idx));
    press(&p, B_DOWN, 200);

    /* The load finished after the highlight moved on. Taking it would paint
     * the previous title's cover beside the current one. */
    TEST_ASSERT_EQUAL_UINT8(PICKER_EVENT_NONE,
                            picker_media_loaded(&p, idx, true, true));
    TEST_ASSERT_EQUAL_UINT8(PICKER_MEDIA_LOADING, p.art_state);
    TEST_ASSERT_EQUAL_UINT8(PICKER_MEDIA_LOADING, p.shot_state);
}

static void test_opening_a_settled_title_keeps_its_images(void)
{
    uint16_t idx = 0;
    uint16_t d = 0xFFFF;

    fill_library(LIB_COUNT);
    picker_t p = fresh(PICKER_MODE_PENDING, true, false, NULL);

    TEST_ASSERT_TRUE(picker_media_due(&p, 100, &idx));
    picker_media_loaded(&p, idx, true, false);
    TEST_ASSERT_FALSE(picker_desc_due(&p, &d));

    open_row(&p, 0, 200);
    TEST_ASSERT_EQUAL_UINT8(PICKER_MEDIA_READY, p.art_state);
    TEST_ASSERT_EQUAL_UINT8(PICKER_MEDIA_MISSING, p.shot_state);
    TEST_ASSERT_FALSE(picker_media_due(&p, 300, &idx));

    /* The description is wanted once per open. */
    TEST_ASSERT_TRUE(picker_desc_due(&p, &d));
    TEST_ASSERT_EQUAL_UINT16(0, d);
    TEST_ASSERT_FALSE(picker_desc_due(&p, &d));

    /* B keeps the images, because the highlight has not moved; opening again
     * asks for the description again. */
    press(&p, B_B, 400);
    press(&p, B_NONE, 401);
    TEST_ASSERT_EQUAL_UINT8(PICKER_MEDIA_READY, p.art_state);
    TEST_ASSERT_FALSE(picker_desc_due(&p, &d));
    open_row(&p, 0, 500);
    TEST_ASSERT_TRUE(picker_desc_due(&p, &d));
}

/* ─── events ──────────────────────────────────────────────────────────────── */

static void test_a_move_within_the_window_names_its_rows_and_media(void)
{
    fill_library(LIB_COUNT);
    picker_t p = fresh(PICKER_MODE_PENDING, true, false, NULL);

    TEST_ASSERT_EQUAL_UINT8(PICKER_EVENT_ROWS | PICKER_EVENT_MEDIA,
                            press(&p, B_DOWN, 0));
    TEST_ASSERT_EQUAL_UINT16(0, p.prev_cursor);
    TEST_ASSERT_EQUAL_UINT16(1, list_cursor(&p.list));
}

static void test_a_move_that_scrolls_the_window_redraws_it(void)
{
    fill_library(LIB_COUNT);
    picker_t p = fresh(PICKER_MODE_PENDING, true, false, NULL);

    /* Up from the top wraps to the last page. */
    TEST_ASSERT_EQUAL_UINT8(PICKER_EVENT_REDRAW,
                            press(&p, B_UP, 0));
}

static void test_opening_leaving_and_paging_redraw_the_screen(void)
{
    fill_library(LIB_COUNT);
    picker_t p = fresh(PICKER_MODE_PENDING, true, false, NULL);

    TEST_ASSERT_EQUAL_UINT8(PICKER_EVENT_REDRAW,
                            press(&p, B_RIGHT, 0));
    press(&p, B_NONE, 10);
    TEST_ASSERT_EQUAL_UINT8(PICKER_EVENT_REDRAW, press(&p, B_A, 20));
    press(&p, B_NONE, 30);
    TEST_ASSERT_EQUAL_UINT8(PICKER_EVENT_REDRAW, press(&p, B_B, 40));
}

static void test_the_hold_names_only_the_bar(void)
{
    fill_library(LIB_COUNT);
    picker_t p = fresh(PICKER_MODE_PENDING, true, false, NULL);

    open_row(&p, 0, 0);
    TEST_ASSERT_EQUAL_UINT8(PICKER_EVENT_BAR, press(&p, B_A, 100));
    TEST_ASSERT_EQUAL_UINT8(PICKER_EVENT_BAR, press(&p, B_A, 116));
    TEST_ASSERT_EQUAL_UINT8(PICKER_EVENT_BAR, press(&p, B_NONE, 132));
    TEST_ASSERT_EQUAL_UINT8(PICKER_EVENT_NONE, press(&p, B_NONE, 148));
}

/* ─── the marquee ─────────────────────────────────────────────────────────── */

/* Sample the list with nothing held at `at`, and return what it said. */
static uint8_t idle(picker_t* p, uint32_t at)
{
    return picker_input(p, B_NONE, at);
}

static void test_the_marquee_runs_its_timeline(void)
{
    const int16_t span = 30;
    const uint32_t run = span * PICKER_MARQUEE_STEP_MS;
    const uint32_t t0 = 1000;

    fill_library(LIB_COUNT);
    picker_t p = fresh(PICKER_MODE_PENDING, true, false, NULL);

    press(&p, B_DOWN, t0);
    press(&p, B_NONE, t0 + 1);
    TEST_ASSERT_EQUAL_INT(PICKER_OK, picker_set_marquee_span(&p, span));

    /* Still for the delay. */
    TEST_ASSERT_EQUAL_UINT8(PICKER_EVENT_NONE,
                            idle(&p, t0 + PICKER_MARQUEE_DELAY_MS - 1));
    TEST_ASSERT_EQUAL_INT16(0, p.marquee_px);

    /* One pixel a step. */
    TEST_ASSERT_EQUAL_UINT8(
        PICKER_EVENT_MARQUEE,
        idle(&p, t0 + PICKER_MARQUEE_DELAY_MS + PICKER_MARQUEE_STEP_MS));
    TEST_ASSERT_EQUAL_INT16(1, p.marquee_px);
    TEST_ASSERT_EQUAL_UINT8(
        PICKER_EVENT_NONE,
        idle(&p, t0 + PICKER_MARQUEE_DELAY_MS + PICKER_MARQUEE_STEP_MS + 1));

    /* The end, and the pause there. */
    idle(&p, t0 + PICKER_MARQUEE_DELAY_MS + run);
    TEST_ASSERT_EQUAL_INT16(span, p.marquee_px);
    TEST_ASSERT_EQUAL_UINT8(
        PICKER_EVENT_NONE,
        idle(&p, t0 + PICKER_MARQUEE_DELAY_MS + run + PICKER_MARQUEE_PAUSE_MS -
                     1));
    TEST_ASSERT_EQUAL_INT16(span, p.marquee_px);

    /* Back to the start, and the delay again. */
    TEST_ASSERT_EQUAL_UINT8(
        PICKER_EVENT_MARQUEE,
        idle(&p, t0 + PICKER_MARQUEE_DELAY_MS + run + PICKER_MARQUEE_PAUSE_MS));
    TEST_ASSERT_EQUAL_INT16(0, p.marquee_px);
    TEST_ASSERT_EQUAL_UINT8(
        PICKER_EVENT_NONE,
        idle(&p, t0 + 2 * PICKER_MARQUEE_DELAY_MS + run +
                     PICKER_MARQUEE_PAUSE_MS - 1));
}

static void test_a_move_resets_the_marquee(void)
{
    fill_library(LIB_COUNT);
    picker_t p = fresh(PICKER_MODE_PENDING, true, false, NULL);

    picker_set_marquee_span(&p, 30);
    idle(&p, PICKER_MARQUEE_DELAY_MS + 10 * PICKER_MARQUEE_STEP_MS);
    TEST_ASSERT_EQUAL_INT16(10, p.marquee_px);

    press(&p, B_DOWN, 5000);
    TEST_ASSERT_EQUAL_INT16(0, p.marquee_px);
    TEST_ASSERT_EQUAL_INT16(0, p.marquee_span);
    TEST_ASSERT_EQUAL_UINT32(5000, p.marquee_t0);
}

static void test_a_title_that_fits_never_scrolls(void)
{
    uint32_t t;

    fill_library(LIB_COUNT);
    picker_t p = fresh(PICKER_MODE_PENDING, true, false, NULL);

    picker_set_marquee_span(&p, 0);
    for (t = 0; t < 10000; t += 16) {
        TEST_ASSERT_EQUAL_UINT8(PICKER_EVENT_NONE, idle(&p, t));
    }
    TEST_ASSERT_EQUAL_INT16(0, p.marquee_px);
}

/* ─── argument and empty cases ────────────────────────────────────────────── */

static void test_a_wizard_with_no_starter_is_empty(void)
{
    picker_t p;

    fill_library_without_starters(LIB_COUNT);
    TEST_ASSERT_EQUAL_INT(PICKER_ERR_EMPTY,
                          picker_init(&p, PICKER_MODE_IMMEDIATE, &lib, false,
                                      false, NULL, LIB_ROWS));
    TEST_ASSERT_EQUAL_UINT16(0, p.row_count);
    TEST_ASSERT_EQUAL_UINT8(PICKER_EVENT_NONE, press(&p, B_A, 0));
    TEST_ASSERT_EQUAL_UINT8(PICKER_SCREEN_LIST, p.screen);
}

static void test_init_rejects_bad_arguments(void)
{
    picker_t p;

    fill_library(LIB_COUNT);
    TEST_ASSERT_EQUAL_INT(PICKER_ERR_ARGS,
                          picker_init(NULL, PICKER_MODE_PENDING, &lib, true,
                                      false, NULL, LIB_ROWS));
    TEST_ASSERT_EQUAL_INT(PICKER_ERR_ARGS,
                          picker_init(&p, PICKER_MODE_PENDING, NULL, true,
                                      false, NULL, LIB_ROWS));
    TEST_ASSERT_EQUAL_INT(PICKER_ERR_ARGS,
                          picker_init(&p, PICKER_MODE_PENDING, &lib, true,
                                      false, NULL, 0));
    TEST_ASSERT_EQUAL_INT(PICKER_ERR_ARGS,
                          picker_set_scroll_span(NULL, 19, LIB_ROWS));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_pending_without_a_pending_write_is_the_whole_catalog);
    RUN_TEST(test_a_pending_write_adds_a_cancel_row_at_the_top);
    RUN_TEST(test_the_wizard_shows_starters_only);
    RUN_TEST(test_finish_setup_appears_only_once_the_wildcard_is_written);
    RUN_TEST(test_a_made_filename_marks_exactly_its_row);
    RUN_TEST(test_a_opens_the_highlighted_titles_detail_page);
    RUN_TEST(test_b_returns_to_the_list_with_the_cursor_unchanged);
    RUN_TEST(test_the_a_that_opened_the_page_cannot_confirm_it);
    RUN_TEST(test_a_fresh_hold_completes_at_the_hold_figure);
    RUN_TEST(test_letting_go_early_resets_the_hold);
    RUN_TEST(test_a_done_machine_ignores_every_further_sample);
    RUN_TEST(test_launch_mode_is_every_game_and_no_action_row);
    RUN_TEST(test_a_held_launch_pick_returns_its_filename);
    RUN_TEST(test_an_early_release_in_launch_mode_picks_nothing);
    RUN_TEST(test_a_confirmed_game_row_returns_its_filename);
    RUN_TEST(test_every_mode_returns_the_wildcard_target);
    RUN_TEST(test_the_cancel_row_returns_cancel_pending);
    RUN_TEST(test_the_finish_row_returns_finish);
    RUN_TEST(test_the_band_scrolls_a_line_at_the_shared_cadence);
    RUN_TEST(test_the_band_clamps_at_both_ends_rather_than_wrapping);
    RUN_TEST(test_a_page_with_nothing_to_scroll_ignores_up_and_down);
    RUN_TEST(test_a_shorter_span_clamps_the_offset_it_already_had);
    RUN_TEST(test_left_and_right_do_nothing_on_the_detail_page);
    RUN_TEST(test_opening_a_row_starts_its_page_at_the_top);
    RUN_TEST(test_left_and_right_page_the_list);
    RUN_TEST(test_opening_a_title_requests_its_media_exactly_once);
    RUN_TEST(test_a_media_report_sets_each_image_state);
    RUN_TEST(test_an_action_row_never_requests_media_or_a_description);
    RUN_TEST(test_hovered_media_is_due_once_after_the_settle);
    RUN_TEST(test_a_move_inside_the_settle_restarts_it);
    RUN_TEST(test_a_move_resets_both_images_and_rearms_the_request);
    RUN_TEST(test_a_report_for_a_title_no_longer_highlighted_is_ignored);
    RUN_TEST(test_opening_a_settled_title_keeps_its_images);
    RUN_TEST(test_a_move_within_the_window_names_its_rows_and_media);
    RUN_TEST(test_a_move_that_scrolls_the_window_redraws_it);
    RUN_TEST(test_opening_leaving_and_paging_redraw_the_screen);
    RUN_TEST(test_the_hold_names_only_the_bar);
    RUN_TEST(test_the_marquee_runs_its_timeline);
    RUN_TEST(test_a_move_resets_the_marquee);
    RUN_TEST(test_a_title_that_fits_never_scrolls);
    RUN_TEST(test_a_wizard_with_no_starter_is_empty);
    RUN_TEST(test_init_rejects_bad_arguments);
    return UNITY_END();
}
