#include "ntag.h"

#include <string.h>

/* Longest command this module sends: WRITE is 1 + 1 + 4 bytes. */
#define NTAG_TX_MAX 6

static int dev_usable(const ntag_dev_t* dev)
{
    return dev != NULL && dev->xcv != NULL;
}

/* A transceive failure is never success. NAK and IO stay distinct because
 * ntag_pwd_auth() reads a NAK as "wrong password" and an IO error as "the
 * reader broke", and those must not be confused. */
static int map_xcv(int rc)
{
    if (rc == NTAG_XCV_OK) {
        return NTAG_OK;
    }
    if (rc == NTAG_XCV_NAK) {
        return NTAG_ERR_NAK;
    }
    return NTAG_ERR_IO;
}

int ntag_get_version(const ntag_dev_t* dev, uint8_t* out)
{
    uint8_t tx[1];
    size_t rx_len = 0;
    int rc;

    if (!dev_usable(dev) || out == NULL) {
        return NTAG_ERR_ARGS;
    }
    tx[0] = NTAG_CMD_GET_VERSION;
    rc = map_xcv(dev->xcv(dev->ctx, tx, sizeof(tx), out, NTAG_VERSION_SIZE,
                          &rx_len));
    if (rc != NTAG_OK) {
        return rc;
    }
    return (rx_len == NTAG_VERSION_SIZE) ? NTAG_OK : NTAG_ERR_IO;
}

int ntag_read_pages(const ntag_dev_t* dev, uint8_t first, size_t n_pages,
                    uint8_t* out)
{
    uint8_t tx[2];
    uint8_t rx[NTAG_READ_SIZE];
    size_t done = 0;
    size_t rx_len;
    size_t chunk;
    int rc;

    if (!dev_usable(dev) || out == NULL) {
        return NTAG_ERR_ARGS;
    }
    if (n_pages == 0) {
        return NTAG_ERR_ARGS;
    }
    if ((size_t)first + n_pages > NTAG215_PAGE_COUNT) {
        return NTAG_ERR_RANGE;
    }

    /* One READ per four pages. A READ near the end of memory rolls over, so
     * only the pages actually asked for are copied out. */
    while (done < n_pages) {
        tx[0] = NTAG_CMD_READ;
        tx[1] = (uint8_t)(first + done);
        rx_len = 0;
        rc = map_xcv(dev->xcv(dev->ctx, tx, sizeof(tx), rx, sizeof(rx),
                              &rx_len));
        if (rc != NTAG_OK) {
            return rc;
        }
        if (rx_len != NTAG_READ_SIZE) {
            return NTAG_ERR_IO;
        }
        chunk = n_pages - done;
        if (chunk > NTAG_READ_SIZE / NTAG_PAGE_SIZE) {
            chunk = NTAG_READ_SIZE / NTAG_PAGE_SIZE;
        }
        memcpy(out + done * NTAG_PAGE_SIZE, rx, chunk * NTAG_PAGE_SIZE);
        done += chunk;
    }
    return NTAG_OK;
}

int ntag_read_auth0(const ntag_dev_t* dev, uint8_t* auth0)
{
    uint8_t page[NTAG_PAGE_SIZE];
    int rc;

    if (!dev_usable(dev) || auth0 == NULL) {
        return NTAG_ERR_ARGS;
    }
    rc = ntag_read_pages(dev, NTAG215_PAGE_CFG0, 1, page);
    if (rc != NTAG_OK) {
        return rc;
    }
    *auth0 = page[NTAG215_CFG0_AUTH0];
    return NTAG_OK;
}

int ntag_page_writable(uint8_t page, const uint8_t* data)
{
    if (data == NULL) {
        return 0;
    }
    /* The static lock bytes, the capability container and the dynamic lock
     * bytes are permanent once set. Nothing here writes them, ever — there is
     * no flag to override this. */
    if (page < NTAG215_PAGE_USER_FIRST) {
        return 0;
    }
    if (page == NTAG215_PAGE_DYN_LOCK) {
        return 0;
    }
    if (page > NTAG215_PAGE_PACK) {
        return 0;
    }
    /* Setting CFGLCK would freeze the configuration for good and NAK every
     * later write, so a mis-written cartridge could never be recovered. */
    if (page == NTAG215_PAGE_CFG1 &&
        (data[NTAG215_CFG1_ACCESS] & NTAG215_ACCESS_CFGLCK) != 0) {
        return 0;
    }
    return 1;
}

int ntag_write_page(const ntag_dev_t* dev, uint8_t page, const uint8_t* data)
{
    uint8_t tx[NTAG_TX_MAX];
    uint8_t rx[NTAG_READ_SIZE];
    size_t rx_len = 0;

    if (!dev_usable(dev) || data == NULL) {
        return NTAG_ERR_ARGS;
    }
    /* Checked before the transceive, so a refused write never reaches the
     * tag at all. */
    if (!ntag_page_writable(page, data)) {
        return NTAG_ERR_WHITELIST;
    }
    tx[0] = NTAG_CMD_WRITE;
    tx[1] = page;
    memcpy(tx + 2, data, NTAG_PAGE_SIZE);
    return map_xcv(dev->xcv(dev->ctx, tx, sizeof(tx), rx, sizeof(rx),
                            &rx_len));
}

/* User memory holds len bytes starting at first_page? */
static int fits_user_memory(uint8_t first_page, size_t len)
{
    size_t n_pages = len / NTAG_PAGE_SIZE;

    if (first_page < NTAG215_PAGE_USER_FIRST) {
        return 0;
    }
    return (size_t)first_page + n_pages <= (size_t)NTAG215_PAGE_USER_LAST + 1u;
}

int ntag_write_bytes(const ntag_dev_t* dev, uint8_t first_page,
                     const uint8_t* buf, size_t len)
{
    size_t i;
    int rc;

    if (!dev_usable(dev) || buf == NULL || len == 0) {
        return NTAG_ERR_ARGS;
    }
    if (len % NTAG_PAGE_SIZE != 0) {
        return NTAG_ERR_ARGS;
    }
    if (!fits_user_memory(first_page, len)) {
        return NTAG_ERR_RANGE;
    }
    for (i = 0; i < len / NTAG_PAGE_SIZE; i++) {
        rc = ntag_write_page(dev, (uint8_t)(first_page + i),
                             buf + i * NTAG_PAGE_SIZE);
        if (rc != NTAG_OK) {
            return rc;
        }
    }
    return NTAG_OK;
}

int ntag_verify_bytes(const ntag_dev_t* dev, uint8_t first_page,
                      const uint8_t* buf, size_t len)
{
    uint8_t page[NTAG_PAGE_SIZE];
    size_t n_pages;
    size_t i;
    uint8_t addr;
    int rc;

    if (!dev_usable(dev) || buf == NULL || len == 0) {
        return NTAG_ERR_ARGS;
    }
    if (len % NTAG_PAGE_SIZE != 0) {
        return NTAG_ERR_ARGS;
    }
    n_pages = len / NTAG_PAGE_SIZE;
    if ((size_t)first_page + n_pages > NTAG215_PAGE_COUNT) {
        return NTAG_ERR_RANGE;
    }

    for (i = 0; i < n_pages; i++) {
        addr = (uint8_t)(first_page + i);
        /* PWD and PACK read back as zeros by design, so comparing them would
         * fail every time and prove nothing. */
        if (addr == NTAG215_PAGE_PWD || addr == NTAG215_PAGE_PACK) {
            continue;
        }
        rc = ntag_read_pages(dev, addr, 1, page);
        if (rc != NTAG_OK) {
            return rc;
        }
        if (memcmp(page, buf + i * NTAG_PAGE_SIZE, NTAG_PAGE_SIZE) != 0) {
            return NTAG_ERR_VERIFY;
        }
    }
    return NTAG_OK;
}

int ntag_pwd_auth(const ntag_dev_t* dev, const uint8_t* pwd,
                  const uint8_t* pack)
{
    uint8_t tx[1 + NTAG_PWD_SIZE];
    uint8_t rx[NTAG_READ_SIZE];
    size_t rx_len = 0;
    int rc;

    if (!dev_usable(dev) || pwd == NULL || pack == NULL) {
        return NTAG_ERR_ARGS;
    }
    tx[0] = NTAG_CMD_PWD_AUTH;
    memcpy(tx + 1, pwd, NTAG_PWD_SIZE);
    rc = map_xcv(dev->xcv(dev->ctx, tx, sizeof(tx), rx, sizeof(rx), &rx_len));
    /* A NAK here means the tag rejected the password — a foreign cart, not a
     * broken reader. Everything else stays an IO error. */
    if (rc == NTAG_ERR_NAK) {
        return NTAG_ERR_AUTH;
    }
    if (rc != NTAG_OK) {
        return rc;
    }
    if (rx_len < NTAG_PACK_SIZE) {
        return NTAG_ERR_IO;
    }
    /* The tag answered, but with someone else's PACK. */
    if (memcmp(rx, pack, NTAG_PACK_SIZE) != 0) {
        return NTAG_ERR_AUTH;
    }
    return NTAG_OK;
}

int ntag_protect(const ntag_dev_t* dev, const uint8_t* pwd,
                 const uint8_t* pack)
{
    uint8_t page[NTAG_PAGE_SIZE];
    uint8_t cfg[NTAG_PAGE_SIZE];
    int rc;

    if (!dev_usable(dev) || pwd == NULL || pack == NULL) {
        return NTAG_ERR_ARGS;
    }

    /* 1. PWD. */
    rc = ntag_write_page(dev, NTAG215_PAGE_PWD, pwd);
    if (rc != NTAG_OK) {
        return rc;
    }

    /* 2. PACK, whose upper two bytes are RFUI and must be written as zero. */
    page[0] = pack[0];
    page[1] = pack[1];
    page[2] = 0x00;
    page[3] = 0x00;
    rc = ntag_write_page(dev, NTAG215_PAGE_PACK, page);
    if (rc != NTAG_OK) {
        return rc;
    }

    /* 3. ACCESS: PROT clear so reads stay open and a phone can still inspect
     * a cart, CFGLCK clear so the configuration is never frozen, AUTHLIM
     * clear so a wrong guess can never brick a cart. Every other bit and
     * byte of the page is written back exactly as it was read. The data
     * sheet marks bit 5 and bytes 1-3 RFUI and says to write them as 0, but
     * real tags ship with a non-zero byte there, so this preserves whatever
     * the part actually holds rather than clearing a value whose meaning is
     * undocumented. */
    rc = ntag_read_pages(dev, NTAG215_PAGE_CFG1, 1, cfg);
    if (rc != NTAG_OK) {
        return rc;
    }
    memcpy(page, cfg, NTAG_PAGE_SIZE);
    page[NTAG215_CFG1_ACCESS] =
        (uint8_t)(cfg[NTAG215_CFG1_ACCESS] &
                  (uint8_t) ~(NTAG215_ACCESS_PROT | NTAG215_ACCESS_CFGLCK |
                              NTAG215_ACCESS_AUTHLIM_MASK));
    rc = ntag_write_page(dev, NTAG215_PAGE_CFG1, page);
    if (rc != NTAG_OK) {
        return rc;
    }

    /* 4. AUTH0 last, so protection only engages once the password above is
     * actually in place. A run that dies before this point leaves the tag
     * open and re-provisionable, which is what the boot flow's heal path
     * relies on. Only AUTH0 changes: MIRROR and MIRROR_PAGE are preserved
     * because STRG_MOD_EN defaults to 1 and clearing it would weaken the
     * tag's modulation. */
    rc = ntag_read_pages(dev, NTAG215_PAGE_CFG0, 1, cfg);
    if (rc != NTAG_OK) {
        return rc;
    }
    memcpy(page, cfg, NTAG_PAGE_SIZE);
    page[NTAG215_CFG0_AUTH0] = NTAG215_PAGE_USER_FIRST;
    return ntag_write_page(dev, NTAG215_PAGE_CFG0, page);
}

int ntag_provision(const ntag_dev_t* dev, const uint8_t* ndef, size_t len,
                   const uint8_t* pwd, const uint8_t* pack)
{
    uint8_t auth0 = 0;
    int rc;

    if (!dev_usable(dev) || ndef == NULL || pwd == NULL || pack == NULL) {
        return NTAG_ERR_ARGS;
    }
    if (len == 0 || len % NTAG_PAGE_SIZE != 0) {
        return NTAG_ERR_ARGS;
    }
    if (!fits_user_memory(NTAG215_PAGE_USER_FIRST, len)) {
        return NTAG_ERR_RANGE;
    }

    /* A tag already protected needs a session before any write. If the
     * password is not ours the tag belongs to someone else and we stop. */
    rc = ntag_read_auth0(dev, &auth0);
    if (rc != NTAG_OK) {
        return rc;
    }
    if (auth0 != NTAG215_AUTH0_OPEN) {
        rc = ntag_pwd_auth(dev, pwd, pack);
        if (rc != NTAG_OK) {
            return rc;
        }
    }

    rc = ntag_write_bytes(dev, NTAG215_PAGE_USER_FIRST, ndef, len);
    if (rc != NTAG_OK) {
        return rc;
    }
    rc = ntag_verify_bytes(dev, NTAG215_PAGE_USER_FIRST, ndef, len);
    if (rc != NTAG_OK) {
        return rc;
    }
    rc = ntag_protect(dev, pwd, pack);
    if (rc != NTAG_OK) {
        return rc;
    }
    /* PWD and PACK read back as zeros, so authenticating against the tag is
     * the only available proof that protection actually took. */
    return ntag_pwd_auth(dev, pwd, pack);
}
