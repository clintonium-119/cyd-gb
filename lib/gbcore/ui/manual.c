#include "manual.h"

#include <string.h>

#define MANUAL_DPAD_MASK \
    (COMBO_BTN_UP | COMBO_BTN_DOWN | COMBO_BTN_LEFT | COMBO_BTN_RIGHT)

#define MANUAL_TURN_MASK (COMBO_BTN_LEFT | COMBO_BTN_RIGHT)

/* Table entries read per chunk: 16 of them is 64 bytes of stack. */
#define MANUAL_TABLE_CHUNK 16

static uint16_t le16(const uint8_t* p)
{
    return (uint16_t)(p[0] | (p[1] << 8));
}

/* Exactly n bytes from off, however the reader chooses to split them. */
static int read_exact(const manual_reader_t* rd, uint32_t off, uint8_t* dst,
                      size_t n)
{
    while (n > 0) {
        size_t got = 0;
        if (rd->read(rd->ctx, off, dst, n, &got) != 0 || got == 0 ||
            got > n) {
            return MANUAL_ERR_IO;
        }
        off += (uint32_t)got;
        dst += got;
        n -= got;
    }
    return MANUAL_OK;
}

static bool usable(const manual_reader_t* rd)
{
    return rd != NULL && rd->read != NULL;
}

int manual_header(const manual_reader_t* rd, uint16_t* count)
{
    uint8_t h[MANUAL_HEADER_BYTES];
    uint16_t n;
    int rc;

    if (!usable(rd) || count == NULL) {
        return MANUAL_ERR_ARGS;
    }
    rc = read_exact(rd, 0, h, sizeof h);
    if (rc != MANUAL_OK) {
        return rc;
    }
    if (memcmp(h, MANUAL_MAGIC, 4) != 0 || le16(h + 4) != MANUAL_VERSION) {
        return MANUAL_ERR_FORMAT;
    }
    n = le16(h + 6);
    if (n == 0 || n > MANUAL_MAX_PAGES) {
        return MANUAL_ERR_FORMAT;
    }
    *count = n;
    return MANUAL_OK;
}

int manual_table(const manual_reader_t* rd, uint32_t file_size,
                 uint16_t win_w, uint16_t win_h, manual_page_t* pages,
                 uint16_t count)
{
    uint8_t buf[MANUAL_TABLE_CHUNK * MANUAL_ENTRY_BYTES];
    uint32_t table_end;
    uint64_t off;
    uint16_t i = 0;

    if (!usable(rd) || pages == NULL || count == 0 || win_w == 0 ||
        win_h == 0) {
        return MANUAL_ERR_ARGS;
    }
    table_end = MANUAL_HEADER_BYTES + (uint32_t)count * MANUAL_ENTRY_BYTES;
    if (file_size < table_end) {
        return MANUAL_ERR_SIZE;
    }

    /* 64-bit so a hostile table cannot wrap the running offset back into
     * range and pass the size check. */
    off = table_end;
    while (i < count) {
        uint16_t n = (uint16_t)(count - i);
        uint16_t k;
        int rc;

        if (n > MANUAL_TABLE_CHUNK) {
            n = MANUAL_TABLE_CHUNK;
        }
        rc = read_exact(rd,
                        MANUAL_HEADER_BYTES +
                            (uint32_t)i * MANUAL_ENTRY_BYTES,
                        buf, (size_t)n * MANUAL_ENTRY_BYTES);
        if (rc != MANUAL_OK) {
            return rc;
        }
        for (k = 0; k < n; k++, i++) {
            uint16_t w = le16(buf + k * MANUAL_ENTRY_BYTES);
            uint16_t h = le16(buf + k * MANUAL_ENTRY_BYTES + 2);

            if (w == 0 || h == 0 || (uint32_t)w > 2u * win_w ||
                (uint32_t)h > 2u * win_h) {
                return MANUAL_ERR_FORMAT;
            }
            if (off > file_size) {
                return MANUAL_ERR_SIZE;
            }
            pages[i].w = w;
            pages[i].h = h;
            pages[i].offset = (uint32_t)off;
            off += (uint64_t)((w + 7u) / 8u) * h;
        }
    }
    return (off == file_size) ? MANUAL_OK : MANUAL_ERR_SIZE;
}

static uint8_t tiles_along(uint16_t len, uint16_t win)
{
    return (uint8_t)((len + win - 1u) / win);
}

static uint16_t origin_along(uint16_t len, uint16_t win, uint8_t t)
{
    uint32_t o = (uint32_t)t * win;

    if (len <= win) {
        return 0;
    }
    if (o > (uint32_t)(len - win)) {
        o = (uint32_t)(len - win);
    }
    return (uint16_t)o;
}

void manual_tiles(const manual_page_t* page, uint16_t win_w, uint16_t win_h,
                  uint8_t* nx, uint8_t* ny)
{
    if (page == NULL || win_w == 0 || win_h == 0) {
        *nx = 1;
        *ny = 1;
        return;
    }
    *nx = tiles_along(page->w, win_w);
    *ny = tiles_along(page->h, win_h);
}

void manual_tile_origin(const manual_page_t* page, uint16_t win_w,
                        uint16_t win_h, uint8_t tx, uint8_t ty,
                        uint16_t* x0, uint16_t* y0)
{
    if (page == NULL) {
        *x0 = 0;
        *y0 = 0;
        return;
    }
    *x0 = origin_along(page->w, win_w, tx);
    *y0 = origin_along(page->h, win_h, ty);
}

void manual_nav_init(manual_nav_t* nav, uint16_t count)
{
    if (nav == NULL) {
        return;
    }
    nav->count = count;
    nav->page = 0;
    nav->tx = 0;
    nav->ty = 0;
    nav->overview = false;
    nav->prev_buttons = 0xFF;
    nav->held_dir = 0;
    nav->repeat_due_ms = 0;
}

/* To the next page's first tile. False at the last page: the end clamps. */
static bool next_page(manual_nav_t* nav)
{
    if ((uint16_t)(nav->page + 1) >= nav->count) {
        return false;
    }
    nav->page++;
    nav->tx = 0;
    nav->ty = 0;
    return true;
}

/* To the previous page, at its last tile or its first. */
static bool prev_page(manual_nav_t* nav, const manual_page_t* pages,
                      uint16_t win_w, uint16_t win_h, bool last_tile)
{
    uint8_t nx;
    uint8_t ny;

    if (nav->page == 0) {
        return false;
    }
    nav->page--;
    nav->tx = 0;
    nav->ty = 0;
    if (last_tile) {
        manual_tiles(&pages[nav->page], win_w, win_h, &nx, &ny);
        nav->tx = (uint8_t)(nx - 1);
        nav->ty = (uint8_t)(ny - 1);
    }
    return true;
}

static bool single_bit(uint8_t v)
{
    return v != 0 && (uint8_t)(v & (uint8_t)(v - 1)) == 0;
}

/* One step of the tile walk in the reading view. */
static bool walk(manual_nav_t* nav, const manual_page_t* pages,
                 uint16_t win_w, uint16_t win_h, uint8_t dir)
{
    uint8_t nx;
    uint8_t ny;

    manual_tiles(&pages[nav->page], win_w, win_h, &nx, &ny);
    switch (dir) {
    case COMBO_BTN_RIGHT:
        if ((uint8_t)(nav->tx + 1) < nx) {
            nav->tx++;
            return true;
        }
        if ((uint8_t)(nav->ty + 1) < ny) {
            nav->tx = 0;
            nav->ty++;
            return true;
        }
        return next_page(nav);
    case COMBO_BTN_LEFT:
        if (nav->tx > 0) {
            nav->tx--;
            return true;
        }
        if (nav->ty > 0) {
            nav->tx = (uint8_t)(nx - 1);
            nav->ty--;
            return true;
        }
        return prev_page(nav, pages, win_w, win_h, true);
    case COMBO_BTN_DOWN:
        if ((uint8_t)(nav->ty + 1) < ny) {
            nav->ty++;
            return true;
        }
        return next_page(nav);
    case COMBO_BTN_UP:
        if (nav->ty > 0) {
            nav->ty--;
            return true;
        }
        return prev_page(nav, pages, win_w, win_h, true);
    default:
        return false;
    }
}

/*
 * Left and Right in the overview: a fresh press turns at once, then waits
 * COMBO_REPEAT_DELAY_MS before turning every COMBO_REPEAT_MS while held. Only
 * an edge arms the repeat, so a direction already down when the overview
 * opened does nothing until pressed again.
 */
static bool turn(manual_nav_t* nav, const manual_page_t* pages,
                 uint16_t win_w, uint16_t win_h, uint8_t buttons,
                 uint8_t pressed, uint32_t now_ms)
{
    uint8_t dir = (uint8_t)(buttons & MANUAL_TURN_MASK);

    if (!single_bit(dir)) {
        nav->held_dir = 0;
        return false;
    }
    if (pressed & dir) {
        nav->held_dir = dir;
        nav->repeat_due_ms = now_ms + (uint32_t)COMBO_REPEAT_DELAY_MS;
    } else if (dir == nav->held_dir &&
               (int32_t)(now_ms - nav->repeat_due_ms) >= 0) {
        /* Off now_ms, like the list machine: a slow redraw costs one late
         * turn rather than a burst of catch-up turns. */
        nav->repeat_due_ms = now_ms + (uint32_t)COMBO_REPEAT_MS;
    } else {
        return false;
    }
    return (dir == COMBO_BTN_RIGHT)
               ? next_page(nav)
               : prev_page(nav, pages, win_w, win_h, false);
}

uint8_t manual_nav_input(manual_nav_t* nav, const manual_page_t* pages,
                         uint16_t win_w, uint16_t win_h, uint8_t buttons,
                         uint32_t now_ms)
{
    uint8_t pressed;
    uint8_t dir;

    if (nav == NULL || pages == NULL || nav->count == 0 || win_w == 0 ||
        win_h == 0) {
        return MANUAL_EVENT_NONE;
    }
    pressed = (uint8_t)(buttons & ~nav->prev_buttons);
    nav->prev_buttons = buttons;

    if (pressed & COMBO_BTN_B) {
        return MANUAL_EVENT_EXIT;
    }
    if (pressed & COMBO_BTN_A) {
        nav->overview = !nav->overview;
        nav->held_dir = 0;
        return MANUAL_EVENT_REDRAW;
    }
    if (nav->overview) {
        return turn(nav, pages, win_w, win_h, buttons, pressed, now_ms)
                   ? MANUAL_EVENT_REDRAW
                   : MANUAL_EVENT_NONE;
    }
    dir = (uint8_t)(pressed & MANUAL_DPAD_MASK);
    if (!single_bit(dir)) {
        return MANUAL_EVENT_NONE;
    }
    return walk(nav, pages, win_w, win_h, dir) ? MANUAL_EVENT_REDRAW
                                               : MANUAL_EVENT_NONE;
}

static bool bit_at(const uint8_t* row, uint32_t x)
{
    return (row[x >> 3] & (0x80u >> (x & 7u))) != 0;
}

void manual_expand_row(const uint8_t* bits, uint16_t x0, uint16_t n,
                       uint16_t ink, uint16_t paper, uint16_t* out)
{
    uint16_t i;

    if (bits == NULL || out == NULL) {
        return;
    }
    for (i = 0; i < n; i++) {
        out[i] = bit_at(bits, (uint32_t)x0 + i) ? ink : paper;
    }
}

void manual_decimate_row(const uint8_t* a, const uint8_t* b, uint16_t w,
                         uint8_t* out)
{
    uint16_t half = (uint16_t)((w + 1u) / 2u);
    uint16_t i;

    if (a == NULL || out == NULL) {
        return;
    }
    memset(out, 0, (half + 7u) / 8u);
    for (i = 0; i < half; i++) {
        uint32_t x = (uint32_t)i * 2u;
        /* The odd column past the right edge is padding, not page. */
        bool two = x + 1u < w;
        bool black = bit_at(a, x) || (two && bit_at(a, x + 1u));

        if (!black && b != NULL) {
            black = bit_at(b, x) || (two && bit_at(b, x + 1u));
        }
        if (black) {
            out[i >> 3] |= (uint8_t)(0x80u >> (i & 7u));
        }
    }
}
