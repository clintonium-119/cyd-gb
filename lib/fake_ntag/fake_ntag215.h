#pragma once
// Fake NTAG215 — TEST-ONLY. Models the 135 pages of an NTAG215 along with
// AUTH0 / ACCESS protection, PWD_AUTH, and the zero read-back of PWD and
// PACK, behind the ntag_xcv_fn seam. Never reference this from anything under
// src/: it must never link into the firmware.
//
// It sits BELOW the PN532 — no reader framing is modelled, only the tag's own
// command set.
//
// Two deliberate choices make it a test instrument rather than a simulator:
//
//   * It does NOT refuse writes to the lock bytes, the capability container
//     or the dynamic lock bytes. It stores them and counts them, so a test
//     can prove the driver's whitelist never sent one. A fake that refused
//     them would leave the whitelist untested.
//   * It honours AUTHLIM when non-zero, so a test can prove the driver never
//     sets it.
//
// Every command is logged, which is what lets a test assert on what was never
// sent and in what order the rest arrived.
//
// Factory values follow the NXP NTAG213/215/216 data sheet Rev. 3.2: Table 6
// for the delivery content of pages 03h-05h, Table 8 and Table 11 for the
// configuration page defaults.
//
// Pure C, no Arduino/ESP-IDF headers.

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#define FAKE_NTAG215_PAGES 0x87
#define FAKE_NTAG215_LOG_MAX 64

/* Passed as `page` to fake_ntag215_count() to count a command regardless of
 * which page it addressed. */
#define FAKE_NTAG215_ANY_PAGE (-1)

typedef struct fake_ntag215_cmd_s {
    uint8_t cmd;
    uint8_t page; /* 0 for commands that take no page */
} fake_ntag215_cmd_t;

typedef struct fake_ntag215_s {
    uint8_t pages[FAKE_NTAG215_PAGES][4];
    bool authed;

    /* Command log. `seen` counts every command; `logged` stops at
     * FAKE_NTAG215_LOG_MAX so a runaway loop cannot overrun the array. */
    fake_ntag215_cmd_t log[FAKE_NTAG215_LOG_MAX];
    size_t logged;
    size_t seen;

    /* Failed password verifications, against AUTHLIM. */
    uint8_t auth_fails;

    /* Test knobs. */
    bool io_fail;         /* every transceive reports a transport failure */
    bool nak_one_write;   /* NAK writes addressed to nak_write_page       */
    uint8_t nak_write_page;
} fake_ntag215_t;

/* A factory tag: unprotected, empty NDEF message in user memory. */
void fake_ntag215_init(fake_ntag215_t* t);

/* The ntag_xcv_fn for this tag. `ctx` is the fake_ntag215_t*. */
int fake_ntag215_xcv(void* ctx, const uint8_t* tx, size_t tx_len, uint8_t* rx,
                     size_t rx_cap, size_t* rx_len);

/* How many times `cmd` was sent, optionally only to `page`. Pass
 * FAKE_NTAG215_ANY_PAGE for any. */
size_t fake_ntag215_count(const fake_ntag215_t* t, uint8_t cmd, int page);

/* Writes the driver sent to a page it must never write: the static lock
 * bytes, the capability container, or the dynamic lock bytes. Must be zero. */
size_t fake_ntag215_lock_pages_touched(const fake_ntag215_t* t);

/* Turn the tag into one already protected — by this build's password, or by
 * another's, depending on what is passed. */
void fake_ntag215_set_protected(fake_ntag215_t* t, const uint8_t* pwd,
                                const uint8_t* pack, uint8_t auth0);

/* Blank the user area the other way: all zeros rather than an empty NDEF
 * message. Both shapes are valid write targets. */
void fake_ntag215_set_all_zero_user(fake_ntag215_t* t);

/* Read the stored ACCESS byte and AUTH0 without going through a command. */
uint8_t fake_ntag215_access(const fake_ntag215_t* t);
uint8_t fake_ntag215_auth0(const fake_ntag215_t* t);

/* Forget the log and the session, keeping the memory contents — used to
 * assert on a second provisioning run in isolation. */
void fake_ntag215_reset_log(fake_ntag215_t* t);

#ifdef __cplusplus
}
#endif
