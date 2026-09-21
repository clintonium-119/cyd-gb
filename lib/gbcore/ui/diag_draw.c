#include <stdio.h>
#include <string.h>

#include "audio/mix.h"
#include "ui/diag_draw.h"

/*
 * Colours, matching the writer's layout and the in-game menu. Raw RGB565, as
 * everywhere else in this firmware — there is no named-colour layer to reach
 * for.
 */
#define COL_BG     0x0000
#define COL_ROW_BG 0x1082
#define COL_TITLE  0xFFE0
#define COL_DIM    0x7BEF
#define COL_TEXT   0xFFFF
#define COL_OK     0x07E0
#define COL_WARN   0xF800

/* The colour-bar page, left to right: a descending luminance ramp, so a
 * channel that is dead shows up as a bar that matches its neighbour. */
static const uint16_t BAR_COLORS[8] = {
    0xFFFF, /* white   */
    0xFFE0, /* yellow  */
    0x07FF, /* cyan    */
    0x07E0, /* green   */
    0xF81F, /* magenta */
    0xF800, /* red     */
    0x001F, /* blue    */
    0x0000, /* black   */
};

#define BAR_COUNT 8

/* Peanut-GB's 12-colour pixel byte: bits 5-4 pick the ramp (BG is 0x20) and
 * bits 1-0 pick the shade, which is what palette_build_lut() indexes by. The
 * checkerboard wants the background ramp's two ends. */
#define LUT_BG_DARKEST 0x20
#define LUT_BG_LIGHTEST 0x23

/* Joypad bit order, so the buttons page reads down the pad rather than up the
 * register. The GPA number beside each comes from the binding's snapshot. */
static const char* const BTN_LABELS[8] = {
    "Up", "Down", "Left", "Right", "Start", "Select", "A", "B",
};

/* COMBO_BTN_* values for the same eight, mirrored here for the same reason
 * the combo module mirrors them: this file includes no firmware header. */
static const uint8_t BTN_BITS[8] = {
    0x04, /* Up     */
    0x08, /* Down   */
    0x02, /* Left   */
    0x01, /* Right  */
    0x80, /* Start  */
    0x40, /* Select */
    0x10, /* A      */
    0x20, /* B      */
};

/* Same four names the in-game menu uses for the same four states. */
static const char* const VOL_NAMES[4] = { "High", "Med", "Low", "Off" };

static const char* const PATTERN_NAMES[DIAG_PATTERN_COUNT] = {
    "Colour bars", "Border", "Checkerboard",
};

static const char* const NFC_STATE_NAMES[] = {
    "not scanned", "reader not answering", "no tag", "one tag",
    "more than one tag", "read error",
};

/* The four classes the boot classifier reports, in its own order. */
static const char* const CLASS_NAMES[4] = { "blank", "MENU", "WILD", "game" };

/* One footer line per page: what the buttons do here. Paging is named on
 * every one of them, because it is the only way off a page. */
static const char* const FOOTERS[DIAG_PAGE_COUNT] = {
    "Select+L/R: page",
    "Select+L/R: page",
    "A: scan   Select+L/R: page",
    "Select+L/R: page",
    "A: tone   Up/Down: volume   Select+L/R: page",
    "Up/Down: pattern   Select+L/R: page",
    "D-pad: move   A: save   B: default",
    "Start: count   D-pad: porch   A: save   B: default",
    "Up/Down: frameskip   Select+L/R: page",
};

/* ─── geometry ────────────────────────────────────────────────────────────── */

int diag_layout(int16_t w, int16_t h, diag_layout_t* out)
{
    int16_t body_h;

    if (out == NULL) {
        return DIAG_ERR_ARGS;
    }
    memset(out, 0, sizeof(*out));

    if (w <= 0 || h <= 0) {
        return DIAG_ERR_ARGS;
    }

    out->w = w;
    out->h = h;
    /* Two pixels under the header's rule, so a body row's box never touches
     * it. */
    out->body_y = (int16_t)(DIAG_HEADER_H + 2);

    body_h = (int16_t)(h - out->body_y - 2);
    if (body_h < (int16_t)(DIAG_MIN_ROWS * DIAG_ROW_H)) {
        return DIAG_ERR_ARGS;
    }
    out->rows = (uint8_t)(body_h / DIAG_ROW_H);

    /* Twelve font-1 columns: "more than one" is the longest label any page
     * puts in the left column, and 12 x 6 = 72 px holds it. */
    out->label_w = (int16_t)(12 * UI_FONT_SMALL_ADV);
    /* A 4 px margin either side of the pair. */
    out->col_w = (int16_t)(w - out->label_w - 8);
    if (out->col_w <= 0) {
        return DIAG_ERR_ARGS;
    }

    out->bar_w = (int16_t)(w / BAR_COUNT);
    out->footer_y = (int16_t)(h - DIAG_ROW_H - 2);

    return DIAG_OK;
}

/* ─── small helpers ───────────────────────────────────────────────────────── */

static int16_t row_y(const diag_layout_t* g, uint8_t row)
{
    return (int16_t)(g->body_y + row * DIAG_ROW_H);
}

/* One "Label   value" row. Both boxes are one font-1 line, so the pair
 * occupies exactly DIAG_ROW_H and rows stack without arithmetic at the call
 * site. */
static void kv_row(const ui_canvas_t* cv, const diag_layout_t* g, uint8_t row,
                   const char* label, const char* value, uint16_t fg)
{
    int16_t y = row_y(g, row);

    cv->text(cv->ctx, label, 4, y, g->label_w, 1, UI_FONT_SMALL,
             UI_ALIGN_LEFT, COL_DIM, COL_BG);
    cv->text(cv->ctx, value, (int16_t)(4 + g->label_w), y, g->col_w, 1,
             UI_FONT_SMALL, UI_ALIGN_LEFT, fg, COL_BG);
}

/* A whole-width line, for anything that has no label. */
static void full_row(const ui_canvas_t* cv, const diag_layout_t* g,
                     uint8_t row, const char* s, uint16_t fg)
{
    cv->text(cv->ctx, s, 4, row_y(g, row), (int16_t)(g->w - 8), 1,
             UI_FONT_SMALL, UI_ALIGN_LEFT, fg, COL_BG);
}

static void draw_header(const ui_canvas_t* cv, const diag_layout_t* g,
                        uint8_t page)
{
    const char* title = diag_page_title(page);
    char num[8];

    cv->fill(cv->ctx, 0, 0, g->w, g->h, COL_BG);
    cv->fill(cv->ctx, 0, 0, g->w, DIAG_HEADER_H, COL_ROW_BG);

    snprintf(num, sizeof(num), "%u/%u", (unsigned)(page + 1),
             (unsigned)DIAG_PAGE_COUNT);
    /* diag_page_title() answers NULL only past the last page, which cannot
     * reach here — but the canvas must never see a NULL string, so this
     * substitutes rather than trusting the caller. */
    cv->text(cv->ctx, (title != NULL) ? title : "?", 4, 0,
             (int16_t)(g->w - 48), 1, UI_FONT_ROW, UI_ALIGN_LEFT, COL_TITLE,
             COL_ROW_BG);
    cv->text(cv->ctx, num, (int16_t)(g->w - 44), 0, 40, 1, UI_FONT_ROW,
             UI_ALIGN_RIGHT, COL_DIM, COL_ROW_BG);

    cv->fill(cv->ctx, 0, DIAG_HEADER_H, g->w, 1, COL_DIM);
}

static void draw_footer(const ui_canvas_t* cv, const diag_layout_t* g,
                        uint8_t page)
{
    const char* s = (page < DIAG_PAGE_COUNT) ? FOOTERS[page] : "";

    cv->text(cv->ctx, s, 4, g->footer_y, (int16_t)(g->w - 8), 1,
             UI_FONT_SMALL, UI_ALIGN_LEFT, COL_DIM, COL_BG);
}

/* ─── pages ───────────────────────────────────────────────────────────────── */

static void page_buttons(const ui_canvas_t* cv, const diag_layout_t* g,
                         const diag_data_t* data)
{
    uint8_t i;

    for (i = 0; i < 8; i++) {
        int16_t y = row_y(g, i);
        bool down = (data->buttons & BTN_BITS[i]) != 0;
        char gpa[12];

        snprintf(gpa, sizeof(gpa), "GPA%u", (unsigned)data->gpa[i]);
        cv->text(cv->ctx, BTN_LABELS[i], 4, y, g->label_w, 1, UI_FONT_SMALL,
                 UI_ALIGN_LEFT, down ? COL_TEXT : COL_DIM, COL_BG);
        cv->text(cv->ctx, gpa, (int16_t)(4 + g->label_w), y, 48, 1,
                 UI_FONT_SMALL, UI_ALIGN_LEFT, COL_DIM, COL_BG);
        /* The box is the part a builder watches: one press, one row lights. */
        cv->fill(cv->ctx, (int16_t)(4 + g->label_w + 52), (int16_t)(y + 1), 16,
                 8, down ? COL_OK : COL_ROW_BG);
    }
}

static void page_sd(const ui_canvas_t* cv, const diag_layout_t* g,
                    const diag_data_t* data)
{
    char buf[40];

    kv_row(cv, g, 0, "Card", data->sd_ok ? "mounted" : "not mounted",
           data->sd_ok ? COL_OK : COL_WARN);

    snprintf(buf, sizeof(buf), "%u", (unsigned)data->rom_count);
    kv_row(cv, g, 1, "ROMs", buf, COL_TEXT);

    if (data->catalog_ok) {
        snprintf(buf, sizeof(buf), "%u entries",
                 (unsigned)data->catalog_count);
        kv_row(cv, g, 2, "Catalog", buf, COL_TEXT);
    } else {
        kv_row(cv, g, 2, "Catalog", "missing", COL_WARN);
    }

    if (data->sd_stats_ok) {
        snprintf(buf, sizeof(buf), "%lu MB of %lu MB",
                 (unsigned long)data->sd_used_mb,
                 (unsigned long)data->sd_total_mb);
        kv_row(cv, g, 3, "Used", buf, COL_TEXT);
    } else {
        kv_row(cv, g, 3, "Used", "size unavailable", COL_DIM);
    }
}

/* The first 48 of the 96 raw bytes, as four rows of 12 pairs. Twelve pairs is
 * 12 x 3 x UI_FONT_SMALL_ADV = 216 px, which fits the narrower window with
 * 24 px to spare. The first 48 bytes cover the TLV, the record header and the
 * start of whatever text the tag carries. */
#define HEX_COLS 12
#define HEX_ROWS 4

static void page_nfc(const ui_canvas_t* cv, const diag_layout_t* g,
                     const diag_data_t* data)
{
    char buf[64];
    uint8_t row;
    uint8_t r;
    uint8_t i;

    if (data->nfc_fw != 0) {
        snprintf(buf, sizeof(buf), "fw %u.%u",
                 (unsigned)((data->nfc_fw >> 16) & 0xFFu),
                 (unsigned)((data->nfc_fw >> 8) & 0xFFu));
        kv_row(cv, g, 0, "Reader", buf, COL_OK);
    } else {
        kv_row(cv, g, 0, "Reader", "not answering", COL_WARN);
    }

    kv_row(cv, g, 1, "State",
           (data->nfc_state < (uint8_t)(sizeof(NFC_STATE_NAMES)
                                        / sizeof(NFC_STATE_NAMES[0])))
               ? NFC_STATE_NAMES[data->nfc_state] : "?",
           COL_TEXT);

    kv_row(cv, g, 2, "UID", (data->uid_hex[0] != '\0') ? data->uid_hex : "-",
           COL_TEXT);

    if (data->version_ok) {
        snprintf(buf, sizeof(buf), "%02X %02X %02X %02X %02X %02X %02X %02X",
                 data->version[0], data->version[1], data->version[2],
                 data->version[3], data->version[4], data->version[5],
                 data->version[6], data->version[7]);
        kv_row(cv, g, 3, "Version", buf, COL_TEXT);
    } else {
        kv_row(cv, g, 3, "Version", "-", COL_DIM);
    }

    if (data->cfg_ok) {
        snprintf(buf, sizeof(buf), "0x%02X (%s)", data->auth0,
                 (data->auth0 == 0xFF) ? "open" : "protected");
        kv_row(cv, g, 4, "AUTH0", buf, COL_TEXT);
        snprintf(buf, sizeof(buf), "0x%02X PROT %u CFGLCK %u AUTHLIM %u",
                 data->access, (unsigned)((data->access >> 7) & 1u),
                 (unsigned)((data->access >> 6) & 1u),
                 (unsigned)(data->access & 7u));
        kv_row(cv, g, 5, "ACCESS", buf, COL_TEXT);
    } else {
        kv_row(cv, g, 4, "AUTH0", "-", COL_DIM);
        kv_row(cv, g, 5, "ACCESS", "-", COL_DIM);
    }

    kv_row(cv, g, 6, "Class",
           (data->cls < 4u) ? CLASS_NAMES[data->cls] : "?", COL_TEXT);

    /* Two rows for the decoded text: the box clips at its own width, and two
     * font-1 rows of (w - 8 - label_w) hold more than the tag's own cap. */
    if (data->ndef_read_ok) {
        cv->text(cv->ctx, "Text", 4, row_y(g, 7), g->label_w, 1,
                 UI_FONT_SMALL, UI_ALIGN_LEFT, COL_DIM, COL_BG);
        cv->text(cv->ctx,
                 (data->payload[0] != '\0') ? data->payload : "(empty)",
                 (int16_t)(4 + g->label_w), row_y(g, 7), g->col_w, 2,
                 UI_FONT_SMALL, UI_ALIGN_LEFT, COL_TEXT, COL_BG);
    } else {
        snprintf(buf, sizeof(buf), "not read (%d)", data->ndef_rc);
        kv_row(cv, g, 7, "Text", buf, COL_DIM);
    }

    row = 9;
    for (r = 0; r < HEX_ROWS; r++) {
        char hex[HEX_COLS * 3 + 1];
        size_t at = 0;

        for (i = 0; i < HEX_COLS; i++) {
            size_t idx = (size_t)r * HEX_COLS + i;
            at += (size_t)snprintf(hex + at, sizeof(hex) - at, "%02X%s",
                                   data->ndef_raw[idx],
                                   (i + 1 < HEX_COLS) ? " " : "");
        }
        full_row(cv, g, (uint8_t)(row + r), hex, COL_DIM);
    }
}

static void page_battery(const ui_canvas_t* cv, const diag_layout_t* g,
                         const diag_data_t* data)
{
    char buf[40];

    snprintf(buf, sizeof(buf), "%u", (unsigned)data->bat_raw);
    kv_row(cv, g, 0, "ADC raw", buf, COL_TEXT);

    snprintf(buf, sizeof(buf), "%u mV", (unsigned)data->bat_pin_mv);
    kv_row(cv, g, 1, "Pin", buf, COL_TEXT);

    /* The divider is shown as the number the firmware actually used, because
     * it is a placeholder until the bench meters the real one. */
    snprintf(buf, sizeof(buf), "%u mV (x%u.%02u divider)",
             (unsigned)data->bat_cell_mv,
             (unsigned)(data->bat_divider_x100 / 100u),
             (unsigned)(data->bat_divider_x100 % 100u));
    kv_row(cv, g, 2, "Cell", buf, COL_TEXT);
}

static void page_audio(const ui_canvas_t* cv, const diag_layout_t* g,
                       const diag_t* d)
{
    uint8_t vol = diag_volume(d);

    kv_row(cv, g, 0, "Tone", diag_tone_on(d) ? "on" : "off",
           diag_tone_on(d) ? COL_OK : COL_DIM);
    kv_row(cv, g, 1, "Volume",
           (vol <= (uint8_t)MIX_VOL_OFF) ? VOL_NAMES[vol] : "?", COL_TEXT);
    /* No hardware mute exists on this board, and a builder who does not know
     * that reads a silent "Off" as a dead amplifier. */
    full_row(cv, g, 3, "Off holds the DAC at mid-scale.", COL_DIM);
}

/* The four window edges, one pixel each, plus a cross through the middle. The
 * edges are what a bezel hides; the cross is what tells a builder whether the
 * window is off-centre or just shifted. */
static void draw_border(const ui_canvas_t* cv, const diag_layout_t* g)
{
    cv->fill(cv->ctx, 0, 0, g->w, 1, COL_TEXT);
    cv->fill(cv->ctx, 0, (int16_t)(g->h - 1), g->w, 1, COL_TEXT);
    cv->fill(cv->ctx, 0, 0, 1, g->h, COL_TEXT);
    cv->fill(cv->ctx, (int16_t)(g->w - 1), 0, 1, g->h, COL_TEXT);

    cv->fill(cv->ctx, 0, (int16_t)(g->h / 2), g->w, 1, COL_DIM);
    cv->fill(cv->ctx, (int16_t)(g->w / 2), 0, 1, g->h, COL_DIM);
}

void diag_checker_build(diag_checker_t* ck, uint8_t idx)
{
    uint16_t dark;
    uint16_t light;
    int16_t i;

    if (ck == NULL) {
        return;
    }

    palette_build_lut(idx, ck->lut);
    dark = ck->lut[LUT_BG_DARKEST];
    light = ck->lut[LUT_BG_LIGHTEST];

    for (i = 0; i < SCALER_SRC_W; i++) {
        bool even = ((i / DIAG_CHECKER_CELL) & 1) == 0;
        ck->line[0][i] = even ? dark : light;
        ck->line[1][i] = even ? light : dark;
    }
}

/*
 * The checkerboard, pushed through the real scaler in blend mode.
 *
 * The scaler emits whole blocks only, so the body holds as many complete
 * blocks as fit under the header and stops: 240 - 20 = 220 rows is 44 blocks
 * of 5 at 5/3. The leftover row or six stay background, which is what the
 * border and the bars already show anyway.
 */
static void page_checker(const ui_canvas_t* cv, const diag_layout_t* g,
                         const diag_data_t* data, diag_checker_t* ck)
{
    const scaler_geom_info_t* info = NULL;
    enum scaler_geom_e geom = SCALER_GEOM_5_3;
    unsigned idx;
    int16_t blocks;
    int16_t b;
    uint8_t i;

    if (ck == NULL) {
        draw_border(cv, g);
        full_row(cv, g, 0, "Checkerboard needs a buffer.", COL_WARN);
        return;
    }

    /* The window width picks the geometry, because it is the scaler's own
     * output width. scaler_geom_info() returns NULL past the last one, so
     * this walk reads the table rather than naming the one geometry there is,
     * and needs no edit if another is ever added; no match means there is
     * nothing to push. */
    for (idx = 0; (info = scaler_geom_info((enum scaler_geom_e)idx)) != NULL;
         idx++) {
        if (g->w == (int16_t)info->dst_w) {
            geom = (enum scaler_geom_e)idx;
            break;
        }
    }
    if (info == NULL) {
        draw_border(cv, g);
        full_row(cv, g, 0, "No scaler geometry for this width.", COL_WARN);
        return;
    }

    diag_checker_build(ck, data->palette);

    blocks = (int16_t)((g->h - g->body_y) / info->dst_rows_per_block);
    for (b = 0; b < blocks; b++) {
        int16_t y = (int16_t)(g->body_y + b * info->dst_rows_per_block);
        unsigned first = (unsigned)b * info->src_lines_per_block;

        /* A block starts on source line first, and the chequer alternates
         * every line, so the parity is the block's own — not line 0's. With
         * an even lines-per-block every block starts even and this is the
         * same pointers as before; at 5/3's three it alternates. */
        for (i = 0; i < info->src_lines_per_block; i++) {
            ck->lines[i] = ck->line[(first + i) & 1u];
        }

        if (scaler_scale_block(geom, SCALER_MODE_BLEND, ck->lines,
                               /* the next block's first line, which is what
                                * the game path hands the scaler */
                               ck->line[(first + info->src_lines_per_block)
                                        & 1u],
                               ck->block, ck->scratch)
            != SCALER_OK) {
            return;
        }
        cv->image(cv->ctx, 0, y, (int16_t)info->dst_w,
                  (int16_t)info->dst_rows_per_block, ck->block, 0,
                  (int16_t)info->dst_rows_per_block);
    }
}

static void page_display(const ui_canvas_t* cv, const diag_layout_t* g,
                         const diag_data_t* data, const diag_t* d,
                         diag_checker_t* ck)
{
    uint8_t pattern = diag_pattern(d);
    int16_t bars_y = (int16_t)(g->body_y + DIAG_ROW_H);
    int16_t i;

    switch (pattern) {
    case DIAG_PATTERN_BARS:
        /* The bars start a row down, so the pattern's name is not painted
         * over the white one. */
        for (i = 0; i < BAR_COUNT; i++) {
            cv->fill(cv->ctx, (int16_t)(i * g->bar_w), bars_y, g->bar_w,
                     (int16_t)(g->footer_y - bars_y), BAR_COLORS[i]);
        }
        break;
    case DIAG_PATTERN_BORDER:
        draw_border(cv, g);
        break;
    case DIAG_PATTERN_CHECKER:
        page_checker(cv, g, data, ck);
        break;
    default:
        break;
    }

    /* Last, so it stays legible over whatever the pattern painted. */
    if (pattern < DIAG_PATTERN_COUNT) {
        full_row(cv, g, 0, PATTERN_NAMES[pattern], COL_TEXT);
    }
}

static void page_nudge(const ui_canvas_t* cv, const diag_layout_t* g,
                       const diag_t* d, uint32_t now_ms)
{
    int16_t x = 0;
    int16_t y = 0;
    char buf[40];

    draw_border(cv, g);
    diag_origin(d, &x, &y);

    snprintf(buf, sizeof(buf), "X %d   Y %d", (int)x, (int)y);
    full_row(cv, g, 0, buf, COL_TEXT);
    snprintf(buf, sizeof(buf), "default %d, %d", (int)d->default_x,
             (int)d->default_y);
    full_row(cv, g, 1, buf, COL_DIM);

    if (diag_toast_active(d, now_ms)) {
        full_row(cv, g, 3, "Saved", COL_OK);
    }
}

/*
 * The trim page, idle. There is no drawing for the running state: the fixture
 * fills the window and the binding pushes it frame by frame, which is the
 * whole reason the page has two states rather than a live readout.
 */
static void page_trim(const ui_canvas_t* cv, const diag_layout_t* g,
                      const diag_t* d, uint32_t now_ms)
{
    uint8_t fpa = 0;
    uint8_t ratio = 0;
    uint32_t span = diag_trim_span(d);
    char buf[40];

    diag_trim(d, &fpa, &ratio);

    snprintf(buf, sizeof(buf), "%u + %u/64", (unsigned)fpa, (unsigned)ratio);
    kv_row(cv, g, 0, "Porch", buf, COL_TEXT);

    snprintf(buf, sizeof(buf), "%u + %u/64", (unsigned)d->default_trim_fpa,
             (unsigned)d->default_trim_ratio);
    kv_row(cv, g, 1, "Default", buf, COL_DIM);

    if (span > 0u) {
        uint32_t tenths = (span * 1000u + DIAG_TRIM_FPS_X100 / 2u)
                        / (uint32_t)DIAG_TRIM_FPS_X100;

        snprintf(buf, sizeof(buf), "%lu frames, %lu.%lu s",
                 (unsigned long)span, (unsigned long)(tenths / 10u),
                 (unsigned long)(tenths % 10u));
        kv_row(cv, g, 2, "Crossing", buf, COL_TEXT);
    } else {
        kv_row(cv, g, 2, "Crossing", "not counted yet", COL_DIM);
    }

    if (d->trim_step != 0) {
        snprintf(buf, sizeof(buf), "%+d/64", (int)-d->trim_step);
        kv_row(cv, g, 3, "Last move", buf, COL_TEXT);
    } else if (span > 0u) {
        /* A run long enough that the correction rounded to nothing is the
         * end of the road, not a failure: the register cannot express a
         * smaller change. */
        kv_row(cv, g, 3, "Last move", "none left to give", COL_OK);
    } else {
        kv_row(cv, g, 3, "Last move", "-", COL_DIM);
    }

    snprintf(buf, sizeof(buf), "Start, then mark each of %u crossings",
             (unsigned)DIAG_TRIM_MARKS);
    full_row(cv, g, 5, buf, COL_DIM);

    if (diag_toast_active(d, now_ms)) {
        full_row(cv, g, 6, "Saved", COL_OK);
    }
}

static void page_system(const ui_canvas_t* cv, const diag_layout_t* g,
                        const diag_data_t* data, const diag_t* d)
{
    char buf[16];

    snprintf(buf, sizeof(buf), "%u", (unsigned)diag_frameskip(d));
    kv_row(cv, g, 0, "Frameskip", buf, COL_TEXT);
    kv_row(cv, g, 1, "Version",
           (data->fw_version[0] != '\0') ? data->fw_version : "unknown",
           COL_TEXT);
    kv_row(cv, g, 2, "Built",
           (data->build_time[0] != '\0') ? data->build_time : "unknown",
           COL_TEXT);
}

/* ─── the draw ────────────────────────────────────────────────────────────── */

void diag_draw(const diag_t* d, const diag_data_t* data,
               const diag_layout_t* g, diag_checker_t* ck, uint32_t now_ms,
               const ui_canvas_t* cv)
{
    uint8_t page;

    if (d == NULL || data == NULL || g == NULL || cv == NULL) {
        return;
    }
    if (cv->fill == NULL || cv->text == NULL || cv->image == NULL) {
        return;
    }

    page = diag_page(d);
    draw_header(cv, g, page);

    switch (page) {
    case DIAG_PAGE_BUTTONS:
        page_buttons(cv, g, data);
        break;
    case DIAG_PAGE_SD:
        page_sd(cv, g, data);
        break;
    case DIAG_PAGE_NFC:
        page_nfc(cv, g, data);
        break;
    case DIAG_PAGE_BATTERY:
        page_battery(cv, g, data);
        break;
    case DIAG_PAGE_AUDIO:
        page_audio(cv, g, d);
        break;
    case DIAG_PAGE_DISPLAY:
        page_display(cv, g, data, d, ck);
        break;
    case DIAG_PAGE_NUDGE:
        page_nudge(cv, g, d, now_ms);
        break;
    case DIAG_PAGE_TRIM:
        page_trim(cv, g, d, now_ms);
        break;
    case DIAG_PAGE_SYSTEM:
        page_system(cv, g, data, d);
        break;
    default:
        break;
    }

    draw_footer(cv, g, page);
}
