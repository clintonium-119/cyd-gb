#include "diag.h"

#include "battery.h"
#include "build_info.h"
#include "button_input.h"
#include "display.h"
#include "hw_config.h"
#include "nfc_cart.h"
#include "render_config.h"
#include "render/scaler.h"
#include "sd_manager.h"
#include "speaker.h"
#include "audio/mix.h"
#include "audio/tone.h"
#include "cart/boot.h"
#include "cart/catalog.h"
#include "cart/ndef.h"
#include "cart/ntag.h"
#include "input/combo.h"
#include "ui/diag.h"
#include "ui/diag_draw.h"

#include <stdlib.h>

#include <Arduino.h>

// The diagnostic screen's binding.
//
// Everything the mode decides is in lib/gbcore/ui/diag.c and everything it
// lays out is in lib/gbcore/ui/diag_draw.c, both pure C and both host-tested.
// This file supplies the seven things they cannot have: a clock, the button
// expander through the combo module, the tag reader, the card, the ADC, the
// speaker and the panel.
//
//   * poll  — one expander read every DIAG_POLL_MS, fed through the combo
//             module so Select+Left/Right arrive as events and the bare D-pad
//             arrives as a masked word
//   * draw  — the display module's canvas, asked for at the WORKING origin
//             rather than the stored one, so the nudge page moves the window
//             live
//   * read  — the card once at entry, the ADC once a second on its own page,
//             and the tag only when someone asks
//
// It reaches for nothing above or below itself. The names it must not mention
// are the guard test's list, not repeated here, because that test scans this
// file's comments too.
//
// Every exit is a power cycle (rule 2 in diag.h), so the buffers below are
// static and generous.

// One expander read every DIAG_POLL_MS, matching the in-game menu and the
// writer. Longer than the input module's debounce window, so two successive
// samples are already stable.
#define DIAG_POLL_MS 16

// UID bytes as hex, which is what the page shows. NTAG215's is seven.
#define DIAG_UID_BYTES 7

static combo_state_t combo;
static diag_t d;
static diag_data_t data;                       // about 300 B
static diag_layout_t geom;
static diag_checker_t checker;                 // 8,062 B — the scaled block
// Named for the state rather than the tone, because Arduino.h already
// declares a tone().
static tone_state_t tone_st;
static int16_t stereo[2 * SPEAKER_SAMPLES_PER_FRAME];  // 2,192 B
static uint8_t mono[SPEAKER_SAMPLES_PER_FRAME];        //   548 B

// The tag layer talks through the reader's transceive and knows nothing else
// about it. Read-only use throughout: nothing here composes a write.
static const ntag_dev_t tag_dev = { NULL, nfc_transceive };

// The origin the last draw used, so a moved window can be cleared before the
// next one paints — otherwise the old border stays on the panel.
static int16_t drawn_ox = -1;
static int16_t drawn_oy = -1;

// ─── data gathering ─────────────────────────────────────────────────────────

static void hex_uid(const uint8_t* uid, uint8_t len, char* out, size_t out_sz)
{
    size_t at = 0;
    uint8_t i;

    out[0] = '\0';
    for (i = 0; i < len && at + 2 < out_sz; i++) {
        at += (size_t)snprintf(out + at, out_sz - at, "%02X", uid[i]);
    }
}

// The card is read once, at entry: a diagnostic page that re-walked the
// directory every redraw would take longer than the redraw itself.
static void gather_sd(bool sd_ok)
{
    catalog_reader_t cat;
    size_t n = 0;

    data.sd_ok = sd_ok;
    if (!sd_ok) {
        return;
    }

    data.rom_count = sd_rom_count();
    data.sd_stats_ok = sd_card_stats(&data.sd_total_mb, &data.sd_used_mb);

    if (sd_catalog_reader(&cat) && catalog_count(&cat, &n) == CATALOG_OK) {
        data.catalog_ok = true;
        data.catalog_count = (uint16_t)n;
    }
}

// One look at whatever is in the field, on entry to the page and on each A.
// Never from the loop: nfc_detect() blocks for about a second with no tag
// present, and a page that polled would answer no buttons while it did.
static void scan_tag()
{
    uint8_t uid[DIAG_UID_BYTES] = { 0 };
    uint8_t uid_len = 0;
    uint8_t cfg[NTAG_PAGE_SIZE] = { 0 };
    char rom[ROM_STORE_NAME_MAX];
    enum boot_class_e cls = BOOT_CLASS_BLANK;

    // Everything the previous cart left behind goes first, so a failed scan
    // cannot show the last tag's numbers as if they were this one's.
    data.uid_hex[0] = '\0';
    data.version_ok = false;
    data.cfg_ok = false;
    data.auth0 = 0;
    data.access = 0;
    data.ndef_read_ok = false;
    data.ndef_rc = 0;
    data.payload[0] = '\0';
    data.cls = BOOT_CLASS_BLANK;
    memset(data.ndef_raw, 0, sizeof(data.ndef_raw));

    switch (nfc_detect(uid, &uid_len)) {
    case NFC_DETECT_NONE:
        data.nfc_state = DIAG_NFC_NONE;
        break;
    case NFC_DETECT_MULTI:
        data.nfc_state = DIAG_NFC_MULTI;
        break;
    case NFC_DETECT_ERR:
        data.nfc_state = DIAG_NFC_ERR;
        break;
    case NFC_DETECT_ONE:
        data.nfc_state = DIAG_NFC_ONE;
        break;
    }

    if (data.nfc_state != DIAG_NFC_ONE) {
        Serial.printf("[DIAG] tag state=%u\n", (unsigned)data.nfc_state);
        return;
    }

    hex_uid(uid, uid_len, data.uid_hex, sizeof(data.uid_hex));
    data.version_ok =
        ntag_get_version(&tag_dev, data.version) == NTAG_OK;

    if (ntag_read_pages(&tag_dev, NTAG215_PAGE_USER_FIRST,
                        NDEF_BUF_MAX / NTAG_PAGE_SIZE, data.ndef_raw)
        == NTAG_OK) {
        data.ndef_read_ok = true;
        data.ndef_rc = ndef_parse_text(data.ndef_raw, sizeof(data.ndef_raw),
                                       data.payload, sizeof(data.payload));
        if (data.ndef_rc == NDEF_OK
            && boot_classify(data.payload, &cls, rom, sizeof(rom))
                   == BOOT_CLASSIFY_OK) {
            data.cls = (uint8_t)cls;
        }
    }

    data.cfg_ok = ntag_read_auth0(&tag_dev, &data.auth0) == NTAG_OK
                  && ntag_read_pages(&tag_dev, NTAG215_PAGE_CFG1, 1, cfg)
                         == NTAG_OK;
    if (data.cfg_ok) {
        data.access = cfg[NTAG215_CFG1_ACCESS];
    }

    Serial.printf("[DIAG] tag state=%u uid=%s auth0=0x%02X access=0x%02X\n",
                  (unsigned)data.nfc_state, data.uid_hex, data.auth0,
                  data.access);
}

// ─── drawing ────────────────────────────────────────────────────────────────

static void redraw(uint32_t now_ms)
{
    int16_t ox = 0;
    int16_t oy = 0;

    diag_origin(&d, &ox, &oy);

    // A window that moved leaves its old border behind, and on the one page
    // whose whole job is showing that border, a stale one is worse than a
    // slow redraw.
    if (ox != drawn_ox || oy != drawn_oy) {
        tft.fillScreen(TFT_BLACK);
        drawn_ox = ox;
        drawn_oy = oy;
    }

    diag_draw(&d, &data, &geom, &checker, now_ms, display_canvas(ox, oy));
}

// The one line the page cannot draw itself, because it has to be on the panel
// before the reader blocks rather than after it answers.
static void say_scanning()
{
    int16_t ox = 0;
    int16_t oy = 0;
    const ui_canvas_t* cv;

    diag_origin(&d, &ox, &oy);
    cv = display_canvas(ox, oy);
    cv->fill(cv->ctx, 0, geom.footer_y, geom.w, DIAG_ROW_H, TFT_BLACK);
    cv->text(cv->ctx, "Scanning...", 4, geom.footer_y,
             (int16_t)(geom.w - 8), 1, UI_FONT_SMALL, UI_ALIGN_LEFT,
             TFT_WHITE, TFT_BLACK);
}

// ─── the panel-trim fixture ─────────────────────────────────────────────────
// The one page that pushes frames rather than drawing them, and the two
// properties that make its calibration mean anything:
//
//   * It pushes in the SHIPPING ORDER. Under PUSH_COL display_frame_begin()
//     opens a transposed portrait window that fills along a gate line, so it
//     owes that window whole output COLUMNS of GAME_H pixels, descending
//     landscape x. A row-major push through it still totals the right pixel
//     count and still draws something — just not the pattern it claims, which
//     is how the fixture this page replaces went stale.
//   * It pays the EMULATOR'S pacer. The page loop's delay(DIAG_POLL_MS) is
//     about 62.5 fps and the beat being nulled is against the audio-paced
//     59.7275 — 2.8 Hz apart, which is wider than the whole trim range. So
//     this blocks in speaker_write_frame() on a frame of silence, the same
//     way the tone page already takes its cadence from the DMA queue.
//
// The 12-colour pixel byte's background ramp, which is where the palette LUT
// keeps the four shades a game's background is drawn in. The fixture borrows
// them so a builder judges the seam in the colours they will be looking at.
#define TRIM_LUT_BG 0x20

// Output columns (or rows) per transfer, matching the frame path's own block
// so the transfer size a game produces is the transfer size this produces.
#if PUSH_TRANSPOSED
#define TRIM_BLOCK_N   COL_BLOCK_COLS
#define TRIM_BLOCK_PX  (COL_BLOCK_COLS * GAME_H)
#define TRIM_LINE_PX   GAME_H
#define TRIM_LINES     GAME_W
#else
#define TRIM_BLOCK_N   BLOCK_ROWS
#define TRIM_BLOCK_PX  (BLOCK_ROWS * GAME_W)
#define TRIM_LINE_PX   GAME_W
#define TRIM_LINES     GAME_H
#endif

// Two, alternating, for the same reason the frame path has two: a pushed
// buffer belongs to the driver until the transfer completes. Heap rather than
// static — 10 KB would not fit in the static segment beside the frame path's
// own buffers, and the diagnostic mode never returns, so nothing frees them.
//
// In the panel's format, not in RGB565, for the same reason the transfer size
// matches the frame path's: a fixture that wrote 16-bit pixels into a window
// the frame path had put in 12-bit would push a third more bytes than the
// game it is calibrating for, and the panel would read them as the wrong
// pixels besides. trim_scratch holds one line in 565 on the way.
#if PIXEL_PACKED
typedef uint8_t trim_px_t;
static uint16_t trim_scratch[TRIM_LINE_PX];
#else
typedef uint16_t trim_px_t;
#endif
static trim_px_t* trim_buf[2] = { nullptr, nullptr };

// The fixture's pacer. One frame of silence per pushed frame, blocking on the
// speaker's DMA queue — the emulator's own pacer, not an interval this loop
// picks — and spending the fractional sample so the page's cadence is a
// game's rather than 0.068 Hz above it. Same shape as the porch divider it is
// calibrating: a whole count most frames, one more on the rest.
static_assert(SPEAKER_SAMPLES_X10000 / 10000 == SPEAKER_SAMPLES_PER_FRAME,
              "SPEAKER_SAMPLES_X10000 must be SPEAKER_SAMPLES_PER_FRAME plus "
              "a fraction");

static size_t trim_pace_samples()
{
    static uint32_t acc = 0;

    acc += SPEAKER_SAMPLES_X10000 % 10000;
    if (acc >= 10000) {
        acc -= 10000;
        return SPEAKER_SAMPLES_PER_FRAME + 1;
    }
    return SPEAKER_SAMPLES_PER_FRAME;
}

// The porch the panel is actually running at, so a value the page has changed
// reaches the hardware. Compared once a pass rather than pushed from each of
// the four places that can move it — a correction, the D-pad, B, and a save —
// because a path that forgot to push is exactly the bug this replaces: the
// page showed a corrected porch the panel had never been told about, so every
// round of a calibration measured the same beat and the loop could not close.
static uint8_t applied_fpa = 0xFF;
static uint8_t applied_ratio = 0xFF;

static void trim_follow()
{
    uint8_t fpa = 0;
    uint8_t ratio = 0;

    diag_trim(&d, &fpa, &ratio);
    if (fpa == applied_fpa && ratio == applied_ratio) {
        return;
    }
    applied_fpa = fpa;
    applied_ratio = ratio;
    display_set_trim(fpa, ratio);
}

static bool trim_buffers()
{
    if (trim_buf[0] != nullptr) {
        return true;
    }
#if PIXEL_PACKED
    trim_buf[0] = (trim_px_t*)malloc(SCALER_PACKED_BYTES(TRIM_BLOCK_PX));
    trim_buf[1] = (trim_px_t*)malloc(SCALER_PACKED_BYTES(TRIM_BLOCK_PX));
#else
    trim_buf[0] = (trim_px_t*)malloc(TRIM_BLOCK_PX * sizeof(uint16_t));
    trim_buf[1] = (trim_px_t*)malloc(TRIM_BLOCK_PX * sizeof(uint16_t));
#endif
    if (trim_buf[0] == nullptr || trim_buf[1] == nullptr) {
        free(trim_buf[0]);
        free(trim_buf[1]);
        trim_buf[0] = trim_buf[1] = nullptr;
        return false;
    }
    return true;
}

// One line of the fixture — an output column under PUSH_COL, an output row
// otherwise — at window-relative landscape (x, y). The scroll is vertical
// either way, because the seam the page is here to count is a vertical line
// with a vertical displacement across it.
static void trim_line(uint16_t* px, int16_t line, uint8_t pat, int32_t sx,
                      int32_t sy, const uint16_t* lut)
{
    int16_t i;

    for (i = 0; i < TRIM_LINE_PX; i++) {
#if PUSH_TRANSPOSED
        int32_t u = (int32_t)line + sx;
        int32_t v = (int32_t)i + sy;
#else
        int32_t u = (int32_t)i + sx;
        int32_t v = (int32_t)line + sy;
#endif
        px[i] = lut[TRIM_LUT_BG + diag_trim_shade(pat, u, v)];
    }
}

static void trim_push(int16_t ox, int16_t oy, uint8_t pat, int32_t sx,
                      int32_t sy, const uint16_t* lut)
{
    int16_t at = 0;
    int16_t n = 0;
    unsigned buf = 0;

    display_frame_begin(ox, oy);
    while (at < TRIM_LINES) {
        int16_t line;

#if FRAME_COLS_DESCENDING && PUSH_TRANSPOSED
        // The walk follows the panel, not the image: the window fills from
        // the right-hand edge of the game window inwards.
        line = (int16_t)(TRIM_LINES - 1 - (at + n));
#else
        line = (int16_t)(at + n);
#endif
#if PIXEL_PACKED
        trim_line(trim_scratch, line, pat, sx, sy, lut);
        scaler_pack_444(trim_buf[buf] + SCALER_PACKED_BYTES((size_t)n
                                                            * TRIM_LINE_PX),
                        trim_scratch, TRIM_LINE_PX);
#else
        trim_line(trim_buf[buf] + (size_t)n * TRIM_LINE_PX, line, pat, sx, sy,
                  lut);
#endif
        n++;
        if (n == TRIM_BLOCK_N || at + n == TRIM_LINES) {
#if PIXEL_PACKED
            display_push_packed_dma(trim_buf[buf],
                                    SCALER_PACKED_BYTES((size_t)n
                                                        * TRIM_LINE_PX));
#else
            display_push_rows_dma(trim_buf[buf], (size_t)n * TRIM_LINE_PX);
#endif
            buf ^= 1u;
            at = (int16_t)(at + n);
            n = 0;
        }
    }
    display_dma_wait();
    display_frame_end();
}

// ─── the mode ───────────────────────────────────────────────────────────────

void diag_run(settings_t* s, bool nfc_ok, bool sd_ok)
{
    uint32_t next_bat_ms = 0;
    uint8_t last_buttons = 0;
    bool logged = false;
    uint16_t frame_flags = 0;

    // The eight expander bits behind the eight joypad bits, in joypad order,
    // so the buttons page can name the pin a dead switch is on.
    static const uint8_t GPA_BY_GB_BIT[8] = {
        BTN_GPA_RIGHT, BTN_GPA_LEFT, BTN_GPA_UP, BTN_GPA_DOWN,
        BTN_GPA_A, BTN_GPA_B, BTN_GPA_SELECT, BTN_GPA_START,
    };

    memcpy(data.gpa, GPA_BY_GB_BIT, sizeof(data.gpa));
    strncpy(data.fw_version, BUILD_FW_VERSION, sizeof(data.fw_version) - 1);
    data.fw_version[sizeof(data.fw_version) - 1] = '\0';
    strncpy(data.build_time, BUILD_TIME_UTC, sizeof(data.build_time) - 1);
    data.build_time[sizeof(data.build_time) - 1] = '\0';

    // The checkerboard fixture needs four shades out of the gbcore table, and
    // Auto's are not in it — they come from the cartridge. The fixture is
    // about geometry and timing, not about which colours are running, so it
    // takes the fallback rather than teaching diag_draw the auto path.
    data.palette = (s->palette == PALETTE_AUTO) ? PALETTE_FALLBACK : s->palette;
    // The divider as the firmware actually used it, so the page reports the
    // placeholder rather than implying a measured ratio.
    data.bat_divider_x100 = (uint16_t)(BAT_DIVIDER * 100.0f);
    data.nfc_fw = nfc_firmware_version();
    data.nfc_state = nfc_ok ? DIAG_NFC_NOT_SCANNED : DIAG_NFC_NO_READER;

    gather_sd(sd_ok);

    if (diag_layout(GAME_W, GAME_H, &geom) != DIAG_OK) {
        // Cannot happen at any RENDER_GEOM, so it is logged rather than
        // handled: a window this small means render_config.h changed.
        Serial.printf("[DIAG] layout refused %dx%d\n", GAME_W, GAME_H);
        return;
    }
    if (diag_init(&d, SCREEN_W, SCREEN_H, GAME_W, GAME_H, s->game_x,
                  s->game_y, GAME_X, GAME_Y, s->volume, s->frameskip,
                  s->trim_fpa, s->trim_ratio, PANEL_TRIM_FPA,
                  PANEL_TRIM_RATIO)
        != DIAG_OK) {
        Serial.println("[DIAG] state refused the window");
        return;
    }
    // After the init, which opens on the uncalibrated guess. A unit that has
    // been trimmed before carries the direction its last session converged
    // in, so this boot's first run continues that convergence instead of
    // starting the guess over and walking a good porch off its null.
    diag_trim_set_dir(&d, s->trim_dir);
    combo_init(&combo);
    tone_init(&tone_st, TONE_HZ, SPEAKER_SAMPLE_RATE);

    Serial.printf("[DIAG] enter %s built %s\n", BUILD_FW_VERSION,
                  BUILD_TIME_UTC);

    tft.fillScreen(TFT_BLACK);
    redraw(millis());

    for (;;) {
        uint32_t now = millis();
        uint8_t ev = COMBO_EVENT_NONE;
        uint16_t flags;
        uint8_t page;
        bool dirty = false;

        button_update();
        combo_update(&combo, button_get_buttons(), now, &ev);

        flags = diag_input(&d, ev, combo_joypad(&combo), now);
        flags |= diag_tick(&d, now);
        // A run that ended itself on last pass's frame, handled like one that
        // ended on a mark.
        flags |= frame_flags;
        frame_flags = 0;
        page = diag_page(&d);

        // The raw word, not the masked one: the buttons page exists to show
        // what the expander reports, and the combo module's masking is the
        // very thing a builder needs to see through.
        data.buttons = (uint8_t)button_get_buttons();

        if (flags & DIAG_EV_PAGE) {
            Serial.printf("[DIAG] page %s\n", diag_page_title(page));
        }
        if (flags & DIAG_EV_SAVE_NUDGE) {
            diag_origin(&d, &s->game_x, &s->game_y);
            settings_save(s);
            Serial.printf("[DIAG] nudge saved gx=%d gy=%d\n", s->game_x,
                          s->game_y);
        }
        if (flags & DIAG_EV_SAVE_TRIM) {
            diag_trim(&d, &s->trim_fpa, &s->trim_ratio);
            s->trim_dir = diag_trim_dir(&d);
            settings_save(s);
            // The direction as a word, not as its sign: +1 means the loop is
            // SHORTENING the porch, which is the opposite sign to the page's
            // "Last move" row, and two conventions for one fact is how a
            // bench session gets read backwards.
            Serial.printf("[DIAG] trim saved porch %u + %u/64, %s\n",
                          (unsigned)s->trim_fpa, (unsigned)s->trim_ratio,
                          (s->trim_dir < 0) ? "lengthening" : "shortening");
        }
        if (flags & DIAG_EV_TRIM_FIXTURE) {
            int8_t vx = 0;
            int8_t vy = 0;
            const char* name = diag_trim_pat_name(diag_trim_pattern(&d));

            diag_trim_rate(&d, &vx, &vy);
            Serial.printf("[DIAG] fixture %s %+d,%+d px/frame\n",
                          (name != nullptr) ? name : "?", (int)vx, (int)vy);
        }
        if (flags & DIAG_EV_TRIM_STATE) {
            switch (diag_trim_verdict(&d)) {
            case DIAG_TRIM_SCATTERED:
                Serial.println("[DIAG] trim run thrown away: the marks "
                               "disagreed too much to average");
                break;
            case DIAG_TRIM_SLOWING:
                Serial.println("[DIAG] trim run not averaged: every interval "
                               "longer than the last, the seam is slowing");
                break;
            case DIAG_TRIM_SETTLED:
                Serial.println("[DIAG] trim run settled: no crossing in "
                               "20000 frames, nothing left to null");
                break;
            default:
                break;
            }
            if (diag_trim_running(&d)) {
                if (trim_buffers()) {
                    // The fixture's four shades are the running palette's
                    // background ramp, which diag_checker_build() already
                    // builds for the checkerboard pattern.
                    diag_checker_build(&checker, data.palette);
                } else {
                    Serial.println("[DIAG] no heap for the trim fixture");
                }
            } else {
                // The frame path left the panel in its own orientation and an
                // address window behind; the handover puts both back.
                display_bus_acquire();
                drawn_ox = -1;
                drawn_oy = -1;
                flags |= DIAG_EV_REDRAW;
            }
        }
        if (flags & DIAG_EV_FRAMESKIP) {
            s->frameskip = diag_frameskip(&d);
            settings_save(s);
            Serial.printf("[DIAG] frameskip %u\n", (unsigned)s->frameskip);
        }
        if ((flags & DIAG_EV_TONE) && !diag_tone_on(&d)) {
            speaker_silence();
        }
        if (flags & DIAG_EV_NFC_SCAN) {
            if (nfc_ok) {
                say_scanning();
                scan_tag();
            }
            dirty = true;
        }

        // Before the draw and before any frame goes out, so a corrected porch
        // is on the panel for the frames the next run counts.
        trim_follow();

        // Live data. Only the page showing it pays for the read.
        if (page == DIAG_PAGE_BUTTONS && data.buttons != last_buttons) {
            dirty = true;
        }
        last_buttons = data.buttons;

        if (page == DIAG_PAGE_BATTERY && (int32_t)(now - next_bat_ms) >= 0) {
            uint16_t cell = 0;

            next_bat_ms = now + BAT_SAMPLE_MS;
            data.bat_raw = battery_read_raw(&data.bat_pin_mv);
            if (battery_poll(now, &cell)) {
                data.bat_cell_mv = cell;
            }
            dirty = true;
        }

        if (((flags & DIAG_EV_REDRAW) || dirty) && !diag_trim_running(&d)) {
            uint32_t began = micros();

            redraw(now);
            if (!logged) {
                // Once per session: the figure the bench needs, without a
                // per-frame print in the way of it.
                Serial.printf("[DIAG] redraw %lu us\n",
                              (unsigned long)(micros() - began));
                logged = true;
            }
        }

        if (diag_trim_running(&d) && trim_buf[0] != nullptr) {
            int16_t ox = 0;
            int16_t oy = 0;

            int32_t sx = 0;
            int32_t sy = 0;

            diag_origin(&d, &ox, &oy);
            diag_trim_offsets(&d, &sx, &sy);
            trim_push(ox, oy, diag_trim_pattern(&d), sx, sy, checker.lut);
            // Counted only once it is on the panel: the count IS the
            // measurement, and a frame counted that was not pushed would put
            // the calibration out by exactly its own error.
            frame_flags = diag_trim_frame(&d);
            // A frame of silence, for the pacing and nothing else. This is
            // the emulator's own pacer — the DMA queue — rather than an
            // interval this loop picks, which is the whole point: the beat
            // being nulled is against the audio clock.
            size_t n = trim_pace_samples();

            memset(mono, 128, n);
            speaker_write_frame(mono, n);
        } else if (diag_tone_on(&d)) {
            // One frame per pass, so the DMA queue paces the loop at about
            // 16.7 ms — near enough the poll interval that button response is
            // the same either way.
            tone_fill(&tone_st, TONE_AMPLITUDE, stereo,
                      SPEAKER_SAMPLES_PER_FRAME);
            mix_mono(stereo, SPEAKER_SAMPLES_PER_FRAME, diag_volume(&d),
                     mono);
            speaker_write_frame(mono, SPEAKER_SAMPLES_PER_FRAME);
        } else {
            delay(DIAG_POLL_MS);
        }
    }
}
