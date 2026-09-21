#include <unity.h>

#include <string.h>

#include "audio/mix.h"
#include "input/combo.h"
#include "ui/diag.h"

/*
 * The diagnostic mode's decisions, driven with literal timestamps.
 *
 * Two windows appear throughout — 240x216 and 260x234 on a 320x240 panel —
 * because diag_init() takes any window and the clamp is arithmetic on its
 * size, not on a geometry. They were the two shipped render geometries when
 * this suite was written, and they stay because two sizes prove the clamp
 * moves with the window. The nudge clamps therefore
 * fall at 320 - 240 = 80 by 240 - 216 = 24, and at 320 - 260 = 60 by
 * 240 - 234 = 6 — the second window has almost no vertical slack, which is
 * exactly the case a one-pixel nudge has to get right.
 */

#define PANEL_W 320
#define PANEL_H 240

/* A stored trim to init from, and deliberately not the shipping default: a
 * suite that passed only at the batch-typical porch would be pinning one
 * board's measurement through the back door. */
#define TRIM_FPA 20
#define TRIM_RATIO 32

#define WIN24_W 240
#define WIN24_H 216
#define WIN24_X_MAX 80
#define WIN24_Y_MAX 24

#define WIN26_W 260
#define WIN26_H 234
#define WIN26_X_MAX 60
#define WIN26_Y_MAX  6

static diag_t d;

void setUp(void)
{
    memset(&d, 0, sizeof(d));
    TEST_ASSERT_EQUAL_INT(DIAG_OK,
        diag_init(&d, PANEL_W, PANEL_H, WIN24_W, WIN24_H, 40, 12, 40, 12,
                  MIX_VOL_MED, 0, TRIM_FPA, TRIM_RATIO));
}

void tearDown(void)
{
}

/* Every sample the machine ever sees. `ev` is the combo event this call
 * produced; `word` is the masked joypad word that came with it. */
static uint16_t sample(uint8_t ev, uint8_t word, uint32_t now_ms)
{
    return diag_input(&d, ev, word, now_ms);
}

/*
 * Walk to `page` from wherever the machine is, the only way a builder can —
 * one Select+Right at a time, wrapping round the end if that is the short way
 * there.
 */
static void goto_page(uint8_t page)
{
    uint8_t steps = (uint8_t)((page + DIAG_PAGE_COUNT - diag_page(&d))
                              % DIAG_PAGE_COUNT);
    uint8_t i;

    for (i = 0; i < steps; i++) {
        sample(COMBO_EVENT_BRIGHT_UP, COMBO_BTN_SELECT, 0);
    }
    TEST_ASSERT_EQUAL_UINT8(page, diag_page(&d));
}

static int16_t x_of(void)
{
    int16_t x = -1;
    diag_origin(&d, &x, NULL);
    return x;
}

static int16_t y_of(void)
{
    int16_t y = -1;
    diag_origin(&d, NULL, &y);
    return y;
}

/* ─── init ────────────────────────────────────────────────────────────────── */

static void test_init_rejects_null_and_a_window_bigger_than_the_panel(void)
{
    TEST_ASSERT_EQUAL_INT(DIAG_ERR_ARGS,
        diag_init(NULL, PANEL_W, PANEL_H, WIN24_W, WIN24_H, 0, 0, 0, 0, 0, 0,
                  TRIM_FPA, TRIM_RATIO));
    TEST_ASSERT_EQUAL_INT(DIAG_ERR_ARGS,
        diag_init(&d, PANEL_W, PANEL_H, PANEL_W + 1, WIN24_H,
                  0, 0, 0, 0, 0, 0, TRIM_FPA, TRIM_RATIO));
    TEST_ASSERT_EQUAL_INT(DIAG_ERR_ARGS,
        diag_init(&d, PANEL_W, PANEL_H, WIN24_W, PANEL_H + 1,
                  0, 0, 0, 0, 0, 0, TRIM_FPA, TRIM_RATIO));
    /* A window exactly the size of the panel is legal and leaves no slack. */
    TEST_ASSERT_EQUAL_INT(DIAG_OK,
        diag_init(&d, PANEL_W, PANEL_H, PANEL_W, PANEL_H, 0, 0, 0, 0, 0, 0,
                  TRIM_FPA, TRIM_RATIO));
    TEST_ASSERT_EQUAL_INT16(0, d.x_max);
    TEST_ASSERT_EQUAL_INT16(0, d.y_max);
}

static void test_init_clamps_a_stored_origin_and_the_default(void)
{
    TEST_ASSERT_EQUAL_INT(DIAG_OK,
        diag_init(&d, PANEL_W, PANEL_H, WIN24_W, WIN24_H, 999, -5, 999, -5,
                  MIX_VOL_HIGH, 0, TRIM_FPA, TRIM_RATIO));
    TEST_ASSERT_EQUAL_INT16(WIN24_X_MAX, x_of());
    TEST_ASSERT_EQUAL_INT16(0, y_of());
    TEST_ASSERT_EQUAL_INT16(WIN24_X_MAX, d.default_x);
    TEST_ASSERT_EQUAL_INT16(0, d.default_y);
}

static void test_init_clamps_a_stored_volume_and_frameskip(void)
{
    TEST_ASSERT_EQUAL_INT(DIAG_OK,
        diag_init(&d, PANEL_W, PANEL_H, WIN24_W, WIN24_H, 0, 0, 0, 0,
                  200, 200, 200, 200));
    TEST_ASSERT_EQUAL_UINT8(MIX_VOL_OFF, diag_volume(&d));
    TEST_ASSERT_EQUAL_UINT8(DIAG_FRAMESKIP_MAX, diag_frameskip(&d));
}

static void test_init_starts_on_the_buttons_page_with_the_tone_off(void)
{
    TEST_ASSERT_EQUAL_UINT8(DIAG_PAGE_BUTTONS, diag_page(&d));
    TEST_ASSERT_FALSE(diag_tone_on(&d));
    TEST_ASSERT_EQUAL_UINT8(DIAG_PATTERN_BARS, diag_pattern(&d));
    TEST_ASSERT_FALSE(diag_toast_active(&d, 0));
}

/* ─── paging ──────────────────────────────────────────────────────────────── */

static void test_eight_select_rights_walk_every_page_and_wrap(void)
{
    uint8_t i;

    for (i = 1; i <= DIAG_PAGE_COUNT; i++) {
        uint16_t ev = sample(COMBO_EVENT_BRIGHT_UP, COMBO_BTN_SELECT, 0);
        /* Arriving at the tag page is a scan as well as a page change; every
         * other arrival is just the page change. */
        uint16_t expect = DIAG_EV_REDRAW | DIAG_EV_PAGE;
        if ((i % DIAG_PAGE_COUNT) == DIAG_PAGE_NFC) {
            expect |= DIAG_EV_NFC_SCAN;
        }
        TEST_ASSERT_EQUAL_HEX16(expect, ev);
        TEST_ASSERT_EQUAL_UINT8((uint8_t)(i % DIAG_PAGE_COUNT),
                                diag_page(&d));
    }
    TEST_ASSERT_EQUAL_UINT8(DIAG_PAGE_BUTTONS, diag_page(&d));
}

static void test_eight_select_lefts_walk_every_page_backwards(void)
{
    uint8_t i;

    for (i = 1; i <= DIAG_PAGE_COUNT; i++) {
        uint16_t ev = sample(COMBO_EVENT_BRIGHT_DOWN, COMBO_BTN_SELECT, 0);
        uint8_t page = (uint8_t)((DIAG_PAGE_COUNT - i) % DIAG_PAGE_COUNT);
        uint16_t expect = DIAG_EV_REDRAW | DIAG_EV_PAGE;
        if (page == DIAG_PAGE_NFC) {
            expect |= DIAG_EV_NFC_SCAN;
        }
        TEST_ASSERT_EQUAL_HEX16(expect, ev);
        TEST_ASSERT_EQUAL_UINT8(page, diag_page(&d));
    }
    TEST_ASSERT_EQUAL_UINT8(DIAG_PAGE_BUTTONS, diag_page(&d));
}

static void test_no_bare_dpad_or_face_word_changes_the_page(void)
{
    static const uint8_t words[] = {
        COMBO_BTN_LEFT, COMBO_BTN_RIGHT, COMBO_BTN_UP, COMBO_BTN_DOWN,
        COMBO_BTN_A, COMBO_BTN_B, COMBO_BTN_START, COMBO_BTN_SELECT,
    };
    size_t i;

    for (i = 0; i < sizeof(words) / sizeof(words[0]); i++) {
        TEST_ASSERT_EQUAL_HEX16(0,
            sample(COMBO_EVENT_NONE, words[i], (uint32_t)(i * 100)));
        TEST_ASSERT_EQUAL_UINT8(DIAG_PAGE_BUTTONS, diag_page(&d));
        /* Release, so the next word is a fresh edge rather than a level. */
        sample(COMBO_EVENT_NONE, 0, (uint32_t)(i * 100 + 50));
    }
}

static void test_the_menu_and_volume_combos_do_nothing_here(void)
{
    TEST_ASSERT_EQUAL_HEX16(0, sample(COMBO_EVENT_MENU, 0, 0));
    TEST_ASSERT_EQUAL_HEX16(0, sample(COMBO_EVENT_VOL_UP, 0, 10));
    TEST_ASSERT_EQUAL_HEX16(0, sample(COMBO_EVENT_VOL_DOWN, 0, 20));
    TEST_ASSERT_EQUAL_UINT8(DIAG_PAGE_BUTTONS, diag_page(&d));
}

/* ─── the nudge page ──────────────────────────────────────────────────────── */

static void test_a_held_direction_steps_once_then_repeats_at_the_cadence(void)
{
    goto_page(DIAG_PAGE_NUDGE);

    /* A fresh press acts at once. */
    TEST_ASSERT_EQUAL_HEX16(DIAG_EV_REDRAW,
        sample(COMBO_EVENT_NONE, COMBO_BTN_RIGHT, 0));
    TEST_ASSERT_EQUAL_INT16(41, x_of());

    /* Nothing for COMBO_REPEAT_DELAY_MS. */
    TEST_ASSERT_EQUAL_HEX16(0,
        sample(COMBO_EVENT_NONE, COMBO_BTN_RIGHT,
               COMBO_REPEAT_DELAY_MS - 1));
    TEST_ASSERT_EQUAL_INT16(41, x_of());

    /* Then one step per COMBO_REPEAT_MS. */
    TEST_ASSERT_EQUAL_HEX16(DIAG_EV_REDRAW,
        sample(COMBO_EVENT_NONE, COMBO_BTN_RIGHT, COMBO_REPEAT_DELAY_MS));
    TEST_ASSERT_EQUAL_INT16(42, x_of());
    TEST_ASSERT_EQUAL_HEX16(DIAG_EV_REDRAW,
        sample(COMBO_EVENT_NONE, COMBO_BTN_RIGHT,
               COMBO_REPEAT_DELAY_MS + COMBO_REPEAT_MS));
    TEST_ASSERT_EQUAL_INT16(43, x_of());
}

static void test_two_directions_at_once_do_nothing(void)
{
    goto_page(DIAG_PAGE_NUDGE);

    TEST_ASSERT_EQUAL_HEX16(0,
        sample(COMBO_EVENT_NONE, COMBO_BTN_UP | COMBO_BTN_LEFT, 0));
    TEST_ASSERT_EQUAL_INT16(40, x_of());
    TEST_ASSERT_EQUAL_INT16(12, y_of());

    /* Whichever direction survives the fumble acts immediately rather than
     * inheriting a deadline. */
    TEST_ASSERT_EQUAL_HEX16(DIAG_EV_REDRAW,
        sample(COMBO_EVENT_NONE, COMBO_BTN_LEFT, 1));
    TEST_ASSERT_EQUAL_INT16(39, x_of());
}

/* Hold `word` long enough to run the origin into its stop. Each press is a
 * fresh one, so the repeat cadence is not in the way. */
static void hammer(uint8_t word, int presses)
{
    int i;
    for (i = 0; i < presses; i++) {
        sample(COMBO_EVENT_NONE, 0, (uint32_t)(i * 10));
        sample(COMBO_EVENT_NONE, word, (uint32_t)(i * 10 + 5));
    }
}

static void test_the_nudge_clamps_at_the_panel_edges_in_a_240x216_window(void)
{
    goto_page(DIAG_PAGE_NUDGE);

    /* 40 + 60 presses is 20 past the stop at 80. */
    hammer(COMBO_BTN_RIGHT, 100);
    TEST_ASSERT_EQUAL_INT16(WIN24_X_MAX, x_of());
    hammer(COMBO_BTN_LEFT, 100);
    TEST_ASSERT_EQUAL_INT16(0, x_of());
    hammer(COMBO_BTN_DOWN, 40);
    TEST_ASSERT_EQUAL_INT16(WIN24_Y_MAX, y_of());
    hammer(COMBO_BTN_UP, 40);
    TEST_ASSERT_EQUAL_INT16(0, y_of());

    /* A press into the stop reports nothing: the screen did not change. */
    TEST_ASSERT_EQUAL_HEX16(0, sample(COMBO_EVENT_NONE, 0, 9000));
    TEST_ASSERT_EQUAL_HEX16(0, sample(COMBO_EVENT_NONE, COMBO_BTN_UP, 9005));
}

static void test_the_nudge_clamps_at_the_panel_edges_in_a_260x234_window(void)
{
    TEST_ASSERT_EQUAL_INT(DIAG_OK,
        diag_init(&d, PANEL_W, PANEL_H, WIN26_W, WIN26_H, 30, 3, 30, 3,
                  MIX_VOL_MED, 0, TRIM_FPA, TRIM_RATIO));
    goto_page(DIAG_PAGE_NUDGE);

    hammer(COMBO_BTN_RIGHT, 100);
    TEST_ASSERT_EQUAL_INT16(WIN26_X_MAX, x_of());
    hammer(COMBO_BTN_LEFT, 100);
    TEST_ASSERT_EQUAL_INT16(0, x_of());
    hammer(COMBO_BTN_DOWN, 20);
    TEST_ASSERT_EQUAL_INT16(WIN26_Y_MAX, y_of());
    hammer(COMBO_BTN_UP, 20);
    TEST_ASSERT_EQUAL_INT16(0, y_of());
}

static void test_a_saves_the_nudge_and_the_toast_expires_once(void)
{
    goto_page(DIAG_PAGE_NUDGE);

    TEST_ASSERT_EQUAL_HEX16(DIAG_EV_SAVE_NUDGE | DIAG_EV_REDRAW,
        sample(COMBO_EVENT_NONE, COMBO_BTN_A, 5000));

    TEST_ASSERT_TRUE(diag_toast_active(&d, 5000));
    TEST_ASSERT_TRUE(diag_toast_active(&d, 5000 + DIAG_TOAST_MS - 1));
    TEST_ASSERT_FALSE(diag_toast_active(&d, 5000 + DIAG_TOAST_MS));

    /* Time alone clears it, and reports the redraw exactly once. */
    TEST_ASSERT_EQUAL_HEX16(0, diag_tick(&d, 5000 + DIAG_TOAST_MS - 1));
    TEST_ASSERT_EQUAL_HEX16(DIAG_EV_REDRAW,
        diag_tick(&d, 5000 + DIAG_TOAST_MS));
    TEST_ASSERT_EQUAL_HEX16(0, diag_tick(&d, 5000 + DIAG_TOAST_MS));
    TEST_ASSERT_EQUAL_HEX16(0, diag_tick(&d, 60000));
}

static void test_b_restores_the_compile_time_default(void)
{
    goto_page(DIAG_PAGE_NUDGE);

    hammer(COMBO_BTN_RIGHT, 5);
    hammer(COMBO_BTN_UP, 5);
    TEST_ASSERT_EQUAL_INT16(45, x_of());
    TEST_ASSERT_EQUAL_INT16(7, y_of());

    sample(COMBO_EVENT_NONE, 0, 1000);
    TEST_ASSERT_EQUAL_HEX16(DIAG_EV_REDRAW,
        sample(COMBO_EVENT_NONE, COMBO_BTN_B, 1005));
    TEST_ASSERT_EQUAL_INT16(40, x_of());
    TEST_ASSERT_EQUAL_INT16(12, y_of());
}

static void test_a_held_a_fires_once(void)
{
    goto_page(DIAG_PAGE_NUDGE);

    TEST_ASSERT_EQUAL_HEX16(DIAG_EV_SAVE_NUDGE | DIAG_EV_REDRAW,
        sample(COMBO_EVENT_NONE, COMBO_BTN_A, 100));
    TEST_ASSERT_EQUAL_HEX16(0, sample(COMBO_EVENT_NONE, COMBO_BTN_A, 200));
    TEST_ASSERT_EQUAL_HEX16(0, sample(COMBO_EVENT_NONE, COMBO_BTN_A, 300));
}

/* ─── the tag page ────────────────────────────────────────────────────────── */

static void test_arriving_at_the_tag_page_asks_for_one_scan_either_way(void)
{
    /* Forwards, from the SD page. */
    goto_page(DIAG_PAGE_SD);
    TEST_ASSERT_EQUAL_HEX16(DIAG_EV_REDRAW | DIAG_EV_PAGE | DIAG_EV_NFC_SCAN,
        sample(COMBO_EVENT_BRIGHT_UP, COMBO_BTN_SELECT, 0));
    TEST_ASSERT_EQUAL_UINT8(DIAG_PAGE_NFC, diag_page(&d));

    /* Backwards, from the battery page. */
    TEST_ASSERT_EQUAL_HEX16(DIAG_EV_REDRAW | DIAG_EV_PAGE,
        sample(COMBO_EVENT_BRIGHT_UP, COMBO_BTN_SELECT, 0));
    TEST_ASSERT_EQUAL_UINT8(DIAG_PAGE_BATTERY, diag_page(&d));
    TEST_ASSERT_EQUAL_HEX16(DIAG_EV_REDRAW | DIAG_EV_PAGE | DIAG_EV_NFC_SCAN,
        sample(COMBO_EVENT_BRIGHT_DOWN, COMBO_BTN_SELECT, 0));
    TEST_ASSERT_EQUAL_UINT8(DIAG_PAGE_NFC, diag_page(&d));
}

static void test_a_rescans_on_the_tag_page_and_nowhere_else(void)
{
    goto_page(DIAG_PAGE_NFC);

    TEST_ASSERT_EQUAL_HEX16(DIAG_EV_NFC_SCAN,
        sample(COMBO_EVENT_NONE, COMBO_BTN_A, 100));

    /* A on the readout pages does nothing at all. */
    sample(COMBO_EVENT_BRIGHT_DOWN, COMBO_BTN_SELECT, 200);
    TEST_ASSERT_EQUAL_UINT8(DIAG_PAGE_SD, diag_page(&d));
    TEST_ASSERT_EQUAL_HEX16(0, sample(COMBO_EVENT_NONE, COMBO_BTN_A, 300));

    sample(COMBO_EVENT_BRIGHT_DOWN, COMBO_BTN_SELECT, 400);
    TEST_ASSERT_EQUAL_UINT8(DIAG_PAGE_BUTTONS, diag_page(&d));
    sample(COMBO_EVENT_NONE, 0, 450);
    TEST_ASSERT_EQUAL_HEX16(0, sample(COMBO_EVENT_NONE, COMBO_BTN_A, 500));
}

/* ─── the audio page ──────────────────────────────────────────────────────── */

static void test_a_toggles_the_tone(void)
{
    goto_page(DIAG_PAGE_AUDIO);

    TEST_ASSERT_EQUAL_HEX16(DIAG_EV_TONE | DIAG_EV_REDRAW,
        sample(COMBO_EVENT_NONE, COMBO_BTN_A, 100));
    TEST_ASSERT_TRUE(diag_tone_on(&d));

    sample(COMBO_EVENT_NONE, 0, 150);
    TEST_ASSERT_EQUAL_HEX16(DIAG_EV_TONE | DIAG_EV_REDRAW,
        sample(COMBO_EVENT_NONE, COMBO_BTN_A, 200));
    TEST_ASSERT_FALSE(diag_tone_on(&d));
}

static void test_the_volume_index_steps_without_wrapping(void)
{
    goto_page(DIAG_PAGE_AUDIO);

    /* Start at the loudest and try to go further. */
    hammer(COMBO_BTN_UP, 4);
    TEST_ASSERT_EQUAL_UINT8(MIX_VOL_HIGH, diag_volume(&d));
    sample(COMBO_EVENT_NONE, 0, 1000);
    TEST_ASSERT_EQUAL_HEX16(0, sample(COMBO_EVENT_NONE, COMBO_BTN_UP, 1005));
    TEST_ASSERT_EQUAL_UINT8(MIX_VOL_HIGH, diag_volume(&d));

    /* Down walks 0 -> 1 -> 2 -> 3 and then stops. */
    sample(COMBO_EVENT_NONE, 0, 2000);
    TEST_ASSERT_EQUAL_HEX16(DIAG_EV_TONE | DIAG_EV_REDRAW,
        sample(COMBO_EVENT_NONE, COMBO_BTN_DOWN, 2005));
    TEST_ASSERT_EQUAL_UINT8(MIX_VOL_MED, diag_volume(&d));
    hammer(COMBO_BTN_DOWN, 2);
    TEST_ASSERT_EQUAL_UINT8(MIX_VOL_OFF, diag_volume(&d));
    sample(COMBO_EVENT_NONE, 0, 3000);
    TEST_ASSERT_EQUAL_HEX16(0, sample(COMBO_EVENT_NONE, COMBO_BTN_DOWN, 3005));
    TEST_ASSERT_EQUAL_UINT8(MIX_VOL_OFF, diag_volume(&d));
}

static void test_leaving_the_audio_page_silences_the_tone(void)
{
    goto_page(DIAG_PAGE_AUDIO);
    sample(COMBO_EVENT_NONE, COMBO_BTN_A, 100);
    TEST_ASSERT_TRUE(diag_tone_on(&d));

    TEST_ASSERT_EQUAL_HEX16(DIAG_EV_REDRAW | DIAG_EV_PAGE | DIAG_EV_TONE,
        sample(COMBO_EVENT_BRIGHT_UP, COMBO_BTN_SELECT, 200));
    TEST_ASSERT_FALSE(diag_tone_on(&d));
    TEST_ASSERT_EQUAL_UINT8(DIAG_PAGE_DISPLAY, diag_page(&d));

    /* Leaving with the tone already off says nothing about the tone. */
    goto_page(DIAG_PAGE_AUDIO);
    TEST_ASSERT_EQUAL_UINT8(DIAG_PAGE_AUDIO, diag_page(&d));
    TEST_ASSERT_EQUAL_HEX16(DIAG_EV_REDRAW | DIAG_EV_PAGE,
        sample(COMBO_EVENT_BRIGHT_UP, COMBO_BTN_SELECT, 300));
}

/* ─── the display page ────────────────────────────────────────────────────── */

static void test_the_display_pattern_cycles_three_ways_round(void)
{
    goto_page(DIAG_PAGE_DISPLAY);

    hammer(COMBO_BTN_DOWN, 1);
    TEST_ASSERT_EQUAL_UINT8(DIAG_PATTERN_BORDER, diag_pattern(&d));
    hammer(COMBO_BTN_DOWN, 1);
    TEST_ASSERT_EQUAL_UINT8(DIAG_PATTERN_CHECKER, diag_pattern(&d));
    hammer(COMBO_BTN_DOWN, 1);
    TEST_ASSERT_EQUAL_UINT8(DIAG_PATTERN_BARS, diag_pattern(&d));

    hammer(COMBO_BTN_UP, 1);
    TEST_ASSERT_EQUAL_UINT8(DIAG_PATTERN_CHECKER, diag_pattern(&d));
    hammer(COMBO_BTN_UP, 1);
    TEST_ASSERT_EQUAL_UINT8(DIAG_PATTERN_BORDER, diag_pattern(&d));
    hammer(COMBO_BTN_UP, 1);
    TEST_ASSERT_EQUAL_UINT8(DIAG_PATTERN_BARS, diag_pattern(&d));
}

/* ─── the system page ─────────────────────────────────────────────────────── */

static void test_frameskip_steps_within_its_range(void)
{
    uint8_t i;

    goto_page(DIAG_PAGE_SYSTEM);
    TEST_ASSERT_EQUAL_UINT8(0, diag_frameskip(&d));

    for (i = 1; i <= DIAG_FRAMESKIP_MAX; i++) {
        sample(COMBO_EVENT_NONE, 0, (uint32_t)(i * 100));
        TEST_ASSERT_EQUAL_HEX16(DIAG_EV_FRAMESKIP | DIAG_EV_REDRAW,
            sample(COMBO_EVENT_NONE, COMBO_BTN_DOWN,
                   (uint32_t)(i * 100 + 5)));
        TEST_ASSERT_EQUAL_UINT8(i, diag_frameskip(&d));
    }

    sample(COMBO_EVENT_NONE, 0, 1000);
    TEST_ASSERT_EQUAL_HEX16(0, sample(COMBO_EVENT_NONE, COMBO_BTN_DOWN, 1005));
    TEST_ASSERT_EQUAL_UINT8(DIAG_FRAMESKIP_MAX, diag_frameskip(&d));

    hammer(COMBO_BTN_UP, DIAG_FRAMESKIP_MAX);
    TEST_ASSERT_EQUAL_UINT8(0, diag_frameskip(&d));
    sample(COMBO_EVENT_NONE, 0, 4000);
    TEST_ASSERT_EQUAL_HEX16(0, sample(COMBO_EVENT_NONE, COMBO_BTN_UP, 4005));
    TEST_ASSERT_EQUAL_UINT8(0, diag_frameskip(&d));
}

/* ─── titles and NULL handling ────────────────────────────────────────────── */

static void test_every_page_has_a_title_and_the_count_has_none(void)
{
    uint8_t page;

    for (page = 0; page < DIAG_PAGE_COUNT; page++) {
        const char* title = diag_page_title(page);
        TEST_ASSERT_NOT_NULL(title);
        TEST_ASSERT_TRUE(title[0] != '\0');
    }
    TEST_ASSERT_NULL(diag_page_title(DIAG_PAGE_COUNT));
    TEST_ASSERT_NULL(diag_page_title(200));
}

static void test_a_null_state_is_inert(void)
{
    int16_t x = -7;
    int16_t y = -7;

    TEST_ASSERT_EQUAL_HEX16(0,
        diag_input(NULL, COMBO_EVENT_BRIGHT_UP, COMBO_BTN_A, 100));
    TEST_ASSERT_EQUAL_HEX16(0, diag_tick(NULL, 100));
    TEST_ASSERT_EQUAL_UINT8(DIAG_PAGE_BUTTONS, diag_page(NULL));
    TEST_ASSERT_FALSE(diag_tone_on(NULL));
    TEST_ASSERT_EQUAL_UINT8(MIX_VOL_OFF, diag_volume(NULL));
    TEST_ASSERT_EQUAL_UINT8(DIAG_PATTERN_BARS, diag_pattern(NULL));
    TEST_ASSERT_EQUAL_UINT8(0, diag_frameskip(NULL));
    TEST_ASSERT_FALSE(diag_toast_active(NULL, 100));

    diag_origin(NULL, &x, &y);
    TEST_ASSERT_EQUAL_INT16(-7, x);
    TEST_ASSERT_EQUAL_INT16(-7, y);
}


/* ─── the panel trim page ─────────────────────────────────────────────────── */

/* Mark a crossing `frames` frames after the previous one, pushing every frame
 * in between the way the binding does. */
static uint16_t run_frames_then_mark(unsigned frames)
{
    unsigned i;

    for (i = 0; i < frames; i++) {
        diag_trim_frame(&d);
    }
    /* The press edge and its release, at a timestamp nothing here reads: the
     * trim page counts frames, never milliseconds. */
    sample(COMBO_EVENT_NONE, COMBO_BTN_START, 0);
    return sample(COMBO_EVENT_NONE, 0, 0);
}

/* A whole run: start, then DIAG_TRIM_MARKS + 1 marks `span` frames apart. */
static void trim_run(unsigned span)
{
    uint8_t i;

    sample(COMBO_EVENT_NONE, COMBO_BTN_START, 0);
    sample(COMBO_EVENT_NONE, 0, 0);
    TEST_ASSERT_TRUE(diag_trim_running(&d));
    for (i = 0; i <= (uint8_t)DIAG_TRIM_MARKS; i++) {
        run_frames_then_mark(span);
    }
    TEST_ASSERT_FALSE(diag_trim_running(&d));
}

static int32_t trim_x64_of(void)
{
    uint8_t fpa = 0;
    uint8_t ratio = 0;

    diag_trim(&d, &fpa, &ratio);
    return (int32_t)fpa * 64 + ratio;
}

static void test_the_trim_page_starts_from_the_stored_porch(void)
{
    uint8_t fpa = 0;
    uint8_t ratio = 0;

    diag_trim(&d, &fpa, &ratio);
    TEST_ASSERT_EQUAL_UINT8(TRIM_FPA, fpa);
    TEST_ASSERT_EQUAL_UINT8(TRIM_RATIO, ratio);
    TEST_ASSERT_FALSE(diag_trim_running(&d));
    TEST_ASSERT_EQUAL_UINT32(0, diag_trim_span(&d));
}

static void test_init_clamps_a_stored_porch_into_what_the_register_holds(void)
{
    uint8_t fpa = 0;
    uint8_t ratio = 0;

    TEST_ASSERT_EQUAL_INT(DIAG_OK,
        diag_init(&d, PANEL_W, PANEL_H, WIN24_W, WIN24_H, 0, 0, 0, 0,
                  MIX_VOL_MED, 0, 0, 200));
    diag_trim(&d, &fpa, &ratio);
    /* A porch of 0 is a frame the panel cannot scan; 200 sixty-fourths would
     * carry into the whole-line count on the wrong frame. */
    TEST_ASSERT_EQUAL_UINT8(1, fpa);
    TEST_ASSERT_EQUAL_UINT8(63, ratio);
}

static void test_the_dpad_steps_the_porch_coarse_and_fine_with_carry(void)
{
    int32_t base;

    goto_page(DIAG_PAGE_TRIM);
    base = trim_x64_of();

    sample(COMBO_EVENT_NONE, COMBO_BTN_RIGHT, 0);
    TEST_ASSERT_EQUAL_INT32(base + 64, trim_x64_of());
    sample(COMBO_EVENT_NONE, 0, 0);
    sample(COMBO_EVENT_NONE, COMBO_BTN_LEFT, 10);
    TEST_ASSERT_EQUAL_INT32(base, trim_x64_of());
    sample(COMBO_EVENT_NONE, 0, 20);

    sample(COMBO_EVENT_NONE, COMBO_BTN_UP, 30);
    TEST_ASSERT_EQUAL_INT32(base + DIAG_TRIM_FINE, trim_x64_of());
    sample(COMBO_EVENT_NONE, 0, 40);
    sample(COMBO_EVENT_NONE, COMBO_BTN_DOWN, 50);
    TEST_ASSERT_EQUAL_INT32(base, trim_x64_of());
}

static void test_the_fine_knob_carries_into_the_whole_line_knob(void)
{
    uint8_t fpa = 0;
    uint8_t ratio = 0;
    uint8_t i;

    /* A null just the other side of a line boundary has to be reachable by
     * holding one direction, not by knowing to step the porch — which is the
     * property the fixture this page replaces had and is worth keeping. */
    TEST_ASSERT_EQUAL_INT(DIAG_OK,
        diag_init(&d, PANEL_W, PANEL_H, WIN24_W, WIN24_H, 0, 0, 0, 0,
                  MIX_VOL_MED, 0, 11, 60));
    goto_page(DIAG_PAGE_TRIM);

    for (i = 0; i < 2; i++) {
        sample(COMBO_EVENT_NONE, COMBO_BTN_UP, (uint32_t)(i * 100));
        sample(COMBO_EVENT_NONE, 0, (uint32_t)(i * 100 + 10));
    }
    diag_trim(&d, &fpa, &ratio);
    TEST_ASSERT_EQUAL_UINT8(12, fpa);
    TEST_ASSERT_EQUAL_UINT8(4, ratio);
}

static void test_the_porch_clamps_at_what_porctrl_can_express(void)
{
    uint8_t fpa = 0;
    uint8_t ratio = 0;

    goto_page(DIAG_PAGE_TRIM);
    hammer(COMBO_BTN_RIGHT, 300);
    diag_trim(&d, &fpa, &ratio);
    TEST_ASSERT_EQUAL_UINT8(126, fpa);
    TEST_ASSERT_EQUAL_UINT8(63, ratio);

    hammer(COMBO_BTN_LEFT, 300);
    diag_trim(&d, &fpa, &ratio);
    TEST_ASSERT_EQUAL_UINT8(1, fpa);
    TEST_ASSERT_EQUAL_UINT8(0, ratio);
}

static void test_start_runs_the_fixture_and_the_dpad_stops_moving_the_porch(void)
{
    int32_t before;

    goto_page(DIAG_PAGE_TRIM);
    before = trim_x64_of();

    TEST_ASSERT_TRUE((sample(COMBO_EVENT_NONE, COMBO_BTN_START, 0)
                      & DIAG_EV_TRIM_STATE) != 0);
    sample(COMBO_EVENT_NONE, 0, 10);
    TEST_ASSERT_TRUE(diag_trim_running(&d));

    /* Moving the porch under a count in progress would change the thing the
     * count is measuring. */
    hammer(COMBO_BTN_RIGHT, 20);
    TEST_ASSERT_EQUAL_INT32(before, trim_x64_of());
}

static void test_a_run_ends_on_the_last_mark_and_reports_the_mean_span(void)
{
    goto_page(DIAG_PAGE_TRIM);
    trim_run(600);
    /* Three intervals of 600 frames: the mean is 600, whatever the count. */
    TEST_ASSERT_EQUAL_UINT32(600, diag_trim_span(&d));
}

static void test_the_correction_is_the_line_count_over_the_frames_counted(void)
{
    int32_t before;
    int32_t moved;
    int32_t want;

    /* One crossing is one frame of slip, so over N frames the rates differ by
     * one part in N and the frame's line count moves by the same fraction.
     * At 20 + 32/64 of porch the total is 332 + 20.5 lines, and 600 frames
     * between crossings therefore asks for (332*64 + 20*64 + 32) / 600. */
    goto_page(DIAG_PAGE_TRIM);
    before = trim_x64_of();
    want = (332 * 64 + 20 * 64 + 32 + 300) / 600;

    trim_run(600);
    moved = before - trim_x64_of();
    TEST_ASSERT_EQUAL_INT32(want, moved);
}

static void test_a_run_that_made_the_beat_worse_reverses_the_direction(void)
{
    int32_t after_first;
    int32_t after_second;

    goto_page(DIAG_PAGE_TRIM);

    /* First run: the page has no way to know which way to go, so it guesses
     * and shortens the porch. */
    trim_run(4000);
    after_first = trim_x64_of();
    TEST_ASSERT_TRUE(after_first < (int32_t)TRIM_FPA * 64 + TRIM_RATIO);

    /* Second run: the interval collapsed, which only happens when the beat
     * grew — so the guess was wrong and the next correction goes the other
     * way, past where it started. */
    trim_run(500);
    after_second = trim_x64_of();
    TEST_ASSERT_TRUE(after_second > after_first);
}

static void test_a_run_that_helped_keeps_going_the_same_way(void)
{
    int32_t after_first;
    int32_t after_second;

    goto_page(DIAG_PAGE_TRIM);

    trim_run(600);
    after_first = trim_x64_of();
    /* The interval grew, so the direction stands and the porch keeps
     * shortening — by less, because a longer interval is a smaller error. */
    trim_run(6000);
    after_second = trim_x64_of();
    TEST_ASSERT_TRUE(after_second < after_first);
    TEST_ASSERT_TRUE((after_first - after_second)
                     < ((int32_t)TRIM_FPA * 64 + TRIM_RATIO - after_first));
}

static void test_a_run_too_long_to_correct_leaves_the_porch_alone(void)
{
    int32_t before;

    /* Past about 22,000 frames — six minutes — the correction rounds below
     * one 64th of a line, which is the floor the register sets and the end of
     * what any calibration can do. */
    goto_page(DIAG_PAGE_TRIM);
    before = trim_x64_of();
    trim_run(60000);
    TEST_ASSERT_EQUAL_INT32(before, trim_x64_of());
}

static void test_a_bounced_mark_is_not_a_crossing(void)
{
    goto_page(DIAG_PAGE_TRIM);
    sample(COMBO_EVENT_NONE, COMBO_BTN_START, 0);
    sample(COMBO_EVENT_NONE, 0, 0);

    run_frames_then_mark(600);
    /* Two presses a handful of frames apart are one crossing and a bounce. */
    run_frames_then_mark(2);
    run_frames_then_mark(2);
    TEST_ASSERT_TRUE(diag_trim_running(&d));

    run_frames_then_mark(596);
    run_frames_then_mark(600);
    run_frames_then_mark(600);
    TEST_ASSERT_FALSE(diag_trim_running(&d));
    /* Three intervals of 600 frames, with the bounces counted into the
     * second rather than splitting it. */
    TEST_ASSERT_EQUAL_UINT32(600, diag_trim_span(&d));
}

static void test_leaving_the_page_ends_a_run(void)
{
    goto_page(DIAG_PAGE_TRIM);
    sample(COMBO_EVENT_NONE, COMBO_BTN_START, 0);
    sample(COMBO_EVENT_NONE, 0, 0);
    TEST_ASSERT_TRUE(diag_trim_running(&d));

    TEST_ASSERT_TRUE((sample(COMBO_EVENT_BRIGHT_UP, COMBO_BTN_SELECT, 100)
                      & DIAG_EV_TRIM_STATE) != 0);
    TEST_ASSERT_FALSE(diag_trim_running(&d));
}

static void test_a_abandons_a_run_and_saves_from_an_idle_page(void)
{
    goto_page(DIAG_PAGE_TRIM);
    sample(COMBO_EVENT_NONE, COMBO_BTN_START, 0);
    sample(COMBO_EVENT_NONE, 0, 0);

    /* Half a count is not a measurement, so A abandons it rather than
     * storing what it has. */
    TEST_ASSERT_TRUE((sample(COMBO_EVENT_NONE, COMBO_BTN_A, 10)
                      & DIAG_EV_SAVE_TRIM) == 0);
    TEST_ASSERT_FALSE(diag_trim_running(&d));
    sample(COMBO_EVENT_NONE, 0, 20);

    TEST_ASSERT_TRUE((sample(COMBO_EVENT_NONE, COMBO_BTN_A, 30)
                      & DIAG_EV_SAVE_TRIM) != 0);
}

static void test_b_restores_the_porch_the_page_was_entered_with(void)
{
    goto_page(DIAG_PAGE_TRIM);
    hammer(COMBO_BTN_RIGHT, 5);
    TEST_ASSERT_TRUE(trim_x64_of() != (int32_t)TRIM_FPA * 64 + TRIM_RATIO);

    sample(COMBO_EVENT_NONE, COMBO_BTN_B, 900);
    TEST_ASSERT_EQUAL_INT32((int32_t)TRIM_FPA * 64 + TRIM_RATIO,
                            trim_x64_of());
}

static void test_start_means_nothing_on_any_other_page(void)
{
    goto_page(DIAG_PAGE_NUDGE);
    TEST_ASSERT_EQUAL_HEX16(0, sample(COMBO_EVENT_NONE, COMBO_BTN_START, 0));
    TEST_ASSERT_FALSE(diag_trim_running(&d));
}

/* ─── the fixture ─────────────────────────────────────────────────────────── */

static void test_the_fixture_is_deterministic_in_its_block_coordinates(void)
{
    int32_t u;
    int32_t v;

    /* The same offset gives the same field: a fixture that fizzed would put
     * a changing picture under a seam the builder is trying to count. */
    for (v = 0; v < 64; v++) {
        for (u = 0; u < 64; u++) {
            TEST_ASSERT_EQUAL_UINT8(diag_trim_shade(u, v),
                                    diag_trim_shade(u, v));
            /* And it is a field of blocks, not of pixels. */
            TEST_ASSERT_EQUAL_UINT8(
                diag_trim_shade(u - (u % DIAG_TRIM_BLOCK_W),
                                v - (v % DIAG_TRIM_BLOCK_H)),
                diag_trim_shade(u, v));
        }
    }
}

static void test_the_fixture_uses_all_four_shades(void)
{
    unsigned seen[4] = { 0, 0, 0, 0 };
    int32_t u;
    int32_t v;

    for (v = 0; v < 240; v++) {
        for (u = 0; u < 266; u++) {
            uint8_t sh = diag_trim_shade(u, v);

            TEST_ASSERT_TRUE(sh < 4u);
            seen[sh]++;
        }
    }
    /* Every shade well represented, so the field has contrast wherever a seam
     * lands rather than only where one of two colours happens to meet. */
    for (u = 0; u < 4; u++) {
        TEST_ASSERT_TRUE(seen[u] > 240u * 266u / 8u);
    }
}

static void test_the_fixture_decorrelates_at_every_displacement(void)
{
    int32_t shift;

    /*
     * The assertion that would catch a periodic pattern being substituted
     * later. Every periodic field is CONDITIONALLY BLIND: the displacement
     * across a seam is one frame of motion, and where that equals a whole
     * period the two sides line up and a real seam disappears. Stripes
     * measured 100 % of pixels changed at a 2 px displacement and 0 % at 4.
     * A field with no period has nothing to line up with, so every
     * displacement has to clear the floor — including the page's own
     * DIAG_TRIM_SCROLL, which is only one of the rates checked here because
     * a fixture that were sensitive at just its own rate would be a
     * coincidence rather than a property.
     */
    for (shift = 1; shift <= 8; shift++) {
        unsigned changed = 0;
        unsigned total = 0;
        int32_t u;
        int32_t v;

        for (v = 0; v < 240; v++) {
            for (u = 0; u < 266; u++) {
                if (diag_trim_shade(u, v) != diag_trim_shade(u, v + shift)) {
                    changed++;
                }
                total++;
            }
        }
        /* A third, which one row in two of a two-pixel block already clears
         * and which no periodic field clears at every rate. */
        TEST_ASSERT_TRUE(changed * 3u > total);
    }
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_init_rejects_null_and_a_window_bigger_than_the_panel);
    RUN_TEST(test_init_clamps_a_stored_origin_and_the_default);
    RUN_TEST(test_init_clamps_a_stored_volume_and_frameskip);
    RUN_TEST(test_init_starts_on_the_buttons_page_with_the_tone_off);
    RUN_TEST(test_eight_select_rights_walk_every_page_and_wrap);
    RUN_TEST(test_eight_select_lefts_walk_every_page_backwards);
    RUN_TEST(test_no_bare_dpad_or_face_word_changes_the_page);
    RUN_TEST(test_the_menu_and_volume_combos_do_nothing_here);
    RUN_TEST(test_a_held_direction_steps_once_then_repeats_at_the_cadence);
    RUN_TEST(test_two_directions_at_once_do_nothing);
    RUN_TEST(test_the_nudge_clamps_at_the_panel_edges_in_a_240x216_window);
    RUN_TEST(test_the_nudge_clamps_at_the_panel_edges_in_a_260x234_window);
    RUN_TEST(test_a_saves_the_nudge_and_the_toast_expires_once);
    RUN_TEST(test_b_restores_the_compile_time_default);
    RUN_TEST(test_a_held_a_fires_once);
    RUN_TEST(test_arriving_at_the_tag_page_asks_for_one_scan_either_way);
    RUN_TEST(test_a_rescans_on_the_tag_page_and_nowhere_else);
    RUN_TEST(test_a_toggles_the_tone);
    RUN_TEST(test_the_volume_index_steps_without_wrapping);
    RUN_TEST(test_leaving_the_audio_page_silences_the_tone);
    RUN_TEST(test_the_display_pattern_cycles_three_ways_round);
    RUN_TEST(test_frameskip_steps_within_its_range);
    RUN_TEST(test_every_page_has_a_title_and_the_count_has_none);
    RUN_TEST(test_a_null_state_is_inert);
    RUN_TEST(test_the_trim_page_starts_from_the_stored_porch);
    RUN_TEST(test_init_clamps_a_stored_porch_into_what_the_register_holds);
    RUN_TEST(test_the_dpad_steps_the_porch_coarse_and_fine_with_carry);
    RUN_TEST(test_the_fine_knob_carries_into_the_whole_line_knob);
    RUN_TEST(test_the_porch_clamps_at_what_porctrl_can_express);
    RUN_TEST(test_start_runs_the_fixture_and_the_dpad_stops_moving_the_porch);
    RUN_TEST(test_a_run_ends_on_the_last_mark_and_reports_the_mean_span);
    RUN_TEST(test_the_correction_is_the_line_count_over_the_frames_counted);
    RUN_TEST(test_a_run_that_made_the_beat_worse_reverses_the_direction);
    RUN_TEST(test_a_run_that_helped_keeps_going_the_same_way);
    RUN_TEST(test_a_run_too_long_to_correct_leaves_the_porch_alone);
    RUN_TEST(test_a_bounced_mark_is_not_a_crossing);
    RUN_TEST(test_leaving_the_page_ends_a_run);
    RUN_TEST(test_a_abandons_a_run_and_saves_from_an_idle_page);
    RUN_TEST(test_b_restores_the_porch_the_page_was_entered_with);
    RUN_TEST(test_start_means_nothing_on_any_other_page);
    RUN_TEST(test_the_fixture_is_deterministic_in_its_block_coordinates);
    RUN_TEST(test_the_fixture_uses_all_four_shades);
    RUN_TEST(test_the_fixture_decorrelates_at_every_displacement);
    return UNITY_END();
}
