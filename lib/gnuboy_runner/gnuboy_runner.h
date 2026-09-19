#pragma once
// Headless gnuboy runner — TEST-ONLY. Boots an in-memory ROM and captures the
// raw 160x144 index buffer (gnuboy pixel bytes, before any palette LUT).
// Never reference this from anything under src/: it must never link into the
// firmware.
//
// Pure C, no Arduino/ESP-IDF headers. The ROM buffer is caller-owned and must
// stay valid for the lifetime of the run — gnuboy points its bank table
// straight into it rather than copying.

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define GNUBOY_RUNNER_W 160
#define GNUBOY_RUNNER_H 144

enum gnuboy_runner_result_e {
    GNUBOY_RUNNER_OK = 0,
    GNUBOY_RUNNER_ERR_ARGS = -1, /* NULL/empty ROM buffer   */
    GNUBOY_RUNNER_ERR_INIT = -2, /* gnuboy rejected the ROM */
};

/* Boot the emulator on the in-memory ROM. Resets all runner state. */
int gnuboy_runner_init(const uint8_t* rom, size_t len);

/* Run n frames. Returns GNUBOY_RUNNER_OK, or GNUBOY_RUNNER_ERR_INIT if not
 * booted. */
int gnuboy_runner_run_frames(unsigned n);

/* The captured index buffer: GNUBOY_RUNNER_H rows of GNUBOY_RUNNER_W raw
 * gnuboy pixel bytes — background 0-3, window 4-7, OBP0 32-35, OBP1 36-39,
 * carrying the tile's RAW two bits, not the shade the palette register
 * selects. NULL before a successful init. */
const uint8_t* gnuboy_runner_frame(void);

/*
 * The DMG palette registers as they stood after the last emulated frame.
 *
 * These are not a convenience: gnuboy applies BGP / OBP0 / OBP1 when it
 * builds its colour table, not when it draws the pixel, so the index buffer
 * above cannot be resolved to a shade without them. The other core's buffer
 * needs no equivalent because it bakes the register into the pixel byte.
 */
uint8_t gnuboy_runner_bgp(void);
uint8_t gnuboy_runner_obp0(void);
uint8_t gnuboy_runner_obp1(void);

/* The sound unit's output for the last emulated frame: interleaved stereo,
 * left then right, gnuboy_runner_audio_samples() frames long. NULL before a
 * successful init. */
const int16_t* gnuboy_runner_audio(void);

/*
 * Samples per channel in the LAST emulated frame. Unlike the Peanut-GB
 * runner's, this varies: gnuboy emits a sample every snd.rate cycles, so the
 * count follows the frame's real emulated length.
 */
unsigned gnuboy_runner_audio_samples(void);

/* The cartridge RAM gnuboy allocated, and its length in bytes. This is the
 * allocation, not the header's save size — gnuboy rounds up to whole 8 KB
 * banks. NULL before a successful init. */
const uint8_t* gnuboy_runner_cart_ram(size_t* len);

/*
 * Failures the runner itself saw: a rejected ROM, or a frame run before boot.
 * gnuboy has no error callback to register, unlike the other core, so this
 * counts what is observable here rather than pretending to more.
 */
unsigned gnuboy_runner_error_count(void);

/*
 * Per-line hook bookkeeping for the last gnuboy_runner_run_frames(1) call.
 *
 * `line_calls` is every hook fire in that run, and it is NOT capped at 144:
 * gnuboy's run loop tests the line counter between CPU steps, so a step that
 * carries the LCD past the last line and around to the top goes unnoticed and
 * the run continues into the next frame. The first run after a reset draws two
 * frames' worth. A front end that treats one run as one frame gets the frame
 * boundary wrong, which is why this is reported rather than hidden.
 *
 * `frames` counts LCD frames in that run by the only reliable rule — the line
 * number failing to advance — and `lines_ordered` is whether, within each of
 * those frames, the numbers ran 0..143 with no repeats and no gaps.
 */
unsigned gnuboy_runner_line_calls(void);
unsigned gnuboy_runner_frames(void);
int gnuboy_runner_lines_ordered(void);

#ifdef __cplusplus
}
#endif
