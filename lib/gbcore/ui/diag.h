#pragma once
// The diagnostic mode's state machine — everything it decides, with nothing it
// draws and nothing it measures (design §2.2, §8.2).
//
// Which of the eight pages is current and how Select+Left/Right move between
// them, what the bare D-pad, A and B mean on each one, the working game-window
// origin with its clamp and its key repeat, the test tone's on/off and volume
// index, the display pattern cursor, the frameskip value, whether a tag scan
// has been asked for, and how long the "Saved" toast stays up. Plain values in
// — one combo event, the masked joypad word, a millisecond timestamp — event
// flags out. The Arduino binding polls it and draws it; the host suite drives
// it with literal timestamps.
//
// It holds no geometry beyond the two clamp limits it is given, and no
// measured value: those live in diag_data_t, which the binding fills and the
// layout module reads.
//
// The rules this mode is built on:
//
//   1. It is entered by holding Start+Select at power-on and by nothing else,
//      and there is no way back: every exit is a power cycle, so its buffers
//      may be static and generous.
//   2. It reaches for nothing below itself. No tag, reader, storage or
//      emulator symbol appears here, in code or in prose.
//   3. It writes no tag and offers no route to one.
//   4. It selects no game, and has no list of games to select from.
//   5. When it renders, it renders inside the game window — every coordinate
//      the layout module produces is window-relative.
//
// Pages change on Select+Right and Select+Left, which arrive as the combo
// module's existing brightness events. The bare D-pad is never a page change:
// it belongs to the page, which is what makes a one-pixel nudge possible.
//
// Pure C, no Arduino/ESP-IDF headers, no allocation.

#include <stdint.h>
#include <stdbool.h>

#include "cart/ndef.h"

#ifdef __cplusplus
extern "C" {
#endif

enum diag_page_e {
    DIAG_PAGE_BUTTONS = 0,
    DIAG_PAGE_SD,
    DIAG_PAGE_NFC,
    DIAG_PAGE_BATTERY,
    DIAG_PAGE_AUDIO,
    DIAG_PAGE_DISPLAY,
    DIAG_PAGE_NUDGE,
    DIAG_PAGE_SYSTEM,
    DIAG_PAGE_COUNT,
};

enum diag_pattern_e {
    DIAG_PATTERN_BARS = 0,
    DIAG_PATTERN_BORDER,
    DIAG_PATTERN_CHECKER,
    DIAG_PATTERN_COUNT,
};

/* What the tag inspector found on its last look. NOT_SCANNED is the state
 * before the first scan completes, which is why it is distinct from NONE. */
enum diag_nfc_state_e {
    DIAG_NFC_NOT_SCANNED = 0,
    DIAG_NFC_NO_READER,
    DIAG_NFC_NONE,
    DIAG_NFC_ONE,
    DIAG_NFC_MULTI,
    DIAG_NFC_ERR,
};

/* The bridge takes any uint8_t and shows one frame in every skip + 1, so this
 * ceiling is a judgement rather than a limit: 3 matches the range the fork's
 * menu offered and is the highest setting that still leaves a picture worth
 * looking at. */
#define DIAG_FRAMESKIP_MAX 3

/* How long "Saved" stays up after the nudge is committed. */
#define DIAG_TOAST_MS 800

/* Length cap on the two build strings the binding copies in, terminator
 * included. A `git describe` on this repository plus a UTC stamp both fit
 * inside it with room to spare. */
#define DIAG_VERSION_MAX 48

/*
 * Event flags, OR-able: one call can both move the nudge and commit it.
 * REDRAW means the screen changed; the rest are work for the binding.
 */
#define DIAG_EV_REDRAW      0x01
#define DIAG_EV_PAGE        0x02
#define DIAG_EV_SAVE_NUDGE  0x04
#define DIAG_EV_NFC_SCAN    0x08
#define DIAG_EV_TONE        0x10
#define DIAG_EV_FRAMESKIP   0x20

enum diag_result_e {
    DIAG_OK = 0,
    DIAG_ERR_ARGS = -1, /* NULL state, or a window larger than the panel */
};

/*
 * Everything the pages report, as one snapshot. The binding fills the fields
 * its own page needs before each draw and the layout module reads them; no
 * field here is ever written by the state machine.
 *
 * Nothing in here is a handle or a pointer into another subsystem: it is
 * numbers and strings already gathered, which is what keeps the layout module
 * host-testable.
 */
typedef struct diag_data_s {
    /* Buttons page. `buttons` is the live word in COMBO_BTN_* bits, unmasked
     * — the page exists to show what the expander actually reports, so the
     * combo module's masking would defeat it. gpa[] gives the expander bit
     * behind joypad bit 0..7, which is what turns "Up does nothing" into
     * "GPA7 never goes low". */
    uint8_t buttons;
    uint8_t gpa[8];

    /* SD page. */
    bool sd_ok;
    uint16_t rom_count;
    uint16_t catalog_count;
    bool catalog_ok;
    uint32_t sd_total_mb;
    uint32_t sd_used_mb;
    bool sd_stats_ok;

    /* Tag inspector. Read-only throughout: every field is something the
     * inspector saw, and there is no field for anything it could change. */
    uint8_t nfc_state;          /* enum diag_nfc_state_e                    */
    uint32_t nfc_fw;            /* the reader's own firmware word           */
    char uid_hex[15];           /* 7 bytes as hex, NUL-terminated           */
    bool version_ok;
    uint8_t version[8];
    bool cfg_ok;
    uint8_t auth0;
    uint8_t access;
    uint8_t ndef_raw[NDEF_BUF_MAX];
    bool ndef_read_ok;
    int ndef_rc;                /* the decoder's own result code            */
    char payload[NDEF_TEXT_MAX + 1];
    uint8_t cls;                /* the tag class the boot classifier gives  */

    /* Battery page. The divider is carried as a value rather than read from a
     * header so the page can show the number the firmware actually used. */
    uint16_t bat_raw;
    uint16_t bat_pin_mv;
    uint16_t bat_cell_mv;
    uint16_t bat_divider_x100;

    /* Display page: which palette's darkest and lightest shades the
     * checkerboard is built from. */
    uint8_t palette;

    /* System page. */
    char fw_version[DIAG_VERSION_MAX];
    char build_time[DIAG_VERSION_MAX];
} diag_data_t;

/*
 * The machine. The layout module in this same library reads these fields
 * directly, because it is the other half of one screen; everything outside
 * gbcore goes through the functions below.
 */
typedef struct diag_s {
    uint8_t page;
    int16_t x;            /* working window origin                        */
    int16_t y;
    int16_t default_x;    /* the compile-time origin B restores           */
    int16_t default_y;
    int16_t x_max;        /* panel minus window, so the clamp is [0, max] */
    int16_t y_max;
    uint8_t held_dir;     /* the one direction bit held, 0 = none         */
    uint32_t repeat_due_ms;
    uint8_t prev_word;    /* last joypad word, for the A and B edges      */
    bool tone_on;
    uint8_t volume;       /* MIX_VOL_HIGH .. MIX_VOL_OFF                  */
    uint8_t pattern;      /* enum diag_pattern_e                          */
    uint8_t frameskip;
    uint32_t toast_until_ms;
    bool toast;
} diag_t;

/*
 * Start on the buttons page.
 *
 *   panel_w/h    the whole display, in pixels
 *   win_w/h      the game window drawn inside it
 *   x, y         the stored origin; clamped into [0, panel - window]
 *   default_x/y  the compile-time origin, clamped the same way
 *   volume       stored volume index, clamped into MIX_VOL_HIGH..MIX_VOL_OFF
 *   frameskip    stored frameskip, clamped into 0..DIAG_FRAMESKIP_MAX
 *
 * A stored value that arrives out of range is clamped rather than refused:
 * the page has to show something, and a setting that repairs itself is better
 * than a diagnostic screen that reports its own reading wrongly.
 *
 * Returns DIAG_OK, or DIAG_ERR_ARGS without writing anything for a NULL state
 * or a window that does not fit the panel.
 */
int diag_init(diag_t* d, int16_t panel_w, int16_t panel_h,
              int16_t win_w, int16_t win_h, int16_t x, int16_t y,
              int16_t default_x, int16_t default_y,
              uint8_t volume, uint8_t frameskip);

/*
 * One sample taken at now_ms: the combo event this call produced (an
 * enum combo_event_e value) and the masked joypad word that came with it.
 *
 * Select+Right and Select+Left move a page either way with wrap. Leaving the
 * audio page with the tone on silences it; arriving at the tag page asks for
 * a scan. Every other combo event is ignored here and the joypad word decides
 * instead: A and B act on their press edge, and one held direction repeats at
 * the combo module's cadence — two directions at once are a fumble and do
 * nothing.
 *
 * Returns the OR of the DIAG_EV_* flags, or 0 for a NULL state.
 */
uint16_t diag_input(diag_t* d, uint8_t combo_event, uint8_t joypad,
                    uint32_t now_ms);

/*
 * Time passing, with no input. Expires the "Saved" toast and reports
 * DIAG_EV_REDRAW on the call that does it — once, not on every later call.
 */
uint16_t diag_tick(diag_t* d, uint32_t now_ms);

uint8_t diag_page(const diag_t* d);

/* The working origin. Both pointers may be NULL. */
void diag_origin(const diag_t* d, int16_t* out_x, int16_t* out_y);

bool diag_tone_on(const diag_t* d);
uint8_t diag_volume(const diag_t* d);
uint8_t diag_pattern(const diag_t* d);
uint8_t diag_frameskip(const diag_t* d);
bool diag_toast_active(const diag_t* d, uint32_t now_ms);

/* The page's name, for its header and for the serial line on every switch.
 * NULL for DIAG_PAGE_COUNT and anything past it. */
const char* diag_page_title(uint8_t page);

#ifdef __cplusplus
}
#endif
