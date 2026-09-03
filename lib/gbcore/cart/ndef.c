#include "ndef.h"

#include <string.h>

/* Framing bytes a composed message adds around the text: the message TLV's
 * type and length, the record header / type length / payload length, the
 * type byte 'T', the status byte, the two language bytes and the terminator
 * TLV. Padding to a page boundary is on top of this. */
#define NDEF_FRAMING 10

/* Either factory-empty shape: the empty message written by a tag formatter,
 * or a user area that has never been written at all. */
static int is_blank(const uint8_t* buf, size_t len)
{
    size_t i;

    if (len >= 3 && buf[0] == 0x03 && buf[1] == 0x00 && buf[2] == 0xFE) {
        return 1;
    }
    for (i = 0; i < len; i++) {
        if (buf[i] != 0x00) {
            return 0;
        }
    }
    return 1;
}

int ndef_parse_text(const uint8_t* buf, size_t len, char* out, size_t out_sz)
{
    size_t pos;
    size_t mlen;
    size_t mend;
    size_t plen;
    size_t id_len;
    size_t lang_len;
    size_t text_len;
    uint8_t hdr;
    uint8_t type_len;
    uint8_t status;

    if (buf == NULL || out == NULL || out_sz == 0) {
        return NDEF_ERR_ARGS;
    }
    out[0] = '\0';
    if (len == 0) {
        return NDEF_ERR_ARGS;
    }
    if (is_blank(buf, len)) {
        return NDEF_BLANK;
    }

    /* TLV walk. NULL TLVs are padding a formatter may leave before the
     * message; anything else that is not the 0x03 message TLV means these
     * bytes are not an NDEF message at all. */
    pos = 0;
    while (pos < len && buf[pos] == 0x00) {
        pos++;
    }
    if (pos >= len || buf[pos] != 0x03) {
        return NDEF_ERR_NOT_NDEF;
    }
    pos++;
    if (pos >= len) {
        return NDEF_ERR_TRUNCATED;
    }
    if (buf[pos] == 0xFF) {
        /* Long form: 0xFF then a 2-byte big-endian length. Only a legacy
         * hand-written tag would use it — the device never composes one. */
        if (len - pos < 3) {
            return NDEF_ERR_TRUNCATED;
        }
        mlen = ((size_t)buf[pos + 1] << 8) | (size_t)buf[pos + 2];
        pos += 3;
    } else {
        mlen = buf[pos];
        pos += 1;
    }
    if (mlen > len - pos) {
        return NDEF_ERR_TRUNCATED;
    }
    mend = pos + mlen;
    /* The terminator TLV must follow the message, or the read was short and
     * the bytes past it are whatever was left on the tag. */
    if (mend >= len || buf[mend] != 0xFE) {
        return NDEF_ERR_TRUNCATED;
    }
    /* An empty message is the blank shape again, reached here only when a
     * formatter left NULL TLVs in front of it. */
    if (mlen == 0) {
        return NDEF_BLANK;
    }

    /* Exactly one record, and it must be the well-known Text type. */
    hdr = buf[pos];
    pos++;
    if ((hdr & 0xC0) != 0xC0) { /* MB and ME both set */
        return NDEF_ERR_NOT_TEXT;
    }
    if ((hdr & 0x07) != 0x01) { /* TNF well-known */
        return NDEF_ERR_NOT_TEXT;
    }
    if (pos >= mend) {
        return NDEF_ERR_TRUNCATED;
    }
    type_len = buf[pos];
    pos++;
    if (hdr & 0x10) { /* SR: one-byte payload length */
        if (pos >= mend) {
            return NDEF_ERR_TRUNCATED;
        }
        plen = buf[pos];
        pos += 1;
    } else {
        if (mend - pos < 4) {
            return NDEF_ERR_TRUNCATED;
        }
        plen = ((size_t)buf[pos] << 24) | ((size_t)buf[pos + 1] << 16) |
               ((size_t)buf[pos + 2] << 8) | (size_t)buf[pos + 3];
        pos += 4;
    }
    id_len = 0;
    if (hdr & 0x08) { /* IL: an ID field follows the type */
        if (pos >= mend) {
            return NDEF_ERR_TRUNCATED;
        }
        id_len = buf[pos];
        pos += 1;
    }
    if (type_len != 1) {
        return NDEF_ERR_NOT_TEXT;
    }
    if (pos >= mend) {
        return NDEF_ERR_TRUNCATED;
    }
    if (buf[pos] != 'T') {
        return NDEF_ERR_NOT_TEXT;
    }
    pos++;
    if (id_len > mend - pos) {
        return NDEF_ERR_TRUNCATED;
    }
    pos += id_len;
    if (plen > mend - pos) {
        return NDEF_ERR_TRUNCATED;
    }
    if (plen < 1) {
        return NDEF_ERR_NOT_TEXT; /* no status byte */
    }

    /* The status byte carries the encoding in bit 7 and the language-code
     * length in the low 6 bits — read it, never assume "en". */
    status = buf[pos];
    if (status & 0x80) {
        return NDEF_ERR_NOT_TEXT; /* UTF-16 */
    }
    lang_len = (size_t)(status & 0x3F);
    if (lang_len + 1 > plen) {
        return NDEF_ERR_TRUNCATED;
    }
    text_len = plen - 1 - lang_len;
    if (text_len + 1 > out_sz) {
        return NDEF_ERR_TOO_LONG;
    }
    memcpy(out, buf + pos + 1 + lang_len, text_len);
    out[text_len] = '\0';
    return NDEF_OK;
}

int ndef_compose_text(const char* text, uint8_t* buf, size_t buf_sz,
                      size_t* out_len)
{
    size_t text_len;
    size_t total;
    size_t padded;
    size_t i;

    if (text == NULL || buf == NULL || out_len == NULL || buf_sz == 0) {
        return NDEF_ERR_ARGS;
    }
    text_len = strlen(text);
    if (text_len > NDEF_TEXT_MAX) {
        return NDEF_ERR_TOO_LONG;
    }
    total = NDEF_FRAMING + text_len;
    padded = (total + 3u) & ~(size_t)3u;
    if (padded > buf_sz) {
        return NDEF_ERR_TOO_LONG;
    }

    i = 0;
    buf[i++] = 0x03;                        /* message TLV                 */
    buf[i++] = (uint8_t)(7 + text_len);     /* message length              */
    buf[i++] = 0xD1;                        /* MB | ME | SR | TNF well-known */
    buf[i++] = 0x01;                        /* type length                 */
    buf[i++] = (uint8_t)(3 + text_len);     /* payload length              */
    buf[i++] = 0x54;                        /* type 'T'                    */
    buf[i++] = 0x02;                        /* UTF-8, 2-byte language code */
    buf[i++] = 0x65;                        /* 'e'                         */
    buf[i++] = 0x6E;                        /* 'n'                         */
    memcpy(buf + i, text, text_len);
    i += text_len;
    buf[i++] = 0xFE;                        /* terminator TLV              */
    while (i < padded) {
        buf[i++] = 0x00;
    }

    *out_len = padded;
    return NDEF_OK;
}
