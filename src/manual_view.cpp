#include "manual_view.h"
#include "display.h"
#include "button_input.h"
#include "render_config.h"
#include "sd_manager.h"
#include "ui/manual.h"
#include "ui/theme_draw.h"
#include <Arduino.h>
#include <stdlib.h>

// The game's scanned manual, in the game window. Everything that can be
// decided without a panel or a card — the format, the tile walk, the row
// operations — is the pure module in gbcore; this is the file, the buffers
// and the drawing.

// The in-game menu's poll period, for the same reason: longer than the input
// module's debounce, so successive samples are already stable.
#define MANUAL_POLL_MS 16

// A stored page is at most 2 x GAME_W wide, so a decoded band of
// MANUAL_BAND_ROWS rows is at most 16 x 133 = 2,128 bytes, and its compressed
// block at most LZ4's worst case for that many bytes, 2,152.
#define MANUAL_MAX_STRIDE ((2 * GAME_W + 3) / 4)
#define MANUAL_BAND_BYTES (MANUAL_BAND_ROWS * MANUAL_MAX_STRIDE)
#define MANUAL_BLOCK_BYTES (MANUAL_BAND_BYTES + MANUAL_BAND_BYTES / 255 + 16)

// The overview's chrome is the theme's: a header naming the page, the help
// line and the hint footer, when the half-size page fits between them. A
// portrait page is 240 rows at half size and does not, so it keeps the whole
// window and only the footer goes over its bottom rows, with the page number
// at the footer's left.
#define MANUAL_ROOM_H (GAME_H - UI_HEADER_H - UI_HELP_H - UI_FOOT_H)

// The four stored levels, white paper to black ink, not the emulator palette.
// Native RGB565 like the theme's colours: cv->image pushes with
// setSwapBytes(true), as it does the little-endian .565 covers.
static const uint16_t MANUAL_LEVELS[4] = { 0xFFFF, 0xAD55, 0x52AA, 0x0000 };

static const ui_hint_t HINTS[] = { { "B", "Back" }, { "A", "Zoom" } };

// Everything one open reader holds, all of it heap and all of it freed on
// the way out.
typedef struct view_s {
    const ui_canvas_t* cv;
    manual_reader_t rd;
    uint32_t size;   // the file's, for the band-offset checks
    manual_page_t* pages;
    uint16_t count;
    uint8_t* block;  // one band's compressed block
    uint8_t* band;   // MANUAL_BAND_ROWS page rows, decoded
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

// Band b of the current page, decoded into v->band.
static bool decode_band(const view_t* v, const manual_page_t* p, uint16_t b)
{
    return manual_band(&v->rd, v->size, p, b, v->block, MANUAL_BLOCK_BYTES,
                       v->band, MANUAL_BAND_BYTES) == MANUAL_OK;
}

// The tile the nav machine is on, full size. A page narrower or shorter than
// the window is centred in it on black. The tile's origin is clamped to the
// page's edge, not to a band, so its first and last bands may be partial.
static bool draw_tile(const view_t* v, const manual_nav_t* nav)
{
    const manual_page_t* p = &v->pages[nav->page];
    const uint32_t stride = (p->w + 3u) / 4u;
    const uint16_t vw = p->w < GAME_W ? p->w : GAME_W;
    const uint16_t vh = p->h < GAME_H ? p->h : GAME_H;
    const int16_t x = (int16_t)((GAME_W - vw) / 2);
    const int16_t y = (int16_t)((GAME_H - vh) / 2);
    uint16_t x0;
    uint16_t y0;

    manual_tile_origin(p, GAME_W, GAME_H, nav->tx, nav->ty, &x0, &y0);
    if (vw < GAME_W || vh < GAME_H) {
        v->cv->fill(v->cv->ctx, 0, 0, GAME_W, GAME_H, UI_COL_BG);
    }
    for (uint16_t r = y0; r < y0 + vh;) {
        const uint16_t b = (uint16_t)(r / MANUAL_BAND_ROWS);
        uint16_t end = (uint16_t)((b + 1) * MANUAL_BAND_ROWS);

        if (end > y0 + vh) {
            end = (uint16_t)(y0 + vh);
        }
        if (!decode_band(v, p, b)) {
            return false;
        }
        for (; r < end; r++) {
            manual_expand_row(v->band + (r % MANUAL_BAND_ROWS) * stride, x0,
                              vw, MANUAL_LEVELS, v->row);
            v->cv->image(v->cv->ctx, x, (int16_t)(y + r - y0), vw, 1, v->row,
                         0, 1);
        }
    }
    return true;
}

// The whole page at half size, centred, with rounded corners, under the
// theme's chrome or, for a page too tall for it, over the hint footer alone.
// Two page rows in, one screen row out: no page-sized buffer anywhere.
static bool draw_overview(const view_t* v, const manual_nav_t* nav)
{
    const ui_canvas_t* cv = v->cv;
    const manual_page_t* p = &v->pages[nav->page];
    const uint32_t stride = (p->w + 3u) / 4u;
    const uint16_t hw = (uint16_t)((p->w + 1u) / 2u);
    const uint16_t hh = (uint16_t)((p->h + 1u) / 2u);
    const bool chrome = hh <= MANUAL_ROOM_H;
    const int16_t x = (int16_t)((GAME_W - hw) / 2);
    const int16_t y = chrome ? (int16_t)(UI_HEADER_H + (MANUAL_ROOM_H - hh) / 2)
                             : (int16_t)((GAME_H - hh) / 2);
    char label[12];

    snprintf(label, sizeof(label), "%u/%u", (unsigned)(nav->page + 1),
             (unsigned)v->count);
    cv->fill(cv->ctx, 0, 0, GAME_W, GAME_H, UI_COL_BG);
    if (chrome) {
        ui_header(cv, GAME_W, "Manual", label);
        ui_help_line(cv, GAME_W, GAME_H - UI_FOOT_H - UI_HELP_H,
                     "Left and Right turn pages");
    }
    for (uint16_t r = 0; r < p->h; r += MANUAL_BAND_ROWS) {
        uint16_t n = (uint16_t)(p->h - r);

        if (n > MANUAL_BAND_ROWS) {
            n = MANUAL_BAND_ROWS;
        }
        if (!decode_band(v, p, (uint16_t)(r / MANUAL_BAND_ROWS))) {
            return false;
        }
        for (uint16_t i = 0; i < n; i += 2) {
            const uint8_t* b = (i + 1 < n) ? v->band + (i + 1) * stride : NULL;

            manual_decimate_row(v->band + i * stride, b, p->w, v->half);
            manual_expand_row(v->half, 0, hw, MANUAL_LEVELS, v->row);
            ui_round_corners_565(v->row, (int16_t)hw, (int16_t)hh,
                                 (int16_t)((r + i) / 2), 1, UI_IMG_R,
                                 UI_COL_BG);
            cv->image(cv->ctx, x, (int16_t)(y + (r + i) / 2), hw, 1, v->row,
                      0, 1);
        }
    }

    ui_hint_bar(cv, GAME_W, GAME_H - UI_FOOT_H, HINTS,
                (uint8_t)(sizeof(HINTS) / sizeof(HINTS[0])));
    if (!chrome) {
        // On the hint labels' line: the footer's pill is a pixel inside its
        // band and two short of it.
        cv->text(cv->ctx, label, UI_TEXT_X,
                 (int16_t)(GAME_H - UI_FOOT_H + 1 +
                           ui_text_dy(UI_FOOT_H - 2, UI_FONT_HINT)),
                 (int16_t)(GAME_W / 2), 1, UI_FONT_HINT, UI_ALIGN_LEFT,
                 UI_COL_TEXT, UI_COL_BG);
    }
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
    int rc;

    if (!sd_manual_reader(rom_filename, &v->rd, &v->size)) {
        Serial.printf("[MANUAL] no manual for %s\n", rom_filename);
        return false;
    }
    rc = manual_header(&v->rd, &v->count);
    if (rc != MANUAL_OK) {
        Serial.printf("[MANUAL] refused header: %d\n", rc);
        return false;
    }
    v->pages = (manual_page_t*)malloc(v->count * sizeof(manual_page_t));
    v->block = (uint8_t*)malloc(MANUAL_BLOCK_BYTES);
    v->band = (uint8_t*)malloc(MANUAL_BAND_BYTES);
    v->half = (uint8_t*)malloc((GAME_W + 3) / 4);
    v->row = (uint16_t*)malloc(GAME_W * sizeof(uint16_t));
    if (!v->pages || !v->block || !v->band || !v->half || !v->row) {
        Serial.printf("[MANUAL] no memory for %u pages\n",
                      (unsigned)v->count);
        return false;
    }
    rc = manual_table(&v->rd, v->size, GAME_W, GAME_H, v->pages, v->count);
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

    if (s && rom_filename && view_setup(&v, rom_filename)) {
        v.cv = display_canvas(s->game_x, s->game_y);
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
    free(v.block);
    free(v.band);
    free(v.half);
    free(v.row);
    sd_manual_close();
    return opened;
}
