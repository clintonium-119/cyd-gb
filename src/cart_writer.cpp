#include "cart_writer.h"

#include "button_input.h"
#include "display.h"
#include "render_config.h"
#include "sd_manager.h"
#include "settings.h"
#include "ui/picker.h"
#include "ui/picker_draw.h"

#include <Arduino.h>

// The writer's binding.
//
// Everything the writer decides is in lib/gbcore/ui/picker.c and everything it
// lays out is in lib/gbcore/ui/picker_draw.c, both pure C and both host-tested.
// This file supplies the four things they cannot have: a clock, the expander,
// the panel and the SD card.
//
//   * poll  — one expander read every WRITER_POLL_MS, fed to the picker as a
//             button word and a millis() timestamp
//   * draw  — the display module's canvas, asked for at the per-unit
//             game_x / game_y so the writer renders inside the game window
//   * read  — the catalog index, a title's description, and its two images
//
// It reaches for nothing below itself. The names it must not mention are the
// guard test's list, not repeated here, because that test scans this file's
// comments too.
//
// Every exit is a halt, so the buffers below are static and generous (rule 5
// in cart_writer.h). The two image buffers are separate rather than shared
// because the detail page's band scrolls over both at once.

// One expander read every WRITER_POLL_MS, matching the in-game menu. That is
// longer than the input module's debounce window, so two successive samples
// are already stable and the picker's edge detection needs no filter.
#define WRITER_POLL_MS 16

static catalog_index_t idx;                 // about 19 KB
static uint16_t art[PICKER_ART_PX];         // 18,432 B — the box art
static uint16_t shot[PICKER_ART_PX];        // 18,432 B — the gameplay snapshot
static char desc[CATALOG_DESC_MAX];
static picker_t picker;
static boot_made_t made;
static picker_layout_t geom;
static settings_t cfg;

// ─── the writer ─────────────────────────────────────────────────────────────

enum boot_pick_e writer_open(enum writer_mode_e mode,
                             const catalog_reader_t* cat,
                             const boot_flags_t* flags, bool pending_set,
                             boot_selection_t* out) {
    bool immediate = (mode == WRITER_MODE_IMMEDIATE);
    uint32_t drawn_us = 0;
    bool logged = false;
    int rc;

    if (!out || !cat) {
        // No catalog to show. The caller halts on whatever it booted from,
        // exactly as it did when this body was a stub.
        return BOOT_PICK_NONE;
    }

    rc = catalog_index_build(cat, &idx);
    if (rc != CATALOG_OK && rc != CATALOG_ERR_FULL) {
        // ERR_FULL is fine: the first CATALOG_MAX entries are intact.
        Serial.printf("[WRITER] catalog build failed (%d)\n", rc);
        return BOOT_PICK_NONE;
    }

    if (!settings_load(&cfg)) {
        settings_defaults(&cfg);
    }

    rc = picker_layout(GAME_W, GAME_H, &geom);
    if (rc != PICKER_OK) {
        // Cannot happen at any RENDER_GEOM, so it is logged rather than
        // handled: a window this small means render_config.h changed.
        Serial.printf("[WRITER] layout refused %dx%d (%d)\n", GAME_W, GAME_H,
                      rc);
        return BOOT_PICK_NONE;
    }

    if (immediate) {
        // Which starters this setup has already written, so their rows show a
        // mark. Absent is the empty record, not a failure.
        if (!settings_made_load(&made)) {
            boot_made_clear(&made);
        }
    }

    rc = picker_init(&picker, immediate ? PICKER_MODE_IMMEDIATE
                                        : PICKER_MODE_PENDING,
                     &idx, flags ? flags->wild_done : false, pending_set,
                     immediate ? &made : NULL, geom.rows);
    if (rc != PICKER_OK) {
        Serial.printf("[WRITER] no rows to show (%d)\n", rc);
        return BOOT_PICK_NONE;
    }

    desc[0] = '\0';
    tft.fillScreen(TFT_BLACK);
    picker_draw(&picker, &geom, NULL, art, shot,
                display_canvas(cfg.game_x, cfg.game_y));

    for (;;) {
        button_update();

        uint8_t word = (uint8_t)button_get_buttons();
        uint32_t now = millis();
        uint8_t ev = picker_input(&picker, word, now);
        uint16_t ci = 0;

        if (ev == PICKER_EVENT_DONE) {
            break;
        }

        // A title was opened: its cover, its snapshot and its description are
        // wanted once, behind the screen transition rather than mid-scroll.
        if (picker_media_due(&picker, &ci)) {
            bool have_art =
                sd_media_read(ART_PATH, idx.e[ci].filename, art, PICKER_ART_PX);
            bool have_shot = sd_media_read(SHOT_PATH, idx.e[ci].filename, shot,
                                           PICKER_ART_PX);

            if (catalog_read_desc(cat, idx.e[ci].offset, desc, sizeof desc) !=
                CATALOG_OK) {
                desc[0] = '\0';
            }
            // How far this page can scroll depends on the description that
            // just arrived, so the span is handed over before the redraw.
            picker_set_scroll_span(
                &picker, picker_page_lines(&geom, desc[0] ? desc : NULL),
                geom.band_rows);

            if (picker_media_loaded(&picker, ci, have_art, have_shot) ==
                PICKER_EVENT_REDRAW) {
                ev = PICKER_EVENT_REDRAW;
            }
        }

        if (ev == PICKER_EVENT_REDRAW) {
            uint32_t began = micros();

            picker_draw(&picker, &geom, desc[0] ? desc : NULL, art, shot,
                        display_canvas(cfg.game_x, cfg.game_y));
            drawn_us = micros() - began;
            if (!logged) {
                // Once per session: the figure the bench needs, without a
                // line per keypress.
                Serial.printf("[WRITER] redraw %lu us\n",
                              (unsigned long)drawn_us);
                logged = true;
            }
        }

        delay(WRITER_POLL_MS);
    }

    return picker_result(&picker, out);
}
