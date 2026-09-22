#include "menu.h"
#include "display.h"
#include "button_input.h"
#include "emulator_bridge.h"
#include "hw_config.h"
#include "render_config.h"
#include "input/combo.h"
#include "ui/list.h"
#include "sd_manager.h"
#include "cart/catalog.h"
#include <Arduino.h>
#include <stdlib.h>

// The in-game pause menu: six rows inside the game window, worked with the
// D-pad. The highlight is the pure list machine in gbcore; everything here is
// drawing and the four side effects the rows have.
//
// One expander read every MENU_POLL_MS. That is longer than the input
// module's debounce window, so two successive samples are already stable and
// the edge detection below needs no filter of its own.
#define MENU_POLL_MS 16

// 6 x 26 + 40 = 196, inside GAME_H at every geometry — 216, 234, 240.
#define MENU_ROWS  6
#define MENU_ROW_H 26
#define MENU_TOP   40   /* the title band above the first row */

// The fork's two row colours, kept so this looks like the rest of the UI.
#define MENU_HL_BG  0x2945
#define MENU_ROW_BG 0x1082
#define MENU_TITLE  0xFFE0
#define MENU_DIM    0x7BEF

// An unprotected cartridge reads 0xFF in its configuration byte. The value
// belongs to the tag layer, but this page only ever displays it, so the one
// number is spelled out here rather than reaching into that layer for it.
#define MENU_AUTH0_OPEN 0xFF

enum menu_row_e {
    ROW_RESUME = 0,
    ROW_VOLUME,
    ROW_BRIGHT,
    ROW_PALETTE,
    ROW_INFO,
    ROW_RESET,
};

static const char* const ROW_LABELS[MENU_ROWS] = {
    "Resume",
    "Volume",
    "Brightness",
    "Palette",
    "Cart Info",
    "Reset",
};

// The stored volume is an index counting down towards louder, so the names
// read in the order the indices do.
static const char* const VOL_NAMES[] = {
    "High",
    "Med",
    "Low",
    "Off",
};

// ─── Helpers ────────────────────────────────────────────────────────────────

// Nothing acts until every button is up: the combo that opened the menu is
// still held when it first draws, and it must not also pick a row. The same
// on the way out — the A or B that closed the menu is still held when the
// game's per-frame poll resumes, and it must not also act in the game.
static void wait_release()
{
    button_update();
    while (button_get_buttons()) {
        button_update();
        delay(10);
    }
    delay(100);
}

// Which of the eight backlight steps the stored level is. Rounded up: the
// ladder lands on 255 exactly, but rounding up also keeps a level that was
// stored under an older ladder reading as the nearest step up rather than
// falling a whole step.
static uint8_t bright_level(uint8_t level)
{
    if (level <= BL_MIN) {
        return 1;
    }
    return (uint8_t)((level - BL_MIN + BL_STEP - 1) / BL_STEP + 1);
}

// The right-hand value for a row, or NULL for the rows that are actions.
static const char* row_value(const settings_t* s, uint8_t row, char* buf,
                             size_t buf_sz)
{
    switch (row) {
    case ROW_VOLUME:
        return VOL_NAMES[s->volume <= SETTINGS_VOL_OFF ? s->volume
                                                       : SETTINGS_VOL_OFF];
    case ROW_BRIGHT:
        snprintf(buf, buf_sz, "%u/8", (unsigned)bright_level(s->brightness));
        return buf;
    case ROW_PALETTE:
        return emu_get_palette_name(s->palette);
    default:
        return NULL;
    }
}

// ─── Drawing ────────────────────────────────────────────────────────────────

static void draw_row(const settings_t* s, uint8_t row, bool highlighted)
{
    char buf[16];
    const char* value = row_value(s, row, buf, sizeof(buf));
    int16_t y = (int16_t)(s->game_y + MENU_TOP + row * MENU_ROW_H);
    uint16_t bg = highlighted ? MENU_HL_BG : MENU_ROW_BG;

    tft.fillRect(s->game_x + 4, y, GAME_W - 8, MENU_ROW_H - 2, bg);
    tft.setTextColor(TFT_WHITE, bg);
    tft.setTextDatum(ML_DATUM);
    tft.drawString(ROW_LABELS[row], s->game_x + 8, y + MENU_ROW_H / 2, 2);
    if (value) {
        tft.setTextDatum(MR_DATUM);
        tft.drawString(value, s->game_x + GAME_W - 8, y + MENU_ROW_H / 2, 2);
    }
}

static void draw_menu(const settings_t* s, uint8_t cursor)
{
    uint8_t row;

    tft.fillRect(s->game_x, s->game_y, GAME_W, GAME_H, TFT_BLACK);
    tft.setTextDatum(MC_DATUM);
    tft.setTextColor(MENU_TITLE, TFT_BLACK);
    tft.drawString("PAUSED", s->game_x + GAME_W / 2, s->game_y + 18, 4);
    for (row = 0; row < MENU_ROWS; row++) {
        draw_row(s, row, row == cursor);
    }
}

static const char* class_name(enum boot_class_e cls)
{
    switch (cls) {
    case BOOT_CLASS_MENU:
        return "Menu cart";
    case BOOT_CLASS_WILD:
        return "Wildcard";
    case BOOT_CLASS_GAME:
        return "Game cart";
    default:
        return "Blank";
    }
}

// UNKNOWN means the cartridge was never authenticated against, which happens
// whenever the outcome did not depend on the answer. A configuration byte
// that is not the open value still says it is protected, which is all this
// page claims to know.
static const char* auth_name(const menu_cart_info_t* info)
{
    switch (info->auth) {
    case BOOT_AUTH_OPEN:
        return "Open";
    case BOOT_AUTH_OURS:
        return "Ours";
    case BOOT_AUTH_FOREIGN:
        return "Foreign";
    default:
        return info->auth0 == MENU_AUTH0_OPEN ? "Open" : "Protected";
    }
}

/* Both media files are 96x96 raw RGB565, the same imaging run the writer's
 * detail page reads — one file per ROM under /art and /shot. */
#define CART_ART_W  96
#define CART_ART_H  96
#define CART_ART_PX (CART_ART_W * CART_ART_H)

/* Between the two images, and under the image band. */
#define CART_ART_GAP 8

// The file name out of the stored path: /art, /shot and the catalog are all
// keyed by it, and it is the only key any of them accepts.
static const char* rom_basename(const char* path)
{
    const char* slash = strrchr(path, '/');

    return slash ? slash + 1 : path;
}

// Read-only, and B is the only way out. This is the running cartridge's own
// page — its cover, its gameplay snapshot, its title and its description —
// and there is no way from here to any other cartridge.
//
// The catalog is streamed, never indexed: catalog_index_t is about 19 KB and
// catalog.h bars it from any translation unit the emulator links at game
// time, which this one is. catalog_find() and catalog_read_desc() walk the
// same lines with the same parser for one line buffer apiece.
//
// Everything below the title is optional and independently so. A card with no
// /art, a title with no catalog entry, and a description that failed to read
// each drop out on their own and give their space back, because media
// coverage across the library is partial by design.
static void draw_cart_info(const settings_t* s, const menu_cart_info_t* info)
{
    char line[128];
    char desc[CATALOG_DESC_MAX];
    catalog_reader_t cat;
    catalog_entry_t entry;
    const int16_t x = (int16_t)(s->game_x + 8);
    const int16_t max_w = GAME_W - 16;
    const int16_t foot_y = (int16_t)(s->game_y + GAME_H - 18);
    const int16_t status_y = (int16_t)(foot_y - 10);
    int16_t y = (int16_t)(s->game_y + 8);
    const char* name;
    bool have_entry = false;
    bool have_art = false;
    uint16_t* px;

    tft.fillRect(s->game_x, s->game_y, GAME_W, GAME_H, TFT_BLACK);
    tft.setTextDatum(TL_DATUM);
    tft.setTextColor(TFT_WHITE, TFT_BLACK);

    if (info) {
        name = rom_basename(info->path);
        desc[0] = '\0';

        if (sd_catalog_reader(&cat)
            && catalog_find(&cat, name, &entry) == CATALOG_OK) {
            have_entry = true;
            if (catalog_read_desc(&cat, entry.offset, desc, sizeof(desc))
                != CATALOG_OK) {
                desc[0] = '\0';
            }
        }

        // The catalog's title when the card knows this game, the cartridge
        // header's otherwise — the header is always answerable and is never
        // the nicer of the two. Two rows because a catalog title runs to 47
        // characters; display_draw_wrapped returns the rows it actually used,
        // so a short title costs one and the description gets the other.
        y = display_draw_wrapped(have_entry ? entry.title : info->title, x, y,
                                 max_w, 2, 2);
        y = (int16_t)(y + 2);

        // One buffer, filled twice: this page is drawn once per visit, and
        // the cover is on the panel before the snapshot is read over it. The
        // pipeline is paused and the bus is ours, so the 18 KB is transient
        // against the emulator's own heap rather than a static reservation.
        // setSwapBytes(true) is the resting state display_bus_acquire()
        // leaves in force, and the .565 files are little-endian, so there is
        // no swap to do here.
        px = (uint16_t*)malloc(CART_ART_PX * sizeof(uint16_t));
        if (px) {
            if (sd_media_read(ART_PATH, name, px, CART_ART_PX)) {
                tft.pushImage(x, y, CART_ART_W, CART_ART_H, px);
                have_art = true;
            }
            if (sd_media_read(SHOT_PATH, name, px, CART_ART_PX)) {
                tft.pushImage((int16_t)(x + CART_ART_W + CART_ART_GAP), y,
                              CART_ART_W, CART_ART_H, px);
                have_art = true;
            }
            free(px);
        }
        if (have_art) {
            y = (int16_t)(y + CART_ART_H + CART_ART_GAP);
        }

        // Which of the three lookups the card answered. Media coverage is
        // partial across the library, so a sparse page is usually the card's
        // state and not a fault here — this is the line that tells the two
        // apart without opening the card on a computer.
        Serial.printf("[INFO] '%s' catalog=%d art=%d desc=%d\n", name,
                      (int)have_entry, (int)have_art, (int)(desc[0] != '\0'));

        // Whatever is left between the art and the status line. A taller
        // geometry spends it on more of the description rather than on gap.
        if (desc[0]) {
            int16_t room = (int16_t)(status_y - y - 2);
            int16_t pitch = (int16_t)(tft.fontHeight(1) + 2);
            if (room >= pitch) {
                display_draw_wrapped(desc, x, y, max_w, (uint8_t)(room / pitch),
                                     1);
            }
        } else if (!have_art) {
            // Nothing but the title would leave the page looking broken
            // rather than sparse, so name the file that is running instead.
            display_draw_wrapped(info->path, x, y, max_w, 4, 1);
        }
    }

    // The one cartridge fact that lives nowhere else. The diagnostic screen's
    // inspector reads AUTH0 and the access byte, but it never authenticates,
    // so Ours and Foreign are resolved only on the boot that used the cart —
    // here. Dim, one row, and below the page's own content: this is a
    // footnote about the cartridge, not the subject of the page.
    tft.setTextColor(MENU_DIM, TFT_BLACK);
    if (info && info->valid) {
        snprintf(line, sizeof(line), "Tag: %s", auth_name(info));
    } else {
        snprintf(line, sizeof(line), "No tag read (bench build)");
    }
    tft.drawString(line, x, status_y, 1);

    tft.setTextDatum(TL_DATUM);
    tft.drawString("B: back", x, foot_y, 2);
}

// ─── Input ──────────────────────────────────────────────────────────────────

// Left and Right on a value row; dir is +1 for Right. Backlight and palette
// are applied as they change, because seeing the effect is the point of
// adjusting them here; volume is stored for the audio path to read. Returns
// whether the value actually moved, so one held against an end of its range
// costs no redraw.
static bool adjust(settings_t* s, uint8_t row, int8_t dir)
{
    uint8_t next;

    switch (row) {
    case ROW_VOLUME:
        // Louder is the lower index, so Right counts down — the same
        // direction the Select combo applies outside the menu.
        next = combo_step_u8(s->volume, (int8_t)-dir, SETTINGS_VOL_HIGH,
                             SETTINGS_VOL_OFF, 1);
        if (next == s->volume) {
            return false;
        }
        s->volume = next;
        return true;

    case ROW_BRIGHT:
        next = combo_step_u8(s->brightness, dir, BL_MIN, 255, BL_STEP);
        if (next == s->brightness) {
            return false;
        }
        s->brightness = next;
        display_set_backlight(s->brightness);
        return true;

    case ROW_PALETTE:
        next = (uint8_t)((s->palette + (dir > 0 ? 1 : PALETTE_COUNT - 1))
                         % PALETTE_COUNT);
        if (next == s->palette) {
            return false;
        }
        s->palette = next;
        emu_set_palette(s->palette);
        return true;

    default:
        return false;
    }
}

// Hold on the info page until B, then leave with the buttons all up so the
// menu underneath cannot read the same press again.
static void wait_for_back()
{
    wait_release();
    for (;;) {
        button_update();
        if (button_get_buttons() & GB_BTN_B) {
            wait_release();
            return;
        }
        delay(MENU_POLL_MS);
    }
}

enum menu_result_e menu_open(settings_t* s, const menu_cart_info_t* info)
{
    list_state_t ls;
    uint16_t prev = 0;

    if (!s) {
        return MENU_RESUME;
    }
    list_init(&ls, MENU_ROWS, MENU_ROWS);
    draw_menu(s, (uint8_t)list_cursor(&ls));
    wait_release();

    for (;;) {
        uint32_t now = millis();
        uint16_t word;
        uint8_t cursor = (uint8_t)list_cursor(&ls);
        bool left;
        bool right;

        button_update();
        word = button_get_buttons();

        // Only Up and Down reach the list machine. Left and Right are this
        // screen's value keys, and feeding them in would page the highlight
        // instead of adjusting anything.
        if (list_input(&ls, (uint8_t)(word & (COMBO_BTN_UP | COMBO_BTN_DOWN)),
                       now) == LIST_EVENT_MOVED) {
            draw_row(s, cursor, false);
            cursor = (uint8_t)list_cursor(&ls);
            draw_row(s, cursor, true);
        }

        left = (word & GB_BTN_LEFT) && !(prev & GB_BTN_LEFT);
        right = (word & GB_BTN_RIGHT) && !(prev & GB_BTN_RIGHT);
        if ((left || right) && adjust(s, cursor, right ? +1 : -1)) {
            draw_row(s, cursor, true);
        }

        if ((word & GB_BTN_A) && !(prev & GB_BTN_A)) {
            if (cursor == ROW_RESUME) {
                wait_release();
                return MENU_RESUME;
            }
            if (cursor == ROW_RESET) {
                wait_release();
                return MENU_RESET;
            }
            if (cursor == ROW_INFO) {
                draw_cart_info(s, info);
                wait_for_back();
                draw_menu(s, cursor);
            }
            // A on a value row does nothing: Left and Right are its keys.
        }
        if ((word & GB_BTN_B) && !(prev & GB_BTN_B)) {
            wait_release();
            return MENU_RESUME;
        }

        prev = word;
        delay(MENU_POLL_MS);
    }
}
