#include "manual_view.h"
#include "display.h"
#include "button_input.h"
#include "render_config.h"
#include "sd_manager.h"
#include "ui/manual.h"
#include <Arduino.h>
#include <stdlib.h>

// The game's scanned manual, in the game window. Everything that can be
// decided without a panel or a card — the format, the tile walk, the row
// operations — is the pure module in gbcore; this is the file, the buffers
// and the drawing.

// The in-game menu's poll period, for the same reason: longer than the input
// module's debounce, so successive samples are already stable.
#define MANUAL_POLL_MS 16

// Page rows read from the card per File::read. A stored page is at most
// 2 x GAME_W wide, so the band is at most 16 x 67 = 1,072 bytes. Even, so an
// overview's row pairs never straddle two bands.
#define MANUAL_BAND_ROWS 16
#define MANUAL_MAX_STRIDE ((2 * GAME_W + 7) / 8)

// The overview's footer strip, sized and placed like Cart Info's so the
// navigation hints read the same on both. A page drawn the full window tall
// loses its bottom 18 rows under it; most pages are shorter at half size and
// clear it.
#define MANUAL_FOOT_H 18

// Black ink on white paper, not the emulator palette. Both values read the
// same in either byte order, so the resting setSwapBytes(true) is moot.
#define MANUAL_INK   0x0000
#define MANUAL_PAPER 0xFFFF

// Everything one open reader holds, all of it heap and all of it freed on
// the way out.
typedef struct view_s {
    const settings_t* s;
    manual_reader_t rd;
    manual_page_t* pages;
    uint16_t count;
    uint8_t* band;   // MANUAL_BAND_ROWS page rows, packed
    uint8_t* half;   // one decimated row, packed
    uint16_t* row;   // one GAME_W-pixel RGB565 row
} view_t;

// Every button up, then a beat: the B that closed the reader must not also
// act on the menu underneath.
static void wait_release()
{
    button_update();
    while (button_get_buttons()) {
        button_update();
        delay(10);
    }
    delay(100);
}

// Exactly n bytes at off, however the card splits them.
static bool read_exact(const view_t* v, uint32_t off, uint8_t* dst, size_t n)
{
    while (n > 0) {
        size_t got = 0;

        if (v->rd.read(v->rd.ctx, off, dst, n, &got) != 0 || got == 0) {
            return false;
        }
        off += (uint32_t)got;
        dst += got;
        n -= got;
    }
    return true;
}

// The tile the nav machine is on, full size. A page narrower or shorter than
// the window is centred in it on black.
static bool draw_tile(const view_t* v, const manual_nav_t* nav)
{
    const manual_page_t* p = &v->pages[nav->page];
    const uint32_t stride = (p->w + 7u) / 8u;
    const uint16_t vw = p->w < GAME_W ? p->w : GAME_W;
    const uint16_t vh = p->h < GAME_H ? p->h : GAME_H;
    const int16_t x = (int16_t)(v->s->game_x + (GAME_W - vw) / 2);
    const int16_t y = (int16_t)(v->s->game_y + (GAME_H - vh) / 2);
    uint16_t x0;
    uint16_t y0;

    manual_tile_origin(p, GAME_W, GAME_H, nav->tx, nav->ty, &x0, &y0);
    if (vw < GAME_W || vh < GAME_H) {
        tft.fillRect(v->s->game_x, v->s->game_y, GAME_W, GAME_H, TFT_BLACK);
    }
    for (uint16_t r = 0; r < vh; r += MANUAL_BAND_ROWS) {
        uint16_t n = (uint16_t)(vh - r);

        if (n > MANUAL_BAND_ROWS) {
            n = MANUAL_BAND_ROWS;
        }
        if (!read_exact(v, p->offset + (uint32_t)(y0 + r) * stride, v->band,
                        (size_t)n * stride)) {
            return false;
        }
        for (uint16_t i = 0; i < n; i++) {
            manual_expand_row(v->band + i * stride, x0, vw, MANUAL_INK,
                              MANUAL_PAPER, v->row);
            tft.pushImage(x, (int16_t)(y + r + i), vw, 1, v->row);
        }
    }
    return true;
}

// The whole page at half size, centred, over a footer of hints and the
// page number.
// Two page rows in, one screen row out: no page-sized buffer anywhere.
static bool draw_overview(const view_t* v, const manual_nav_t* nav)
{
    const manual_page_t* p = &v->pages[nav->page];
    const uint32_t stride = (p->w + 7u) / 8u;
    const uint16_t hw = (uint16_t)((p->w + 1u) / 2u);
    const uint16_t hh = (uint16_t)((p->h + 1u) / 2u);
    const int16_t x = (int16_t)(v->s->game_x + (GAME_W - hw) / 2);
    const int16_t y = (int16_t)(v->s->game_y + (GAME_H - hh) / 2);
    char label[12];

    tft.fillRect(v->s->game_x, v->s->game_y, GAME_W, GAME_H, TFT_BLACK);
    for (uint16_t r = 0; r < p->h; r += MANUAL_BAND_ROWS) {
        uint16_t n = (uint16_t)(p->h - r);

        if (n > MANUAL_BAND_ROWS) {
            n = MANUAL_BAND_ROWS;
        }
        if (!read_exact(v, p->offset + (uint32_t)r * stride, v->band,
                        (size_t)n * stride)) {
            return false;
        }
        for (uint16_t i = 0; i < n; i += 2) {
            const uint8_t* b = (i + 1 < n) ? v->band + (i + 1) * stride : NULL;

            manual_decimate_row(v->band + i * stride, b, p->w, v->half);
            manual_expand_row(v->half, 0, hw, MANUAL_INK, MANUAL_PAPER,
                              v->row);
            tft.pushImage(x, (int16_t)(y + (r + i) / 2), hw, 1, v->row);
        }
    }

    snprintf(label, sizeof(label), "%u/%u", (unsigned)(nav->page + 1),
             (unsigned)v->count);
    const int16_t fy = (int16_t)(v->s->game_y + GAME_H - MANUAL_FOOT_H);
    tft.fillRect(v->s->game_x, fy, GAME_W, MANUAL_FOOT_H, TFT_BLACK);
    tft.setTextColor(TFT_WHITE, TFT_BLACK);
    tft.setTextDatum(TL_DATUM);
    tft.drawString("A: Zoom  B: Back", v->s->game_x + 8, fy, 2);
    tft.setTextDatum(TR_DATUM);
    tft.drawString(label, v->s->game_x + GAME_W - 8, fy, 2);
    return true;
}

// One repaint and its log line, which the bench reads for timing.
static bool draw(const view_t* v, const manual_nav_t* nav)
{
    uint32_t t0 = micros();
    bool ok = nav->overview ? draw_overview(v, nav) : draw_tile(v, nav);

    Serial.printf("[MANUAL] p=%u/%u t=%u,%u ov=%u %lu us%s\n",
                  (unsigned)(nav->page + 1), (unsigned)v->count,
                  (unsigned)nav->tx, (unsigned)nav->ty,
                  (unsigned)nav->overview, (unsigned long)(micros() - t0),
                  ok ? "" : " read failed");
    return ok;
}

// Bind, validate and allocate. False with one log line on any refusal.
static bool view_setup(view_t* v, const char* rom_filename)
{
    uint32_t size = 0;
    int rc;

    if (!sd_manual_reader(rom_filename, &v->rd, &size)) {
        Serial.printf("[MANUAL] no manual for %s\n", rom_filename);
        return false;
    }
    rc = manual_header(&v->rd, &v->count);
    if (rc != MANUAL_OK) {
        Serial.printf("[MANUAL] refused header: %d\n", rc);
        return false;
    }
    v->pages = (manual_page_t*)malloc(v->count * sizeof(manual_page_t));
    v->band = (uint8_t*)malloc(MANUAL_BAND_ROWS * MANUAL_MAX_STRIDE);
    v->half = (uint8_t*)malloc((GAME_W + 7) / 8);
    v->row = (uint16_t*)malloc(GAME_W * sizeof(uint16_t));
    if (!v->pages || !v->band || !v->half || !v->row) {
        Serial.printf("[MANUAL] no memory for %u pages\n",
                      (unsigned)v->count);
        return false;
    }
    rc = manual_table(&v->rd, size, GAME_W, GAME_H, v->pages, v->count);
    if (rc != MANUAL_OK) {
        Serial.printf("[MANUAL] refused table: %d\n", rc);
        return false;
    }
    return true;
}

bool manual_view_open(const settings_t* s, const char* rom_filename)
{
    view_t v = {};
    manual_nav_t nav;
    bool opened = false;

    v.s = s;
    if (s && rom_filename && view_setup(&v, rom_filename)) {
        opened = true;
        manual_nav_init(&nav, v.count);
        if (draw(&v, &nav)) {
            for (;;) {
                uint8_t ev;

                button_update();
                ev = manual_nav_input(&nav, v.pages, GAME_W, GAME_H,
                                      (uint8_t)button_get_buttons(), millis());
                if (ev == MANUAL_EVENT_EXIT) {
                    break;
                }
                // A read that fails mid-manual means the card went away or
                // the file changed under us; leave rather than keep drawing.
                if (ev == MANUAL_EVENT_REDRAW && !draw(&v, &nav)) {
                    break;
                }
                delay(MANUAL_POLL_MS);
            }
        }
        wait_release();
    }
    free(v.pages);
    free(v.band);
    free(v.half);
    free(v.row);
    sd_manual_close();
    return opened;
}
