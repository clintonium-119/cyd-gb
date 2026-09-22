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
    DIAG_PAGE_TRIM,
    DIAG_PAGE_SYSTEM,
    DIAG_PAGE_COUNT,
};

/*
 * The panel-trim page's two states. It is the only page with any, and it has
 * them because the fixture it calibrates against fills the whole window: at
 * the shipped geometry the window is 266 x 240 of a 320 x 240 panel, so there
 * is no margin to put a readout in. The page therefore either shows numbers or
 * shows the fixture, never both.
 */
enum diag_trim_state_e {
    DIAG_TRIM_IDLE = 0,
    DIAG_TRIM_RUNNING,
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

/* ─── Panel trim ─────────────────────────────────────────────────────────────
 * The page nulls the beat between the panel's free-running refresh and the
 * emulator's audio-paced cadence, by lengthening the frame with PORCTRL's
 * front porch. It does not ask the builder to judge when the seam has stopped
 * — a static discontinuity in scrolling content is perceived as travelling
 * with the content, which is what biased the measurement this replaces. It
 * asks them to COUNT, which that illusion cannot affect: the seam wraps as
 * often as it wraps whatever it looks like it is doing between wraps.
 *
 * The arithmetic needs no clock and no anchor. One crossing is exactly one
 * frame of slip, so over N frames between crossings the two rates differ by
 * 1 part in N, and the frame's line count has to move by the same fraction:
 *
 *     correction, in 64ths of a line  =  (64 * total lines) / N
 *
 * with the total in 64ths already, which makes it a single integer divide. N
 * is a count of frames the page itself pushed, so the cadence it nulls against
 * is by construction the one it ran at.
 */

/* 64ths of a line per Up/Down press, carrying into the whole-line knob. One
 * press is about 0.011 Hz, which parks a seam for a minute and a half, and is
 * the finest distinction an eye can make. */
#define DIAG_TRIM_FINE 4

/* Crossings averaged before the page corrects itself. Three, because a
 * builder's reaction spread is a few tenths of a second either way: at the
 * 9-12 s interval a badly trimmed unit shows, that is a few per cent on one
 * interval and the mean of three brings it under two. */
#define DIAG_TRIM_MARKS 3

/* A mark this soon after the last one is a bounced button rather than a
 * crossing — half a second at the emulator's cadence. */
#define DIAG_TRIM_MIN_FRAMES 30

/* How far the intervals in one run may disagree before the run is thrown
 * away rather than averaged: longest over shortest, as a fraction.
 *
 * This is the guard against a count that was never a count. A builder who
 * cannot make out the seam has no way to stop a run except by pressing the
 * mark button until it ends, and those presses are indistinguishable from
 * crossings one at a time — but not as a set: marking a real repeating event
 * gives intervals within a few per cent of each other, and pressing at random
 * does not. A missed crossing lands at exactly 2, so the threshold has to sit
 * below that; reaction spread on even a two-second interval is nowhere near
 * 1.5, so it can sit well below.
 */
#define DIAG_TRIM_SPREAD_NUM 3
#define DIAG_TRIM_SPREAD_DEN 2

/* The fixture's vertical scroll as the page enters a run, in output pixels per
 * frame. Signed: negative runs the field the other way, and the sign is part
 * of the setting rather than a detail — the bench found the seam legible
 * scrolling one way and asked for this value specifically.
 *
 * Five. The bound this has to stay under is the feature height: a
 * displacement of a WHOLE feature loses the continuity the eye needs, because
 * the random field re-randomises every block and a periodic field inverts or
 * realigns, and a seam has to be a break in a picture that is otherwise the
 * same picture translated. Every feature here is 8 tall, so anything below 8
 * clears that, and the first version of this page scrolled a whole block a
 * frame and showed a builder a field with nothing in it.
 *
 * Five rather than four within that range, because four made the field
 * STROBE. The scroll returns to its own starting position every
 * period / gcd(rate, period) frames: at four that is two frames for the
 * stripes and the noise field, four for the checkerboard and the grid. A
 * fixture cycling through two states is not a scrolling field, and a
 * one-frame displacement inside a two-state flicker is the thing a builder
 * could not see. Five is coprime with 8 and with 16, so the shortest cycle
 * any pattern has is 8 frames and all four genuinely move.
 */
#define DIAG_TRIM_SCROLL (-5)

/* Output pixels per frame the D-pad can reach, either way on either axis. */
#define DIAG_TRIM_RATE_MAX 8

/*
 * The fixture's patterns. Noise is the default and the one the procedure
 * names, because it is the only one that is not CONDITIONALLY BLIND: the rest
 * are periodic on one axis or both, and where the scroll equals a whole
 * period the two sides of a seam line up and a real seam disappears. Measured
 * on the bench that built them: stripes changed 100 % of pixels under a
 * one-frame displacement at one rate and 0 % at another.
 *
 * They are all here anyway. A periodic pattern that aliases gives a builder
 * nothing to press rather than a wrong number to store, because the crossing
 * count is a human pressing Start at something they can see; and having the
 * blind ones to hand is how the claim that noise is better stops being a
 * table in a comment and becomes something a builder can check in a minute.
 */
enum diag_trim_pat_e {
    DIAG_TRIM_PAT_NOISE = 0,
    DIAG_TRIM_PAT_CHECK,
    DIAG_TRIM_PAT_STRIPE,
    DIAG_TRIM_PAT_GRID,
    DIAG_TRIM_PAT_COUNT,
};

/*
 * What the page enters a run on, which is what the documented procedure
 * calibrates at: the stripe field, scrolled at five output pixels a frame.
 * Chosen on the bench of 2026-09-22, where a builder calibrating a real unit
 * found it the one they could actually read, after the checkerboard at four
 * had left them unable to close a run.
 *
 * Two properties decide a fixture and only one of them was encoded here
 * before.
 *
 *   * A pattern must not go BLIND: where the one-frame DISPLACEMENT equals a
 *     whole period the two sides of a seam line up and a real seam vanishes.
 *     Stripes have the shortest period of the four at 8, so they are the only
 *     one the D-pad can blind at all, and only at its extreme of 8. At five
 *     they are nowhere near it.
 *   * A pattern must also SCROLL, and that is a different arithmetic: the
 *     field returns to its own starting position every period / gcd(rate,
 *     period) frames. At the old default of four that is TWO frames for the
 *     stripes and for the noise field, and four for the checkerboard and the
 *     grid — so the fixture did not scroll, it strobed between a handful of
 *     states. Spotting a one-frame displacement inside a two-state flicker is
 *     the hard problem the builder ran into. Five is coprime with every
 *     pattern's period, so the shortest cycle any of them has is 8 frames.
 *
 * Five costs the old default's tidiest property — it is no longer half a
 * feature, the largest displacement that still translates rather than
 * decorrelating. That bound was reasoning about what the eye could follow;
 * the bench is a builder reporting what the eye could follow, and it wins.
 * Five-eighths of a stripe period is still most of the way to inversion.
 */
#define DIAG_TRIM_PAT_DEFAULT DIAG_TRIM_PAT_STRIPE

/* Every pattern's feature is 8 output pixels tall, which is what makes one
 * default rate safe for all of them: switching pattern mid-run never lands on
 * a whole-feature displacement, because the default rate is below 8 and every
 * period is 8 or 16. The noise block is square at 8; a 4-tall block was the
 * first version's and put the default rate on a pathological point. */
#define DIAG_TRIM_BLOCK_W 8
#define DIAG_TRIM_BLOCK_H 8
#define DIAG_TRIM_FEATURE_H 8

/* The periodic patterns' periods, in output pixels: a checkerboard cell, a
 * stripe band, and the grid's rule spacing. Sized so they read on the panel
 * roughly as their ancestors did through the scaler's 5/3 vertical blend. */
#define DIAG_TRIM_CHECK_CELL 8
#define DIAG_TRIM_STRIPE_BAND 4
#define DIAG_TRIM_GRID_PITCH 16

/* Lines in a frame the front porch does not contribute, in 64ths: the panel's
 * active lines plus its back porch and pulse width. Mirrored from
 * render/panel_rate.h's PANEL_RATE_BASE_LINES rather than included, for the
 * same reason this file mirrors the combo module's button bits — the state
 * machine reaches for nothing below itself. */
#define DIAG_TRIM_BASE_LINES_X64 (332 * 64)

/* The emulator's audio-paced cadence, x100, for turning a frame count into
 * the seconds a builder can relate to. Display only: nothing the page
 * calculates uses it, which is why an approximate cadence here costs the
 * calibration nothing. */
#define DIAG_TRIM_FPS_X100 5973

/* What PORCTRL's 7-bit front porch can hold, in 64ths. The top is 126 + 63/64
 * because the divider programs one line more on its long frames. */
#define DIAG_TRIM_MIN_X64 (1 * 64)
#define DIAG_TRIM_MAX_X64 (126 * 64 + 63)

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
#define DIAG_EV_SAVE_TRIM   0x40
/* The trim page entered or left its fixture: the binding runs a different
 * loop in each state, so this is the one event it must not miss. */
#define DIAG_EV_TRIM_STATE  0x80
/* The fixture's pattern or scroll moved. The binding has no redraw to do — a
 * run paints every frame anyway — but it does have a serial line to print,
 * because the fixture a reading was taken on cannot be shown on a screen the
 * fixture fills. */
#define DIAG_EV_TRIM_FIXTURE 0x100

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

    /* Panel trim. The working porch, the one B restores, and everything the
     * crossing count needs. */
    uint8_t trim_fpa;
    uint8_t trim_ratio;
    uint8_t default_trim_fpa;
    uint8_t default_trim_ratio;
    /* What the store holds, as distinct from what the page is showing. The
     * page moves the working porch on its own — a correction at the end of
     * every run — so "is what I am looking at the thing that will come back
     * after a power cycle" is a real question with a non-obvious answer, and
     * it cost a bench session to ask it the slow way. */
    uint8_t stored_trim_fpa;
    uint8_t stored_trim_ratio;
    uint8_t trim_state;       /* enum diag_trim_state_e                     */
    /* Which way the last correction moved the porch. Never asked of the
     * builder: a correction that made the interval shorter went the wrong
     * way, and the page reads that off its own two measurements.
     *
     * It travels with the porch into the store, because a direction the page
     * only learns from a SECOND run is worthless to a builder who power-cycles
     * between rounds — they would re-guess at every boot, and against a porch
     * already near its null that guess walks a correctly trimmed unit away
     * from it. */
    int8_t trim_dir;
    uint8_t trim_marks;       /* crossings marked this run                  */
    uint8_t trim_pat;         /* enum diag_trim_pat_e                       */
    int8_t trim_vx;           /* fixture scroll, output px per frame         */
    int8_t trim_vy;
    int32_t trim_ox;          /* accumulated scroll offset, this run         */
    int32_t trim_oy;
    /* The fixture the last run was measured on, kept beside its span: a
     * crossing interval means nothing without the pattern and rate it was
     * counted at, and those are a builder's to change now. */
    uint8_t span_pat;
    int8_t span_vx;
    int8_t span_vy;
    uint32_t trim_frames;     /* frames the binding has pushed this run     */
    uint32_t trim_first_frame; /* the frame count the first mark landed on   */
    uint32_t trim_last_mark;  /* frame count the last accepted mark landed on */
    uint32_t trim_int_min;    /* shortest and longest interval this run       */
    uint32_t trim_int_max;
    /* The last run's marks disagreed too much to be a measurement, so no
     * correction came out of it. Kept so the page can say so: silently
     * declining to move would look like a page that had stopped working. */
    bool trim_rejected;
    uint32_t trim_span;       /* mean frames per crossing, last GOOD run      */
    uint32_t trim_prev_span;  /* the run before, for the direction test     */
    int16_t trim_step;        /* 64ths the last correction moved, signed    */
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
 *   trim_fpa     stored front porch in whole lines, clamped into 1..126
 *   trim_ratio   stored 64ths of a line, clamped into 0..63
 *   default_fpa  the compile-time porch B restores, clamped the same way
 *   default_ratio
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
              uint8_t volume, uint8_t frameskip,
              uint8_t trim_fpa, uint8_t trim_ratio,
              uint8_t default_fpa, uint8_t default_ratio);

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

/* ─── Panel trim ─────────────────────────────────────────────────────────── */

/* The working porch. Both pointers may be NULL. */
void diag_trim(const diag_t* d, uint8_t* fpa, uint8_t* ratio);

/* True while the page wants the fixture pushed rather than the page drawn. */
bool diag_trim_running(const diag_t* d);

/*
 * One pushed frame of the fixture, counted. The binding calls this once per
 * frame it puts on the panel and nowhere else — the count IS the measurement,
 * so a frame counted that was not pushed, or pushed and not counted, is an
 * error in the calibration rather than in the bookkeeping.
 *
 * Returns DIAG_EV_REDRAW on the frame that ends a run, which is the frame
 * after the last crossing the page needed; 0 otherwise.
 */
uint16_t diag_trim_frame(diag_t* d);

/*
 * The fixture's shade, 0..3, at output pixel (u, v). A pseudo-random field of
 * pattern `pat`, deterministic in its own coordinates so the field scrolls
 * with the offset instead of fizzing.
 *
 * An unknown pattern draws the noise field, so a caller can never paint a
 * blank screen by holding a value this enum does not have.
 */
uint8_t diag_trim_shade(uint8_t pat, int32_t u, int32_t v);

/* The fixture's accumulated scroll offset for the frame about to be pushed.
 * Accumulated rather than derived from the frame count, because the rate is a
 * knob during a run and a multiply would rewrite the field's whole history
 * every time it moved. Either pointer may be NULL. */
void diag_trim_offsets(const diag_t* d, int32_t* out_x, int32_t* out_y);

/* Which pattern the fixture is drawing, and its two scroll rates. */
uint8_t diag_trim_pattern(const diag_t* d);
void diag_trim_rate(const diag_t* d, int8_t* out_vx, int8_t* out_vy);

/* The pattern's name, for the readout and the serial line on every change.
 * NULL for DIAG_TRIM_PAT_COUNT and anything past it. */
const char* diag_trim_pat_name(uint8_t pat);

/* The last good run's mean frames between crossings, 0 before the first one
 * that produced a measurement. */
uint32_t diag_trim_span(const diag_t* d);

/* True when the last run's marks disagreed too much to average. */
bool diag_trim_rejected(const diag_t* d);

/* What the store held when the page opened, plus anything saved since. Both
 * pointers may be NULL. */
void diag_trim_stored(const diag_t* d, uint8_t* fpa, uint8_t* ratio);

/* True when the working porch is not the stored one — so the value on screen
 * is not the value a power cycle brings back. */
bool diag_trim_unsaved(const diag_t* d);

/*
 * The direction the loop is currently correcting in, and the restore that
 * puts a stored one back. Saved beside the porch and handed back after
 * diag_init(), so the first run of a boot continues the last session's
 * convergence instead of starting the guess over.
 *
 * Any non-negative value restores +1 — shorten the porch — which is the guess
 * a unit that has never been calibrated starts from. One place knows the
 * encoding, the same stance the porch's own clamp takes.
 */
int8_t diag_trim_dir(const diag_t* d);
void diag_trim_set_dir(diag_t* d, int8_t dir);

/* The page's name, for its header and for the serial line on every switch.
 * NULL for DIAG_PAGE_COUNT and anything past it. */
const char* diag_page_title(uint8_t page);

#ifdef __cplusplus
}
#endif
