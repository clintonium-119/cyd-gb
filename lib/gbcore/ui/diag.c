#include <stddef.h>

#include "audio/mix.h"
#include "input/combo.h"
#include "ui/diag.h"

/* The four direction bits, as one mask. */
#define DIAG_DPAD_MASK \
    (COMBO_BTN_UP | COMBO_BTN_DOWN | COMBO_BTN_LEFT | COMBO_BTN_RIGHT)

static const char* const page_titles[DIAG_PAGE_COUNT] = {
    "Buttons",
    "SD card",
    "NFC tag",
    "Battery",
    "Audio",
    "Display",
    "Nudge",
    "Panel trim",
    "System",
};

static int16_t clamp_i16(int16_t v, int16_t lo, int16_t hi)
{
    if (v < lo) {
        return lo;
    }
    if (v > hi) {
        return hi;
    }
    return v;
}

/*
 * Arriving at a page can be work in itself: the tag inspector looks once on
 * entry rather than polling, which is the whole reason the reader is quiet
 * unless someone asked it a question.
 */
static uint16_t on_enter(diag_t* d)
{
    if (d->page == DIAG_PAGE_NFC) {
        return DIAG_EV_NFC_SCAN;
    }
    return 0;
}

/*
 * Leaving a page has to undo anything the page left running, or a builder
 * walking through all eight would end up with a tone playing under the system
 * page with nothing on screen to explain it.
 */
static uint16_t on_leave(diag_t* d)
{
    if (d->page == DIAG_PAGE_AUDIO && d->tone_on) {
        d->tone_on = false;
        return DIAG_EV_TONE;
    }
    /* A run left going would hold the binding in the fixture loop with
     * another page's header under it. */
    if (d->page == DIAG_PAGE_TRIM && d->trim_state == DIAG_TRIM_RUNNING) {
        d->trim_state = DIAG_TRIM_IDLE;
        return DIAG_EV_TRIM_STATE;
    }
    return 0;
}

static uint16_t change_page(diag_t* d, int8_t dir)
{
    uint16_t ev = DIAG_EV_REDRAW | DIAG_EV_PAGE;

    ev |= on_leave(d);

    if (dir > 0) {
        d->page = (uint8_t)((d->page + 1u) % DIAG_PAGE_COUNT);
    } else {
        d->page = (uint8_t)((d->page + DIAG_PAGE_COUNT - 1u)
                            % DIAG_PAGE_COUNT);
    }

    /* A direction still held across the switch belongs to the page that is
     * gone. Clearing it means the first press on the new page acts at once
     * instead of inheriting a deadline. */
    d->held_dir = 0;

    ev |= on_enter(d);

    return ev;
}

int diag_init(diag_t* d, int16_t panel_w, int16_t panel_h,
              int16_t win_w, int16_t win_h, int16_t x, int16_t y,
              int16_t default_x, int16_t default_y,
              uint8_t volume, uint8_t frameskip,
              uint8_t trim_fpa, uint8_t trim_ratio,
              uint8_t default_fpa, uint8_t default_ratio)
{
    if (d == NULL) {
        return DIAG_ERR_ARGS;
    }
    if (win_w <= 0 || win_h <= 0 || panel_w <= 0 || panel_h <= 0) {
        return DIAG_ERR_ARGS;
    }
    if (win_w > panel_w || win_h > panel_h) {
        return DIAG_ERR_ARGS;
    }

    d->page = DIAG_PAGE_BUTTONS;
    d->x_max = (int16_t)(panel_w - win_w);
    d->y_max = (int16_t)(panel_h - win_h);
    d->x = clamp_i16(x, 0, d->x_max);
    d->y = clamp_i16(y, 0, d->y_max);
    d->default_x = clamp_i16(default_x, 0, d->x_max);
    d->default_y = clamp_i16(default_y, 0, d->y_max);
    d->held_dir = 0;
    d->repeat_due_ms = 0;
    d->prev_word = 0;
    d->tone_on = false;
    d->volume = combo_step_u8(volume, 0, MIX_VOL_OFF, MIX_VOL_HIGH, 1);
    d->pattern = DIAG_PATTERN_BARS;
    d->frameskip = combo_step_u8(frameskip, 0, 0, DIAG_FRAMESKIP_MAX, 1);
    d->list_mode = false;
    d->toast_until_ms = 0;
    d->toast = false;

    d->trim_fpa = (uint8_t)clamp_i16((int16_t)trim_fpa, 1, 126);
    d->trim_ratio = (uint8_t)clamp_i16((int16_t)trim_ratio, 0, 63);
    /* The COMPILE-TIME porch, not the stored one — the same split the nudge
     * page has between `x` and `default_x`, and for a sharper reason here.
     * A correction lands on an arbitrary 64th, while the knobs step by 4 and
     * by 64, so a porch the page has moved itself can never be walked back to
     * a round number by hand: the value's remainder mod 4 is invariant under
     * both knobs. Without a default that is a fixed number, there would be no
     * way back to a known starting point at all. */
    d->default_trim_fpa = (uint8_t)clamp_i16((int16_t)default_fpa, 1, 126);
    d->default_trim_ratio = (uint8_t)clamp_i16((int16_t)default_ratio, 0, 63);
    d->stored_trim_fpa = d->trim_fpa;
    d->stored_trim_ratio = d->trim_ratio;
    d->trim_state = DIAG_TRIM_IDLE;
    /* Either way is a guess until a run has been measured against another.
     * Shortening the porch speeds the panel up, which is the direction a
     * panel running slow needs, and one of the two has to go first. A unit
     * that has been calibrated before does better than guess: its binding
     * hands the stored direction back through diag_trim_set_dir(). */
    d->trim_dir = +1;
    d->trim_marks = 0;
    d->trim_frames = 0;
    d->trim_pat = DIAG_TRIM_PAT_DEFAULT;
    d->trim_vx = 0;
    d->trim_vy = DIAG_TRIM_SCROLL;
    d->trim_ox = 0;
    d->trim_oy = 0;
    d->span_pat = DIAG_TRIM_PAT_DEFAULT;
    d->span_vx = 0;
    d->span_vy = 0;
    d->trim_first_frame = 0;
    d->trim_last_mark = 0;
    d->trim_int_min = 0;
    d->trim_int_max = 0;
    d->trim_int_last = 0;
    d->trim_lengthening = true;
    d->trim_verdict = DIAG_TRIM_NO_RUN;
    d->trim_span = 0;
    d->trim_prev_span = 0;
    d->trim_step = 0;

    return DIAG_OK;
}

/* The nudge page: the D-pad moves the window a pixel at a time and clamps at
 * the panel edges, which is the only place on the panel a builder can reach
 * with no computer attached. */
static uint16_t nudge_step(diag_t* d, uint8_t dir_bits)
{
    int16_t before_x = d->x;
    int16_t before_y = d->y;

    switch (dir_bits) {
    case COMBO_BTN_RIGHT:
        d->x = clamp_i16((int16_t)(d->x + 1), 0, d->x_max);
        break;
    case COMBO_BTN_LEFT:
        d->x = clamp_i16((int16_t)(d->x - 1), 0, d->x_max);
        break;
    case COMBO_BTN_DOWN:
        d->y = clamp_i16((int16_t)(d->y + 1), 0, d->y_max);
        break;
    case COMBO_BTN_UP:
        d->y = clamp_i16((int16_t)(d->y - 1), 0, d->y_max);
        break;
    default:
        break;
    }

    return (uint16_t)((d->x != before_x || d->y != before_y) ? DIAG_EV_REDRAW
                                                             : 0);
}

/* A bigger index is louder, matching the stored setting's own encoding, so
 * Up is up on screen as well as in the ear. */
static uint16_t audio_step(diag_t* d, uint8_t dir_bits)
{
    uint8_t before = d->volume;
    int8_t dir = 0;

    if (dir_bits == COMBO_BTN_UP) {
        dir = +1;
    } else if (dir_bits == COMBO_BTN_DOWN) {
        dir = -1;
    } else {
        return 0;
    }

    d->volume = combo_step_u8(d->volume, dir, MIX_VOL_OFF, MIX_VOL_HIGH, 1);

    return (uint16_t)((d->volume != before) ? (DIAG_EV_TONE | DIAG_EV_REDRAW)
                                            : 0);
}

/* The patterns wrap, unlike every other stepped value here: there are three
 * of them and no reason to make a builder walk back the way they came. */
static uint16_t pattern_step(diag_t* d, uint8_t dir_bits)
{
    if (dir_bits == COMBO_BTN_DOWN) {
        d->pattern = (uint8_t)((d->pattern + 1u) % DIAG_PATTERN_COUNT);
    } else if (dir_bits == COMBO_BTN_UP) {
        d->pattern = (uint8_t)((d->pattern + DIAG_PATTERN_COUNT - 1u)
                               % DIAG_PATTERN_COUNT);
    } else {
        return 0;
    }
    return DIAG_EV_REDRAW;
}

static uint16_t frameskip_step(diag_t* d, uint8_t dir_bits)
{
    uint8_t before = d->frameskip;
    int8_t dir = 0;

    if (dir_bits == COMBO_BTN_DOWN) {
        dir = +1;
    } else if (dir_bits == COMBO_BTN_UP) {
        dir = -1;
    } else {
        return 0;
    }

    d->frameskip = combo_step_u8(d->frameskip, dir, 0, DIAG_FRAMESKIP_MAX, 1);

    return (uint16_t)((d->frameskip != before)
                          ? (DIAG_EV_FRAMESKIP | DIAG_EV_REDRAW) : 0);
}


/* ─── Panel trim ─────────────────────────────────────────────────────────── */

/* The working porch as one number, in 64ths of a line, which is the unit the
 * fine knob steps in and the unit the correction comes out in. */
static int32_t trim_x64(const diag_t* d)
{
    return (int32_t)d->trim_fpa * 64 + (int32_t)d->trim_ratio;
}

static void trim_set_x64(diag_t* d, int32_t x64)
{
    if (x64 < DIAG_TRIM_MIN_X64) {
        x64 = DIAG_TRIM_MIN_X64;
    } else if (x64 > DIAG_TRIM_MAX_X64) {
        x64 = DIAG_TRIM_MAX_X64;
    }
    d->trim_fpa = (uint8_t)(x64 / 64);
    d->trim_ratio = (uint8_t)(x64 % 64);
}

/*
 * Left/Right a whole line, Up/Down DIAG_TRIM_FINE 64ths. The fine knob
 * carries into the coarse one, which is the fixture's model and worth
 * keeping: a null that sits just the other side of a line boundary is
 * otherwise reachable only by knowing to step the porch.
 */
static uint16_t trim_step(diag_t* d, uint8_t dir_bits)
{
    int32_t before = trim_x64(d);

    switch (dir_bits) {
    case COMBO_BTN_RIGHT:
        trim_set_x64(d, before + 64);
        break;
    case COMBO_BTN_LEFT:
        trim_set_x64(d, before - 64);
        break;
    case COMBO_BTN_UP:
        trim_set_x64(d, before + DIAG_TRIM_FINE);
        break;
    case COMBO_BTN_DOWN:
        trim_set_x64(d, before - DIAG_TRIM_FINE);
        break;
    default:
        break;
    }

    return (uint16_t)((trim_x64(d) != before) ? DIAG_EV_REDRAW : 0);
}

/* A run starts with nothing measured: the marks from the last one describe a
 * porch this one has already moved off. */
static void trim_run_begin(diag_t* d)
{
    d->trim_state = DIAG_TRIM_RUNNING;
    d->trim_marks = 0;
    d->trim_frames = 0;
    d->trim_first_frame = 0;
    d->trim_last_mark = 0;
    d->trim_int_min = 0;
    d->trim_int_max = 0;
    d->trim_int_last = 0;
    d->trim_lengthening = true;
    d->trim_verdict = DIAG_TRIM_NO_RUN;
    /* The pattern and the rates carry over between runs, because finding the
     * pair that shows a seam on this panel is the first thing a builder does
     * and having it reset every time would make that unbearable. The offset
     * does not: a run starts the field from a known place. */
    d->trim_ox = 0;
    d->trim_oy = 0;
}

/*
 * The correction, applied at the end of a run. One crossing is one frame of
 * slip, so a mean of N frames between crossings means the two rates differ by
 * one part in N and the frame's line count has to move by the same fraction.
 * The total is already in 64ths, which makes that a single divide.
 *
 * Nothing here needs a clock, an anchor, or a frequency: N is a count of
 * frames this page pushed, so the cadence being nulled is the one it ran at.
 */
static void trim_apply(diag_t* d)
{
    int32_t span = (int32_t)d->trim_span;
    int32_t lines_x64 = DIAG_TRIM_BASE_LINES_X64 + trim_x64(d);
    int32_t step;

    if (span <= 0) {
        d->trim_step = 0;
        return;
    }

    /* A run whose interval collapsed to well under the last one's was
     * corrected the wrong way: the beat grew. A quarter is the margin — wide
     * enough that a builder's reaction spread and the 64th-of-a-line rounding
     * cannot trip it, narrow enough that a genuine reversal always does. */
    if (d->trim_prev_span > 0 && span * 4 < (int32_t)d->trim_prev_span * 3) {
        d->trim_dir = (int8_t)-d->trim_dir;
    }

    step = (lines_x64 + span / 2) / span;
    /* Below one 64th there is nothing left to give: the register cannot
     * express a smaller change, and a run this long is already at the floor
     * the hardware sets. */
    if (step < 1) {
        d->trim_step = 0;
        return;
    }

    d->trim_step = (int16_t)(step * d->trim_dir);
    trim_set_x64(d, trim_x64(d) - (int32_t)d->trim_step);
}

/*
 * The fixture's scroll, stepped and clamped. Through zero and out the other
 * side rather than stopping there: which direction the field runs is half of
 * what a builder is hunting for, and a knob that would not cross zero would
 * hide the other half.
 */
static uint16_t trim_rate_step(diag_t* d, uint8_t dir_bits)
{
    int8_t before_x = d->trim_vx;
    int8_t before_y = d->trim_vy;

    switch (dir_bits) {
    case COMBO_BTN_UP:
        if (d->trim_vy < DIAG_TRIM_RATE_MAX) {
            d->trim_vy++;
        }
        break;
    case COMBO_BTN_DOWN:
        if (d->trim_vy > -DIAG_TRIM_RATE_MAX) {
            d->trim_vy--;
        }
        break;
    case COMBO_BTN_RIGHT:
        if (d->trim_vx < DIAG_TRIM_RATE_MAX) {
            d->trim_vx++;
        }
        break;
    case COMBO_BTN_LEFT:
        if (d->trim_vx > -DIAG_TRIM_RATE_MAX) {
            d->trim_vx--;
        }
        break;
    default:
        break;
    }

    return (uint16_t)((d->trim_vx != before_x || d->trim_vy != before_y)
                          ? DIAG_EV_TRIM_FIXTURE : 0);
}

/* The patterns wrap, like the display page's: there are four and no reason to
 * make a builder walk back the way they came. */
static uint16_t trim_pat_step(diag_t* d)
{
    d->trim_pat = (uint8_t)((d->trim_pat + 1u) % DIAG_TRIM_PAT_COUNT);
    return DIAG_EV_TRIM_FIXTURE;
}

/*
 * A crossing, marked. The first one only starts the baseline — an interval
 * needs two — and the run ends on the mark that completes DIAG_TRIM_MARKS
 * intervals, applying the correction the marks imply unless they disagree too
 * much to be a measurement.
 */
static uint16_t trim_mark(diag_t* d)
{
    uint32_t interval;

    if (d->trim_state != DIAG_TRIM_RUNNING) {
        return 0;
    }

    if (d->trim_marks == 0) {
        d->trim_marks = 1;
        d->trim_first_frame = d->trim_frames;
        d->trim_last_mark = d->trim_frames;
        return DIAG_EV_REDRAW;
    }

    /* A bounced button is not a crossing. The rejected press does not move
     * the baseline, so two contacts on one crossing fold into that crossing's
     * interval rather than splitting it in two. */
    interval = d->trim_frames - d->trim_last_mark;
    if (interval < (uint32_t)DIAG_TRIM_MIN_FRAMES) {
        return 0;
    }
    d->trim_last_mark = d->trim_frames;

    if (d->trim_int_min == 0 || interval < d->trim_int_min) {
        d->trim_int_min = interval;
    }
    if (interval > d->trim_int_max) {
        d->trim_int_max = interval;
    }
    if (d->trim_int_last != 0 && interval <= d->trim_int_last) {
        d->trim_lengthening = false;
    }
    d->trim_int_last = interval;

    d->trim_marks++;
    if (d->trim_marks <= (uint8_t)DIAG_TRIM_MARKS) {
        return DIAG_EV_REDRAW;
    }

    d->trim_state = DIAG_TRIM_IDLE;

    /*
     * Intervals that disagree by more than the threshold were not a count of
     * one repeating event, so the run is thrown away rather than averaged.
     * Nothing is written: not the span, not the previous span, not the
     * direction — a bad run that left any of those behind would go on to
     * decide the next good run's direction from a number that meant nothing.
     * Intervals that grew every time are still not averaged, but they are the
     * seam slowing down rather than the builder marking badly, and the page
     * says which.
     */
    if (d->trim_int_max * (uint32_t)DIAG_TRIM_SPREAD_DEN
        > d->trim_int_min * (uint32_t)DIAG_TRIM_SPREAD_NUM) {
        d->trim_verdict = d->trim_lengthening ? DIAG_TRIM_SLOWING
                                              : DIAG_TRIM_SCATTERED;
        d->trim_step = 0;
        return DIAG_EV_REDRAW | DIAG_EV_TRIM_STATE;
    }

    /* marks - 1 intervals between the first mark and this one. */
    d->trim_prev_span = d->trim_span;
    d->trim_span = (d->trim_frames - d->trim_first_frame)
                 / (uint32_t)(d->trim_marks - 1u);
    d->span_pat = d->trim_pat;
    d->span_vx = d->trim_vx;
    d->span_vy = d->trim_vy;
    d->trim_verdict = DIAG_TRIM_MEASURED;
    trim_apply(d);

    return DIAG_EV_REDRAW | DIAG_EV_TRIM_STATE;
}

static const char* const trim_pat_names[DIAG_TRIM_PAT_COUNT] = {
    "noise",
    "check",
    "stripe",
    "grid",
};

/* One block's shade in the noise field. The mix from the xorshift family,
 * over the block coordinates rather than a
 * sequence, so the field is stable in space and scrolls with the offset
 * instead of fizzing. */
static uint8_t trim_noise(int32_t u, int32_t v)
{
    uint32_t h = ((uint32_t)u / DIAG_TRIM_BLOCK_W) * 0x9E3779B1u
               ^ ((uint32_t)v / DIAG_TRIM_BLOCK_H) * 0x85EBCA77u;

    h ^= h >> 15;
    h *= 0x2545F491u;
    h ^= h >> 13;
    return (uint8_t)(h & 3u);
}

uint8_t diag_trim_shade(uint8_t pat, int32_t u, int32_t v)
{
    /* Unsigned before the divides and masks below, so the field stays endless
     * under a negative offset rather than folding at zero. */
    uint32_t uu = (uint32_t)u;
    uint32_t vv = (uint32_t)v;

    switch (pat) {
    case DIAG_TRIM_PAT_CHECK:
        /* Frequency on both axes, so it shows a displacement whichever way it
         * runs — and blind wherever the rate hits a multiple of two cells. */
        return (((uu / DIAG_TRIM_CHECK_CELL)
               ^ (vv / DIAG_TRIM_CHECK_CELL)) & 1u) ? 3u : 0u;
    case DIAG_TRIM_PAT_STRIPE:
        /* All of the frequency on the vertical axis, which is the one the
         * column-major seam displaces — the sharpest of these at the right
         * rate and blind at twice it. */
        return ((vv / DIAG_TRIM_STRIPE_BAND) & 1u) ? 3u : 0u;
    case DIAG_TRIM_PAT_GRID:
        /* The least sensitive, and the only one that lets the step be
         * COUNTED in pixels rather than just seen. */
        if (vv % DIAG_TRIM_GRID_PITCH == 0u) {
            return 3u;
        }
        if (uu % DIAG_TRIM_GRID_PITCH == 0u) {
            return 2u;
        }
        if ((uu + vv) % (2u * DIAG_TRIM_GRID_PITCH) < 2u) {
            return 1u;
        }
        return 0u;
    default:
        return trim_noise(u, v);
    }
}

const char* diag_trim_pat_name(uint8_t pat)
{
    if (pat >= DIAG_TRIM_PAT_COUNT) {
        return NULL;
    }
    return trim_pat_names[pat];
}

void diag_trim_offsets(const diag_t* d, int32_t* out_x, int32_t* out_y)
{
    if (d == NULL) {
        return;
    }
    if (out_x != NULL) {
        *out_x = d->trim_ox;
    }
    if (out_y != NULL) {
        *out_y = d->trim_oy;
    }
}

uint8_t diag_trim_pattern(const diag_t* d)
{
    return (d != NULL) ? d->trim_pat : (uint8_t)DIAG_TRIM_PAT_NOISE;
}

void diag_trim_rate(const diag_t* d, int8_t* out_vx, int8_t* out_vy)
{
    if (d == NULL) {
        return;
    }
    if (out_vx != NULL) {
        *out_vx = d->trim_vx;
    }
    if (out_vy != NULL) {
        *out_vy = d->trim_vy;
    }
}

uint16_t diag_trim_frame(diag_t* d)
{
    if (d == NULL || d->trim_state != DIAG_TRIM_RUNNING) {
        return 0;
    }
    d->trim_frames++;
    /* Accumulated, so a rate changed mid-run moves the field on from where it
     * had got to rather than teleporting it. */
    d->trim_ox += d->trim_vx;
    d->trim_oy += d->trim_vy;

    /* Counted from the run's start until the first mark, then from the last
     * one. Like a rejected run, a settled one writes nothing: there is no
     * interval to correct from, and none is needed. */
    if (d->trim_frames - d->trim_last_mark >= DIAG_TRIM_SETTLED_FRAMES) {
        d->trim_state = DIAG_TRIM_IDLE;
        d->trim_verdict = DIAG_TRIM_SETTLED;
        d->trim_step = 0;
        return DIAG_EV_REDRAW | DIAG_EV_TRIM_STATE;
    }
    return 0;
}

bool diag_trim_running(const diag_t* d)
{
    return (d != NULL) && d->trim_state == DIAG_TRIM_RUNNING;
}

void diag_trim(const diag_t* d, uint8_t* fpa, uint8_t* ratio)
{
    if (d == NULL) {
        return;
    }
    if (fpa != NULL) {
        *fpa = d->trim_fpa;
    }
    if (ratio != NULL) {
        *ratio = d->trim_ratio;
    }
}

uint32_t diag_trim_span(const diag_t* d)
{
    return (d != NULL) ? d->trim_span : 0u;
}

bool diag_trim_rejected(const diag_t* d)
{
    return (d != NULL) && (d->trim_verdict == DIAG_TRIM_SCATTERED
                           || d->trim_verdict == DIAG_TRIM_SLOWING
                           || d->trim_verdict == DIAG_TRIM_SETTLED);
}

uint8_t diag_trim_verdict(const diag_t* d)
{
    return (d != NULL) ? d->trim_verdict : (uint8_t)DIAG_TRIM_NO_RUN;
}

void diag_trim_stored(const diag_t* d, uint8_t* fpa, uint8_t* ratio)
{
    if (d == NULL) {
        return;
    }
    if (fpa != NULL) {
        *fpa = d->stored_trim_fpa;
    }
    if (ratio != NULL) {
        *ratio = d->stored_trim_ratio;
    }
}

bool diag_trim_unsaved(const diag_t* d)
{
    if (d == NULL) {
        return false;
    }
    return d->trim_fpa != d->stored_trim_fpa
        || d->trim_ratio != d->stored_trim_ratio;
}

int8_t diag_trim_dir(const diag_t* d)
{
    return (d != NULL) ? d->trim_dir : (int8_t)+1;
}

void diag_trim_set_dir(diag_t* d, int8_t dir)
{
    if (d == NULL) {
        return;
    }
    d->trim_dir = (dir < 0) ? (int8_t)-1 : (int8_t)+1;
}

/*
 * The held-direction cadence, taken from the list module so a nudge held down
 * moves at the same rate as a cursor held down: nothing for the first
 * COMBO_REPEAT_DELAY_MS, then one step every COMBO_REPEAT_MS.
 */
static bool direction_due(diag_t* d, uint8_t dir_bits, uint32_t now_ms)
{
    if (dir_bits == 0
        || (uint8_t)(dir_bits & (uint8_t)(dir_bits - 1)) != 0) {
        /* Two at once is a fumble rather than an instruction. Clearing the
         * held bit as well means whichever direction survives it acts
         * immediately. */
        d->held_dir = 0;
        return false;
    }

    if (dir_bits != d->held_dir) {
        d->held_dir = dir_bits;
        d->repeat_due_ms = now_ms + (uint32_t)COMBO_REPEAT_DELAY_MS;
        return true;
    }
    if ((int32_t)(now_ms - d->repeat_due_ms) >= 0) {
        /* The deadline comes off now_ms rather than off its own previous
         * value, matching the combo and list modules: a stall costs one late
         * step instead of a burst of catch-up steps. */
        d->repeat_due_ms = now_ms + (uint32_t)COMBO_REPEAT_MS;
        return true;
    }
    return false;
}

uint16_t diag_input(diag_t* d, uint8_t combo_event, uint8_t joypad,
                    uint32_t now_ms)
{
    uint16_t ev = 0;
    uint8_t pressed;
    uint8_t dir_bits;

    if (d == NULL) {
        return 0;
    }

    if (combo_event == COMBO_EVENT_BRIGHT_UP) {
        d->prev_word = joypad;
        return change_page(d, +1);
    }
    if (combo_event == COMBO_EVENT_BRIGHT_DOWN) {
        d->prev_word = joypad;
        return change_page(d, -1);
    }
    /* Every other event — the menu combo included — means nothing in this
     * mode: there is nothing to open and nowhere to go back to. */

    pressed = (uint8_t)(joypad & ~d->prev_word);
    d->prev_word = joypad;

    dir_bits = (uint8_t)(joypad & DIAG_DPAD_MASK);
    if (direction_due(d, dir_bits, now_ms)) {
        switch (d->page) {
        case DIAG_PAGE_NUDGE:
            ev |= nudge_step(d, dir_bits);
            break;
        case DIAG_PAGE_AUDIO:
            ev |= audio_step(d, dir_bits);
            break;
        case DIAG_PAGE_DISPLAY:
            ev |= pattern_step(d, dir_bits);
            break;
        case DIAG_PAGE_TRIM:
            /* Two jobs, and the page's state picks which. Idle, the D-pad is
             * the porch. Running, it is the fixture's scroll — the porch must
             * not move under a count that is measuring it, and the scroll is
             * exactly what a builder needs to reach while watching. */
            if (d->trim_state == DIAG_TRIM_IDLE) {
                ev |= trim_step(d, dir_bits);
            } else {
                ev |= trim_rate_step(d, dir_bits);
            }
            break;
        case DIAG_PAGE_SYSTEM:
            ev |= frameskip_step(d, dir_bits);
            break;
        default:
            /* Buttons, SD, tag and battery are readouts: the D-pad has
             * nothing to move on them. */
            break;
        }
    }

    if (pressed & COMBO_BTN_A) {
        switch (d->page) {
        case DIAG_PAGE_NUDGE:
            d->toast = true;
            d->toast_until_ms = now_ms + (uint32_t)DIAG_TOAST_MS;
            ev |= DIAG_EV_SAVE_NUDGE | DIAG_EV_REDRAW;
            break;
        case DIAG_PAGE_NFC:
            ev |= DIAG_EV_NFC_SCAN;
            break;
        case DIAG_PAGE_AUDIO:
            d->tone_on = !d->tone_on;
            ev |= DIAG_EV_TONE | DIAG_EV_REDRAW;
            break;
        case DIAG_PAGE_TRIM:
            if (d->trim_state == DIAG_TRIM_RUNNING) {
                /* Abandon the run rather than save from it: half a count is
                 * not a measurement. */
                d->trim_state = DIAG_TRIM_IDLE;
                ev |= DIAG_EV_REDRAW | DIAG_EV_TRIM_STATE;
            } else {
                /* The binding writes the store on this event and has no way
                 * to fail it, so the page records the write here rather than
                 * waiting to be told. */
                d->stored_trim_fpa = d->trim_fpa;
                d->stored_trim_ratio = d->trim_ratio;
                d->toast = true;
                d->toast_until_ms = now_ms + (uint32_t)DIAG_TOAST_MS;
                ev |= DIAG_EV_SAVE_TRIM | DIAG_EV_REDRAW;
            }
            break;
        case DIAG_PAGE_SYSTEM:
            d->list_mode = !d->list_mode;
            ev |= DIAG_EV_LIST_MODE | DIAG_EV_REDRAW;
            break;
        default:
            break;
        }
    }

    if (pressed & COMBO_BTN_B) {
        if (d->page == DIAG_PAGE_NUDGE) {
            d->x = d->default_x;
            d->y = d->default_y;
            ev |= DIAG_EV_REDRAW;
        } else if (d->page == DIAG_PAGE_TRIM
                   && d->trim_state == DIAG_TRIM_RUNNING) {
            ev |= trim_pat_step(d);
        } else if (d->page == DIAG_PAGE_TRIM
                   && d->trim_state == DIAG_TRIM_IDLE) {
            d->trim_fpa = d->default_trim_fpa;
            d->trim_ratio = d->default_trim_ratio;
            /* The direction this unit was heading is a property of the porch
             * it was heading from, so it goes back with it. */
            d->trim_dir = +1;
            d->trim_span = 0;
            d->trim_prev_span = 0;
            d->trim_step = 0;
            ev |= DIAG_EV_REDRAW;
        }
    }

    /* Start is the crossing mark, and the trim page is the only place it
     * means anything: every other page leaves it to the combo module, which
     * is where Start+Select got the builder into this mode in the first
     * place. It starts the run as well as marking within one, so the builder
     * never has to reach for a second button mid-count. */
    if ((pressed & COMBO_BTN_START) && d->page == DIAG_PAGE_TRIM) {
        if (d->trim_state == DIAG_TRIM_IDLE) {
            trim_run_begin(d);
            ev |= DIAG_EV_REDRAW | DIAG_EV_TRIM_STATE;
        } else {
            ev |= trim_mark(d);
        }
    }

    return ev;
}

uint16_t diag_tick(diag_t* d, uint32_t now_ms)
{
    if (d == NULL) {
        return 0;
    }
    if (d->toast && (int32_t)(now_ms - d->toast_until_ms) >= 0) {
        d->toast = false;
        return DIAG_EV_REDRAW;
    }
    return 0;
}

uint8_t diag_page(const diag_t* d)
{
    return (d != NULL) ? d->page : (uint8_t)DIAG_PAGE_BUTTONS;
}

void diag_origin(const diag_t* d, int16_t* out_x, int16_t* out_y)
{
    if (d == NULL) {
        return;
    }
    if (out_x != NULL) {
        *out_x = d->x;
    }
    if (out_y != NULL) {
        *out_y = d->y;
    }
}

bool diag_tone_on(const diag_t* d)
{
    return (d != NULL) ? d->tone_on : false;
}

uint8_t diag_volume(const diag_t* d)
{
    return (d != NULL) ? d->volume : (uint8_t)MIX_VOL_OFF;
}

uint8_t diag_pattern(const diag_t* d)
{
    return (d != NULL) ? d->pattern : (uint8_t)DIAG_PATTERN_BARS;
}

uint8_t diag_frameskip(const diag_t* d)
{
    return (d != NULL) ? d->frameskip : 0;
}

void diag_set_list_mode(diag_t* d, bool on)
{
    if (d != NULL) {
        d->list_mode = on;
    }
}

bool diag_list_mode(const diag_t* d)
{
    return (d != NULL) ? d->list_mode : false;
}

bool diag_toast_active(const diag_t* d, uint32_t now_ms)
{
    if (d == NULL || !d->toast) {
        return false;
    }
    return (int32_t)(now_ms - d->toast_until_ms) < 0;
}

const char* diag_page_title(uint8_t page)
{
    if (page >= DIAG_PAGE_COUNT) {
        return NULL;
    }
    return page_titles[page];
}
