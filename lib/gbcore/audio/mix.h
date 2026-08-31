#pragma once
// Audio mix seam (mono sum, volume scale, dither) — WS-08 fills the real
// logic.
//
// Placeholder semantics (this workstream): mix_mono() rejects every input
// with MIX_ERR_NOT_IMPLEMENTED and writes nothing to out.
//
// Pure C, no Arduino/ESP-IDF headers, no allocation.

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

enum mix_result_e {
    MIX_OK = 0,
    MIX_ERR_NOT_IMPLEMENTED = -1, /* stub: mixing not implemented yet */
    MIX_ERR_ARGS = -2,            /* NULL buffer */
};

/*
 * Mix n_samples of stereo left/right into mono out, applying the 0..255
 * volume scale and dither. All buffers caller-owned, n_samples each.
 * WS-08 fills.
 */
int mix_mono(const int16_t* left, const int16_t* right, size_t n_samples,
             uint8_t volume, int16_t* out);

#ifdef __cplusplus
}
#endif
