#include "cart_writer.h"

#include "button_input.h"
#include "display.h"
#include "render_config.h"
#include "sd_manager.h"
#include "settings.h"
#include "ui/picker.h"
#include "ui/picker_draw.h"
#include "ui/theme_draw.h"

#include <Arduino.h>
#include <esp_heap_caps.h>

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
//   * read  — the catalog index, the highlighted title's two images once the
//             highlight has settled on it, and an opened title's description
//
// It reaches for nothing below itself. The names it must not mention are the
// guard test's list, not repeated here, because that test scans this file's
// comments too.
//
// Every exit is a halt, so the buffers below may be generous (rule 5 in
// cart_writer.h). The three big ones are on the heap for the writer's lifetime
// rather than static: .bss is reserved in every boot, and these 56 KB would
// otherwise sit idle through every game, in DRAM whose largest free block at
// game time is about 15 KB (TASK-0001). The writer's boot runs nothing else,
// so they come from a heap that has room. The two image buffers are separate
// rather than shared because both images are on screen at once, and a partial
// redraw may repaint either without reading the card again.

// One expander read every WRITER_POLL_MS, matching the in-game menu. That is
// longer than the input module's debounce window, so two successive samples
// are already stable and the picker's edge detection needs no filter.
#define WRITER_POLL_MS 16

static catalog_index_t* idx;                // about 19 KB
static uint16_t* art;                       // 18,432 B — the box art
static uint16_t* shot;                      // 18,432 B — the gameplay snapshot
// The title's full description, DESC_MAX on the heap while the writer is up:
// this translation unit links into every image, and a 4 KB static would come
// out of DRAM that has about 15 KB to spare. NULL when it was refused.
static char* desc;
static picker_t picker;
static boot_made_t made;
static picker_layout_t geom;
static settings_t cfg;

// ─── the writer ─────────────────────────────────────────────────────────────

static enum boot_pick_e writer_run(enum writer_mode_e mode,
                                   const catalog_reader_t* cat,
                                   const boot_flags_t* flags, bool pending_set,
                                   boot_selection_t* out) {
    bool immediate = (mode == WRITER_MODE_IMMEDIATE);
    uint32_t drawn_us = 0;
    bool logged = false;
    bool media_logged = false;
    const ui_canvas_t* cv;
    int rc;

    rc = catalog_index_build(cat, idx);
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
                     idx, flags ? flags->wild_done : false, pending_set,
                     immediate ? &made : NULL, geom.rows);
    if (rc != PICKER_OK) {
        Serial.printf("[WRITER] no rows to show (%d)\n", rc);
        return BOOT_PICK_NONE;
    }

    desc = (char*)malloc(DESC_MAX);
    if (!desc) {
        Serial.printf("[WRITER] no %u B for descriptions\n",
                      (unsigned)DESC_MAX);
    }
    cv = display_canvas(cfg.game_x, cfg.game_y);
    picker_layout_measure(&geom, cv);
    picker_set_marquee_span(&picker, picker_row_overflow(&picker, &geom, cv));
    tft.fillScreen(TFT_BLACK);
    picker_draw(&picker, &geom, NULL, art, shot, cv);

    for (;;) {
        button_update();

        uint8_t word = (uint8_t)button_get_buttons();
        uint32_t now = millis();
        uint8_t ev = picker_input(&picker, word, now);
        uint16_t ci = 0;

        if (ev == PICKER_EVENT_DONE) {
            break;
        }

        // A new highlight: how far its label overflows, so the marquee
        // knows whether to run and how far.
        if (picker.screen == PICKER_SCREEN_LIST &&
            (ev & (PICKER_EVENT_ROWS | PICKER_EVENT_REDRAW))) {
            picker_set_marquee_span(&picker,
                                    picker_row_overflow(&picker, &geom, cv));
        }

        // The highlight settled on a title: its cover and snapshot.
        if (picker_media_due(&picker, now, &ci)) {
            uint32_t began = micros();
            bool have_art =
                sd_media_read(ART_PATH, idx->e[ci].filename, art, PICKER_ART_PX);
            bool have_shot = sd_media_read(SHOT_PATH, idx->e[ci].filename, shot,
                                           PICKER_ART_PX);

            // The theme rounds every image's corners, and the buffer is ours.
            // Both files letterbox their picture on black, so the corners
            // are found rather than assumed at the file's edge.
            if (have_art) {
                ui_inset_t in;

                ui_inset_begin(&in, PICKER_ART_W, PICKER_ART_H);
                ui_round_inset_565(&in, art, 0, PICKER_ART_H, UI_IMG_R,
                                   UI_COL_BG);
            }
            if (have_shot) {
                ui_inset_t in;

                ui_inset_begin(&in, PICKER_ART_W, PICKER_ART_H);
                ui_round_inset_565(&in, shot, 0, PICKER_ART_H, UI_IMG_R,
                                   UI_COL_BG);
            }

            if (!media_logged) {
                // Once per session, like the redraw figure: how long a held
                // Down stalls at each title it settles on.
                Serial.printf("[WRITER] media %lu us\n",
                              (unsigned long)(micros() - began));
                media_logged = true;
            }
            ev |= picker_media_loaded(&picker, ci, have_art, have_shot);
        }

        // A title was opened: its description, once, behind the screen
        // transition rather than mid-scroll.
        if (picker_desc_due(&picker, &ci)) {
            // The full text from /desc, else the catalog's blurb.
            if (desc && !sd_desc_read(idx->e[ci].filename, desc, DESC_MAX)
                && catalog_read_desc(cat, idx->e[ci].offset, desc, DESC_MAX) !=
                       CATALOG_OK) {
                desc[0] = '\0';
            }
            // How far this page can scroll depends on the description that
            // just arrived, so the span is handed over before the redraw.
            picker_set_scroll_span(
                &picker,
                picker_page_lines(&geom, desc && desc[0] ? desc : NULL),
                picker_band_rows(&picker, &geom, cv));
            ev |= PICKER_EVENT_BAND;
        }

        if (ev != PICKER_EVENT_NONE) {
            uint32_t began = micros();

            // Only what changed. A full repaint on every hold tick blanked
            // the window under the panel's refresh, which was the flicker.
            picker_draw_events(&picker, &geom, ev,
                               desc && desc[0] ? desc : NULL, art, shot, cv);
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

    free(desc);
    desc = NULL;
    return picker_result(&picker, out);
}

enum boot_pick_e writer_open(enum writer_mode_e mode,
                             const catalog_reader_t* cat,
                             const boot_flags_t* flags, bool pending_set,
                             boot_selection_t* out) {
    enum boot_pick_e pick = BOOT_PICK_NONE;

    if (!out || !cat) {
        // No catalog to show. The caller halts on whatever it booted from,
        // exactly as it did when this body was a stub.
        return BOOT_PICK_NONE;
    }

    idx = (catalog_index_t*)malloc(sizeof(*idx));
    art = (uint16_t*)malloc(PICKER_ART_PX * sizeof(uint16_t));
    shot = (uint16_t*)malloc(PICKER_ART_PX * sizeof(uint16_t));
    if (idx && art && shot) {
        pick = writer_run(mode, cat, flags, pending_set, out);
    } else {
        // Refused, the same way a missing catalog is: the caller halts.
        Serial.printf("[WRITER] no heap for the picker (largest block %u)\n",
                      (unsigned)heap_caps_get_largest_free_block(
                          MALLOC_CAP_8BIT));
    }
    free(shot);
    free(art);
    free(idx);
    shot = NULL;
    art = NULL;
    idx = NULL;
    return pick;
}
