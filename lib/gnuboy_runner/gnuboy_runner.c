#include "gnuboy_runner.h"

#include <string.h>

#include "gnuboy.h"
#include "hw.h"

/*
 * Mirrors the firmware's wiring in src/emulator_bridge_gnuboy.cpp — gnuboy's
 * framebuffer IS the captured frame, the per-line hook does the bookkeeping,
 * and the sound unit fills a caller-owned buffer — with none of the display,
 * queue or speaker path.
 *
 * The same headroom rule as the bridge: gnuboy wraps and loses samples if a
 * frame fills the buffer, since no audio callback is passed for it to flush
 * through, and the first frame after a reset emits more than a steady one.
 */
#define GNUBOY_RUNNER_RATE 32768
#define GNUBOY_RUNNER_PER_FRAME 548
#define GNUBOY_RUNNER_HEADROOM 64
#define GNUBOY_RUNNER_AUDIO_TOTAL \
    (2 * (GNUBOY_RUNNER_PER_FRAME + GNUBOY_RUNNER_HEADROOM))

static uint8_t frame[GNUBOY_RUNNER_H * GNUBOY_RUNNER_W];
static int16_t audio_frame[GNUBOY_RUNNER_AUDIO_TOTAL];
static unsigned error_count;
static int booted;

/* Per-frame hook bookkeeping. Reset at the top of every emulated frame, so
 * they describe the last one and not the run. */
static unsigned line_calls;
static int lines_ordered;
static int next_line;

/*
 * The vendored renderer's per-line hook. gnuboy has already written the line
 * into its framebuffer — which is `frame` — by the time this fires, so there
 * is nothing to copy here and the argument goes unused, exactly as in the
 * firmware's bridge.
 */
void emu_gnuboy_line(const unsigned char* line, int index)
{
    (void)line;
    if (index != next_line) {
        lines_ordered = 0;
    }
    next_line = index + 1;
    line_calls++;
}

int gnuboy_runner_init(const uint8_t* rom, size_t len)
{
    booted = 0;
    error_count = 0;
    line_calls = 0;
    lines_ordered = 1;
    next_line = 0;
    memset(frame, 0, sizeof(frame));
    memset(audio_frame, 0, sizeof(audio_frame));

    if (rom == NULL || len == 0) {
        error_count++;
        return GNUBOY_RUNNER_ERR_ARGS;
    }
    /* A real sample rate even though nothing listens: gnuboy derives its
     * sample counter from it and a zero divisor leaves that counter unable to
     * advance, which hangs the core outright. Stereo because that is the
     * format the firmware's mixer takes. */
    if (gnuboy_init(GNUBOY_RUNNER_RATE, GB_AUDIO_STEREO_S16,
                    GB_PIXEL_PALETTED, NULL, NULL) != 0) {
        error_count++;
        return GNUBOY_RUNNER_ERR_INIT;
    }
    gnuboy_set_hwtype(GB_HW_DMG);
    if (gnuboy_load_rom(rom, len) != 0) {
        error_count++;
        return GNUBOY_RUNNER_ERR_INIT;
    }
    gnuboy_set_framebuffer(frame);
    gnuboy_set_soundbuffer(audio_frame,
                           sizeof(audio_frame) / sizeof(audio_frame[0]));
    gnuboy_reset(true);
    booted = 1;
    return GNUBOY_RUNNER_OK;
}

int gnuboy_runner_run_frames(unsigned n)
{
    unsigned i;

    if (!booted) {
        error_count++;
        return GNUBOY_RUNNER_ERR_INIT;
    }
    for (i = 0; i < n; i++) {
        line_calls = 0;
        lines_ordered = 1;
        next_line = 0;
        gnuboy_run(true);
    }
    return GNUBOY_RUNNER_OK;
}

const uint8_t* gnuboy_runner_frame(void)
{
    return booted ? frame : NULL;
}

uint8_t gnuboy_runner_bgp(void)
{
    return booted ? (uint8_t)R_BGP : 0;
}

uint8_t gnuboy_runner_obp0(void)
{
    return booted ? (uint8_t)R_OBP0 : 0;
}

uint8_t gnuboy_runner_obp1(void)
{
    return booted ? (uint8_t)R_OBP1 : 0;
}

const int16_t* gnuboy_runner_audio(void)
{
    return booted ? audio_frame : NULL;
}

unsigned gnuboy_runner_audio_samples(void)
{
    return booted ? (unsigned)(GB.audio.pos / 2u) : 0u;
}

const uint8_t* gnuboy_runner_cart_ram(size_t* len)
{
    if (len) {
        *len = booted ? (size_t)cart.ramsize * 8192u : 0u;
    }
    return booted ? (const uint8_t*)cart.rambanks : NULL;
}

unsigned gnuboy_runner_error_count(void)
{
    return error_count;
}

unsigned gnuboy_runner_line_calls(void)
{
    return line_calls;
}

int gnuboy_runner_lines_ordered(void)
{
    return lines_ordered;
}
