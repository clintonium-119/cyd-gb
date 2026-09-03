#pragma once
// NDEF Text-record parse and compose — the byte layout every cartridge tag
// carries, in both directions, as pure host-testable C.
//
// A tag's user memory holds a TLV-framed NDEF message wrapping exactly one
// well-known Text record, whose payload is the cartridge string:
//
//   03 <mlen> D1 01 <plen> 54 02 65 6E <text...> FE  [00 pad to a page]
//   │   │     │  │   │     │  │  └"en"┘            └─ terminator TLV
//   │   │     │  │   │     │  └──── status byte: UTF-8, language length 2
//   │   │     │  │   │     └─────── type 'T'
//   │   │     │  │   └───────────── payload length (status + language + text)
//   │   │     └──┴───────────────── record header, type length
//   └───┴────────────────────────── message TLV, message length
//
// The language code's length is read from the low 6 bits of the status byte
// and never assumed to be 2: a tag hand-written from a French phone carries
// "fr-CA", and assuming 2 would turn a filename into "CATetris.gb". The
// device itself always composes with "en".
//
// Both factory-empty shapes — the empty message 03 00 FE and an all-zero
// user area — classify as NDEF_BLANK rather than as an error, because both
// are valid targets for a write.
//
// Pure C, no Arduino/ESP-IDF headers, no allocation: every buffer is
// caller-owned.

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The on-tag region the device reads and writes: 24 NTAG215 pages. The
 * longest payload the device composes is "WILD:" plus a 63-character file
 * name, and the framing above adds 10 bytes plus padding — 96 leaves room
 * for that and for a legacy tag's longer language code. */
#define NDEF_BUF_MAX 96

/* Longest text ndef_compose_text() will accept: NDEF_BUF_MAX less the 10
 * framing bytes, rounded so the padded message lands exactly on
 * NDEF_BUF_MAX. */
#define NDEF_TEXT_MAX (NDEF_BUF_MAX - 12)

enum ndef_result_e {
    NDEF_OK = 0,
    NDEF_BLANK = 1,            /* empty message or all-zero region        */
    NDEF_ERR_ARGS = -1,        /* NULL buffer or zero-sized output        */
    NDEF_ERR_NOT_NDEF = -2,    /* no 0x03 message TLV                     */
    NDEF_ERR_NOT_TEXT = -3,    /* not a well-known Text record, or UTF-16 */
    NDEF_ERR_TRUNCATED = -4,   /* a length runs past len, or no 0xFE      */
    NDEF_ERR_TOO_LONG = -5,    /* text exceeds out_sz / message exceeds   */
};

/*
 * Parse the text payload of the NDEF Text record in buf/len into out,
 * NUL-terminated and at most out_sz bytes including the NUL. Never writes
 * beyond out_sz, and NUL-terminates out on every outcome that is not
 * NDEF_ERR_ARGS. Returns NDEF_BLANK (with an empty out) for either blank
 * shape; UTF-16 text is refused as NDEF_ERR_NOT_TEXT so a misread can never
 * become a file name.
 */
int ndef_parse_text(const uint8_t* buf, size_t len, char* out, size_t out_sz);

/*
 * Compose the message above around text into buf/buf_sz, with language code
 * "en", terminated by 0xFE and zero-padded to a 4-byte page boundary so it
 * can be written page by page. Writes the padded length to *out_len. On
 * NDEF_ERR_TOO_LONG buf is left untouched.
 */
int ndef_compose_text(const char* text, uint8_t* buf, size_t buf_sz,
                      size_t* out_len);

#ifdef __cplusplus
}
#endif
