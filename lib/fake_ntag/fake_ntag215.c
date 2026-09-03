#include "fake_ntag215.h"

#include <string.h>

/* Mirrors of the values in cart/ntag.h. Deliberately duplicated rather than
 * included: the fake must model the part as the data sheet describes it, not
 * as the driver under test believes it to be, or a wrong constant in the
 * driver would agree with itself and the test would pass. */
#define FAKE_PAGE_STATIC_LOCK 0x02
#define FAKE_PAGE_CC 0x03
#define FAKE_PAGE_USER_FIRST 0x04
#define FAKE_PAGE_USER_LAST 0x81
#define FAKE_PAGE_DYN_LOCK 0x82
#define FAKE_PAGE_CFG0 0x83
#define FAKE_PAGE_CFG1 0x84
#define FAKE_PAGE_PWD 0x85
#define FAKE_PAGE_PACK 0x86

#define FAKE_CFG0_AUTH0 3
#define FAKE_ACCESS_PROT 0x80
#define FAKE_ACCESS_AUTHLIM_MASK 0x07

#define FAKE_CMD_GET_VERSION 0x60
#define FAKE_CMD_READ 0x30
#define FAKE_CMD_WRITE 0xA2
#define FAKE_CMD_PWD_AUTH 0x1B

/* Transport results, matching ntag_xcv_result_e. */
#define FAKE_XCV_OK 0
#define FAKE_XCV_NAK (-2)
#define FAKE_XCV_IO (-3)

/* GET_VERSION for NTAG215 — data sheet Table 28. Byte 6, the storage size,
 * is 11h for NTAG215; 0Fh would be an NTAG213 and 13h an NTAG216. */
static const uint8_t fake_version[8] = {
    0x00, 0x04, 0x04, 0x02, 0x01, 0x00, 0x11, 0x03,
};

void fake_ntag215_init(fake_ntag215_t* t)
{
    memset(t, 0, sizeof(*t));

    /* Manufacturer data for the data sheet's example UID 04-E1-41-12-4C-28-80,
     * with its two check bytes and the 48h internal byte. The static lock
     * bytes ship clear. */
    t->pages[0][0] = 0x04;
    t->pages[0][1] = 0xE1;
    t->pages[0][2] = 0x41;
    t->pages[0][3] = 0x2C; /* BCC0 = 88h ^ UID0 ^ UID1 ^ UID2 */
    t->pages[1][0] = 0x12;
    t->pages[1][1] = 0x4C;
    t->pages[1][2] = 0x28;
    t->pages[1][3] = 0x80;
    t->pages[FAKE_PAGE_STATIC_LOCK][0] = 0xF6; /* BCC1 */
    t->pages[FAKE_PAGE_STATIC_LOCK][1] = 0x48; /* internal */
    t->pages[FAKE_PAGE_STATIC_LOCK][2] = 0x00; /* lock bytes */
    t->pages[FAKE_PAGE_STATIC_LOCK][3] = 0x00;

    /* Delivery content, data sheet Table 6: CC for NTAG215's 496-byte NDEF
     * area, then an empty NDEF message in the first user page. */
    t->pages[FAKE_PAGE_CC][0] = 0xE1;
    t->pages[FAKE_PAGE_CC][1] = 0x10;
    t->pages[FAKE_PAGE_CC][2] = 0x3E;
    t->pages[FAKE_PAGE_CC][3] = 0x00;
    t->pages[FAKE_PAGE_USER_FIRST][0] = 0x03;
    t->pages[FAKE_PAGE_USER_FIRST][1] = 0x00;
    t->pages[FAKE_PAGE_USER_FIRST][2] = 0xFE;
    t->pages[FAKE_PAGE_USER_FIRST][3] = 0x00;

    /* CFG0: MIRROR with STRG_MOD_EN set, no ASCII mirror, AUTH0 open. */
    t->pages[FAKE_PAGE_CFG0][0] = 0x04;
    t->pages[FAKE_PAGE_CFG0][1] = 0x00;
    t->pages[FAKE_PAGE_CFG0][2] = 0x00;
    t->pages[FAKE_PAGE_CFG0][3] = 0xFF;

    /* CFG1: ACCESS all clear. Byte 1 is documented RFUI, but real parts ship
     * with 05h there — kept so a test notices if the driver clears it. */
    t->pages[FAKE_PAGE_CFG1][0] = 0x00;
    t->pages[FAKE_PAGE_CFG1][1] = 0x05;
    t->pages[FAKE_PAGE_CFG1][2] = 0x00;
    t->pages[FAKE_PAGE_CFG1][3] = 0x00;

    memset(t->pages[FAKE_PAGE_PWD], 0xFF, 4);
    memset(t->pages[FAKE_PAGE_PACK], 0x00, 4);
}

void fake_ntag215_set_all_zero_user(fake_ntag215_t* t)
{
    uint8_t p;
    for (p = FAKE_PAGE_USER_FIRST; p <= FAKE_PAGE_USER_LAST; p++) {
        memset(t->pages[p], 0x00, 4);
    }
}

void fake_ntag215_set_protected(fake_ntag215_t* t, const uint8_t* pwd,
                                const uint8_t* pack, uint8_t auth0)
{
    memcpy(t->pages[FAKE_PAGE_PWD], pwd, 4);
    t->pages[FAKE_PAGE_PACK][0] = pack[0];
    t->pages[FAKE_PAGE_PACK][1] = pack[1];
    t->pages[FAKE_PAGE_CFG0][FAKE_CFG0_AUTH0] = auth0;
}

uint8_t fake_ntag215_access(const fake_ntag215_t* t)
{
    return t->pages[FAKE_PAGE_CFG1][0];
}

uint8_t fake_ntag215_auth0(const fake_ntag215_t* t)
{
    return t->pages[FAKE_PAGE_CFG0][FAKE_CFG0_AUTH0];
}

void fake_ntag215_reset_log(fake_ntag215_t* t)
{
    memset(t->log, 0, sizeof(t->log));
    t->logged = 0;
    t->seen = 0;
    t->authed = false;
}

size_t fake_ntag215_count(const fake_ntag215_t* t, uint8_t cmd, int page)
{
    size_t n = 0;
    size_t i;

    for (i = 0; i < t->logged; i++) {
        if (t->log[i].cmd != cmd) {
            continue;
        }
        if (page != FAKE_NTAG215_ANY_PAGE && t->log[i].page != (uint8_t)page) {
            continue;
        }
        n++;
    }
    return n;
}

size_t fake_ntag215_lock_pages_touched(const fake_ntag215_t* t)
{
    return fake_ntag215_count(t, FAKE_CMD_WRITE, FAKE_PAGE_STATIC_LOCK) +
           fake_ntag215_count(t, FAKE_CMD_WRITE, FAKE_PAGE_CC) +
           fake_ntag215_count(t, FAKE_CMD_WRITE, FAKE_PAGE_DYN_LOCK);
}

static void log_cmd(fake_ntag215_t* t, uint8_t cmd, uint8_t page)
{
    t->seen++;
    if (t->logged < FAKE_NTAG215_LOG_MAX) {
        t->log[t->logged].cmd = cmd;
        t->log[t->logged].page = page;
        t->logged++;
    }
}

/* Write access needs a session from AUTH0 onwards. Read access needs one too,
 * but only when PROT is set. */
static bool needs_auth(const fake_ntag215_t* t, uint8_t page)
{
    return page >= fake_ntag215_auth0(t);
}

static int do_read(fake_ntag215_t* t, uint8_t page, uint8_t* rx, size_t rx_cap,
                   size_t* rx_len)
{
    uint8_t i;
    uint8_t p;

    if (page >= FAKE_NTAG215_PAGES) {
        return FAKE_XCV_NAK;
    }
    if ((fake_ntag215_access(t) & FAKE_ACCESS_PROT) && needs_auth(t, page) &&
        !t->authed) {
        return FAKE_XCV_NAK;
    }
    if (rx_cap < 16) {
        return FAKE_XCV_IO;
    }
    /* Four pages, rolling over past the last one the way the part does. */
    for (i = 0; i < 4; i++) {
        p = (uint8_t)((page + i) % FAKE_NTAG215_PAGES);
        if (p == FAKE_PAGE_PWD || p == FAKE_PAGE_PACK) {
            memset(rx + i * 4, 0x00, 4); /* both read back as zeros */
        } else {
            memcpy(rx + i * 4, t->pages[p], 4);
        }
    }
    *rx_len = 16;
    return FAKE_XCV_OK;
}

static int do_write(fake_ntag215_t* t, uint8_t page, const uint8_t* data,
                    size_t* rx_len)
{
    if (t->nak_one_write && page == t->nak_write_page) {
        return FAKE_XCV_NAK;
    }
    if (page >= FAKE_NTAG215_PAGES) {
        return FAKE_XCV_NAK;
    }
    if (needs_auth(t, page) && !t->authed) {
        return FAKE_XCV_NAK;
    }
    /* Stored, not refused, even for the lock pages and the CC — the point is
     * that the driver is never supposed to get here for those. */
    memcpy(t->pages[page], data, 4);
    *rx_len = 0;
    return FAKE_XCV_OK;
}

static int do_pwd_auth(fake_ntag215_t* t, const uint8_t* pwd, uint8_t* rx,
                       size_t rx_cap, size_t* rx_len)
{
    uint8_t limit = (uint8_t)(fake_ntag215_access(t) & FAKE_ACCESS_AUTHLIM_MASK);

    if (limit != 0 && t->auth_fails >= limit) {
        return FAKE_XCV_NAK; /* locked out; only reachable if AUTHLIM was set */
    }
    if (memcmp(pwd, t->pages[FAKE_PAGE_PWD], 4) != 0) {
        if (t->auth_fails < 0xFF) {
            t->auth_fails++;
        }
        return FAKE_XCV_NAK;
    }
    if (rx_cap < 2) {
        return FAKE_XCV_IO;
    }
    rx[0] = t->pages[FAKE_PAGE_PACK][0];
    rx[1] = t->pages[FAKE_PAGE_PACK][1];
    *rx_len = 2;
    t->auth_fails = 0;
    t->authed = true;
    return FAKE_XCV_OK;
}

int fake_ntag215_xcv(void* ctx, const uint8_t* tx, size_t tx_len, uint8_t* rx,
                     size_t rx_cap, size_t* rx_len)
{
    fake_ntag215_t* t = (fake_ntag215_t*)ctx;

    *rx_len = 0;
    if (t == NULL || tx == NULL || tx_len == 0 || rx == NULL) {
        return FAKE_XCV_IO;
    }
    if (t->io_fail) {
        /* Logged first: an IO failure still means the command was sent. */
        log_cmd(t, tx[0], tx_len > 1 ? tx[1] : 0);
        return FAKE_XCV_IO;
    }

    switch (tx[0]) {
    case FAKE_CMD_GET_VERSION:
        log_cmd(t, tx[0], 0);
        if (rx_cap < sizeof(fake_version)) {
            return FAKE_XCV_IO;
        }
        memcpy(rx, fake_version, sizeof(fake_version));
        *rx_len = sizeof(fake_version);
        return FAKE_XCV_OK;

    case FAKE_CMD_READ:
        if (tx_len < 2) {
            return FAKE_XCV_NAK;
        }
        log_cmd(t, tx[0], tx[1]);
        return do_read(t, tx[1], rx, rx_cap, rx_len);

    case FAKE_CMD_WRITE:
        if (tx_len < 6) {
            return FAKE_XCV_NAK;
        }
        log_cmd(t, tx[0], tx[1]);
        return do_write(t, tx[1], tx + 2, rx_len);

    case FAKE_CMD_PWD_AUTH:
        if (tx_len < 5) {
            return FAKE_XCV_NAK;
        }
        log_cmd(t, tx[0], 0);
        return do_pwd_auth(t, tx + 1, rx, rx_cap, rx_len);

    default:
        log_cmd(t, tx[0], 0);
        return FAKE_XCV_NAK;
    }
}
