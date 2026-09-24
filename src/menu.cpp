#include "menu.h"
#include "display.h"
#include "button_input.h"
#include "emulator_bridge.h"
#include "hw_config.h"
#include "render_config.h"
#include "input/combo.h"
#include "ui/list.h"
#include "ui/picker_draw.h"
#include "sd_manager.h"
#include "manual_view.h"
#include "cart/catalog.h"
#include <Arduino.h>
#include <stdlib.h>

// The in-game pause menu: a scrolling list inside the game window, worked
// with the D-pad. The highlight is the pure list machine in gbcore; everything here is
// drawing and the four side effects the rows have.
//
// One expander read every MENU_POLL_MS. That is longer than the input
// module's debounce window, so two successive samples are already stable and
// the edge detection below needs no filter of its own.
#define MENU_POLL_MS 16

// More entries than fit, so the list scrolls a window of MENU_VISIBLE of
// them. 7 x 26 + 40 = 222, inside GAME_H (240).
#define MENU_ENTRIES 9
#define MENU_VISIBLE 7
#define MENU_ROW_H 26
#define MENU_TOP   40   /* the title band above the first row */

// The fork's row colour, kept so this looks like the rest of the UI. The
// highlighted row is inverted, bold black on a white bar, because the fork's
// near-black highlight was hard to find at a glance. The dimmed grey is a
// bench value, not derived.
#define MENU_ROW_BG 0x1082
#define MENU_HL_BG  TFT_WHITE
#define MENU_HL_FG  TFT_BLACK
#define MENU_HL_DIM 0x6B4D
#define MENU_TITLE  0xFFE0
#define MENU_DIM    0x7BEF

enum menu_row_e {
    ROW_RESUME = 0,
    ROW_STATE,
    ROW_MANUAL,
    ROW_INFO,
    ROW_PALETTE,
    ROW_VOLUME,
    ROW_BRIGHT,
    ROW_HOTKEYS,
    ROW_RESET,
};

static const char* const ROW_LABELS[MENU_ENTRIES] = {
    "Resume",
    "Save State",
    "Game Manual",
    "Cart Info",
    "Color Palette",
    "Volume",
    "Brightness",
    "Hotkeys",
    "Reset",
};

// Whether the running cartridge has a manual on the card, settled once each
// time the menu opens. Without one the row stays in place, dimmed and inert,
// so the menu is the same shape for every game. Save State follows the same
// rule for a core that has no save states.
static bool manual_available;

// Indexed by the stored volume, which runs Off to High.
static const char* const VOL_NAMES[SETTINGS_VOL_HIGH + 1] = {
    "Off",
    "Low",
    "Med",
    "High",
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
        return VOL_NAMES[s->volume <= SETTINGS_VOL_HIGH ? s->volume
                                                        : SETTINGS_VOL_HIGH];
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

// One bar at y: the label left, the value (if any) right. A dimmed bar is
// unavailable and stays thin even highlighted, so it still reads that way.
static void draw_bar(const settings_t* s, int16_t y, const char* label,
                     const char* value, bool highlighted, bool off)
{
    uint16_t bg = highlighted ? MENU_HL_BG : MENU_ROW_BG;
    uint16_t fg;

    if (highlighted) {
        fg = off ? MENU_HL_DIM : MENU_HL_FG;
    } else {
        fg = off ? MENU_DIM : TFT_WHITE;
    }

    // Bold is the same glyphs struck twice a pixel apart, so the text is
    // drawn transparent over the bar the fill already laid down; an opaque
    // second pass would wipe the first one's edge.
    const int16_t bold = (highlighted && !off) ? 1 : 0;
    const int16_t text_y = y + MENU_ROW_H / 2;

    tft.fillRect(s->game_x + 4, y, GAME_W - 8, MENU_ROW_H - 2, bg);
    tft.setTextColor(fg);
    for (int16_t dx = 0; dx <= bold; dx++) {
        tft.setTextDatum(ML_DATUM);
        tft.drawString(label, s->game_x + 8 + dx, text_y, 2);
        if (value) {
            tft.setTextDatum(MR_DATUM);
            tft.drawString(value, s->game_x + GAME_W - 8 - dx, text_y, 2);
        }
    }
}

// first is the list window's top entry: rows are placed relative to it.
static void draw_row(const settings_t* s, uint16_t first, uint8_t row,
                     bool highlighted)
{
    char buf[16];
    const char* value = row_value(s, row, buf, sizeof(buf));
    const bool off = row == ROW_MANUAL && !manual_available;
    int16_t y = (int16_t)(s->game_y + MENU_TOP + (row - first) * MENU_ROW_H);
    const char* label = ROW_LABELS[row];

    if (off) {
        label = "Game Manual (Unavailable)";
    }
    draw_bar(s, y, label, value, highlighted, off);
}

// The rows in the window only; the title band is left alone, so a scroll
// redraws without blanking the screen.
static void draw_rows(const settings_t* s, const list_state_t* ls)
{
    uint16_t row;

    for (row = list_first(ls); row < MENU_ENTRIES && list_visible(ls, row);
         row++) {
        draw_row(s, list_first(ls), (uint8_t)row, row == list_cursor(ls));
    }
}

static void draw_menu(const settings_t* s, const list_state_t* ls)
{
    tft.fillRect(s->game_x, s->game_y, GAME_W, GAME_H, TFT_BLACK);
    tft.setTextDatum(MC_DATUM);
    tft.setTextColor(MENU_TITLE, TFT_BLACK);
    tft.drawString("PAUSED", s->game_x + GAME_W / 2, s->game_y + 18, 4);
    draw_rows(s, ls);
}

// The fixed combos, for reading only: nothing here is editable and nothing
// comes from NVS. Every line must match a combo_event_e in input/combo.h —
// describe nothing that module does not implement.
static const char* const HOTKEYS[][2] = {
    { "Select + Start", "Menu" },
    { "Select + Up/Down", "Volume" },
    { "Select + Right/Left", "Brightness" },
    { "Select + A + B", "Fast-forward" },
};

static void draw_hotkeys(const settings_t* s)
{
    const int16_t x = (int16_t)(s->game_x + 8);
    const int16_t right = (int16_t)(s->game_x + GAME_W - 8);
    int16_t y = (int16_t)(s->game_y + MENU_TOP + MENU_ROW_H / 2);

    tft.fillRect(s->game_x, s->game_y, GAME_W, GAME_H, TFT_BLACK);
    tft.setTextDatum(MC_DATUM);
    tft.setTextColor(MENU_TITLE, TFT_BLACK);
    tft.drawString("HOTKEYS", s->game_x + GAME_W / 2, s->game_y + 18, 4);
    tft.setTextColor(TFT_WHITE, TFT_BLACK);
    for (size_t i = 0; i < sizeof(HOTKEYS) / sizeof(HOTKEYS[0]); i++) {
        tft.setTextDatum(ML_DATUM);
        tft.drawString(HOTKEYS[i][0], x, y, 2);
        tft.setTextDatum(MR_DATUM);
        tft.drawString(HOTKEYS[i][1], right, y, 2);
        y = (int16_t)(y + MENU_ROW_H);
    }
    tft.setTextDatum(TL_DATUM);
    tft.drawString("B: Back", x, (int16_t)(s->game_y + GAME_H - 18), 2);
}

/* Both media files are 96x96 raw RGB565, the same imaging run the writer's
 * detail page reads — one file per ROM under /art and /shot. */
#define CART_ART_W  96
#define CART_ART_H  96
#define CART_ART_PX (CART_ART_W * CART_ART_H)

/* Between the two images, and under the image band. */
#define CART_ART_GAP 8

/* Rows per band. 96 x 16 x 2 is 3,072 bytes, against a largest contiguous
 * block of about 15 KB at game time — the whole 18,432-byte image is refused
 * outright there, which is why the image arrives in bands at all. */
#define CART_BAND_ROWS 16
#define CART_BAND_PX   (CART_ART_W * CART_BAND_ROWS)

// Where one image's bands are going: its left edge, its top and its width.
typedef struct cart_blit_s {
    int16_t x;
    int16_t y;
    int16_t w;
} cart_blit_t;

// setSwapBytes(true) is the resting state display_bus_acquire() leaves in
// force and the .565 files are little-endian, so there is no swap to do.
static void blit_band(void* ctx, const uint16_t* px, size_t row0, size_t rows)
{
    const cart_blit_t* at = (const cart_blit_t*)ctx;

    tft.pushImage(at->x, (int16_t)(at->y + row0), at->w, (int16_t)rows,
                  (uint16_t*)px);
}

// The file name out of the stored path: /art, /shot and the catalog are all
// keyed by it, and it is the only key any of them accepts.
static const char* rom_basename(const char* path)
{
    const char* slash = strrchr(path, '/');

    return slash ? slash + 1 : path;
}

// The description band under the cover: `rows` lines of font 1 starting at
// wrapped line `first`. Cleared and redrawn alone on a scroll — the cover
// above streams from the card and is never drawn twice.
typedef struct cart_band_s {
    const char* text;
    int16_t x;
    int16_t y;
    int16_t w;
    int16_t pitch;
    uint8_t cols;
    uint16_t rows;
    uint16_t lines;
    uint16_t first;
} cart_band_t;

static void draw_band(const cart_band_t* b)
{
    char line[PICKER_DESC_LINE_MAX];

    tft.fillRect(b->x, b->y, b->w, (int16_t)(b->rows * b->pitch), TFT_BLACK);
    tft.setTextDatum(TL_DATUM);
    tft.setTextColor(TFT_WHITE, TFT_BLACK);
    for (uint16_t i = 0; i < b->rows; i++) {
        if (!picker_desc_line(b->text, b->cols, (uint16_t)(b->first + i), line,
                              sizeof(line))) {
            break;
        }
        tft.drawString(line, b->x, (int16_t)(b->y + i * b->pitch), 1);
    }
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
//
// The description is the full text from /desc, else the catalog's blurb, in
// one DESC_MAX heap buffer that `*desc` hands back for the caller to free —
// the band draws from it for as long as the page is up. `band` is filled in
// with rows == 0 when there is nothing to scroll through.
static void draw_cart_info(const settings_t* s, const menu_cart_info_t* info,
                           char** desc, cart_band_t* band)
{
    catalog_reader_t cat;
    catalog_entry_t entry;
    const int16_t x = (int16_t)(s->game_x + 8);
    const int16_t max_w = GAME_W - 16;
    const int16_t foot_y = (int16_t)(s->game_y + GAME_H - 18);
    int16_t y = (int16_t)(s->game_y + 8);
    const char* name;
    char* text = NULL;
    bool have_entry = false;
    bool have_full = false;
    bool art_ok = false;
    bool shot_ok = false;
    uint16_t* px;

    memset(band, 0, sizeof(*band));
    *desc = NULL;

    tft.fillRect(s->game_x, s->game_y, GAME_W, GAME_H, TFT_BLACK);
    tft.setTextDatum(TL_DATUM);
    tft.setTextColor(TFT_WHITE, TFT_BLACK);

    if (info) {
        name = rom_basename(info->path);

        have_entry = sd_catalog_reader(&cat)
                     && catalog_find(&cat, name, &entry) == CATALOG_OK;

        // 4 KB for the whole of the longest description, against a largest
        // free block of about 15 KB at game time. Refused, the page loses its
        // description and keeps everything else.
        text = (char*)malloc(DESC_MAX);
        if (text) {
            text[0] = '\0';
            have_full = sd_desc_read(name, text, DESC_MAX);
            if (!have_full
                && (!have_entry
                    || catalog_read_desc(&cat, entry.offset, text, DESC_MAX)
                           != CATALOG_OK)) {
                text[0] = '\0';
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

        // One band buffer, both images through it in turn. The bands go
        // straight to the panel as they are read, so nothing here ever holds
        // a whole 96x96 image — which at game time cannot be allocated.
        // setSwapBytes(true) is the resting state display_bus_acquire()
        // leaves in force, and the .565 files are little-endian, so there is
        // no swap to do here.
        px = (uint16_t*)malloc(CART_BAND_PX * sizeof(uint16_t));
        if (px) {
            cart_blit_t at = { x, y, CART_ART_W };

            art_ok = sd_media_stream(ART_PATH, name, px, CART_ART_W,
                                     CART_ART_H, CART_BAND_ROWS, blit_band,
                                     &at);
            at.x = (int16_t)(x + CART_ART_W + CART_ART_GAP);
            shot_ok = sd_media_stream(SHOT_PATH, name, px, CART_ART_W,
                                      CART_ART_H, CART_BAND_ROWS, blit_band,
                                      &at);
            free(px);
        }
        if (art_ok || shot_ok) {
            y = (int16_t)(y + CART_ART_H + CART_ART_GAP);
        }

        // What the card answered, one field per thing that can independently
        // fail. Media coverage is partial across the library, so a sparse
        // page is usually the card's state and not a fault here — but a
        // failed allocation looks identical on the panel, which is why both
        // buffers are reported separately from the reads. desc=2 is the full
        // text, 1 the catalog's blurb.
        Serial.printf("[INFO] '%s' catalog=%d desc=%d dbuf=%d buf=%d art=%d "
                      "shot=%d\n",
                      name, (int)have_entry,
                      have_full ? 2 : (int)(text && text[0] != '\0'),
                      (int)(text != NULL), (int)(px != NULL), (int)art_ok,
                      (int)shot_ok);
        if (!art_ok || !shot_ok) {
            // The exact path that came up empty, so the card can be checked
            // against it directly rather than by guessing at the stem rule.
            Serial.printf("[INFO] wanted %s/<stem>%s and %s/<stem>%s\n",
                          ART_PATH, ART_SUFFIX, SHOT_PATH, ART_SUFFIX);
        }

        // Whatever is left between the art and the footer, as many lines as
        // fit. A taller geometry spends it on more of the description rather
        // than on gap.
        if (text && text[0]) {
            int16_t room = (int16_t)(foot_y - y - 2);

            band->text = text;
            band->x = x;
            band->y = y;
            band->w = max_w;
            band->pitch = (int16_t)(tft.fontHeight(1) + 2);
            band->cols = (uint8_t)(max_w / UI_FONT_SMALL_ADV);
            band->rows = room > 0 ? (uint16_t)(room / band->pitch) : 0;
            band->lines = picker_desc_lines(text, band->cols);
            if (band->rows) {
                draw_band(band);
            }
        }
        *desc = text;
    }

    // Nothing about the tag or the file: this page is for the player, and
    // neither means anything to them. The serial log above carries both.
    tft.setTextDatum(TL_DATUM);
    tft.drawString("B: Back", x, foot_y, 2);
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
        // Right is louder, the same direction brightness moves and the
        // Select combo applies outside the menu.
        next = combo_step_u8(s->volume, dir, SETTINGS_VOL_OFF,
                             SETTINGS_VOL_HIGH, 1);
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
        // PALETTE_UI_COUNT, not PALETTE_COUNT: Auto is the entry past the end
        // of the table, and it has to be reachable both ways or a builder who
        // picks a colour scheme for a cartridge can never put it back.
        next = (uint8_t)((s->palette + (dir > 0 ? 1 : PALETTE_UI_COUNT - 1))
                         % PALETTE_UI_COUNT);
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

// Hold on a page until B, then leave with the buttons all up so the menu
// underneath cannot read the same press again. Up and Down scroll the band a
// line at a time — at once on the press, then after
// COMBO_REPEAT_DELAY_MS every COMBO_REPEAT_MS while held, the writer's detail
// page's rule — and do nothing when the description fits or, as on the
// Hotkeys page, there is no band at all.
static void page_input(cart_band_t* band)
{
    uint16_t last = band->lines > band->rows
                        ? (uint16_t)(band->lines - band->rows)
                        : 0;
    uint8_t held = 0;
    uint32_t due = 0;

    wait_release();
    for (;;) {
        uint32_t now = millis();
        uint16_t word;
        uint8_t dir;
        bool step = false;

        button_update();
        word = button_get_buttons();
        if (word & GB_BTN_B) {
            wait_release();
            return;
        }

        // Exactly one direction, or nothing: the list's fumble rule.
        dir = (uint8_t)(word & (COMBO_BTN_UP | COMBO_BTN_DOWN));
        if (dir != COMBO_BTN_UP && dir != COMBO_BTN_DOWN) {
            held = 0;
        } else if (dir != held) {
            held = dir;
            due = now + (uint32_t)COMBO_REPEAT_DELAY_MS;
            step = true;
        } else if ((int32_t)(now - due) >= 0) {
            due = now + (uint32_t)COMBO_REPEAT_MS;
            step = true;
        }

        if (step && last) {
            uint16_t first = band->first;

            if (dir == COMBO_BTN_DOWN && first < last) {
                first++;
            } else if (dir == COMBO_BTN_UP && first > 0) {
                first--;
            }
            if (first != band->first) {
                band->first = first;
                draw_band(band);
            }
        }
        delay(MENU_POLL_MS);
    }
}

// ─── Save State ─────────────────────────────────────────────────────────────
// One state per game, kept beside its battery save. Because a state carries
// the cartridge RAM, loading one rewinds the battery save too, so a load
// always asks first, and so does a save that would replace a state.

// Where the screen's bars start: under the title band and the snapshot.
#define STATE_BARS_Y (MENU_TOP + EMU_THUMB_H + 12)
// 80 x 24 x 2 is 3,840 bytes; three bands make the snapshot.
#define STATE_BAND_ROWS 24

// Up and Down between n bars at STATE_BARS_Y, A to pick one, B to back out.
// Returns the bar picked, or -1 for B. A dimmed bar can be highlighted but
// not picked. Starts with every button up, so a press still held from the
// screen before cannot pick anything here.
static int choose(const settings_t* s, const char* const* labels,
                  const bool* off, uint8_t n, uint8_t cursor)
{
    const int16_t y0 = (int16_t)(s->game_y + STATE_BARS_Y);
    uint16_t prev = 0;

    for (uint8_t i = 0; i < n; i++) {
        draw_bar(s, (int16_t)(y0 + i * MENU_ROW_H), labels[i], NULL,
                 i == cursor, off && off[i]);
    }
    wait_release();
    for (;;) {
        uint16_t word;
        uint16_t press;
        uint8_t next = cursor;

        button_update();
        word = button_get_buttons();
        press = (uint16_t)(word & ~prev);
        prev = word;

        if ((press & GB_BTN_B) != 0) {
            wait_release();
            return -1;
        }
        if ((press & GB_BTN_A) != 0 && !(off && off[cursor])) {
            wait_release();
            return cursor;
        }
        if ((press & COMBO_BTN_UP) != 0 && cursor > 0) {
            next = (uint8_t)(cursor - 1);
        } else if ((press & COMBO_BTN_DOWN) != 0 && cursor + 1 < n) {
            next = (uint8_t)(cursor + 1);
        }
        if (next != cursor) {
            draw_bar(s, (int16_t)(y0 + cursor * MENU_ROW_H), labels[cursor],
                     NULL, false, off && off[cursor]);
            draw_bar(s, (int16_t)(y0 + next * MENU_ROW_H), labels[next],
                     NULL, true, off && off[next]);
            cursor = next;
        }
        delay(MENU_POLL_MS);
    }
}

static void draw_title(const settings_t* s, const char* title)
{
    tft.fillRect(s->game_x, s->game_y, GAME_W, GAME_H, TFT_BLACK);
    tft.setTextDatum(MC_DATUM);
    tft.setTextColor(MENU_TITLE, TFT_BLACK);
    tft.drawString(title, s->game_x + GAME_W / 2, s->game_y + 18, 4);
}

// The question in the space the snapshot takes, then No and Yes. The cursor
// starts on No, so nothing is lost to a press that was not meant for this.
static bool confirm(const settings_t* s, const char* question)
{
    static const char* const LABELS[2] = { "No", "Yes" };

    draw_title(s, "SAVE STATE");
    tft.setTextDatum(TC_DATUM);
    tft.setTextColor(TFT_WHITE, TFT_BLACK);
    display_draw_wrapped(question, (int16_t)(s->game_x + GAME_W / 2),
                         (int16_t)(s->game_y + MENU_TOP + 8), GAME_W - 32, 3,
                         2);
    return choose(s, LABELS, NULL, 2, 0) == 1;
}

static void notice(const settings_t* s, const char* msg)
{
    cart_band_t none = {};

    draw_title(s, "SAVE STATE");
    tft.setTextDatum(MC_DATUM);
    tft.setTextColor(TFT_WHITE, TFT_BLACK);
    tft.drawString(msg, s->game_x + GAME_W / 2,
                   s->game_y + MENU_TOP + EMU_THUMB_H / 2, 2);
    tft.setTextDatum(TL_DATUM);
    tft.drawString("B: Back", s->game_x + 8, s->game_y + GAME_H - 18, 2);
    page_input(&none);
}

// The state goes to a temp file and is renamed into place only once it is
// whole, so a failed save leaves the previous state as it was. The snapshot
// follows the state, never the other way round: a snapshot that failed only
// costs the picture.
static bool state_save(const char* rom_path)
{
    char vfs[STATE_PATH_MAX];
    char tmp[STATE_PATH_MAX + 4];
    char path[STATE_PATH_MAX];

    if (!sd_get_state_path(rom_path, STATE_SUFFIX, true, vfs, sizeof(vfs))
        || !sd_get_state_path(rom_path, STATE_SUFFIX, false, path,
                              sizeof(path))) {
        return false;
    }
    snprintf(tmp, sizeof(tmp), "%s%s", vfs, SAVE_TMP_SUFFIX);
    if (!emu_state_save(tmp)) {
        remove(tmp);
        return false;
    }
    if (!sd_commit_tmp(path)) {
        return false;
    }
    if (sd_get_state_path(rom_path, THUMB_SUFFIX, true, vfs, sizeof(vfs))
        && !emu_state_thumb_save(vfs)) {
        Serial.println("[STATE] snapshot not written");
    }
    return true;
}

static bool state_load(const char* rom_path)
{
    char vfs[STATE_PATH_MAX];

    return sd_get_state_path(rom_path, STATE_SUFFIX, true, vfs, sizeof(vfs))
           && emu_state_load(vfs);
}

// The snapshot centred under the title, or a line saying there is none.
static void draw_state(const settings_t* s, const char* rom_path, bool have)
{
    const int16_t x = (int16_t)(s->game_x + (GAME_W - EMU_THUMB_W) / 2);
    const int16_t y = (int16_t)(s->game_y + MENU_TOP);
    bool shown = false;

    draw_title(s, "SAVE STATE");
    if (have) {
        uint16_t* px = (uint16_t*)malloc(EMU_THUMB_W * STATE_BAND_ROWS
                                         * sizeof(uint16_t));
        cart_blit_t at = { x, y, EMU_THUMB_W };

        if (px) {
            shown = sd_thumb_stream(rom_path, px, EMU_THUMB_W, EMU_THUMB_H,
                                    STATE_BAND_ROWS, blit_band, &at);
            free(px);
        }
    }
    if (!shown) {
        tft.setTextDatum(MC_DATUM);
        tft.setTextColor(have ? TFT_WHITE : MENU_DIM, TFT_BLACK);
        tft.drawString(have ? "No snapshot" : "No saved state",
                       s->game_x + GAME_W / 2, y + EMU_THUMB_H / 2, 2);
    }
}

// Returns true when a state was loaded, which ends the menu: the game is
// where the state left it and the only thing left to do is play it.
static bool state_screen(const settings_t* s, const menu_cart_info_t* info)
{
    static const char* const LABELS[3] = { "Save", "Load", "Back" };
    uint8_t cursor = 0;

    if (!info) {
        return false;
    }
    for (;;) {
        const bool have = sd_state_exists(info->path);
        const bool off[3] = { false, !have, false };
        int pick;

        draw_state(s, info->path, have);
        pick = choose(s, LABELS, off, 3, cursor);
        if (pick < 0 || pick == 2) {
            return false;
        }
        cursor = (uint8_t)pick;
        if (pick == 0) {
            if (have && !confirm(s, "Replace saved state?")) {
                continue;
            }
            if (!state_save(info->path)) {
                notice(s, "Save failed.");
            }
        } else {
            if (!confirm(s, "Load state? Progress since it was saved will "
                            "be lost.")) {
                continue;
            }
            if (state_load(info->path)) {
                return true;
            }
            notice(s, "Load failed.");
        }
    }
}

enum menu_result_e menu_open(settings_t* s, const menu_cart_info_t* info)
{
    list_state_t ls;
    uint16_t prev = 0;
    char path[ART_PATH_MAX];
    const char* name = info ? rom_basename(info->path) : NULL;

    if (!s) {
        return MENU_RESUME;
    }
    manual_available = name && sd_manual_path(name, path, sizeof(path));
    list_init(&ls, MENU_ENTRIES, MENU_VISIBLE);
    draw_menu(s, &ls);
    wait_release();

    for (;;) {
        uint32_t now = millis();
        uint16_t word;
        uint8_t cursor = (uint8_t)list_cursor(&ls);
        uint16_t first = list_first(&ls);
        bool left;
        bool right;

        button_update();
        word = button_get_buttons();

        // Only Up and Down reach the list machine. Left and Right are this
        // screen's value keys, and feeding them in would page the highlight
        // instead of adjusting anything.
        if (list_input(&ls, (uint8_t)(word & (COMBO_BTN_UP | COMBO_BTN_DOWN)),
                       now) == LIST_EVENT_MOVED) {
            if (list_first(&ls) != first) {
                draw_rows(s, &ls);
            } else {
                draw_row(s, first, cursor, false);
                draw_row(s, first, (uint8_t)list_cursor(&ls), true);
            }
            cursor = (uint8_t)list_cursor(&ls);
            first = list_first(&ls);
        }

        left = (word & GB_BTN_LEFT) && !(prev & GB_BTN_LEFT);
        right = (word & GB_BTN_RIGHT) && !(prev & GB_BTN_RIGHT);
        if ((left || right) && adjust(s, cursor, right ? +1 : -1)) {
            draw_row(s, first, cursor, true);
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
            if (cursor == ROW_STATE) {
                if (state_screen(s, info)) {
                    return MENU_RESUME;
                }
                draw_menu(s, &ls);
            }
            if (cursor == ROW_INFO) {
                char* desc;
                cart_band_t band;

                draw_cart_info(s, info, &desc, &band);
                page_input(&band);
                free(desc);
                draw_menu(s, &ls);
            }
            if (cursor == ROW_HOTKEYS) {
                cart_band_t none = {};

                draw_hotkeys(s);
                page_input(&none);
                draw_menu(s, &ls);
            }
            if (cursor == ROW_MANUAL && manual_available) {
                // The reader waits for every button to be up before it
                // returns, so the B that closed it is not read here; a
                // manual that would not open leaves the menu as it was.
                manual_view_open(s, name);
                draw_menu(s, &ls);
            }
            // A on a value row does nothing: Left and Right are its keys.
            // Nor on an unavailable Game Manual or Save State.
        }
        if ((word & GB_BTN_B) && !(prev & GB_BTN_B)) {
            wait_release();
            return MENU_RESUME;
        }

        prev = word;
        delay(MENU_POLL_MS);
    }
}
