#include "rom_store.h"

#include <string.h>

/* The header is a byte layout: 4 + 4 + 4 + 64. If a compiler ever pads it,
 * fail the build rather than write an unreadable partition. */
typedef char rom_store_hdr_layout_check[(sizeof(rom_store_hdr_t) == 76) ? 1 : -1];

static bool flash_usable(const rom_store_flash_t* flash)
{
    return flash != NULL && flash->read != NULL && flash->write != NULL &&
           flash->erase != NULL && flash->erase_size > 0 &&
           flash->size >= flash->erase_size;
}

/* Round `n` up to the next multiple of `unit`, saturating at UINT32_MAX so an
 * absurd size is caught by the range check instead of wrapping to a small
 * one. `unit` is never zero here — flash_usable() has already run. */
static uint32_t round_up(uint32_t n, uint32_t unit)
{
    uint32_t rem = n % unit;
    if (rem == 0) {
        return n;
    }
    if (n > UINT32_MAX - (unit - rem)) {
        return UINT32_MAX;
    }
    return n + (unit - rem);
}

/* Copy a file name into a ROM_STORE_NAME_MAX field, truncating and always
 * NUL-terminating. Used by both the write and the match path so the two
 * agree on what a too-long name becomes. */
static void copy_name(char* dst, const char* src)
{
    size_t i;
    for (i = 0; i + 1 < ROM_STORE_NAME_MAX && src[i] != '\0'; i++) {
        dst[i] = src[i];
    }
    memset(dst + i, 0, ROM_STORE_NAME_MAX - i);
}

uint32_t rom_store_crc32(uint32_t crc, const void* data, size_t len)
{
    const uint8_t* p = (const uint8_t*)data;
    uint32_t c;
    size_t i;
    int bit;

    if (p == NULL || len == 0) {
        return crc;
    }
    /* Bitwise rather than table-driven: eight shifts per byte costs a few
     * tens of milliseconds over a whole ROM, paid once per write or verify
     * at boot, and it keeps a 1 KB constant table out of a 640 KB image. */
    c = crc ^ 0xFFFFFFFFu;
    for (i = 0; i < len; i++) {
        c ^= p[i];
        for (bit = 0; bit < 8; bit++) {
            c = (c >> 1) ^ (0xEDB88320u & (uint32_t)(-(int32_t)(c & 1u)));
        }
    }
    return c ^ 0xFFFFFFFFu;
}

bool rom_store_read_header(const rom_store_flash_t* flash,
                           rom_store_hdr_t* out)
{
    uint32_t room;

    if (!flash_usable(flash) || out == NULL) {
        return false;
    }
    if (flash->read(flash->ctx, 0, out, (uint32_t)sizeof(*out)) != 0) {
        return false;
    }
    if (out->magic != ROM_STORE_MAGIC) {
        return false;
    }
    /* A header claiming more bytes than the backend holds is corrupt, not a
     * store worth trusting. */
    room = flash->size - flash->erase_size;
    if (out->size == 0 || out->size > room) {
        return false;
    }
    out->filename[ROM_STORE_NAME_MAX - 1] = '\0';
    return true;
}

bool rom_store_matches(const rom_store_hdr_t* hdr, const char* filename,
                       uint32_t size)
{
    char want[ROM_STORE_NAME_MAX];

    if (hdr == NULL || filename == NULL) {
        return false;
    }
    if (hdr->magic != ROM_STORE_MAGIC || hdr->size != size) {
        return false;
    }
    copy_name(want, filename);
    return memcmp(hdr->filename, want, ROM_STORE_NAME_MAX) == 0;
}

int rom_store_write_begin(const rom_store_flash_t* flash,
                          rom_store_writer_t* writer, const char* filename,
                          uint32_t size)
{
    uint32_t erase_len;

    if (!flash_usable(flash) || writer == NULL || filename == NULL ||
        size == 0) {
        return ROM_STORE_ERR_ARGS;
    }
    /* Header sector plus the data sectors the ROM occupies, whole sectors
     * either way — the one erase covers both, and clearing the header first
     * is what makes a torn write detectable. */
    erase_len = round_up(size, flash->erase_size);
    if (erase_len > flash->size - flash->erase_size) {
        return ROM_STORE_ERR_RANGE;
    }
    erase_len += flash->erase_size;

    memset(writer, 0, sizeof(*writer));
    if (flash->erase(flash->ctx, 0, erase_len) != 0) {
        return ROM_STORE_ERR_IO;
    }
    writer->flash = flash;
    writer->declared = size;
    writer->crc = 0; /* zlib's crc32() seed */
    copy_name(writer->filename, filename);
    writer->open = true;
    return ROM_STORE_OK;
}

int rom_store_write_chunk(rom_store_writer_t* writer, const void* data,
                          uint32_t len)
{
    uint32_t off;

    if (writer == NULL || !writer->open) {
        return ROM_STORE_ERR_ORDER;
    }
    if (len == 0) {
        return ROM_STORE_OK;
    }
    if (data == NULL) {
        return ROM_STORE_ERR_ARGS;
    }
    if (len > writer->declared - writer->written) {
        return ROM_STORE_ERR_RANGE;
    }
    off = ROM_STORE_DATA_OFFSET(writer->flash) + writer->written;
    if (writer->flash->write(writer->flash->ctx, off, data, len) != 0) {
        return ROM_STORE_ERR_IO;
    }
    writer->crc = rom_store_crc32(writer->crc, data, len);
    writer->written += len;
    return ROM_STORE_OK;
}

int rom_store_write_end(rom_store_writer_t* writer)
{
    rom_store_hdr_t hdr;
    int rc;

    if (writer == NULL || !writer->open) {
        return ROM_STORE_ERR_ORDER;
    }
    writer->open = false;
    if (writer->written != writer->declared) {
        return ROM_STORE_ERR_ORDER;
    }

    memset(&hdr, 0, sizeof(hdr));
    hdr.magic = ROM_STORE_MAGIC;
    hdr.size = writer->declared;
    hdr.crc32 = writer->crc;
    memcpy(hdr.filename, writer->filename, ROM_STORE_NAME_MAX);

    rc = writer->flash->write(writer->flash->ctx, 0, &hdr,
                              (uint32_t)sizeof(hdr));
    return (rc == 0) ? ROM_STORE_OK : ROM_STORE_ERR_IO;
}

int rom_store_verify(const rom_store_flash_t* flash,
                     const rom_store_hdr_t* hdr, void* scratch,
                     uint32_t scratch_len)
{
    uint32_t crc;
    uint32_t done;

    if (!flash_usable(flash) || hdr == NULL || scratch == NULL ||
        scratch_len == 0) {
        return ROM_STORE_ERR_ARGS;
    }
    if (hdr->magic != ROM_STORE_MAGIC) {
        return ROM_STORE_ERR_ARGS;
    }
    if (hdr->size == 0 || hdr->size > flash->size - flash->erase_size) {
        return ROM_STORE_ERR_RANGE;
    }

    crc = 0; /* zlib's crc32() seed */
    done = 0;
    while (done < hdr->size) {
        uint32_t n = hdr->size - done;
        if (n > scratch_len) {
            n = scratch_len;
        }
        if (flash->read(flash->ctx, ROM_STORE_DATA_OFFSET(flash) + done,
                        scratch, n) != 0) {
            return ROM_STORE_ERR_IO;
        }
        crc = rom_store_crc32(crc, scratch, n);
        done += n;
    }
    return (crc == hdr->crc32) ? ROM_STORE_OK : ROM_STORE_ERR_CRC;
}
