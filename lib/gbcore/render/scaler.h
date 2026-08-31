#pragma once
// Render scaler seam — WS-03 fills the real logic.
//
// Model (design: 2 source pixels -> 3 destination pixels, both axes, 3/2
// scale): each pair of adjacent source pixels (a, b) expands to the triple
// (a, blend(a, b), b), and each pair of adjacent source lines expands to
// three output rows the same way. 160x144 -> 240x216.
//
// Placeholder semantics (this workstream): nearest-neighbour — the middle
// element duplicates the LEFT/TOP source instead of blending, i.e. the
// triple is (a, a, b) and the middle output row is a copy of the top
// scaled row. scaler_avg565() likewise returns its first argument. WS-03
// replaces both with the true RGB565 average without changing signatures.
//
// Pure C, no Arduino/ESP-IDF headers, no allocation: all buffers are
// caller-owned.

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SCALER_SRC_W 160  /* Game Boy line width, pixels */
#define SCALER_DST_W 240  /* output row width, pixels    */

enum scaler_result_e {
    SCALER_OK = 0,
    SCALER_ERR_ARGS = -1, /* NULL buffer */
};

/*
 * Scale two adjacent source lines (SCALER_SRC_W px each) into three output
 * rows (SCALER_DST_W px each). src0 is the upper source line, src1 the
 * lower; dst0..dst2 are the corresponding top/middle/bottom output rows.
 * Returns SCALER_OK or SCALER_ERR_ARGS.
 */
int scaler_scale_pair(const uint16_t* src0, const uint16_t* src1,
                      uint16_t* dst0, uint16_t* dst1, uint16_t* dst2);

/*
 * Blend of two native-bit-layout RGB565 pixels for the interpolated middle
 * element. Placeholder: returns a (nearest-neighbour). WS-03 fills the real
 * average; callers must pass NATIVE (not byte-swapped) RGB565 values.
 */
uint16_t scaler_avg565(uint16_t a, uint16_t b);

#ifdef __cplusplus
}
#endif
