#pragma once
// Every word the setup flow shows, in one table.
//
// Each screen setup can put up — an empty reader, a wrong or foreign cart, a
// write that failed, a missing game list, each success, and the one-line
// header over each setup list — is looked up here by what happened, the
// stored setup flags, and the kind of cart on the reader. The binding draws
// what comes back and owns none of the words, so the copy is reviewed in one
// place and the host suite can prove that every screen has a body and fits.
//
// Every body says which step of three the user is on, or that a step is
// done, and the physical next action: power off, put in a named cart, power
// on. The step is derived from the flags, so per-step wording needs no extra
// screens.
//
// Pure C, no Arduino/ESP-IDF headers, no allocation.

#include <stddef.h>
#include <stdint.h>

#include "cart/boot.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Room for the longest title or body here, with margin. */
#define SETUP_COPY_MAX 160

enum setup_screen_e {
    SETUP_SCREEN_NO_CART = 0,
    SETUP_SCREEN_WRONG_CART,
    SETUP_SCREEN_FOREIGN,
    SETUP_SCREEN_MENU_DONE,
    SETUP_SCREEN_WILD_DONE,
    SETUP_SCREEN_GAME_DONE,
    SETUP_SCREEN_FINISHED,
    SETUP_SCREEN_WRITE_FAILED,
    SETUP_SCREEN_NO_GAMES,
    /* The setup lists' headers. These come back as the title, with an empty
     * body. */
    SETUP_SCREEN_LIST_WILD,
    SETUP_SCREEN_LIST_GAME,
    SETUP_SCREEN_LIST_FINISH,
    SETUP_SCREEN_LIST_MAKE_MENU,
    SETUP_SCREEN_COUNT,
};

enum setup_copy_result_e {
    SETUP_COPY_OK = 0,
    SETUP_COPY_ERR_ARGS = -1, /* NULL, a zero-size buffer, an unknown screen */
};

/* The step setup is on: 1 until the MENU cart is done, 2 until the wildcard
 * is, else 3. A NULL flags pointer is step 1. */
uint8_t setup_copy_step(const boot_flags_t* flags);

/*
 * Fill `title` and `body` for `screen`. `cls` is the kind of cart on the
 * reader, read by the wrong-cart screen and the list headers; `code` appears
 * only in the write-failed body. Both buffers are always NUL-terminated and
 * truncated to fit; SETUP_COPY_MAX each is always enough.
 */
int setup_copy(enum setup_screen_e screen, const boot_flags_t* flags,
               enum boot_class_e cls, int code, char* title, size_t title_sz,
               char* body, size_t body_sz);

#ifdef __cplusplus
}
#endif
