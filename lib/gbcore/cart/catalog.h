#pragma once
// Reader for the generated /catalog.txt.
//
// One entry per line, TSV, exactly four fields:
//
//   <filename>\t<title>\t<flags>\t<description>\n
//
// `flags` is a comma-separated token list — only `starter` is understood, and
// unknown tokens are ignored so the generator can add more without a firmware
// change. `description` runs to the end of the line and may be empty. The
// file is emitted from games.json and never hand-edited; the full contract is
// docs/CATALOG_FORMAT.md.
//
// The firmware never parses games.json: no JSON document, no extra flash, and
// bounded static memory on a board with no PSRAM. Descriptions are NOT held
// in the index — the index keeps each line's byte offset and the description
// is re-read from the file on demand.
//
// Bytes arrive through an injected chunk reader, the same injection style
// rom_store.c uses for flash, so every path here is host-testable without a
// filesystem.
//
// Pure C, no Arduino/ESP-IDF headers, no allocation.

#include <stdint.h>
#include <stddef.h>

#include "rom_store.h"

#ifdef __cplusplus
extern "C" {
#endif

/* The library is 132 entries; the headroom is for growth without a format
 * change. sizeof(catalog_index_t) is around 19 KB, so it belongs in the
 * writer's translation unit — never in one the emulator links at game time. */
#define CATALOG_MAX 160

/* Including the NUL. */
#define CATALOG_TITLE_MAX 48

/* 200 bytes of description plus the NUL — the contract's cap. */
#define CATALOG_DESC_MAX 201

/* 63 + 47 + "starter" + 200 + three tabs + a newline is 321; rounded up. A
 * line that reaches this length without a newline is malformed. */
#define CATALOG_LINE_MAX 384

#define CATALOG_FLAG_STARTER 0x01

enum catalog_result_e {
    CATALOG_OK = 0,
    CATALOG_ERR_ARGS = -1,  /* NULL pointer or unusable reader             */
    CATALOG_ERR_IO = -2,    /* the injected reader reported failure        */
    CATALOG_ERR_LINE = -3,  /* over-long line, too few tabs, over-long field */
    CATALOG_ERR_FULL = -4,  /* more than CATALOG_MAX entries in the file   */
    CATALOG_NOT_FOUND = -5, /* no entry matches                            */
};

/* One indexed entry. `offset` is the byte offset of the line it came from,
 * which is what catalog_read_desc() takes. */
typedef struct catalog_entry_s {
    uint32_t offset;
    char filename[ROM_STORE_NAME_MAX];
    char title[CATALOG_TITLE_MAX];
    uint8_t flags;
} catalog_entry_t;

typedef struct catalog_index_s {
    catalog_entry_t e[CATALOG_MAX];
    size_t count;
} catalog_index_t;

/*
 * Injected chunk reader. Fills up to cap bytes from byte offset off and
 * writes the count to *got. Returns 0 on success and non-zero on failure;
 * *got == 0 means end of file. A short read that is not at end of file is
 * allowed only if it still contains the rest of a line.
 */
typedef struct catalog_reader_s {
    void* ctx;
    int (*read)(void* ctx, uint32_t off, void* dst, size_t cap, size_t* got);
} catalog_reader_t;

/*
 * Parse one line (without its newline) into out. `desc` and `desc_len`, when
 * non-NULL, are set to point into `line` at the description field — they are
 * valid only as long as `line` is. out->offset is zeroed; the caller sets it,
 * since only the caller knows where the line came from.
 */
int catalog_parse_line(const char* line, size_t len, catalog_entry_t* out,
                       const char** desc, size_t* desc_len);

/*
 * Build the static index over the whole file. On CATALOG_ERR_FULL the first
 * CATALOG_MAX entries are intact and count is CATALOG_MAX.
 */
int catalog_index_build(const catalog_reader_t* rd, catalog_index_t* out);

/*
 * Read the description of the line at `offset` into out, truncated to
 * out_sz - 1 bytes and always NUL-terminated.
 */
int catalog_read_desc(const catalog_reader_t* rd, uint32_t offset, char* out,
                      size_t out_sz);

/* Find the entry whose filename matches exactly, byte for byte. */
int catalog_find(const catalog_reader_t* rd, const char* filename,
                 catalog_entry_t* out);

/* The first entry in file order carrying `flag`. */
int catalog_first_flagged(const catalog_reader_t* rd, uint8_t flag,
                          catalog_entry_t* out);

#ifdef __cplusplus
}
#endif
