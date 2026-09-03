#pragma once
// Boot decision table — every branch of the cartridge boot flow that is a
// decision rather than an I/O.
//
// Inputs are plain values: what the tag read returned, how its payload
// classified, what the configuration page says about protection, the three
// setup flags, and the pending-write record. The output is one action for the
// caller to carry out. Nothing here talks to a tag, the NVS, the display or
// the SD card, and nothing here holds a user-facing string — the halt actions
// name a condition and the caller owns the wording.
//
// The order of the table is load-bearing, not incidental:
//
//   1. read failures        — nothing else can be decided without a payload
//   2. setup not finished   — the wizard owns the device until it is
//   3. MENU                 — BEFORE the pending check, or a pending write
//                             would land on the menu cart and destroy it
//   4. pending write        — only for a tag that matches its target
//   5. blank                — a blank cart with nothing to do
//   6. WILD / GAME          — load the game
//
// Authentication is never performed here. When a decision needs to know
// whether a protected tag is ours, the table returns BOOT_NEED_AUTH and the
// caller re-enters with the answer filled in — so a foreign cart is only ever
// authenticated against when the outcome actually depends on it.
//
// Pure C, no Arduino/ESP-IDF headers, no allocation.

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#include "rom_store.h"

#ifdef __cplusplus
extern "C" {
#endif

/* What the tag read produced. Two targets means the DMG's RF shielding was
 * not removed, or a second tag is in the field. */
enum boot_tag_e {
    BOOT_TAG_NONE = 0,
    BOOT_TAG_MULTI,
    BOOT_TAG_UNREADABLE,
    BOOT_TAG_OK,
};

enum boot_class_e {
    BOOT_CLASS_BLANK = 0,
    BOOT_CLASS_MENU,
    BOOT_CLASS_WILD,
    BOOT_CLASS_GAME,
};

/* Protection state. OPEN is known from the configuration read alone — AUTH0
 * is 0xFF — so it costs no PWD_AUTH. OURS and FOREIGN are only knowable by
 * authenticating, which is why UNKNOWN exists and why the table can ask for
 * it rather than assume. */
enum boot_auth_e {
    BOOT_AUTH_UNKNOWN = 0,
    BOOT_AUTH_OPEN,
    BOOT_AUTH_OURS,
    BOOT_AUTH_FOREIGN,
};

/* What kind of cart a pending write is aimed at. Values are stable: this
 * enum is persisted inside the NVS pending record. */
enum boot_target_e {
    BOOT_TARGET_WILDCARD = 0,
    BOOT_TARGET_NEW_CART = 1,
    BOOT_TARGET_REWRITE = 2,
};

/* The writer's return value and the NVS pending record are the same shape,
 * so a recorded selection needs no translation on the way back out. */
typedef struct boot_selection_s {
    char rom[ROM_STORE_NAME_MAX];
    uint8_t target;
} boot_selection_t;

typedef struct boot_flags_s {
    bool menu_done;
    bool wild_done;
    bool setup_done;
} boot_flags_t;

typedef struct boot_input_s {
    enum boot_tag_e tag;
    enum boot_class_e cls;
    char rom[ROM_STORE_NAME_MAX];
    enum boot_auth_e auth;
    boot_flags_t flags;
    bool pending_set;
    boot_selection_t pending;
} boot_input_t;

enum boot_action_e {
    /* Halts. Halt means halt: no retry loop, no fallback browser. The DMG's
     * mechanical interlock already forces a power cycle to change carts, and
     * the power cycle is the retry. */
    BOOT_HALT_NO_CART = 0,
    BOOT_HALT_SHIELDING,
    BOOT_HALT_UNREADABLE,
    BOOT_HALT_BLANK,
    BOOT_HALT_INSERT_WILDCARD,
    BOOT_HALT_INSERT_BLANK,
    BOOT_HALT_INSERT_GAME_CART,
    BOOT_HALT_SETUP_INSERT_BLANK,

    /* Re-enter with `auth` resolved. */
    BOOT_NEED_AUTH,

    /* First-boot wizard steps. */
    BOOT_WIZARD_WRITE_MENU,
    BOOT_WIZARD_ADOPT_MENU,
    BOOT_WIZARD_PICK_WILD,
    BOOT_WIZARD_ADOPT_WILD,
    BOOT_WIZARD_PICK_GAME,

    BOOT_OPEN_WRITER,
    BOOT_EXECUTE_PENDING,
    BOOT_LOAD,
};

/* What the writer or the wizard's picker came back with. */
enum boot_pick_e {
    BOOT_PICK_NONE = 0,
    BOOT_PICK_ROM,
    BOOT_PICK_FINISH,
    BOOT_PICK_CANCEL_PENDING,
};

enum boot_pick_action_e {
    BOOT_PICK_HALT_MENU_CART = 0,
    BOOT_PICK_HALT_NO_SELECTION,
    BOOT_PICK_WRITE_WILD,
    BOOT_PICK_WRITE_GAME,
    BOOT_PICK_FINISH_SETUP,
    BOOT_PICK_RECORD_PENDING,
    BOOT_PICK_CLEAR_PENDING,
    BOOT_PICK_INVALID,
};

enum boot_classify_result_e {
    BOOT_CLASSIFY_OK = 0,
    BOOT_CLASSIFY_ERR_ARGS = -1,
    BOOT_CLASSIFY_ERR_PAYLOAD = -2, /* empty, or a WILD: with nothing after it */
};

/*
 * Classify a tag payload. "MENU" exactly is the menu cart; a "WILD:" prefix
 * with a non-empty remainder is the wildcard and `rom` receives the
 * remainder; anything else is an ordinary game cart and `rom` receives the
 * whole payload. An empty payload or a bare "WILD:" is
 * BOOT_CLASSIFY_ERR_PAYLOAD, which the caller maps to BOOT_TAG_UNREADABLE —
 * the string that was read is still what gets displayed.
 *
 * A blank tag never reaches here: NDEF_BLANK is BOOT_CLASS_BLANK before any
 * classification happens.
 */
int boot_classify(const char* payload, enum boot_class_e* cls, char* rom,
                  size_t rom_sz);

/* The table itself. */
enum boot_action_e boot_decide(const boot_input_t* in);

/* What to do with what the writer or picker returned, given which action
 * opened it. */
enum boot_pick_action_e boot_after_pick(enum boot_action_e opened_by,
                                        enum boot_pick_e pick);

/*
 * Compose the payload for a tag of class `cls` naming `rom`. BOOT_CLASS_BLANK
 * has no payload and is BOOT_CLASSIFY_ERR_ARGS.
 */
int boot_compose_payload(enum boot_class_e cls, const char* rom, char* out,
                         size_t out_sz);

/*
 * The class a write should produce for a given target. REWRITE preserves the
 * class the tag already presented, so rewriting a wildcard leaves it a
 * wildcard rather than silently demoting it to an ordinary cart.
 */
enum boot_class_e boot_target_class(uint8_t target,
                                    enum boot_class_e presented);

/*
 * Whether a tag that just loaded should have protection applied. A valid tag
 * sitting at AUTH0 == 0xFF is one that lost power between its write and its
 * protect; healing it is a configuration-only write.
 */
bool boot_should_heal(const boot_input_t* in);

#ifdef __cplusplus
}
#endif
