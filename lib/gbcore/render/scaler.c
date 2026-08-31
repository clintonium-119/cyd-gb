#include "scaler.h"

uint16_t scaler_avg565(uint16_t a, uint16_t b)
{
    /* Placeholder: nearest-neighbour keeps the left/top pixel. WS-03
     * replaces with the true RGB565 average. */
    (void)b;
    return a;
}

/* 160 -> 240: each source pair (a, b) becomes (a, blend, b). */
static void scale_line(const uint16_t* src, uint16_t* dst)
{
    size_t o = 0;
    size_t x;
    for (x = 0; x + 1 < SCALER_SRC_W; x += 2) {
        uint16_t a = src[x];
        uint16_t b = src[x + 1];
        dst[o++] = a;
        dst[o++] = scaler_avg565(a, b);
        dst[o++] = b;
    }
}

int scaler_scale_pair(const uint16_t* src0, const uint16_t* src1,
                      uint16_t* dst0, uint16_t* dst1, uint16_t* dst2)
{
    size_t x;
    if (src0 == NULL || src1 == NULL ||
        dst0 == NULL || dst1 == NULL || dst2 == NULL) {
        return SCALER_ERR_ARGS;
    }

    scale_line(src0, dst0);
    scale_line(src1, dst2);
    /* Middle row: blend of the two scaled rows. With the placeholder
     * scaler_avg565 this duplicates the top row (nearest-neighbour). */
    for (x = 0; x < SCALER_DST_W; x++) {
        dst1[x] = scaler_avg565(dst0[x], dst2[x]);
    }
    return SCALER_OK;
}
