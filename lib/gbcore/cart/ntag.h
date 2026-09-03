#pragma once
// NTAG215 command layer over an injected transceive.
//
// Every byte of NTAG215 protocol lives here, in pure C over one function
// pointer, so the same code runs on the device through the PN532's
// InDataExchange and on the host against a fake tag model. Nothing in this
// file knows what a PN532 is, and the password is a parameter — never a
// compiled-in constant from a project header.
//
// The page map below is for the NTAG215 specifically; no other part is
// supported. Every constant is taken from the NXP NTAG213/215/216 product
// data sheet, Rev. 3.2 (2 June 2015, doc 265332) — Figure 6 for the memory
// organization, Table 8 for the configuration pages, Table 10 and Table 11
// for the ACCESS byte, and Table 22 for the command codes. A GET_VERSION on
// the first real tag is still a bench item (ROADMAP.md, bench item 12).
//
// Two safety rules are structural, not advisory:
//
//   * The static lock bytes (page 0x02), the capability container (0x03) and
//     the dynamic lock bytes (0x82) are NOT writable through this module by
//     any code path. There is no flag to override it.
//   * CFGLCK is never set. It would freeze the configuration permanently and
//     defeat the whole scheme — a mis-written cartridge could never be
//     recovered. ntag_page_writable() refuses any CFG1 write that sets it.
//
// Tags are write-protected with a password and read-openly: PROT = 0 so a
// phone can still inspect a cart, AUTHLIM = 0 so a wrong guess never bricks
// one, and AUTH0 is written last so protection engages only once the password
// is actually in place.
//
// Pure C, no Arduino/ESP-IDF headers, no allocation.

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- NTAG215 memory map (data sheet Figure 6) --------------------------- *
 * Pages are 4 bytes and the part has 135 of them, 0x00 to 0x86. 0x00-0x02
 * hold the serial number, the internal byte and the static lock bytes, 0x03
 * the capability container, 0x04-0x81 the 504 bytes of user memory, and
 * 0x82-0x86 the dynamic lock bytes and the four configuration pages.       */

#define NTAG215_PAGE_STATIC_LOCK 0x02 /* lock bytes — never written        */
#define NTAG215_PAGE_CC 0x03          /* capability container — never       */
#define NTAG215_PAGE_USER_FIRST 0x04  /* first user page                    */
#define NTAG215_PAGE_USER_LAST 0x81   /* last user page                     */
#define NTAG215_PAGE_DYN_LOCK 0x82    /* dynamic lock bytes — never written */
#define NTAG215_PAGE_CFG0 0x83        /* MIRROR | RFUI | MIRROR_PAGE | AUTH0 */
#define NTAG215_PAGE_CFG1 0x84        /* ACCESS | RFUI | RFUI | RFUI        */
#define NTAG215_PAGE_PWD 0x85         /* PWD, all four bytes                */
#define NTAG215_PAGE_PACK 0x86        /* PACK | PACK | RFUI | RFUI          */
#define NTAG215_PAGE_COUNT 0x87       /* one past the last page             */

/* CFG0 byte 3 is AUTH0, the first page password verification is required
 * from (data sheet Table 8, Table 11). Its default is 0xFF, and any value
 * past the last user page disables protection — so 0xFF is how an
 * unprotected tag reads. Byte 0 of the same page is MIRROR, whose
 * STRG_MOD_EN bit defaults to 1, which is why CFG0 is always written
 * read-modify-write and never composed from nothing. */
#define NTAG215_CFG0_AUTH0 3
#define NTAG215_AUTH0_OPEN 0xFF

/* CFG1 byte 0 is the ACCESS byte (data sheet Table 8, Table 10):
 *   bit 7    PROT              1 = protect reads too; we always write 0
 *   bit 6    CFGLCK            1 = configuration permanently locked, every
 *                              later write NAKed; NEVER written as 1
 *   bit 5    RFUI              written as 0
 *   bit 4    NFC_CNT_EN        left as the tag had it
 *   bit 3    NFC_CNT_PWD_PROT  left as the tag had it
 *   bits 2-0 AUTHLIM           failed-auth limit; we always write 0 = none
 *
 * CFGLCK is in CFG1, not CFG0: CFG0 has no lock bit at all. Bytes 1-3 of
 * CFG1 are RFUI, and Table 11 requires every RFUI bit be written as 0. */
#define NTAG215_CFG1_ACCESS 0
#define NTAG215_ACCESS_PROT 0x80
#define NTAG215_ACCESS_CFGLCK 0x40
#define NTAG215_ACCESS_AUTHLIM_MASK 0x07

/* ---- Type 2 Tag / NTAG commands (data sheet Table 22) ------------------- */

#define NTAG_CMD_GET_VERSION 0x60
#define NTAG_CMD_READ 0x30
#define NTAG_CMD_WRITE 0xA2
#define NTAG_CMD_PWD_AUTH 0x1B

#define NTAG_PAGE_SIZE 4
#define NTAG_READ_SIZE 16   /* READ answers with four pages (data sheet 10.2) */
#define NTAG_VERSION_SIZE 8 /* GET_VERSION answers eight bytes (Table 28)    */
#define NTAG_PACK_SIZE 2    /* PWD_AUTH answers the PACK (Table 39)          */
#define NTAG_PWD_SIZE 4

/* ---- transport --------------------------------------------------------- */

/* What an ntag_xcv_fn returns. The device driver must map a Type 2 NAK — or
 * a tag that simply does not answer — to NTAG_XCV_NAK, and keep NTAG_XCV_IO
 * for a failure of the transport itself, because the two mean very different
 * things to ntag_pwd_auth(). */
enum ntag_xcv_result_e {
    NTAG_XCV_OK = 0,
    NTAG_XCV_NAK = -2,
    NTAG_XCV_IO = -3,
};

/*
 * Send tx_len bytes and collect the answer into rx/rx_cap, writing the length
 * received to *rx_len. A command the tag acknowledges without data answers
 * with *rx_len == 0.
 */
typedef int (*ntag_xcv_fn)(void* ctx, const uint8_t* tx, size_t tx_len,
                           uint8_t* rx, size_t rx_cap, size_t* rx_len);

typedef struct ntag_dev_s {
    void* ctx;
    ntag_xcv_fn xcv;
} ntag_dev_t;

enum ntag_result_e {
    NTAG_OK = 0,
    NTAG_ERR_ARGS = -1,      /* NULL pointer, bad length, unusable device  */
    NTAG_ERR_NAK = -2,       /* the tag refused or went away               */
    NTAG_ERR_IO = -3,        /* the transport failed                       */
    NTAG_ERR_WHITELIST = -4, /* a write this module will not perform       */
    NTAG_ERR_VERIFY = -5,    /* the bytes read back are not what we wrote  */
    NTAG_ERR_AUTH = -6,      /* PWD_AUTH refused, or the PACK did not match */
    NTAG_ERR_RANGE = -7,     /* page range runs off the end of the part    */
};

/* GET_VERSION's eight bytes, which identify the part. */
int ntag_get_version(const ntag_dev_t* dev, uint8_t* out);

/*
 * Read n_pages pages starting at `first` into out (n_pages * NTAG_PAGE_SIZE
 * bytes), one READ per four pages.
 */
int ntag_read_pages(const ntag_dev_t* dev, uint8_t first, size_t n_pages,
                    uint8_t* out);

/* AUTH0 from CFG0 — NTAG215_AUTH0_OPEN means the tag is unprotected. */
int ntag_read_auth0(const ntag_dev_t* dev, uint8_t* auth0);

/*
 * The write whitelist, pure and exported so it can be tested without a tag:
 * 1 when this module is willing to write `page` with `data`, 0 otherwise.
 * User pages and the four configuration pages are writable; the lock bytes
 * and the capability container never are; and a CFG1 write whose ACCESS byte
 * sets CFGLCK is refused.
 */
int ntag_page_writable(uint8_t page, const uint8_t* data);

/* One page. Refused with NTAG_ERR_WHITELIST before any transceive happens. */
int ntag_write_page(const ntag_dev_t* dev, uint8_t page, const uint8_t* data);

/* len must be a whole number of pages and must fit inside user memory. */
int ntag_write_bytes(const ntag_dev_t* dev, uint8_t first_page,
                     const uint8_t* buf, size_t len);

/*
 * Read back and compare. PWD and PACK are skipped: they read as zeros by
 * design, so comparing them would always fail.
 */
int ntag_verify_bytes(const ntag_dev_t* dev, uint8_t first_page,
                      const uint8_t* buf, size_t len);

/*
 * Open a write session. NTAG_ERR_AUTH covers both a refused password and a
 * PACK that does not match — either way the tag is not ours to write.
 */
int ntag_pwd_auth(const ntag_dev_t* dev, const uint8_t* pwd,
                  const uint8_t* pack);

/*
 * Apply protection: PWD, then PACK, then ACCESS with PROT/CFGLCK/AUTHLIM all
 * clear, then AUTH0 last.
 */
int ntag_protect(const ntag_dev_t* dev, const uint8_t* pwd,
                 const uint8_t* pack);

/*
 * The whole provisioning sequence: authenticate if the tag is already
 * protected, write the NDEF message to user memory, verify it, protect the
 * tag, then prove the protection took by authenticating against it. That last
 * step is the only available proof, since PWD and PACK read back as zeros.
 */
int ntag_provision(const ntag_dev_t* dev, const uint8_t* ndef, size_t len,
                   const uint8_t* pwd, const uint8_t* pack);

#ifdef __cplusplus
}
#endif
