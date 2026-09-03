#include <unity.h>

#include <stdio.h>
#include <string.h>

#include "cart/boot.h"
#include "cart/ndef.h"
#include "cart/ntag.h"
#include "fake_ntag215.h"

/* This build's password. Real values live in hw_config.h on the device. */
static const uint8_t PWD[4] = { 0x43, 0x59, 0x44, 0x47 };
static const uint8_t PACK[2] = { 0x47, 0x42 };
static const uint8_t OTHER_PWD[4] = { 0xDE, 0xAD, 0xBE, 0xEF };
static const uint8_t OTHER_PACK[2] = { 0x13, 0x37 };

/* The region the device reads and writes: 24 pages, which is NDEF_BUF_MAX. */
#define USER_PAGES 24
#define USER_BYTES (USER_PAGES * NTAG_PAGE_SIZE)

void setUp(void)
{
}

void tearDown(void)
{
}

/* ---- input builder ----------------------------------------------------- */

/* A finished-setup boot with a blank, unprotected tag. Tests change only the
 * fields they are about, which keeps each case's intent visible. */
static boot_input_t base_input(void)
{
    boot_input_t in;

    memset(&in, 0, sizeof(in));
    in.tag = BOOT_TAG_OK;
    in.cls = BOOT_CLASS_BLANK;
    in.auth = BOOT_AUTH_OPEN;
    in.flags.menu_done = true;
    in.flags.wild_done = true;
    in.flags.setup_done = true;
    return in;
}

static void set_rom(boot_input_t* in, const char* rom)
{
    snprintf(in->rom, sizeof(in->rom), "%s", rom);
}

static void set_pending(boot_input_t* in, const char* rom, uint8_t target)
{
    in->pending_set = true;
    snprintf(in->pending.rom, sizeof(in->pending.rom), "%s", rom);
    in->pending.target = target;
}

/* ---- classification ---------------------------------------------------- */

static void test_classify(void)
{
    enum boot_class_e cls = BOOT_CLASS_BLANK;
    char rom[ROM_STORE_NAME_MAX];

    TEST_ASSERT_EQUAL_INT(BOOT_CLASSIFY_OK,
        boot_classify("MENU", &cls, rom, sizeof(rom)));
    TEST_ASSERT_EQUAL_INT(BOOT_CLASS_MENU, cls);

    TEST_ASSERT_EQUAL_INT(BOOT_CLASSIFY_OK,
        boot_classify("WILD:Tetris.gb", &cls, rom, sizeof(rom)));
    TEST_ASSERT_EQUAL_INT(BOOT_CLASS_WILD, cls);
    TEST_ASSERT_EQUAL_STRING("Tetris.gb", rom);

    TEST_ASSERT_EQUAL_INT(BOOT_CLASSIFY_OK,
        boot_classify("Tetris.gb", &cls, rom, sizeof(rom)));
    TEST_ASSERT_EQUAL_INT(BOOT_CLASS_GAME, cls);
    TEST_ASSERT_EQUAL_STRING("Tetris.gb", rom);

    /* MENU is matched exactly, so a lowercase payload is a file name. */
    TEST_ASSERT_EQUAL_INT(BOOT_CLASSIFY_OK,
        boot_classify("menu", &cls, rom, sizeof(rom)));
    TEST_ASSERT_EQUAL_INT(BOOT_CLASS_GAME, cls);
    TEST_ASSERT_EQUAL_STRING("menu", rom);

    /* Nothing to load and nothing to say: the caller shows what was read. */
    TEST_ASSERT_EQUAL_INT(BOOT_CLASSIFY_ERR_PAYLOAD,
        boot_classify("", &cls, rom, sizeof(rom)));
    TEST_ASSERT_EQUAL_INT(BOOT_CLASSIFY_ERR_PAYLOAD,
        boot_classify("WILD:", &cls, rom, sizeof(rom)));

    TEST_ASSERT_EQUAL_INT(BOOT_CLASSIFY_ERR_ARGS,
        boot_classify(NULL, &cls, rom, sizeof(rom)));
    TEST_ASSERT_EQUAL_INT(BOOT_CLASSIFY_ERR_ARGS,
        boot_classify("MENU", &cls, rom, 0));
}

static void test_classify_truncates_the_rom_to_the_buffer(void)
{
    enum boot_class_e cls = BOOT_CLASS_BLANK;
    char small[8];

    TEST_ASSERT_EQUAL_INT(BOOT_CLASSIFY_OK,
        boot_classify("WILD:abcdefghij.gb", &cls, small, sizeof(small)));
    TEST_ASSERT_EQUAL_INT(BOOT_CLASS_WILD, cls);
    TEST_ASSERT_EQUAL_STRING("abcdefg", small);
    TEST_ASSERT_EQUAL_size_t(sizeof(small) - 1, strlen(small));
}

/* ---- read failures dominate everything --------------------------------- */

static void test_read_failures_ignore_flags_and_pending(void)
{
    static const enum boot_tag_e tags[3] = {
        BOOT_TAG_NONE, BOOT_TAG_MULTI, BOOT_TAG_UNREADABLE,
    };
    static const enum boot_action_e want[3] = {
        BOOT_HALT_NO_CART, BOOT_HALT_SHIELDING, BOOT_HALT_UNREADABLE,
    };
    size_t t;
    int setup;
    int pending;

    for (t = 0; t < 3; t++) {
        for (setup = 0; setup < 2; setup++) {
            for (pending = 0; pending < 2; pending++) {
                boot_input_t in = base_input();
                in.tag = tags[t];
                in.flags.setup_done = (setup != 0);
                in.flags.menu_done = (setup != 0);
                in.flags.wild_done = (setup != 0);
                if (pending) {
                    set_pending(&in, "Tetris.gb", BOOT_TARGET_WILDCARD);
                }
                TEST_ASSERT_EQUAL_INT(want[t], boot_decide(&in));
            }
        }
    }
    TEST_ASSERT_EQUAL_INT(BOOT_HALT_UNREADABLE, boot_decide(NULL));
}

/* ---- the ordering test ------------------------------------------------- */

/* A MENU cart inserted while a write is pending must open the writer, not
 * execute the pending write. The other order would overwrite the menu cart —
 * the one cart that is never a write target. */
static void test_menu_with_pending_opens_the_writer_not_the_pending_write(void)
{
    boot_input_t in = base_input();
    in.cls = BOOT_CLASS_MENU;

    set_pending(&in, "Tetris.gb", BOOT_TARGET_WILDCARD);
    TEST_ASSERT_EQUAL_INT(BOOT_OPEN_WRITER, boot_decide(&in));

    set_pending(&in, "Tetris.gb", BOOT_TARGET_NEW_CART);
    TEST_ASSERT_EQUAL_INT(BOOT_OPEN_WRITER, boot_decide(&in));

    set_pending(&in, "Tetris.gb", BOOT_TARGET_REWRITE);
    in.auth = BOOT_AUTH_OURS;
    TEST_ASSERT_EQUAL_INT(BOOT_OPEN_WRITER, boot_decide(&in));
}

/* ---- pending routing --------------------------------------------------- */

static void test_pending_wildcard(void)
{
    boot_input_t in = base_input();
    set_pending(&in, "Tetris.gb", BOOT_TARGET_WILDCARD);

    in.cls = BOOT_CLASS_WILD;
    TEST_ASSERT_EQUAL_INT(BOOT_EXECUTE_PENDING, boot_decide(&in));

    in.cls = BOOT_CLASS_GAME;
    TEST_ASSERT_EQUAL_INT(BOOT_HALT_INSERT_WILDCARD, boot_decide(&in));

    in.cls = BOOT_CLASS_BLANK;
    TEST_ASSERT_EQUAL_INT(BOOT_HALT_INSERT_WILDCARD, boot_decide(&in));
}

static void test_pending_new_cart(void)
{
    boot_input_t in = base_input();
    set_pending(&in, "Tetris.gb", BOOT_TARGET_NEW_CART);

    in.cls = BOOT_CLASS_BLANK;
    TEST_ASSERT_EQUAL_INT(BOOT_EXECUTE_PENDING, boot_decide(&in));

    in.cls = BOOT_CLASS_GAME;
    TEST_ASSERT_EQUAL_INT(BOOT_HALT_INSERT_BLANK, boot_decide(&in));

    in.cls = BOOT_CLASS_WILD;
    TEST_ASSERT_EQUAL_INT(BOOT_HALT_INSERT_BLANK, boot_decide(&in));
}

static void test_pending_rewrite(void)
{
    boot_input_t in = base_input();
    set_pending(&in, "Tetris.gb", BOOT_TARGET_REWRITE);

    in.cls = BOOT_CLASS_GAME;
    in.auth = BOOT_AUTH_UNKNOWN;
    TEST_ASSERT_EQUAL_INT(BOOT_NEED_AUTH, boot_decide(&in));

    in.auth = BOOT_AUTH_OURS;
    TEST_ASSERT_EQUAL_INT(BOOT_EXECUTE_PENDING, boot_decide(&in));

    /* A cart that lost power between its write and its protect is
     * unprotected, and rewriting it is what this target is for. */
    in.auth = BOOT_AUTH_OPEN;
    TEST_ASSERT_EQUAL_INT(BOOT_EXECUTE_PENDING, boot_decide(&in));

    in.auth = BOOT_AUTH_FOREIGN;
    TEST_ASSERT_EQUAL_INT(BOOT_HALT_INSERT_GAME_CART, boot_decide(&in));

    in.cls = BOOT_CLASS_WILD;
    in.auth = BOOT_AUTH_OURS;
    TEST_ASSERT_EQUAL_INT(BOOT_EXECUTE_PENDING, boot_decide(&in));

    /* A blank cart is not a cart to rewrite. */
    in.cls = BOOT_CLASS_BLANK;
    TEST_ASSERT_EQUAL_INT(BOOT_HALT_INSERT_GAME_CART, boot_decide(&in));
}

static void test_pending_with_an_unknown_target_fails_closed(void)
{
    boot_input_t in = base_input();
    in.cls = BOOT_CLASS_GAME;
    in.auth = BOOT_AUTH_OURS;
    set_pending(&in, "Tetris.gb", 99);

    TEST_ASSERT_EQUAL_INT(BOOT_HALT_INSERT_GAME_CART, boot_decide(&in));
}

/* ---- the ordinary paths ------------------------------------------------ */

static void test_no_pending_routing(void)
{
    boot_input_t in = base_input();

    in.cls = BOOT_CLASS_BLANK;
    TEST_ASSERT_EQUAL_INT(BOOT_HALT_BLANK, boot_decide(&in));

    in.cls = BOOT_CLASS_WILD;
    set_rom(&in, "Tetris.gb");
    TEST_ASSERT_EQUAL_INT(BOOT_LOAD, boot_decide(&in));

    in.cls = BOOT_CLASS_GAME;
    TEST_ASSERT_EQUAL_INT(BOOT_LOAD, boot_decide(&in));

    in.cls = BOOT_CLASS_MENU;
    TEST_ASSERT_EQUAL_INT(BOOT_OPEN_WRITER, boot_decide(&in));
}

/* ---- the wizard -------------------------------------------------------- */

static boot_input_t wizard_input(bool menu_done, bool wild_done)
{
    boot_input_t in = base_input();
    in.flags.setup_done = false;
    in.flags.menu_done = menu_done;
    in.flags.wild_done = wild_done;
    return in;
}

static void test_wizard_step_one_menu_cart(void)
{
    boot_input_t in = wizard_input(false, false);

    in.cls = BOOT_CLASS_BLANK;
    TEST_ASSERT_EQUAL_INT(BOOT_WIZARD_WRITE_MENU, boot_decide(&in));

    in.cls = BOOT_CLASS_MENU;
    in.auth = BOOT_AUTH_UNKNOWN;
    TEST_ASSERT_EQUAL_INT(BOOT_NEED_AUTH, boot_decide(&in));

    in.auth = BOOT_AUTH_OURS;
    TEST_ASSERT_EQUAL_INT(BOOT_WIZARD_ADOPT_MENU, boot_decide(&in));

    in.auth = BOOT_AUTH_OPEN;
    TEST_ASSERT_EQUAL_INT(BOOT_WIZARD_ADOPT_MENU, boot_decide(&in));

    in.auth = BOOT_AUTH_FOREIGN;
    TEST_ASSERT_EQUAL_INT(BOOT_HALT_SETUP_INSERT_BLANK, boot_decide(&in));

    in.cls = BOOT_CLASS_GAME;
    in.auth = BOOT_AUTH_OURS;
    TEST_ASSERT_EQUAL_INT(BOOT_HALT_SETUP_INSERT_BLANK, boot_decide(&in));

    in.cls = BOOT_CLASS_WILD;
    TEST_ASSERT_EQUAL_INT(BOOT_HALT_SETUP_INSERT_BLANK, boot_decide(&in));
}

static void test_wizard_step_two_wildcard(void)
{
    boot_input_t in = wizard_input(true, false);

    in.cls = BOOT_CLASS_BLANK;
    TEST_ASSERT_EQUAL_INT(BOOT_WIZARD_PICK_WILD, boot_decide(&in));

    in.cls = BOOT_CLASS_WILD;
    in.auth = BOOT_AUTH_UNKNOWN;
    TEST_ASSERT_EQUAL_INT(BOOT_NEED_AUTH, boot_decide(&in));

    in.auth = BOOT_AUTH_OURS;
    TEST_ASSERT_EQUAL_INT(BOOT_WIZARD_ADOPT_WILD, boot_decide(&in));

    in.auth = BOOT_AUTH_FOREIGN;
    TEST_ASSERT_EQUAL_INT(BOOT_HALT_SETUP_INSERT_BLANK, boot_decide(&in));

    in.cls = BOOT_CLASS_MENU;
    in.auth = BOOT_AUTH_OURS;
    TEST_ASSERT_EQUAL_INT(BOOT_HALT_SETUP_INSERT_BLANK, boot_decide(&in));

    in.cls = BOOT_CLASS_GAME;
    TEST_ASSERT_EQUAL_INT(BOOT_HALT_SETUP_INSERT_BLANK, boot_decide(&in));
}

static void test_wizard_step_three_game_cart(void)
{
    boot_input_t in = wizard_input(true, true);

    in.cls = BOOT_CLASS_BLANK;
    TEST_ASSERT_EQUAL_INT(BOOT_WIZARD_PICK_GAME, boot_decide(&in));

    in.cls = BOOT_CLASS_GAME;
    TEST_ASSERT_EQUAL_INT(BOOT_HALT_SETUP_INSERT_BLANK, boot_decide(&in));

    in.cls = BOOT_CLASS_MENU;
    TEST_ASSERT_EQUAL_INT(BOOT_HALT_SETUP_INSERT_BLANK, boot_decide(&in));

    in.cls = BOOT_CLASS_WILD;
    TEST_ASSERT_EQUAL_INT(BOOT_HALT_SETUP_INSERT_BLANK, boot_decide(&in));
}

/* No wizard input can reach the writer or load a game. */
static void test_unfinished_setup_never_loads_or_opens_the_writer(void)
{
    int cls;
    int auth;
    int menu;
    int wild;
    int pending;

    for (cls = 0; cls <= BOOT_CLASS_GAME; cls++) {
        for (auth = 0; auth <= BOOT_AUTH_FOREIGN; auth++) {
            for (menu = 0; menu < 2; menu++) {
                for (wild = 0; wild < 2; wild++) {
                    for (pending = 0; pending < 2; pending++) {
                        boot_input_t in = wizard_input(menu != 0, wild != 0);
                        enum boot_action_e a;
                        in.cls = (enum boot_class_e)cls;
                        in.auth = (enum boot_auth_e)auth;
                        if (pending) {
                            set_pending(&in, "Tetris.gb",
                                        BOOT_TARGET_WILDCARD);
                        }
                        a = boot_decide(&in);
                        TEST_ASSERT_NOT_EQUAL(BOOT_LOAD, a);
                        TEST_ASSERT_NOT_EQUAL(BOOT_OPEN_WRITER, a);
                        TEST_ASSERT_NOT_EQUAL(BOOT_EXECUTE_PENDING, a);
                    }
                }
            }
        }
    }
}

/* NEED_AUTH exists to ask a question. It must never be asked twice. */
static void test_need_auth_is_only_returned_while_auth_is_unknown(void)
{
    static const enum boot_auth_e known[3] = {
        BOOT_AUTH_OPEN, BOOT_AUTH_OURS, BOOT_AUTH_FOREIGN,
    };
    int cls;
    size_t a;
    int menu;
    int wild;
    int setup;
    int pending;
    int target;

    for (cls = 0; cls <= BOOT_CLASS_GAME; cls++) {
        for (a = 0; a < 3; a++) {
            for (menu = 0; menu < 2; menu++) {
                for (wild = 0; wild < 2; wild++) {
                    for (setup = 0; setup < 2; setup++) {
                        for (pending = 0; pending < 2; pending++) {
                            for (target = 0; target < 3; target++) {
                                boot_input_t in = base_input();
                                in.cls = (enum boot_class_e)cls;
                                in.auth = known[a];
                                in.flags.menu_done = (menu != 0);
                                in.flags.wild_done = (wild != 0);
                                in.flags.setup_done = (setup != 0);
                                if (pending) {
                                    set_pending(&in, "Tetris.gb",
                                                (uint8_t)target);
                                }
                                TEST_ASSERT_NOT_EQUAL(BOOT_NEED_AUTH,
                                                      boot_decide(&in));
                            }
                        }
                    }
                }
            }
        }
    }
}

/* ---- after the picker -------------------------------------------------- */

static void test_after_pick_table(void)
{
    /* Wizard, wildcard step: immediate mode, blank tag already in the slot.
     * There is nothing to finish before the wildcard exists. */
    TEST_ASSERT_EQUAL_INT(BOOT_PICK_WRITE_WILD,
        boot_after_pick(BOOT_WIZARD_PICK_WILD, BOOT_PICK_ROM));
    TEST_ASSERT_EQUAL_INT(BOOT_PICK_INVALID,
        boot_after_pick(BOOT_WIZARD_PICK_WILD, BOOT_PICK_FINISH));
    TEST_ASSERT_EQUAL_INT(BOOT_PICK_INVALID,
        boot_after_pick(BOOT_WIZARD_PICK_WILD, BOOT_PICK_CANCEL_PENDING));
    TEST_ASSERT_EQUAL_INT(BOOT_PICK_HALT_NO_SELECTION,
        boot_after_pick(BOOT_WIZARD_PICK_WILD, BOOT_PICK_NONE));

    /* Wizard, game step: the one step a kid may decline. */
    TEST_ASSERT_EQUAL_INT(BOOT_PICK_WRITE_GAME,
        boot_after_pick(BOOT_WIZARD_PICK_GAME, BOOT_PICK_ROM));
    TEST_ASSERT_EQUAL_INT(BOOT_PICK_FINISH_SETUP,
        boot_after_pick(BOOT_WIZARD_PICK_GAME, BOOT_PICK_FINISH));
    TEST_ASSERT_EQUAL_INT(BOOT_PICK_INVALID,
        boot_after_pick(BOOT_WIZARD_PICK_GAME, BOOT_PICK_CANCEL_PENDING));
    TEST_ASSERT_EQUAL_INT(BOOT_PICK_HALT_NO_SELECTION,
        boot_after_pick(BOOT_WIZARD_PICK_GAME, BOOT_PICK_NONE));

    /* Pending mode, from a MENU cart. */
    TEST_ASSERT_EQUAL_INT(BOOT_PICK_RECORD_PENDING,
        boot_after_pick(BOOT_OPEN_WRITER, BOOT_PICK_ROM));
    TEST_ASSERT_EQUAL_INT(BOOT_PICK_CLEAR_PENDING,
        boot_after_pick(BOOT_OPEN_WRITER, BOOT_PICK_CANCEL_PENDING));
    TEST_ASSERT_EQUAL_INT(BOOT_PICK_INVALID,
        boot_after_pick(BOOT_OPEN_WRITER, BOOT_PICK_FINISH));
    TEST_ASSERT_EQUAL_INT(BOOT_PICK_HALT_MENU_CART,
        boot_after_pick(BOOT_OPEN_WRITER, BOOT_PICK_NONE));

    /* Nothing else opens a picker. */
    TEST_ASSERT_EQUAL_INT(BOOT_PICK_INVALID,
        boot_after_pick(BOOT_LOAD, BOOT_PICK_ROM));
    TEST_ASSERT_EQUAL_INT(BOOT_PICK_INVALID,
        boot_after_pick(BOOT_HALT_BLANK, BOOT_PICK_ROM));
    TEST_ASSERT_EQUAL_INT(BOOT_PICK_HALT_NO_SELECTION,
        boot_after_pick(BOOT_WIZARD_ADOPT_MENU, BOOT_PICK_NONE));
}

/* ---- payload composition and helpers ----------------------------------- */

static void test_compose_payload(void)
{
    char out[NDEF_BUF_MAX];
    char small[6];

    TEST_ASSERT_EQUAL_INT(BOOT_CLASSIFY_OK,
        boot_compose_payload(BOOT_CLASS_MENU, NULL, out, sizeof(out)));
    TEST_ASSERT_EQUAL_STRING("MENU", out);

    TEST_ASSERT_EQUAL_INT(BOOT_CLASSIFY_OK,
        boot_compose_payload(BOOT_CLASS_WILD, "Tetris.gb", out, sizeof(out)));
    TEST_ASSERT_EQUAL_STRING("WILD:Tetris.gb", out);

    TEST_ASSERT_EQUAL_INT(BOOT_CLASSIFY_OK,
        boot_compose_payload(BOOT_CLASS_GAME, "Tetris.gb", out, sizeof(out)));
    TEST_ASSERT_EQUAL_STRING("Tetris.gb", out);

    /* A blank tag has no payload. */
    TEST_ASSERT_EQUAL_INT(BOOT_CLASSIFY_ERR_ARGS,
        boot_compose_payload(BOOT_CLASS_BLANK, "Tetris.gb", out, sizeof(out)));
    TEST_ASSERT_EQUAL_INT(BOOT_CLASSIFY_ERR_ARGS,
        boot_compose_payload(BOOT_CLASS_GAME, NULL, out, sizeof(out)));
    TEST_ASSERT_EQUAL_INT(BOOT_CLASSIFY_ERR_ARGS,
        boot_compose_payload(BOOT_CLASS_GAME, "", out, sizeof(out)));

    /* The bound is respected, and nothing partial is left behind. */
    TEST_ASSERT_EQUAL_INT(BOOT_CLASSIFY_ERR_ARGS,
        boot_compose_payload(BOOT_CLASS_WILD, "Tetris.gb", small,
                             sizeof(small)));
    TEST_ASSERT_EQUAL_CHAR('\0', small[0]);
    TEST_ASSERT_EQUAL_INT(BOOT_CLASSIFY_ERR_ARGS,
        boot_compose_payload(BOOT_CLASS_MENU, NULL, small, 4));
    TEST_ASSERT_EQUAL_INT(BOOT_CLASSIFY_ERR_ARGS,
        boot_compose_payload(BOOT_CLASS_MENU, NULL, out, 0));

    /* Round trip through the classifier. */
    {
        enum boot_class_e cls;
        char rom[ROM_STORE_NAME_MAX];
        TEST_ASSERT_EQUAL_INT(BOOT_CLASSIFY_OK,
            boot_compose_payload(BOOT_CLASS_WILD, "Dr. Mario.gb", out,
                                 sizeof(out)));
        TEST_ASSERT_EQUAL_INT(BOOT_CLASSIFY_OK,
            boot_classify(out, &cls, rom, sizeof(rom)));
        TEST_ASSERT_EQUAL_INT(BOOT_CLASS_WILD, cls);
        TEST_ASSERT_EQUAL_STRING("Dr. Mario.gb", rom);
    }
}

static void test_target_class(void)
{
    TEST_ASSERT_EQUAL_INT(BOOT_CLASS_WILD,
        boot_target_class(BOOT_TARGET_WILDCARD, BOOT_CLASS_BLANK));
    TEST_ASSERT_EQUAL_INT(BOOT_CLASS_GAME,
        boot_target_class(BOOT_TARGET_NEW_CART, BOOT_CLASS_BLANK));
    /* Rewriting never changes what kind of cart it is. */
    TEST_ASSERT_EQUAL_INT(BOOT_CLASS_WILD,
        boot_target_class(BOOT_TARGET_REWRITE, BOOT_CLASS_WILD));
    TEST_ASSERT_EQUAL_INT(BOOT_CLASS_GAME,
        boot_target_class(BOOT_TARGET_REWRITE, BOOT_CLASS_GAME));
}

static void test_should_heal(void)
{
    boot_input_t in = base_input();

    in.cls = BOOT_CLASS_GAME;
    in.auth = BOOT_AUTH_OPEN;
    TEST_ASSERT_TRUE(boot_should_heal(&in));

    in.cls = BOOT_CLASS_WILD;
    TEST_ASSERT_TRUE(boot_should_heal(&in));
    in.cls = BOOT_CLASS_MENU;
    TEST_ASSERT_TRUE(boot_should_heal(&in));

    /* A blank tag has nothing worth protecting yet. */
    in.cls = BOOT_CLASS_BLANK;
    TEST_ASSERT_FALSE(boot_should_heal(&in));

    /* Already protected, or not ours to protect. */
    in.cls = BOOT_CLASS_GAME;
    in.auth = BOOT_AUTH_OURS;
    TEST_ASSERT_FALSE(boot_should_heal(&in));
    in.auth = BOOT_AUTH_FOREIGN;
    TEST_ASSERT_FALSE(boot_should_heal(&in));
    in.auth = BOOT_AUTH_UNKNOWN;
    TEST_ASSERT_FALSE(boot_should_heal(&in));

    in.auth = BOOT_AUTH_OPEN;
    in.tag = BOOT_TAG_NONE;
    TEST_ASSERT_FALSE(boot_should_heal(&in));

    TEST_ASSERT_FALSE(boot_should_heal(NULL));
}

/* ---- the end-to-end harness -------------------------------------------- */

/* What NVS carries from one boot to the next, and nothing more. */
typedef struct {
    boot_flags_t flags;
    boot_selection_t pending;
    bool pending_set;
} nvs_t;

typedef enum boot_pick_e (*picker_fn)(void* ctx, enum boot_action_e opened_by,
                                      boot_selection_t* sel);

typedef struct {
    enum boot_action_e action;
    enum boot_pick_action_e pick_action;
    bool picked;
    bool wrote;
    int write_rc;
} boot_result_t;

/* Compose the payload for a class, wrap it in an NDEF message, and provision
 * the tag with it — the real gbcore path, not a stand-in for it. */
static int provision_class(ntag_dev_t* dev, enum boot_class_e cls,
                           const char* rom)
{
    char payload[NDEF_BUF_MAX];
    uint8_t msg[NDEF_BUF_MAX];
    size_t len = 0;

    if (boot_compose_payload(cls, rom, payload, sizeof(payload)) !=
        BOOT_CLASSIFY_OK) {
        return NTAG_ERR_ARGS;
    }
    if (ndef_compose_text(payload, msg, sizeof(msg), &len) != NDEF_OK) {
        return NTAG_ERR_ARGS;
    }
    return ntag_provision(dev, msg, len, PWD, PACK);
}

/*
 * One boot, the way main.cpp will run it: read the tag, classify, read the
 * config page for protection state, decide, satisfy a NEED_AUTH by actually
 * authenticating against the tag, then carry out the action.
 */
static boot_result_t harness_boot(fake_ntag215_t* tag, nvs_t* nvs,
                                  picker_fn picker, void* picker_ctx)
{
    ntag_dev_t dev;
    uint8_t user[USER_BYTES];
    char payload[NDEF_BUF_MAX];
    char rom[ROM_STORE_NAME_MAX];
    boot_input_t in;
    boot_result_t res;
    uint8_t auth0 = 0;
    int rc;

    dev.ctx = tag;
    dev.xcv = fake_ntag215_xcv;

    memset(&res, 0, sizeof(res));
    res.pick_action = BOOT_PICK_INVALID;

    memset(&in, 0, sizeof(in));
    in.flags = nvs->flags;
    in.pending_set = nvs->pending_set;
    in.pending = nvs->pending;
    in.tag = BOOT_TAG_UNREADABLE;

    if (ntag_read_pages(&dev, NTAG215_PAGE_USER_FIRST, USER_PAGES, user) ==
        NTAG_OK) {
        rc = ndef_parse_text(user, sizeof(user), payload, sizeof(payload));
        if (rc == NDEF_BLANK) {
            in.tag = BOOT_TAG_OK;
            in.cls = BOOT_CLASS_BLANK;
        } else if (rc == NDEF_OK &&
                   boot_classify(payload, &in.cls, rom, sizeof(rom)) ==
                       BOOT_CLASSIFY_OK) {
            in.tag = BOOT_TAG_OK;
            memcpy(in.rom, rom, sizeof(in.rom));
        }
    }

    /* AUTH0 comes from the configuration page, so OPEN is known without
     * spending a PWD_AUTH on it. */
    if (in.tag == BOOT_TAG_OK && ntag_read_auth0(&dev, &auth0) == NTAG_OK) {
        in.auth = (auth0 == NTAG215_AUTH0_OPEN) ? BOOT_AUTH_OPEN
                                                : BOOT_AUTH_UNKNOWN;
    }

    res.action = boot_decide(&in);
    if (res.action == BOOT_NEED_AUTH) {
        in.auth = (ntag_pwd_auth(&dev, PWD, PACK) == NTAG_OK)
                      ? BOOT_AUTH_OURS
                      : BOOT_AUTH_FOREIGN;
        res.action = boot_decide(&in);
        TEST_ASSERT_NOT_EQUAL(BOOT_NEED_AUTH, res.action);
    }

    switch (res.action) {
    case BOOT_WIZARD_WRITE_MENU:
        res.wrote = true;
        res.write_rc = provision_class(&dev, BOOT_CLASS_MENU, NULL);
        if (res.write_rc == NTAG_OK) {
            nvs->flags.menu_done = true;
        }
        break;

    case BOOT_WIZARD_ADOPT_MENU:
        nvs->flags.menu_done = true;
        break;

    case BOOT_WIZARD_ADOPT_WILD:
        nvs->flags.wild_done = true;
        break;

    case BOOT_WIZARD_PICK_WILD:
    case BOOT_WIZARD_PICK_GAME:
    case BOOT_OPEN_WRITER: {
        boot_selection_t sel;
        enum boot_pick_e pick;

        memset(&sel, 0, sizeof(sel));
        pick = (picker != NULL) ? picker(picker_ctx, res.action, &sel)
                                : BOOT_PICK_NONE;
        res.picked = true;
        res.pick_action = boot_after_pick(res.action, pick);

        switch (res.pick_action) {
        case BOOT_PICK_WRITE_WILD:
            res.wrote = true;
            res.write_rc = provision_class(&dev, BOOT_CLASS_WILD, sel.rom);
            if (res.write_rc == NTAG_OK) {
                nvs->flags.wild_done = true;
            }
            break;
        case BOOT_PICK_WRITE_GAME:
            /* Writing a game cart does not finish setup — the kid may write
             * as many as they like and then choose Finish setup. */
            res.wrote = true;
            res.write_rc = provision_class(&dev, BOOT_CLASS_GAME, sel.rom);
            break;
        case BOOT_PICK_FINISH_SETUP:
            nvs->flags.setup_done = true;
            break;
        case BOOT_PICK_RECORD_PENDING:
            nvs->pending = sel;
            nvs->pending_set = true;
            break;
        case BOOT_PICK_CLEAR_PENDING:
            nvs->pending_set = false;
            break;
        default:
            break;
        }
        break;
    }

    case BOOT_EXECUTE_PENDING:
        res.wrote = true;
        res.write_rc = provision_class(
            &dev, boot_target_class(nvs->pending.target, in.cls),
            nvs->pending.rom);
        /* Cleared only on success, so a failed protect simply repeats on the
         * next boot. */
        if (res.write_rc == NTAG_OK) {
            nvs->pending_set = false;
        }
        break;

    default:
        break;
    }

    return res;
}

static void assert_tag_payload(fake_ntag215_t* tag, const char* expect)
{
    ntag_dev_t dev;
    uint8_t user[USER_BYTES];
    char payload[NDEF_BUF_MAX];

    dev.ctx = tag;
    dev.xcv = fake_ntag215_xcv;
    TEST_ASSERT_EQUAL_INT(NTAG_OK,
        ntag_read_pages(&dev, NTAG215_PAGE_USER_FIRST, USER_PAGES, user));
    TEST_ASSERT_EQUAL_INT(NDEF_OK,
        ndef_parse_text(user, sizeof(user), payload, sizeof(payload)));
    TEST_ASSERT_EQUAL_STRING(expect, payload);
}

static void assert_tag_protected(fake_ntag215_t* tag)
{
    uint8_t access = fake_ntag215_access(tag);

    TEST_ASSERT_EQUAL_HEX8(NTAG215_PAGE_USER_FIRST, fake_ntag215_auth0(tag));
    TEST_ASSERT_EQUAL_HEX8(0, access & NTAG215_ACCESS_PROT);
    TEST_ASSERT_EQUAL_HEX8(0, access & NTAG215_ACCESS_CFGLCK);
    TEST_ASSERT_EQUAL_HEX8(0, access & NTAG215_ACCESS_AUTHLIM_MASK);
    TEST_ASSERT_EQUAL_size_t(0, fake_ntag215_lock_pages_touched(tag));
}

/* ---- pickers ----------------------------------------------------------- */

typedef struct {
    enum boot_pick_e pick;
    const char* rom;
    uint8_t target;
} scripted_pick_t;

static enum boot_pick_e picker_scripted(void* ctx, enum boot_action_e opened_by,
                                        boot_selection_t* sel)
{
    scripted_pick_t* s = (scripted_pick_t*)ctx;

    (void)opened_by;
    if (s->rom != NULL) {
        snprintf(sel->rom, sizeof(sel->rom), "%s", s->rom);
    }
    sel->target = s->target;
    return s->pick;
}

/* The WS-06 writer stub: the first starter while the wildcard step is
 * outstanding, Finish setup afterwards, and no selection at all in pending
 * mode. */
typedef struct {
    const nvs_t* nvs;
    const char* starter;
} stub_ctx_t;

static enum boot_pick_e picker_stub(void* ctx, enum boot_action_e opened_by,
                                    boot_selection_t* sel)
{
    stub_ctx_t* s = (stub_ctx_t*)ctx;

    if (opened_by == BOOT_OPEN_WRITER) {
        return BOOT_PICK_NONE;
    }
    if (!s->nvs->flags.wild_done) {
        snprintf(sel->rom, sizeof(sel->rom), "%s", s->starter);
        return BOOT_PICK_ROM;
    }
    return BOOT_PICK_FINISH;
}

/* ---- the wizard, end to end -------------------------------------------- */

static void test_wizard_completes_on_the_host_against_fake_tags(void)
{
    fake_ntag215_t menu_tag;
    fake_ntag215_t wild_tag;
    fake_ntag215_t game_tag;
    fake_ntag215_t extra_tag;
    nvs_t nvs;
    boot_result_t r;
    scripted_pick_t pick;

    memset(&nvs, 0, sizeof(nvs));
    fake_ntag215_init(&menu_tag);
    fake_ntag215_init(&wild_tag);
    fake_ntag215_init(&game_tag);
    fake_ntag215_init(&extra_tag);

    /* Boot 1 — a blank cart becomes the MENU cart. No picker involved. */
    r = harness_boot(&menu_tag, &nvs, NULL, NULL);
    TEST_ASSERT_EQUAL_INT(BOOT_WIZARD_WRITE_MENU, r.action);
    TEST_ASSERT_EQUAL_INT(NTAG_OK, r.write_rc);
    TEST_ASSERT_TRUE(nvs.flags.menu_done);
    TEST_ASSERT_FALSE(nvs.flags.setup_done);
    assert_tag_payload(&menu_tag, "MENU");
    assert_tag_protected(&menu_tag);

    /* Boot 2 — a blank cart becomes the wildcard, aimed at Tetris. */
    pick.pick = BOOT_PICK_ROM;
    pick.rom = "Tetris.gb";
    pick.target = BOOT_TARGET_WILDCARD;
    r = harness_boot(&wild_tag, &nvs, picker_scripted, &pick);
    TEST_ASSERT_EQUAL_INT(BOOT_WIZARD_PICK_WILD, r.action);
    TEST_ASSERT_EQUAL_INT(BOOT_PICK_WRITE_WILD, r.pick_action);
    TEST_ASSERT_EQUAL_INT(NTAG_OK, r.write_rc);
    TEST_ASSERT_TRUE(nvs.flags.wild_done);
    TEST_ASSERT_FALSE(nvs.flags.setup_done);
    assert_tag_payload(&wild_tag, "WILD:Tetris.gb");
    assert_tag_protected(&wild_tag);

    /* Boot 3 — a blank cart becomes an ordinary game cart. Setup is still
     * open: writing a game cart is not what finishes it. */
    pick.pick = BOOT_PICK_ROM;
    pick.rom = "Dr. Mario.gb";
    pick.target = BOOT_TARGET_NEW_CART;
    r = harness_boot(&game_tag, &nvs, picker_scripted, &pick);
    TEST_ASSERT_EQUAL_INT(BOOT_WIZARD_PICK_GAME, r.action);
    TEST_ASSERT_EQUAL_INT(BOOT_PICK_WRITE_GAME, r.pick_action);
    TEST_ASSERT_EQUAL_INT(NTAG_OK, r.write_rc);
    TEST_ASSERT_FALSE(nvs.flags.setup_done);
    assert_tag_payload(&game_tag, "Dr. Mario.gb");
    assert_tag_protected(&game_tag);

    /* Boot 4 — Finish setup. */
    pick.pick = BOOT_PICK_FINISH;
    pick.rom = NULL;
    pick.target = 0;
    r = harness_boot(&extra_tag, &nvs, picker_scripted, &pick);
    TEST_ASSERT_EQUAL_INT(BOOT_WIZARD_PICK_GAME, r.action);
    TEST_ASSERT_EQUAL_INT(BOOT_PICK_FINISH_SETUP, r.pick_action);
    TEST_ASSERT_FALSE(r.wrote);
    TEST_ASSERT_TRUE(nvs.flags.setup_done);
    /* The cart that was in the slot when they finished is untouched. */
    TEST_ASSERT_EQUAL_size_t(0, fake_ntag215_count(&extra_tag, NTAG_CMD_WRITE,
                                                   FAKE_NTAG215_ANY_PAGE));

    /* Boot 5 — the MENU cart now opens the writer. */
    r = harness_boot(&menu_tag, &nvs, NULL, NULL);
    TEST_ASSERT_EQUAL_INT(BOOT_OPEN_WRITER, r.action);
    TEST_ASSERT_EQUAL_INT(BOOT_PICK_HALT_MENU_CART, r.pick_action);

    /* Boot 6 — the wildcard loads Tetris. */
    r = harness_boot(&wild_tag, &nvs, NULL, NULL);
    TEST_ASSERT_EQUAL_INT(BOOT_LOAD, r.action);
    TEST_ASSERT_FALSE(r.wrote);

    /* And the game cart loads its game. */
    r = harness_boot(&game_tag, &nvs, NULL, NULL);
    TEST_ASSERT_EQUAL_INT(BOOT_LOAD, r.action);
}

static void test_the_stub_picker_finishes_setup_in_three_boots(void)
{
    fake_ntag215_t menu_tag;
    fake_ntag215_t wild_tag;
    fake_ntag215_t last_tag;
    nvs_t nvs;
    stub_ctx_t stub;
    boot_result_t r;

    memset(&nvs, 0, sizeof(nvs));
    fake_ntag215_init(&menu_tag);
    fake_ntag215_init(&wild_tag);
    fake_ntag215_init(&last_tag);
    stub.nvs = &nvs;
    stub.starter = "Tetris.gb";

    r = harness_boot(&menu_tag, &nvs, picker_stub, &stub);
    TEST_ASSERT_EQUAL_INT(BOOT_WIZARD_WRITE_MENU, r.action);

    r = harness_boot(&wild_tag, &nvs, picker_stub, &stub);
    TEST_ASSERT_EQUAL_INT(BOOT_PICK_WRITE_WILD, r.pick_action);
    assert_tag_payload(&wild_tag, "WILD:Tetris.gb");

    /* Third boot: the wildcard exists, so the stub declines and setup ends
     * with no game cart written. */
    r = harness_boot(&last_tag, &nvs, picker_stub, &stub);
    TEST_ASSERT_EQUAL_INT(BOOT_PICK_FINISH_SETUP, r.pick_action);
    TEST_ASSERT_TRUE(nvs.flags.setup_done);
    TEST_ASSERT_EQUAL_size_t(0, fake_ntag215_count(&last_tag, NTAG_CMD_WRITE,
                                                   FAKE_NTAG215_ANY_PAGE));

    /* In pending mode the stub selects nothing and the boot halts. */
    r = harness_boot(&menu_tag, &nvs, picker_stub, &stub);
    TEST_ASSERT_EQUAL_INT(BOOT_OPEN_WRITER, r.action);
    TEST_ASSERT_EQUAL_INT(BOOT_PICK_HALT_MENU_CART, r.pick_action);
}

static void test_re_adoption_after_an_nvs_clear(void)
{
    fake_ntag215_t menu_tag;
    nvs_t nvs;
    boot_result_t r;
    size_t writes_before;

    memset(&nvs, 0, sizeof(nvs));
    fake_ntag215_init(&menu_tag);
    r = harness_boot(&menu_tag, &nvs, NULL, NULL);
    TEST_ASSERT_EQUAL_INT(BOOT_WIZARD_WRITE_MENU, r.action);

    /* A reflash or an NVS clear loses the flags but not the carts. */
    memset(&nvs, 0, sizeof(nvs));
    fake_ntag215_reset_log(&menu_tag);
    writes_before = fake_ntag215_count(&menu_tag, NTAG_CMD_WRITE,
                                       FAKE_NTAG215_ANY_PAGE);

    r = harness_boot(&menu_tag, &nvs, NULL, NULL);
    TEST_ASSERT_EQUAL_INT(BOOT_WIZARD_ADOPT_MENU, r.action);
    TEST_ASSERT_FALSE(r.wrote);
    TEST_ASSERT_TRUE(nvs.flags.menu_done);
    /* Adoption is a flag change, not a write. */
    TEST_ASSERT_EQUAL_size_t(writes_before,
        fake_ntag215_count(&menu_tag, NTAG_CMD_WRITE, FAKE_NTAG215_ANY_PAGE));
    assert_tag_payload(&menu_tag, "MENU");
}

static void test_a_foreign_menu_cart_cannot_be_adopted(void)
{
    fake_ntag215_t menu_tag;
    nvs_t nvs;
    boot_result_t r;

    memset(&nvs, 0, sizeof(nvs));
    fake_ntag215_init(&menu_tag);
    (void)harness_boot(&menu_tag, &nvs, NULL, NULL);

    /* Same payload, another group's password. */
    fake_ntag215_set_protected(&menu_tag, OTHER_PWD, OTHER_PACK,
                               NTAG215_PAGE_USER_FIRST);
    memset(&nvs, 0, sizeof(nvs));
    fake_ntag215_reset_log(&menu_tag);

    r = harness_boot(&menu_tag, &nvs, NULL, NULL);
    TEST_ASSERT_EQUAL_INT(BOOT_HALT_SETUP_INSERT_BLANK, r.action);
    TEST_ASSERT_FALSE(nvs.flags.menu_done);
    TEST_ASSERT_EQUAL_size_t(0, fake_ntag215_count(&menu_tag, NTAG_CMD_WRITE,
                                                   FAKE_NTAG215_ANY_PAGE));
}

static void test_pending_write_executes_end_to_end(void)
{
    fake_ntag215_t wild_tag;
    nvs_t nvs;
    boot_result_t r;
    scripted_pick_t pick;

    /* Start from a finished setup with a provisioned wildcard. */
    memset(&nvs, 0, sizeof(nvs));
    fake_ntag215_init(&wild_tag);
    pick.pick = BOOT_PICK_ROM;
    pick.rom = "Tetris.gb";
    pick.target = BOOT_TARGET_WILDCARD;
    nvs.flags.menu_done = true;
    (void)harness_boot(&wild_tag, &nvs, picker_scripted, &pick);
    nvs.flags.setup_done = true;
    assert_tag_payload(&wild_tag, "WILD:Tetris.gb");

    /* The menu recorded a new game for the wildcard. */
    memset(&nvs.pending, 0, sizeof(nvs.pending));
    snprintf(nvs.pending.rom, sizeof(nvs.pending.rom), "%s", "Dr. Mario.gb");
    nvs.pending.target = BOOT_TARGET_WILDCARD;
    nvs.pending_set = true;

    r = harness_boot(&wild_tag, &nvs, NULL, NULL);
    TEST_ASSERT_EQUAL_INT(BOOT_EXECUTE_PENDING, r.action);
    TEST_ASSERT_EQUAL_INT(NTAG_OK, r.write_rc);
    TEST_ASSERT_FALSE(nvs.pending_set);
    /* Retargeted, and still a wildcard. */
    assert_tag_payload(&wild_tag, "WILD:Dr. Mario.gb");
    assert_tag_protected(&wild_tag);
}

static void test_a_pending_write_repeats_after_a_failed_protect(void)
{
    fake_ntag215_t tag;
    nvs_t nvs;
    boot_result_t r;

    /* A blank cart, and a pending write aimed at a new cart. */
    memset(&nvs, 0, sizeof(nvs));
    nvs.flags.menu_done = true;
    nvs.flags.wild_done = true;
    nvs.flags.setup_done = true;
    fake_ntag215_init(&tag);
    snprintf(nvs.pending.rom, sizeof(nvs.pending.rom), "%s", "Dr. Mario.gb");
    nvs.pending.target = BOOT_TARGET_NEW_CART;
    nvs.pending_set = true;

    /* The tag stops answering on the very last write of the protect
     * sequence — the AUTH0 write. */
    tag.nak_one_write = true;
    tag.nak_write_page = NTAG215_PAGE_CFG0;

    r = harness_boot(&tag, &nvs, NULL, NULL);
    TEST_ASSERT_EQUAL_INT(BOOT_EXECUTE_PENDING, r.action);
    TEST_ASSERT_EQUAL_INT(NTAG_ERR_NAK, r.write_rc);
    /* Not cleared, so the next boot tries again. */
    TEST_ASSERT_TRUE(nvs.pending_set);
    /* The content landed; only the protection did not. */
    assert_tag_payload(&tag, "Dr. Mario.gb");
    TEST_ASSERT_EQUAL_HEX8(NTAG215_AUTH0_OPEN, fake_ntag215_auth0(&tag));

    /* Second boot, tag healthy. The cart now classifies as GAME rather than
     * BLANK, and NEW_CART would refuse it — so this is REWRITE's job, which
     * is why an unprotected cart must be acceptable to it. */
    tag.nak_one_write = false;
    nvs.pending.target = BOOT_TARGET_REWRITE;
    r = harness_boot(&tag, &nvs, NULL, NULL);
    TEST_ASSERT_EQUAL_INT(BOOT_EXECUTE_PENDING, r.action);
    TEST_ASSERT_EQUAL_INT(NTAG_OK, r.write_rc);
    TEST_ASSERT_FALSE(nvs.pending_set);
    assert_tag_payload(&tag, "Dr. Mario.gb");
    assert_tag_protected(&tag);
}

static void test_a_loaded_but_unprotected_cart_is_flagged_for_healing(void)
{
    fake_ntag215_t tag;
    ntag_dev_t dev;
    nvs_t nvs;
    boot_result_t r;
    uint8_t msg[NDEF_BUF_MAX];
    size_t len = 0;

    /* A cart written but never protected: exactly what a power cut between
     * the two leaves behind. */
    memset(&nvs, 0, sizeof(nvs));
    nvs.flags.menu_done = true;
    nvs.flags.wild_done = true;
    nvs.flags.setup_done = true;
    fake_ntag215_init(&tag);
    dev.ctx = &tag;
    dev.xcv = fake_ntag215_xcv;
    TEST_ASSERT_EQUAL_INT(NDEF_OK,
        ndef_compose_text("Tetris.gb", msg, sizeof(msg), &len));
    TEST_ASSERT_EQUAL_INT(NTAG_OK,
        ntag_write_bytes(&dev, NTAG215_PAGE_USER_FIRST, msg, len));

    r = harness_boot(&tag, &nvs, NULL, NULL);
    TEST_ASSERT_EQUAL_INT(BOOT_LOAD, r.action);
    TEST_ASSERT_EQUAL_HEX8(NTAG215_AUTH0_OPEN, fake_ntag215_auth0(&tag));

    /* The heal predicate is what main.cpp consults after a successful load. */
    {
        boot_input_t in = base_input();
        in.cls = BOOT_CLASS_GAME;
        in.auth = BOOT_AUTH_OPEN;
        TEST_ASSERT_TRUE(boot_should_heal(&in));
    }
    /* And healing is a configuration write, not a rewrite of the content. */
    TEST_ASSERT_EQUAL_INT(NTAG_OK, ntag_protect(&dev, PWD, PACK));
    assert_tag_protected(&tag);
    assert_tag_payload(&tag, "Tetris.gb");
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_classify);
    RUN_TEST(test_classify_truncates_the_rom_to_the_buffer);
    RUN_TEST(test_read_failures_ignore_flags_and_pending);
    RUN_TEST(test_menu_with_pending_opens_the_writer_not_the_pending_write);
    RUN_TEST(test_pending_wildcard);
    RUN_TEST(test_pending_new_cart);
    RUN_TEST(test_pending_rewrite);
    RUN_TEST(test_pending_with_an_unknown_target_fails_closed);
    RUN_TEST(test_no_pending_routing);
    RUN_TEST(test_wizard_step_one_menu_cart);
    RUN_TEST(test_wizard_step_two_wildcard);
    RUN_TEST(test_wizard_step_three_game_cart);
    RUN_TEST(test_unfinished_setup_never_loads_or_opens_the_writer);
    RUN_TEST(test_need_auth_is_only_returned_while_auth_is_unknown);
    RUN_TEST(test_after_pick_table);
    RUN_TEST(test_compose_payload);
    RUN_TEST(test_target_class);
    RUN_TEST(test_should_heal);
    RUN_TEST(test_wizard_completes_on_the_host_against_fake_tags);
    RUN_TEST(test_the_stub_picker_finishes_setup_in_three_boots);
    RUN_TEST(test_re_adoption_after_an_nvs_clear);
    RUN_TEST(test_a_foreign_menu_cart_cannot_be_adopted);
    RUN_TEST(test_pending_write_executes_end_to_end);
    RUN_TEST(test_a_pending_write_repeats_after_a_failed_protect);
    RUN_TEST(test_a_loaded_but_unprotected_cart_is_flagged_for_healing);
    return UNITY_END();
}
