#pragma once
// Headless Peanut-GB runner — TEST-ONLY. Boots an in-memory ROM and captures
// the raw 160x144 index buffer (Peanut-GB pixel bytes, before any palette
// LUT). Never reference this from anything under src/: it must never link
// into the firmware.
//
// Pure C, no Arduino/ESP-IDF headers. The ROM buffer is caller-owned and
// must stay valid for the lifetime of the run.

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define GB_RUNNER_W 160
#define GB_RUNNER_H 144

enum gb_runner_result_e {
    GB_RUNNER_OK = 0,
    GB_RUNNER_ERR_ARGS = -1, /* NULL/empty ROM buffer */
    GB_RUNNER_ERR_INIT = -2, /* gb_init rejected the ROM */
};

/* Boot the emulator on the in-memory ROM. Resets all runner state. */
int gb_runner_init(const uint8_t* rom, size_t len);

/* Run n frames. Returns GB_RUNNER_OK, or GB_RUNNER_ERR_INIT if not booted. */
int gb_runner_run_frames(unsigned n);

/* The captured index buffer: GB_RUNNER_H rows of GB_RUNNER_W raw Peanut-GB
 * pixel bytes (shade in bits 1-0, palette id in bits 5-4). NULL before a
 * successful init. */
const uint8_t* gb_runner_frame(void);

/* Number of times the Peanut-GB error callback fired since init. */
unsigned gb_runner_error_count(void);

#ifdef __cplusplus
}
#endif
