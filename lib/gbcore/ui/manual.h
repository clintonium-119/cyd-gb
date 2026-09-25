#pragma once
// Game manual reader: the .2bp format, tile geometry and navigation.
//
// A manual is one file per game, at two bits per pixel, each page cut into
// bands of MANUAL_BAND_ROWS rows and each band compressed on its own as a raw
// LZ4 block:
//
//   "GBMN"  u16 version  u16 page count          8-byte header
//   u16 width  u16 height                        one per page
//   u32 band offsets                             ceil(h / 16) + 1 per page,
//                                                every page's, in page order
//   LZ4 blocks                                   every band of every page
//
// Integers are little-endian. Offsets are absolute: band i of a page is the
// block from its offset i to its offset i + 1, a page's last offset is the
// next page's first, and the last page's last offset is the file size — a
// file of any other size is refused, the way a .565 file is. A band decodes
// to exactly rows x ceil(w / 4) bytes, rows being MANUAL_BAND_ROWS but for a
// short last band. Pixels are MSB-first, two bits each, 0 white, 1 light
// grey, 2 dark grey and 3 black, and a row's pad bits are zero. The encoder
// is tools/image_sd.py's encode_manual().
//
// A page is shown a window-sized tile at a time. Every stored page fits in
// twice the window each way, so a page is at most 2 x 2 tiles, and the last
// tile in each direction is clamped to the page's edge rather than running
// off it. The window size is always a parameter: this module does not know
// how big the game window is.
//
// Everything that needs neither a panel nor a card lives here, so it is
// host-testable: parsing and bounding the header and tables, decoding one
// band, the tile geometry, the navigation machine, and the two row operations
// the screen needs — expanding levels to RGB565 and halving two rows into one
// for the overview. Bytes arrive through an injected reader, catalog-style.
//
// Pure C, no Arduino/ESP-IDF headers, no allocation.

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#include "input/combo.h"

#ifdef __cplusplus
extern "C" {
#endif

#define MANUAL_MAGIC "GBMN"
#define MANUAL_VERSION 2
#define MANUAL_HEADER_BYTES 8

/* Bytes per page in the size table: a u16 width and a u16 height. */
#define MANUAL_ENTRY_BYTES 4

/* Page rows per compressed band. Even, so an overview's row pairs never
 * straddle two bands. */
#define MANUAL_BAND_ROWS 16

/* Bytes per band offset. */
#define MANUAL_OFFSET_BYTES 4

/* A bound on the caller's page-table allocation. The longest manual is 80
 * pages before its spreads are split, so this is ample. */
#define MANUAL_MAX_PAGES 1024

enum manual_result_e {
    MANUAL_OK = 0,
    MANUAL_ERR_ARGS = -1,   /* NULL pointer, unusable reader, zero window    */
    MANUAL_ERR_IO = -2,     /* the injected reader failed or ran out early   */
    MANUAL_ERR_FORMAT = -3, /* magic, version, page count, a page outside
                               twice the window, or a band that will not
                               decode to its size                            */
    MANUAL_ERR_SIZE = -4,   /* the file is not exactly what its table implies */
};

enum manual_event_e {
    MANUAL_EVENT_NONE = 0,
    MANUAL_EVENT_REDRAW, /* the page, the tile or the view changed          */
    MANUAL_EVENT_EXIT,   /* B: back to the menu                             */
};

/* One page: its size in pixels and where its band table starts in the
 * file. */
typedef struct manual_page_s {
    uint16_t w;
    uint16_t h;
    uint32_t offset;
} manual_page_t;

/*
 * Injected chunk reader, the same shape as catalog_reader_t. Fills up to cap
 * bytes from byte offset off and writes the count to *got. Returns 0 on
 * success and non-zero on failure; *got == 0 means end of file.
 */
typedef struct manual_reader_s {
    void* ctx;
    int (*read)(void* ctx, uint32_t off, void* dst, size_t cap, size_t* got);
} manual_reader_t;

/*
 * Where the reader is. The fields are the screen's to read and the machine's
 * to write: which page, which tile of it, and whether the overview is up.
 * The tile is kept while the overview is up, so A returns to it; a page turn
 * in the overview resets it to the first tile.
 */
typedef struct manual_nav_s {
    uint16_t count;          /* pages in the manual                       */
    uint16_t page;           /* current page, 0 .. count - 1              */
    uint8_t tx;              /* current tile column                       */
    uint8_t ty;              /* current tile row                          */
    bool overview;           /* the whole page, halved, is showing        */
    uint8_t prev_buttons;    /* last sample, for edge detection           */
    uint8_t held_dir;        /* overview page-turn direction being held   */
    uint32_t repeat_due_ms;  /* when that direction's next turn is due    */
} manual_nav_t;

/*
 * Read the header: check the magic and version and return the page count in
 * *count. MANUAL_ERR_FORMAT for a bad magic or version, a count of zero or
 * one over MANUAL_MAX_PAGES — which is what makes *count safe to size an
 * allocation from.
 */
int manual_header(const manual_reader_t* rd, uint16_t* count);

/*
 * Fill a caller-owned array of `count` pages from the size table, computing
 * where each page's band table starts. MANUAL_ERR_FORMAT for a page of zero
 * size, wider than 2 x win_w or taller than 2 x win_h; MANUAL_ERR_SIZE when
 * the tables do not fit in file_size or the last page's last band offset is
 * not file_size.
 */
int manual_table(const manual_reader_t* rd, uint32_t file_size,
                 uint16_t win_w, uint16_t win_h, manual_page_t* pages,
                 uint16_t count);

/*
 * Decode one band of a page into out: its block is read into scratch, then
 * decompressed into exactly rows x ceil(w / 4) bytes of out. MANUAL_ERR_ARGS
 * for a band past the page or an out too small for it; MANUAL_ERR_FORMAT for
 * offsets that decrease or run past file_size, a block bigger than
 * scratch_cap, or a block that does not decode to exactly the band;
 * MANUAL_ERR_IO when the reader fails. Nothing is written to scratch or out
 * before the offsets have passed.
 */
int manual_band(const manual_reader_t* rd, uint32_t file_size,
                const manual_page_t* page, uint16_t band, uint8_t* scratch,
                size_t scratch_cap, uint8_t* out, size_t out_cap);

/*
 * Decode one raw LZ4 block of n bytes into dst, and return the byte count, or
 * -1 for truncated input, a zero offset, an offset before dst, output past
 * cap, or a block that ends anywhere but after a sequence's literals. Never
 * reads past n or writes past cap.
 */
int manual_lz4_decode(const uint8_t* src, size_t n, uint8_t* dst, size_t cap);

/* How many window-sized tiles the page is across and down. */
void manual_tiles(const manual_page_t* page, uint16_t win_w, uint16_t win_h,
                  uint8_t* nx, uint8_t* ny);

/*
 * The page pixel at the top-left of tile (tx, ty): tx x win_w, clamped so the
 * tile ends on the page's right edge, and 0 when the page is narrower than
 * the window. The same down the page.
 */
void manual_tile_origin(const manual_page_t* page, uint16_t win_w,
                        uint16_t win_h, uint8_t tx, uint8_t ty,
                        uint16_t* x0, uint16_t* y0);

/*
 * Start on the first page's overview, so the whole page is seen before any
 * of it is zoomed into; A then reads it from the first tile. Every button
 * counts as already held, so the A that opened the reader does nothing until
 * it has been released and pressed again.
 */
void manual_nav_init(manual_nav_t* nav, uint16_t count);

/*
 * One button sample, in COMBO_BTN_* bits, taken at now_ms.
 *
 * Reading: Right steps to the next tile in reading order, wrapping to the
 * next tile row, and past the last tile turns to the next page's first tile;
 * Left is its exact inverse, turning to the previous page's last tile. Down
 * steps down a tile row and past the bottom turns to the next page's first
 * tile; Up steps up and past the top turns to the previous page's last tile.
 * A shows the overview.
 *
 * Overview: Left and Right turn pages, repeating while held at the combo
 * module's cadence. A returns to reading.
 *
 * B exits from either. Nothing wraps round the ends of the manual. Two
 * directions pressed at once do nothing.
 */
uint8_t manual_nav_input(manual_nav_t* nav, const manual_page_t* pages,
                         uint16_t win_w, uint16_t win_h, uint8_t buttons,
                         uint32_t now_ms);

/*
 * Write n RGB565 pixels from a 2 bpp row, starting at pixel x0: each pixel is
 * levels[its level]. Byte order is the caller's — the levels are copied as
 * given.
 */
void manual_expand_row(const uint8_t* row, uint16_t x0, uint16_t n,
                       const uint16_t levels[4], uint16_t* out);

/*
 * Halve two 2 bpp rows of width w into one of ceil(w / 2) pixels, packed and
 * byte-padded the same way. An output pixel takes the mean level of its
 * 2 x 2 cell, rounded half up: with four greys to land on, the mean keeps
 * text thin where the darkest level would run its letters together. b may be
 * NULL for an odd last row.
 */
void manual_decimate_row(const uint8_t* a, const uint8_t* b, uint16_t w,
                         uint8_t* out);

#ifdef __cplusplus
}
#endif
