#pragma once
// ROM store — the on-flash layout and write protocol for the raw ROM
// partition, expressed over an injected flash-ops struct so the whole thing
// is testable on the host against a fake NOR backend.
//
// Layout, partition-relative:
//
//   [0, erase_size)                  header sector: rom_store_hdr_t, then 0xFF
//   [erase_size, erase_size + size)  the ROM image, byte for byte
//
// The header lives alone in sector 0 so rewriting it never disturbs a data
// sector, and the ROM starts on a sector boundary so the device side can map
// it directly. ROM_STORE_DATA_OFFSET() names that start.
//
// Write protocol: begin (validate, erase) -> chunk* (stream, fold CRC) ->
// end (write the header). The header is written LAST and the erase clears it
// first, so a write torn by a reset or a power cut leaves no valid magic and
// the next boot rewrites from scratch rather than booting half a ROM.
//
// Re-writing an unchanged ROM is the thing this module exists to avoid: the
// caller reads the header, asks rom_store_matches(), and skips the whole
// erase/write cycle when the stored name and size already agree. The stored
// CRC32 is deliberately NOT part of that test — see rom_store_matches().
//
// Pure C, no Arduino/ESP-IDF headers, no allocation: every buffer is
// caller-owned, and the flash-ops implementation owns any alignment rule its
// hardware imposes on read/write offsets and lengths.

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* "GBRS" as little-endian bytes: G, B, R, S. */
#define ROM_STORE_MAGIC 0x53524247u

/* Header capacity for the ROM's file name, including the NUL. Longer names
 * are truncated — consistently on both the write and the match path, so a
 * truncated name still short-circuits on the next boot. */
#define ROM_STORE_NAME_MAX 64

enum rom_store_result_e {
    ROM_STORE_OK = 0,
    ROM_STORE_ERR_ARGS = -1,  /* NULL pointer, zero size, unusable flash ops */
    ROM_STORE_ERR_RANGE = -2, /* ROM does not fit the backend               */
    ROM_STORE_ERR_IO = -3,    /* a flash op reported failure                */
    ROM_STORE_ERR_ORDER = -4, /* protocol misuse: no begin, or short stream */
    ROM_STORE_ERR_CRC = -5,   /* stored bytes do not match the stored CRC   */
};

/*
 * Injected flash backend. Offsets are partition-relative; erase offsets and
 * lengths are always multiples of erase_size. read/write/erase return 0 on
 * success and non-zero on failure.
 */
typedef struct rom_store_flash_s {
    void* ctx;
    int (*read)(void* ctx, uint32_t off, void* dst, uint32_t len);
    int (*write)(void* ctx, uint32_t off, const void* src, uint32_t len);
    int (*erase)(void* ctx, uint32_t off, uint32_t len);
    uint32_t size;       /* usable bytes in the backend      */
    uint32_t erase_size; /* erase granularity, e.g. 4096      */
} rom_store_flash_t;

/*
 * On-flash header, at partition offset 0. Fixed-width fields in a packed
 * struct: this is a byte layout, not just a C type.
 */
typedef struct rom_store_hdr_s {
    uint32_t magic;
    uint32_t size;
    uint32_t crc32;
    char filename[ROM_STORE_NAME_MAX];
}
#ifdef __GNUC__
__attribute__((packed))
#endif
rom_store_hdr_t;

/* Where the ROM image starts, partition-relative. */
#define ROM_STORE_DATA_OFFSET(flash) ((flash)->erase_size)

/*
 * Streaming writer state. Caller-owned and opaque in practice: begin fills
 * it, chunk/end consume it. One writer per write; no global state.
 */
typedef struct rom_store_writer_s {
    const rom_store_flash_t* flash;
    uint32_t declared; /* byte count promised to write_begin */
    uint32_t written;  /* bytes accepted so far              */
    uint32_t crc;      /* running CRC32 over accepted bytes  */
    char filename[ROM_STORE_NAME_MAX];
    bool open;
} rom_store_writer_t;

/*
 * CRC32, IEEE/zlib flavour (polynomial 0xEDB88320, initial and final XOR
 * 0xFFFFFFFF). Seed the first call with 0 and feed the previous return value
 * to continue over a split buffer, exactly like zlib's crc32(). A NULL buffer
 * or zero length returns crc unchanged.
 */
uint32_t rom_store_crc32(uint32_t crc, const void* data, size_t len);

/*
 * Read the stored header. Returns false when the read fails, when the magic
 * is absent (nothing stored, or a torn write), or when the recorded size
 * cannot fit the backend. The filename is always NUL-terminated on success.
 */
bool rom_store_read_header(const rom_store_flash_t* flash,
                           rom_store_hdr_t* out);

/*
 * True when the stored header already describes this ROM: same size, same
 * file name (exact, case-sensitive, truncated the same way write_begin
 * truncates). The stored CRC is not recomputed from the source, because
 * re-reading the whole file to hash it would cost the very seconds the
 * short-circuit exists to save, and a ROM file of a given name and size is
 * immutable in this product's workflow. Use rom_store_verify() when the
 * question is "are the stored bytes intact" rather than "is this the same
 * ROM".
 */
bool rom_store_matches(const rom_store_hdr_t* hdr, const char* filename,
                       uint32_t size);

/*
 * Start a write of `size` bytes named `filename`: validates the fit, erases
 * the header sector and the data sectors the ROM will occupy, and arms the
 * writer. Nothing is erased when the call fails. Names longer than
 * ROM_STORE_NAME_MAX - 1 are truncated.
 */
int rom_store_write_begin(const rom_store_flash_t* flash,
                          rom_store_writer_t* writer, const char* filename,
                          uint32_t size);

/* Append the next `len` bytes of the ROM. Writing past the declared size is
 * ROM_STORE_ERR_RANGE; a zero length is accepted and does nothing. */
int rom_store_write_chunk(rom_store_writer_t* writer, const void* data,
                          uint32_t len);

/* Finish: requires exactly the declared byte count to have been written,
 * then commits the header. The writer is closed either way. */
int rom_store_write_end(rom_store_writer_t* writer);

/*
 * Re-read the stored ROM through `scratch` and check it against the header's
 * CRC32. Returns ROM_STORE_ERR_CRC on a mismatch, distinct from the
 * ROM_STORE_ERR_IO a failing backend produces — a corrupt image and a broken
 * flash want different diagnostics.
 */
int rom_store_verify(const rom_store_flash_t* flash,
                     const rom_store_hdr_t* hdr, void* scratch,
                     uint32_t scratch_len);

#ifdef __cplusplus
}
#endif
