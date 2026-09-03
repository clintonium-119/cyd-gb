#include "boot.h"

#include <string.h>

#define BOOT_MENU_PAYLOAD "MENU"
#define BOOT_WILD_PREFIX "WILD:"

/* Copy src into dst/dst_sz, NUL-terminated and truncated to fit. */
static void copy_rom(char* dst, size_t dst_sz, const char* src, size_t len)
{
    if (len > dst_sz - 1) {
        len = dst_sz - 1;
    }
    memcpy(dst, src, len);
    dst[len] = '\0';
}

int boot_classify(const char* payload, enum boot_class_e* cls, char* rom,
                  size_t rom_sz)
{
    size_t len;
    size_t prefix = sizeof(BOOT_WILD_PREFIX) - 1;

    if (payload == NULL || cls == NULL || rom == NULL || rom_sz == 0) {
        return BOOT_CLASSIFY_ERR_ARGS;
    }
    rom[0] = '\0';
    len = strlen(payload);
    if (len == 0) {
        return BOOT_CLASSIFY_ERR_PAYLOAD;
    }

    if (strcmp(payload, BOOT_MENU_PAYLOAD) == 0) {
        *cls = BOOT_CLASS_MENU;
        return BOOT_CLASSIFY_OK;
    }
    if (len > prefix && memcmp(payload, BOOT_WILD_PREFIX, prefix) == 0) {
        *cls = BOOT_CLASS_WILD;
        copy_rom(rom, rom_sz, payload + prefix, len - prefix);
        return BOOT_CLASSIFY_OK;
    }
    /* A bare "WILD:" names no game, so there is nothing to load and nothing
     * to say about it beyond showing what was read. */
    if (len == prefix && memcmp(payload, BOOT_WILD_PREFIX, prefix) == 0) {
        return BOOT_CLASSIFY_ERR_PAYLOAD;
    }

    *cls = BOOT_CLASS_GAME;
    copy_rom(rom, rom_sz, payload, len);
    return BOOT_CLASSIFY_OK;
}

/* A tag we may write during the wizard without it being someone else's: one
 * that is unprotected, or one that answers to this build's password. */
static bool adoptable(enum boot_auth_e auth)
{
    return auth == BOOT_AUTH_OURS || auth == BOOT_AUTH_OPEN;
}

/* The first-boot wizard: MENU cart, then wildcard, then one game cart. Each
 * step accepts a blank tag, or re-adopts an existing tag of the class it is
 * looking for — which is what makes the wizard survive an NVS clear without
 * the kid having to find fresh tags. */
static enum boot_action_e decide_wizard(const boot_input_t* in)
{
    if (!in->flags.menu_done) {
        if (in->cls == BOOT_CLASS_BLANK) {
            return BOOT_WIZARD_WRITE_MENU;
        }
        if (in->cls == BOOT_CLASS_MENU) {
            if (in->auth == BOOT_AUTH_UNKNOWN) {
                return BOOT_NEED_AUTH;
            }
            return adoptable(in->auth) ? BOOT_WIZARD_ADOPT_MENU
                                       : BOOT_HALT_SETUP_INSERT_BLANK;
        }
        return BOOT_HALT_SETUP_INSERT_BLANK;
    }

    if (!in->flags.wild_done) {
        if (in->cls == BOOT_CLASS_BLANK) {
            return BOOT_WIZARD_PICK_WILD;
        }
        if (in->cls == BOOT_CLASS_WILD) {
            if (in->auth == BOOT_AUTH_UNKNOWN) {
                return BOOT_NEED_AUTH;
            }
            return adoptable(in->auth) ? BOOT_WIZARD_ADOPT_WILD
                                       : BOOT_HALT_SETUP_INSERT_BLANK;
        }
        return BOOT_HALT_SETUP_INSERT_BLANK;
    }

    /* Last step: one ordinary game cart, which only a blank tag can become. */
    if (in->cls == BOOT_CLASS_BLANK) {
        return BOOT_WIZARD_PICK_GAME;
    }
    return BOOT_HALT_SETUP_INSERT_BLANK;
}

static enum boot_action_e decide_pending(const boot_input_t* in)
{
    switch (in->pending.target) {
    case BOOT_TARGET_WILDCARD:
        return (in->cls == BOOT_CLASS_WILD) ? BOOT_EXECUTE_PENDING
                                            : BOOT_HALT_INSERT_WILDCARD;

    case BOOT_TARGET_NEW_CART:
        return (in->cls == BOOT_CLASS_BLANK) ? BOOT_EXECUTE_PENDING
                                             : BOOT_HALT_INSERT_BLANK;

    case BOOT_TARGET_REWRITE:
        /* A blank cart is not a cart to rewrite — that is what NEW_CART is
         * for, and accepting it here would let the recovery path swallow an
         * unwritten tag. */
        if (in->cls == BOOT_CLASS_BLANK) {
            return BOOT_HALT_INSERT_GAME_CART;
        }
        if (in->auth == BOOT_AUTH_UNKNOWN) {
            return BOOT_NEED_AUTH;
        }
        /* OPEN counts alongside OURS: a cart that lost power between its
         * write and its protect sits unprotected, and rewriting a
         * mislabelled cart is exactly what this target exists for. */
        return adoptable(in->auth) ? BOOT_EXECUTE_PENDING
                                   : BOOT_HALT_INSERT_GAME_CART;

    default:
        /* A pending record with a target this build does not know is not
         * something to guess at. */
        return BOOT_HALT_INSERT_GAME_CART;
    }
}

enum boot_action_e boot_decide(const boot_input_t* in)
{
    if (in == NULL) {
        return BOOT_HALT_UNREADABLE;
    }

    /* 1. Nothing was read, so nothing can be decided. */
    switch (in->tag) {
    case BOOT_TAG_NONE:
        return BOOT_HALT_NO_CART;
    case BOOT_TAG_MULTI:
        return BOOT_HALT_SHIELDING;
    case BOOT_TAG_UNREADABLE:
        return BOOT_HALT_UNREADABLE;
    case BOOT_TAG_OK:
        break;
    default:
        return BOOT_HALT_UNREADABLE;
    }

    /* 2. The wizard owns the device until setup finishes — no wizard input
     * can reach the writer or load a game. */
    if (!in->flags.setup_done) {
        return decide_wizard(in);
    }

    /* 3. MENU before the pending check. Reversing these two would let a
     * pending write land on the menu cart, which is never a write target. */
    if (in->cls == BOOT_CLASS_MENU) {
        return BOOT_OPEN_WRITER;
    }

    /* 4. A pending write, but only onto the cart it was aimed at. */
    if (in->pending_set) {
        return decide_pending(in);
    }

    /* 5. A blank cart with setup done and nothing pending has no purpose. */
    if (in->cls == BOOT_CLASS_BLANK) {
        return BOOT_HALT_BLANK;
    }

    /* 6. WILD or GAME: load the game it names. */
    return BOOT_LOAD;
}

enum boot_pick_action_e boot_after_pick(enum boot_action_e opened_by,
                                        enum boot_pick_e pick)
{
    /* Nothing chosen. From a MENU cart that is simply someone looking at the
     * menu and powering off; in the wizard it is a step that cannot be
     * skipped. */
    if (pick == BOOT_PICK_NONE) {
        if (opened_by == BOOT_OPEN_WRITER) {
            return BOOT_PICK_HALT_MENU_CART;
        }
        if (opened_by == BOOT_WIZARD_WRITE_MENU ||
            opened_by == BOOT_WIZARD_ADOPT_MENU ||
            opened_by == BOOT_WIZARD_PICK_WILD ||
            opened_by == BOOT_WIZARD_ADOPT_WILD ||
            opened_by == BOOT_WIZARD_PICK_GAME) {
            return BOOT_PICK_HALT_NO_SELECTION;
        }
        return BOOT_PICK_INVALID;
    }

    switch (opened_by) {
    case BOOT_WIZARD_PICK_WILD:
        /* Immediate mode: the blank tag is already in the slot. There is
         * nothing to finish before the wildcard exists. */
        return (pick == BOOT_PICK_ROM) ? BOOT_PICK_WRITE_WILD
                                       : BOOT_PICK_INVALID;

    case BOOT_WIZARD_PICK_GAME:
        if (pick == BOOT_PICK_ROM) {
            return BOOT_PICK_WRITE_GAME;
        }
        /* The last wizard step is the one a kid may decline — one game cart
         * is enough to finish setup. */
        if (pick == BOOT_PICK_FINISH) {
            return BOOT_PICK_FINISH_SETUP;
        }
        return BOOT_PICK_INVALID;

    case BOOT_OPEN_WRITER:
        /* Pending mode: record it and tell them to power off and swap. */
        if (pick == BOOT_PICK_ROM) {
            return BOOT_PICK_RECORD_PENDING;
        }
        if (pick == BOOT_PICK_CANCEL_PENDING) {
            return BOOT_PICK_CLEAR_PENDING;
        }
        return BOOT_PICK_INVALID;

    default:
        return BOOT_PICK_INVALID;
    }
}

int boot_compose_payload(enum boot_class_e cls, const char* rom, char* out,
                         size_t out_sz)
{
    size_t prefix = sizeof(BOOT_WILD_PREFIX) - 1;
    size_t len;

    if (out == NULL || out_sz == 0) {
        return BOOT_CLASSIFY_ERR_ARGS;
    }
    out[0] = '\0';

    if (cls == BOOT_CLASS_MENU) {
        if (out_sz < sizeof(BOOT_MENU_PAYLOAD)) {
            return BOOT_CLASSIFY_ERR_ARGS;
        }
        memcpy(out, BOOT_MENU_PAYLOAD, sizeof(BOOT_MENU_PAYLOAD));
        return BOOT_CLASSIFY_OK;
    }

    /* Every other class names a ROM. A blank tag has no payload at all. */
    if (cls != BOOT_CLASS_WILD && cls != BOOT_CLASS_GAME) {
        return BOOT_CLASSIFY_ERR_ARGS;
    }
    if (rom == NULL || rom[0] == '\0') {
        return BOOT_CLASSIFY_ERR_ARGS;
    }
    len = strlen(rom);

    if (cls == BOOT_CLASS_WILD) {
        if (len + prefix + 1 > out_sz) {
            return BOOT_CLASSIFY_ERR_ARGS;
        }
        memcpy(out, BOOT_WILD_PREFIX, prefix);
        memcpy(out + prefix, rom, len + 1);
        return BOOT_CLASSIFY_OK;
    }

    if (len + 1 > out_sz) {
        return BOOT_CLASSIFY_ERR_ARGS;
    }
    memcpy(out, rom, len + 1);
    return BOOT_CLASSIFY_OK;
}

enum boot_class_e boot_target_class(uint8_t target,
                                    enum boot_class_e presented)
{
    switch (target) {
    case BOOT_TARGET_WILDCARD:
        return BOOT_CLASS_WILD;
    case BOOT_TARGET_NEW_CART:
        return BOOT_CLASS_GAME;
    case BOOT_TARGET_REWRITE:
    default:
        /* Keep what the cart already was: a rewritten wildcard stays a
         * wildcard, so retargeting one never costs a kid their wildcard. */
        return presented;
    }
}

bool boot_should_heal(const boot_input_t* in)
{
    if (in == NULL) {
        return false;
    }
    return in->tag == BOOT_TAG_OK && in->cls != BOOT_CLASS_BLANK &&
           in->auth == BOOT_AUTH_OPEN;
}
