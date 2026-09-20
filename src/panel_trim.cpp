#include "panel_trim.h"

#ifdef PANEL_TRIM
#include "button_input.h"
#include "display.h"
#include "hw_config.h"
#include "render_config.h"
#include <Arduino.h>
#include <stdlib.h>
#include <string.h>

// ─── Panel rate trim ────────────────────────────────────────────────────────
// See include/panel_trim.h for what this is and why the knob is the porch.

// ST7789 PORCTRL. The last three parameters are the power-on defaults and are
// not trimmed: PSEN off, and the idle and partial-mode porches left alone.
#define ST7789_PORCTRL 0xB2
#define PORCH_BPA_DEFAULT 0x0C
#define PORCH_FPA_DEFAULT 0x0C
// Gate lines the panel scans, before the porch is added.
#define PANEL_LINES 320

// The emulator's frame period. Audio is the pacer — the speaker's DMA write
// blocks — so the rate is AUDIO_SAMPLE_RATE / 548.62 = 59.7275 fps, and this
// is that in nanoseconds. Held in ns because the truncation to whole
// microseconds is 5 parts per 100,000, which is the same order as the drift
// being trimmed out.
#define FRAME_NS 16742900

// Stripe field: vertical bars that slide sideways a quarter of a period each
// frame. A tear seam is a boundary between two frames, so it shows here as a
// jog in every bar crossing it — visible without the whole screen flickering,
// which an anti-phase pattern would do.
#define STRIPE_PERIOD 16
#define STRIPE_STEP    4

// Two, alternating, for the same reason the frame path has two: a pushed
// buffer belongs to the driver until the transfer completes, so the next
// block cannot be built in the one still going out. Heap rather than static -
// 10,640 bytes would not fit beside the emulator's own buffers.
static uint16_t* block[2];

static void write_porch(uint8_t fpa)
{
    tft.writecommand(ST7789_PORCTRL);
    tft.writedata(PORCH_BPA_DEFAULT);
    tft.writedata(fpa);
    tft.writedata(0x00);
    tft.writedata(0x33);
    tft.writedata(0x33);
}

/* Average frame rate the panel would run at, if its oscillator were nominal.
 * The absolute number is not trustworthy - the oscillator is a +/-10 % part -
 * but the ratio between two settings is, which is all the trim needs. */
static double nominal_hz(uint8_t fpa, uint8_t ratio)
{
    double lines = (double)PANEL_LINES + (double)PORCH_BPA_DEFAULT
                 + (double)fpa + (double)ratio / 64.0;

    return 60.0 * (double)(PANEL_LINES + PORCH_BPA_DEFAULT + PORCH_FPA_DEFAULT)
                / lines;
}

static void report(uint8_t fpa, uint8_t ratio)
{
    double hz = nominal_hz(fpa, ratio);

    Serial.printf("[TRIM] porch %u + %u/64  nominal %.4f Hz  "
                  "beat vs 59.7275 %+.4f Hz\n",
                  (unsigned)fpa, (unsigned)ratio, hz, hz - 59.7275);
}

/* One frame of the stripe field, pushed the way the frame path pushes. */
static void push_pattern(unsigned phase)
{
    unsigned blocks = GAME_H / BLOCK_ROWS;
    unsigned buf = 0;
    unsigned b;
    unsigned i;
    unsigned x;

    display_frame_begin(GAME_X, GAME_Y);
    for (b = 0; b < blocks; b++) {
        uint16_t* px = block[buf];

        for (x = 0; x < GAME_W; x++) {
            px[x] = ((x + phase) % STRIPE_PERIOD < STRIPE_PERIOD / 2)
                  ? 0xFFFF : 0x0000;
        }
        // Every row of a block is the same bar pattern, so the first row is
        // copied down rather than recomputed.
        for (i = 1; i < BLOCK_ROWS; i++) {
            memcpy(px + (size_t)i * GAME_W, px,
                   (size_t)GAME_W * sizeof(px[0]));
        }
        display_push_rows_dma(px, (size_t)BLOCK_ROWS * GAME_W);
        buf ^= 1u;
    }
    display_dma_wait();
    display_frame_end();
}

void panel_trim_run()
{
    uint8_t fpa = PORCH_FPA_DEFAULT;
    uint8_t ratio = 0;          // sixty-fourths of a line added on average
    uint8_t acc = 0;
    uint8_t applied = 0xFF;     // forces the first write
    uint16_t prev_btn = 0;
    unsigned phase = 0;
    int64_t next = esp_timer_get_time() * 1000;

    block[0] = (uint16_t*)malloc((size_t)BLOCK_ROWS * GAME_W * sizeof(uint16_t));
    block[1] = (uint16_t*)malloc((size_t)BLOCK_ROWS * GAME_W * sizeof(uint16_t));
    if (!block[0] || !block[1]) {
        Serial.println("[TRIM] no heap for the pattern buffers");
        for (;;) {
            delay(1000);
        }
    }

    Serial.println("[TRIM] panel rate trim. Left/Right porch, Down/Up dither,");
    Serial.println("[TRIM] A prints state, B restores the power-on porch.");
    Serial.println("[TRIM] Null the seam: coarse until it crawls, then fine.");
    report(fpa, ratio);

    for (;;) {
        uint16_t btn;
        uint16_t edge;
        uint8_t want;

        // Fractional divider: the porch is a whole number of lines, so the
        // average is made by spending `ratio` frames in 64 on the longer one.
        acc = (uint8_t)(acc + ratio);
        want = fpa;
        if (acc >= 64) {
            acc = (uint8_t)(acc - 64);
            want = (uint8_t)(fpa + 1);
        }
        if (want != applied) {
            write_porch(want);
            applied = want;
        }

        push_pattern(phase);
        phase += STRIPE_STEP;

        button_update();
        btn = button_get_buttons();
        edge = (uint16_t)(btn & ~prev_btn);
        prev_btn = btn;
        if (edge & GB_BTN_RIGHT) {
            if (fpa < 0x7E) {
                fpa++;
            }
            report(fpa, ratio);
        }
        if (edge & GB_BTN_LEFT) {
            if (fpa > 1) {
                fpa--;
            }
            report(fpa, ratio);
        }
        if (edge & GB_BTN_UP) {
            if (ratio < 63) {
                ratio++;
            }
            report(fpa, ratio);
        }
        if (edge & GB_BTN_DOWN) {
            if (ratio > 0) {
                ratio--;
            }
            report(fpa, ratio);
        }
        if (edge & GB_BTN_A) {
            report(fpa, ratio);
        }
        if (edge & GB_BTN_B) {
            fpa = PORCH_FPA_DEFAULT;
            ratio = 0;
            acc = 0;
            report(fpa, ratio);
        }

        // Pace to the emulator's frame period, from the same crystal the
        // emulator's audio clock divides down.
        next += FRAME_NS;
        {
            int64_t now = esp_timer_get_time() * 1000;
            if (now < next) {
                delayMicroseconds((uint32_t)((next - now) / 1000));
            } else {
                next = now;
            }
        }
    }
}
#endif
