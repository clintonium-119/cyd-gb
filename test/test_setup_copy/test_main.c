#include <unity.h>

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "ui/setup_copy.h"
#include "ui/theme.h"

/*
 * The setup copy table, measured against the real fonts.
 *
 * The font headers are the ones the panel draws with. They declare their
 * tables PROGMEM against the TFT_eSPI glyph structs, so both are stood in for
 * here with the same field order; nothing else of the driver is needed to sum
 * advances.
 */
#define PROGMEM
typedef struct {
    uint16_t bitmapOffset;
    uint8_t width, height, xAdvance;
    int8_t xOffset, yOffset;
} GFXglyph;
typedef struct {
    uint8_t* bitmap;
    GFXglyph* glyph;
    uint16_t first, last;
    uint8_t yAdvance;
} GFXfont;

#include "../../src/fonts/Sans6p25pt.h"
#include "../../src/fonts/SansBold10pt.h"

/* The one game window, and what the notice and the help line give text. */
#define WIN_W 266
#define NOTICE_W (WIN_W - 2 * UI_PAD)
#define HELP_W (WIN_W - 2 * UI_TEXT_X)
/* The notice draws four body rows; the copy keeps one spare. */
#define BODY_ROWS 3

static char title[SETUP_COPY_MAX];
static char body[SETUP_COPY_MAX];

/* What the driver's textWidth gives: every advance, but the last glyph's ink
 * when that runs past its advance. */
static int width(const GFXfont* f, const char* s, size_t n)
{
    int w = 0;
    size_t i;

    for (i = 0; i < n; i++) {
        const GFXglyph* g;
        unsigned c = (unsigned char)s[i];

        if (c < f->first || c > f->last) {
            continue;
        }
        g = &f->glyph[c - f->first];
        if (i + 1 < n) {
            w += g->xAdvance;
        } else {
            int ink = g->width + g->xOffset;
            w += (ink > g->xAdvance) ? ink : g->xAdvance;
        }
    }
    return w;
}

/* How many rows the display's word wrap breaks `s` into at `max_w`: grow a
 * row while it measures inside, back up to the last space, skip the spaces
 * the break used. */
static unsigned rows(const GFXfont* f, const char* s, int max_w)
{
    size_t at = 0;
    size_t len = strlen(s);
    unsigned r = 0;

    while (at < len) {
        size_t n = 0;
        size_t last_space = 0;

        while (at + n < len && width(f, s + at, n + 1) <= max_w) {
            if (s[at + n] == ' ') {
                last_space = n;
            }
            n++;
        }
        if (at + n < len && s[at + n] != ' ' && last_space > 0) {
            n = last_space;
        }
        if (n == 0) {
            return 99;
        }
        at += n;
        while (at < len && s[at] == ' ') {
            at++;
        }
        r++;
    }
    return r;
}

static boot_flags_t flags_at(uint8_t step, bool rewrite)
{
    boot_flags_t f;

    memset(&f, 0, sizeof(f));
    f.menu_done = (step >= 2);
    f.wild_done = (step >= 3);
    f.rewrite = rewrite;
    return f;
}

static void copy(enum setup_screen_e s, uint8_t step, bool rewrite,
                 enum boot_class_e cls, int code)
{
    boot_flags_t f = flags_at(step, rewrite);

    TEST_ASSERT_EQUAL_INT(SETUP_COPY_OK,
                          setup_copy(s, &f, cls, code, title, sizeof(title),
                                     body, sizeof(body)));
}

static bool is_list(int s)
{
    return s >= SETUP_SCREEN_LIST_WILD && s <= SETUP_SCREEN_LIST_MAKE_MENU;
}

/* The measure is real: a title is wider than its glyph count would say at
 * 1 px, and a long paragraph needs more rows than the budget. */
static void test_the_measure_is_not_vacuous(void)
{
    static const char para[] =
        "Step 2 of 3 done. Power off, put in a blank cart for a game cart, "
        "or your MENU cart to finish setup. Step 2 of 3 done. Power off, put "
        "in a blank cart for a game cart, or your MENU cart to finish setup.";

    TEST_ASSERT_TRUE(width(&SansBold10pt, "Setup: no cartridge", 19) > 19 * 6);
    TEST_ASSERT_TRUE(rows(&Sans6p25pt, para, NOTICE_W) > BODY_ROWS);
    TEST_ASSERT_TRUE(width(&SansBold10pt, para, 60) > NOTICE_W);
}

static void test_the_step_follows_the_flags(void)
{
    boot_flags_t f = flags_at(1, false);

    TEST_ASSERT_EQUAL_UINT8(1, setup_copy_step(NULL));
    TEST_ASSERT_EQUAL_UINT8(1, setup_copy_step(&f));
    f = flags_at(2, false);
    TEST_ASSERT_EQUAL_UINT8(2, setup_copy_step(&f));
    f = flags_at(3, true);
    TEST_ASSERT_EQUAL_UINT8(3, setup_copy_step(&f));
}

/* Every screen, at every step, restarted or not, with every cart: a title,
 * a body unless it is a list header, and both inside their room. */
static void test_every_screen_has_words_that_fit(void)
{
    int s;
    uint8_t step;
    int rw;
    int cls;

    for (s = 0; s < SETUP_SCREEN_COUNT; s++) {
        for (step = 1; step <= 3; step++) {
            for (rw = 0; rw < 2; rw++) {
                for (cls = 0; cls <= BOOT_CLASS_GAME; cls++) {
                    char msg[96];

                    copy((enum setup_screen_e)s, step, rw != 0,
                         (enum boot_class_e)cls, -12345);
                    snprintf(msg, sizeof(msg), "screen %d step %u rw %d cls %d",
                             s, (unsigned)step, rw, cls);
                    TEST_ASSERT_NOT_EQUAL_MESSAGE(0, title[0], msg);
                    if (is_list(s)) {
                        TEST_ASSERT_EQUAL_CHAR_MESSAGE('\0', body[0], msg);
                        TEST_ASSERT_TRUE_MESSAGE(
                            width(&Sans6p25pt, title, strlen(title)) <=
                                HELP_W,
                            msg);
                        continue;
                    }
                    TEST_ASSERT_NOT_EQUAL_MESSAGE(0, body[0], msg);
                    TEST_ASSERT_TRUE_MESSAGE(
                        width(&SansBold10pt, title, strlen(title)) <= NOTICE_W,
                        msg);
                    TEST_ASSERT_TRUE_MESSAGE(
                        rows(&Sans6p25pt, body, NOTICE_W) <= BODY_ROWS, msg);
                }
            }
        }
    }
}

static void test_the_successes_say_what_comes_next(void)
{
    copy(SETUP_SCREEN_MENU_DONE, 2, false, BOOT_CLASS_MENU, 0);
    TEST_ASSERT_EQUAL_STRING("MENU cart ready", title);
    TEST_ASSERT_EQUAL_STRING("Step 1 of 3 done. Power off, put in a blank "
                             "cart, power on to make your wildcard.",
                             body);
    copy(SETUP_SCREEN_WILD_DONE, 3, false, BOOT_CLASS_WILD, 0);
    TEST_ASSERT_EQUAL_STRING("Wildcard ready", title);
    TEST_ASSERT_EQUAL_STRING("Step 2 of 3 done. Power off, put in a blank "
                             "cart for a game cart, or your MENU cart to "
                             "finish setup.",
                             body);
    copy(SETUP_SCREEN_GAME_DONE, 3, false, BOOT_CLASS_GAME, 0);
    TEST_ASSERT_EQUAL_STRING("Game cart ready", title);
    TEST_ASSERT_EQUAL_STRING("Power off. Put in another blank cart for "
                             "another game, or your MENU cart to finish "
                             "setup.",
                             body);
    copy(SETUP_SCREEN_FINISHED, 3, false, BOOT_CLASS_MENU, 0);
    TEST_ASSERT_EQUAL_STRING("Setup finished", title);
    TEST_ASSERT_EQUAL_STRING("Power off. Put in a game cart or your wildcard "
                             "and power on to play. Your MENU cart picks the "
                             "wildcard's game.",
                             body);
}

static void test_an_empty_reader_names_the_step(void)
{
    copy(SETUP_SCREEN_NO_CART, 1, false, BOOT_CLASS_BLANK, 0);
    TEST_ASSERT_EQUAL_STRING("Setup: no cartridge", title);
    TEST_ASSERT_EQUAL_STRING("Step 1 of 3: put in a blank cart or your MENU "
                             "cart and power on.",
                             body);
    copy(SETUP_SCREEN_NO_CART, 2, false, BOOT_CLASS_BLANK, 0);
    TEST_ASSERT_EQUAL_STRING("Step 2 of 3: put in a blank cart or your "
                             "wildcard and power on.",
                             body);
    copy(SETUP_SCREEN_NO_CART, 3, false, BOOT_CLASS_BLANK, 0);
    TEST_ASSERT_EQUAL_STRING("Step 3 of 3: put in a blank cart for a game "
                             "cart, or your MENU cart to finish setup.",
                             body);
}

/* Only a restarted setup's empty reader at step 1 says it was restarted. */
static void test_only_the_restarted_first_step_says_restarted(void)
{
    int s;
    uint8_t step;
    int rw;
    int cls;

    copy(SETUP_SCREEN_NO_CART, 1, true, BOOT_CLASS_BLANK, 0);
    TEST_ASSERT_EQUAL_STRING("Setup restarted. Step 1 of 3: put in a blank "
                             "cart or your MENU cart and power on.",
                             body);

    for (s = 0; s < SETUP_SCREEN_COUNT; s++) {
        for (step = 1; step <= 3; step++) {
            for (rw = 0; rw < 2; rw++) {
                for (cls = 0; cls <= BOOT_CLASS_GAME; cls++) {
                    if (s == SETUP_SCREEN_NO_CART && step == 1 && rw) {
                        continue;
                    }
                    copy((enum setup_screen_e)s, step, rw != 0,
                         (enum boot_class_e)cls, 0);
                    TEST_ASSERT_NULL(strstr(title, "restarted"));
                    TEST_ASSERT_NULL(strstr(body, "restarted"));
                }
            }
        }
    }
}

static void test_a_wrong_cart_is_named(void)
{
    copy(SETUP_SCREEN_WRONG_CART, 1, false, BOOT_CLASS_WILD, 0);
    TEST_ASSERT_EQUAL_STRING("Setup: wrong cart", title);
    TEST_ASSERT_EQUAL_STRING("This is a wildcard. Step 1 of 3 needs a blank "
                             "cart or your MENU cart.",
                             body);
    copy(SETUP_SCREEN_WRONG_CART, 2, false, BOOT_CLASS_MENU, 0);
    TEST_ASSERT_EQUAL_STRING("This is your MENU cart. Step 2 of 3 needs a "
                             "blank cart or your wildcard.",
                             body);
    copy(SETUP_SCREEN_WRONG_CART, 3, false, BOOT_CLASS_GAME, 0);
    TEST_ASSERT_EQUAL_STRING("This is already a game cart. Put in a blank "
                             "cart, or your MENU cart to finish setup.",
                             body);
}

static void test_the_list_headers(void)
{
    copy(SETUP_SCREEN_LIST_WILD, 2, false, BOOT_CLASS_BLANK, 0);
    TEST_ASSERT_EQUAL_STRING("Step 2 of 3: your wildcard's first game", title);
    copy(SETUP_SCREEN_LIST_WILD, 2, true, BOOT_CLASS_MENU, 0);
    TEST_ASSERT_EQUAL_STRING("Step 2 of 3: replaces this MENU cart", title);
    copy(SETUP_SCREEN_LIST_GAME, 3, false, BOOT_CLASS_BLANK, 0);
    TEST_ASSERT_EQUAL_STRING("Step 3 of 3: a game cart, or finish setup",
                             title);
    copy(SETUP_SCREEN_LIST_GAME, 3, true, BOOT_CLASS_WILD, 0);
    TEST_ASSERT_EQUAL_STRING("Step 3 of 3: a game replaces this wildcard",
                             title);
    copy(SETUP_SCREEN_LIST_FINISH, 3, false, BOOT_CLASS_MENU, 0);
    TEST_ASSERT_EQUAL_STRING("Put in a blank cart to make a game cart", title);
    copy(SETUP_SCREEN_LIST_MAKE_MENU, 1, true, BOOT_CLASS_GAME, 0);
    TEST_ASSERT_EQUAL_STRING("Step 1 of 3: replaces this game cart", title);
}

static void test_the_write_failure_carries_its_code(void)
{
    copy(SETUP_SCREEN_WRITE_FAILED, 2, false, BOOT_CLASS_BLANK, -7);
    TEST_ASSERT_EQUAL_STRING("Write failed", title);
    TEST_ASSERT_EQUAL_STRING("Code -7. Nothing was changed. Power off, keep "
                             "the cart still, and power on to try again.",
                             body);
}

/* None of the words a kid won't know. */
static void test_no_jargon(void)
{
    static const char* const banned[] = { "writer", "tag", "adopt", "NDEF",
                                          "Tag", "Writer", "Adopt" };
    int s;
    uint8_t step;
    int rw;
    int cls;
    size_t b;

    for (s = 0; s < SETUP_SCREEN_COUNT; s++) {
        for (step = 1; step <= 3; step++) {
            for (rw = 0; rw < 2; rw++) {
                for (cls = 0; cls <= BOOT_CLASS_GAME; cls++) {
                    copy((enum setup_screen_e)s, step, rw != 0,
                         (enum boot_class_e)cls, 3);
                    for (b = 0; b < sizeof(banned) / sizeof(banned[0]); b++) {
                        TEST_ASSERT_NULL(strstr(title, banned[b]));
                        TEST_ASSERT_NULL(strstr(body, banned[b]));
                    }
                }
            }
        }
    }
}

static void test_bad_arguments_are_refused(void)
{
    boot_flags_t f = flags_at(1, false);

    TEST_ASSERT_EQUAL_INT(SETUP_COPY_ERR_ARGS,
                          setup_copy(SETUP_SCREEN_NO_CART, &f, BOOT_CLASS_BLANK,
                                     0, NULL, 4, body, sizeof(body)));
    TEST_ASSERT_EQUAL_INT(SETUP_COPY_ERR_ARGS,
                          setup_copy(SETUP_SCREEN_NO_CART, &f, BOOT_CLASS_BLANK,
                                     0, title, sizeof(title), body, 0));
    TEST_ASSERT_EQUAL_INT(SETUP_COPY_ERR_ARGS,
                          setup_copy(SETUP_SCREEN_COUNT, &f, BOOT_CLASS_BLANK,
                                     0, title, sizeof(title), body,
                                     sizeof(body)));
    /* A short buffer truncates rather than overruns. */
    {
        char small[8];
        TEST_ASSERT_EQUAL_INT(SETUP_COPY_OK,
                              setup_copy(SETUP_SCREEN_FINISHED, &f,
                                         BOOT_CLASS_BLANK, 0, small,
                                         sizeof(small), small, sizeof(small)));
        TEST_ASSERT_EQUAL_size_t(sizeof(small) - 1, strlen(small));
    }
}

void setUp(void) {}
void tearDown(void) {}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_the_measure_is_not_vacuous);
    RUN_TEST(test_the_step_follows_the_flags);
    RUN_TEST(test_every_screen_has_words_that_fit);
    RUN_TEST(test_the_successes_say_what_comes_next);
    RUN_TEST(test_an_empty_reader_names_the_step);
    RUN_TEST(test_only_the_restarted_first_step_says_restarted);
    RUN_TEST(test_a_wrong_cart_is_named);
    RUN_TEST(test_the_list_headers);
    RUN_TEST(test_the_write_failure_carries_its_code);
    RUN_TEST(test_no_jargon);
    RUN_TEST(test_bad_arguments_are_refused);
    return UNITY_END();
}
