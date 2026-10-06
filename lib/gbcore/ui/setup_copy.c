#include "setup_copy.h"

#include <stdio.h>

/* What each step takes, finishing the sentence "Step N of 3 needs ...". */
static const char* const NEEDS[4] = {
    "",
    "a blank cart or your MENU cart.",
    "a blank cart or your wildcard.",
    "a blank cart, or your MENU cart to finish setup.",
};

/* A cart as the body names it, after "This is ". */
static const char* cart_name(enum boot_class_e cls)
{
    switch (cls) {
    case BOOT_CLASS_MENU:
        return "your MENU cart";
    case BOOT_CLASS_WILD:
        return "a wildcard";
    case BOOT_CLASS_GAME:
        return "a game cart";
    case BOOT_CLASS_BLANK:
    default:
        return "a blank cart";
    }
}

/* A cart as a list header names it, after "replaces this ". */
static const char* cart_noun(enum boot_class_e cls)
{
    switch (cls) {
    case BOOT_CLASS_MENU:
        return "MENU cart";
    case BOOT_CLASS_WILD:
        return "wildcard";
    case BOOT_CLASS_GAME:
    default:
        return "game cart";
    }
}

uint8_t setup_copy_step(const boot_flags_t* flags)
{
    if (flags == NULL || !flags->menu_done) {
        return 1;
    }
    return flags->wild_done ? 3 : 2;
}

static void no_cart(uint8_t step, bool restarted, char* body, size_t body_sz)
{
    switch (step) {
    case 1:
        snprintf(body, body_sz,
                 "%sStep 1 of 3: put in a blank cart or your MENU cart and "
                 "power on.",
                 restarted ? "Setup restarted. " : "");
        break;
    case 2:
        snprintf(body, body_sz,
                 "Step 2 of 3: put in a blank cart or your wildcard and power "
                 "on.");
        break;
    default:
        snprintf(body, body_sz,
                 "Step 3 of 3: put in a blank cart for a game cart, or your "
                 "MENU cart to finish setup.");
        break;
    }
}

static void wrong_cart(uint8_t step, enum boot_class_e cls, char* body,
                       size_t body_sz)
{
    if (step == 3 && cls == BOOT_CLASS_GAME) {
        snprintf(body, body_sz,
                 "This is already a game cart. Put in a blank cart, or your "
                 "MENU cart to finish setup.");
        return;
    }
    snprintf(body, body_sz, "This is %s. Step %u of 3 needs %s",
             cart_name(cls), (unsigned)step, NEEDS[step]);
}

/* A restarted setup's list over a cart of ours names what it replaces. */
static bool replacing(const boot_flags_t* flags, enum boot_class_e cls)
{
    return flags != NULL && flags->rewrite && cls != BOOT_CLASS_BLANK;
}

int setup_copy(enum setup_screen_e screen, const boot_flags_t* flags,
               enum boot_class_e cls, int code, char* title, size_t title_sz,
               char* body, size_t body_sz)
{
    uint8_t step = setup_copy_step(flags);
    bool restarted = (flags != NULL && flags->rewrite);
    const char* t = "";

    if (title == NULL || body == NULL || title_sz == 0 || body_sz == 0) {
        return SETUP_COPY_ERR_ARGS;
    }
    title[0] = '\0';
    body[0] = '\0';

    switch (screen) {
    case SETUP_SCREEN_NO_CART:
        t = "Setup: no cartridge";
        no_cart(step, restarted, body, body_sz);
        break;
    case SETUP_SCREEN_WRONG_CART:
        t = "Setup: wrong cart";
        wrong_cart(step, cls, body, body_sz);
        break;
    case SETUP_SCREEN_FOREIGN:
        t = "Not your cartridge";
        snprintf(body, body_sz,
                 "This cart belongs to another Game Boy and can't be changed. "
                 "Power off and put in a blank cart.");
        break;
    case SETUP_SCREEN_MENU_DONE:
        t = "MENU cart ready";
        snprintf(body, body_sz,
                 "Step 1 of 3 done. Power off, put in a blank cart, power on "
                 "to make your wildcard.");
        break;
    case SETUP_SCREEN_WILD_DONE:
        t = "Wildcard ready";
        snprintf(body, body_sz,
                 "Step 2 of 3 done. Power off, put in a blank cart for a game "
                 "cart, or your MENU cart to finish setup.");
        break;
    case SETUP_SCREEN_GAME_DONE:
        t = "Game cart ready";
        snprintf(body, body_sz,
                 "Power off. Put in another blank cart for another game, or "
                 "your MENU cart to finish setup.");
        break;
    case SETUP_SCREEN_FINISHED:
        t = "Setup finished";
        snprintf(body, body_sz,
                 "Power off. Put in a game cart or your wildcard and power on "
                 "to play. Your MENU cart picks the wildcard's game.");
        break;
    case SETUP_SCREEN_WRITE_FAILED:
        t = "Write failed";
        snprintf(body, body_sz,
                 "Code %d. Nothing was changed. Power off, keep the cart "
                 "still, and power on to try again.",
                 code);
        break;
    case SETUP_SCREEN_NO_GAMES:
        t = "No games found";
        snprintf(body, body_sz,
                 "Setup needs the SD card's game list with at least one "
                 "starter game. Fix the card and power on again.");
        break;
    case SETUP_SCREEN_LIST_WILD:
        if (replacing(flags, cls) && cls != BOOT_CLASS_WILD) {
            snprintf(title, title_sz, "Step 2 of 3: replaces this %s",
                     cart_noun(cls));
            return SETUP_COPY_OK;
        }
        t = "Step 2 of 3: your wildcard's first game";
        break;
    case SETUP_SCREEN_LIST_GAME:
        if (replacing(flags, cls)) {
            snprintf(title, title_sz, "Step 3 of 3: a game replaces this %s",
                     cart_noun(cls));
            return SETUP_COPY_OK;
        }
        t = "Step 3 of 3: a game cart, or finish setup";
        break;
    case SETUP_SCREEN_LIST_FINISH:
        t = "Put in a blank cart to make a game cart";
        break;
    case SETUP_SCREEN_LIST_MAKE_MENU:
        snprintf(title, title_sz, "Step 1 of 3: replaces this %s",
                 cart_noun(cls));
        return SETUP_COPY_OK;
    default:
        return SETUP_COPY_ERR_ARGS;
    }
    snprintf(title, title_sz, "%s", t);
    return SETUP_COPY_OK;
}
