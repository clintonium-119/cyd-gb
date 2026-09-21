#include "scale_bench.h"

#ifdef SCALE_BENCH

#include <Arduino.h>
#include <esp_timer.h>

#include "hw_config.h"
#include "render_config.h"
#include "render/palette.h"
#include "render/scaler.h"

// Frames per printed round. Enough that the mean is not one outlier and short
// enough that a round lands every second or so, which is how a builder
// watching the serial log judges the spread.
#define BENCH_FRAMES 30u

// The widest thing either arm writes: a row block is BLOCK_ROWS x GAME_W, a
// column block UNIT_ROWS columns of GAME_H.
#define OUT_PX_ROW ((size_t)BLOCK_ROWS * GAME_W)
#define OUT_PX_COL ((size_t)UNIT_ROWS * GAME_H)
#define OUT_PX (OUT_PX_ROW > OUT_PX_COL ? OUT_PX_ROW : OUT_PX_COL)
#define SCRATCH_PX (GAME_W > GAME_H ? GAME_W : GAME_H)

// Everything of frame size goes on the heap: the static segment has about
// 15 KB spare with the frame path's buffers in place, and the transposed
// source frame alone is 46 KB.
static uint8_t* idx_frame;  // GB_SCREEN_H x GB_SCREEN_W raw pixel bytes
static uint16_t* lut;
static uint16_t* row_lines; // (BLOCK_LINES + 1) x SCALER_SRC_W
static uint16_t* col_src;   // SCALER_SRC_W columns of GB_SCREEN_H
static uint16_t* out;
static uint16_t* scratch;

static const uint16_t* row_ptrs[BLOCK_LINES + 1];
static const uint16_t* col_ptrs[SCALER_SRC_W];

static const scaler_geom_info_t* geom;

// One half's accumulated cost across a round, in microseconds.
typedef struct {
    uint32_t lut_sum;
    uint32_t lut_max;
    uint32_t scale_sum;
    uint32_t scale_max;
} arm_t;

static void arm_add(arm_t* a, uint32_t lut_us, uint32_t scale_us)
{
    a->lut_sum += lut_us;
    a->scale_sum += scale_us;
    if (lut_us > a->lut_max) {
        a->lut_max = lut_us;
    }
    if (scale_us > a->scale_max) {
        a->scale_max = scale_us;
    }
}

// Row-major: the consumer's own loop, block by block, including the lookahead
// line it colourizes twice for the geometries that read one.
static void bench_row(arm_t* a)
{
    unsigned blocks = GB_SCREEN_H / BLOCK_LINES;
    uint32_t lut_us = 0;
    uint32_t scale_us = 0;
    unsigned b;

    for (b = 0; b < blocks; b++) {
        unsigned first = b * BLOCK_LINES;
        unsigned lines = BLOCK_LINES;
        const uint16_t* lookahead = nullptr;
        unsigned i;
        unsigned u;
        unsigned x;
        int64_t t0;
        int64_t t1;

        if (geom->uses_lookahead && b + 1u < blocks) {
            lines++;
        }
        t0 = esp_timer_get_time();
        for (i = 0; i < lines; i++) {
            const uint8_t* src = idx_frame + (size_t)(first + i) * SCALER_SRC_W;
            uint16_t* dst = row_lines + (size_t)i * SCALER_SRC_W;
            for (x = 0; x < SCALER_SRC_W; x++) {
                dst[x] = lut[src[x]];
            }
        }
        t1 = esp_timer_get_time();
        lut_us += (uint32_t)(t1 - t0);

        if (lines > BLOCK_LINES) {
            lookahead = row_ptrs[BLOCK_LINES];
        }
        for (u = 0; u < BLOCK_UNITS; u++) {
            const uint16_t* la = (u + 1u < BLOCK_UNITS)
                ? row_ptrs[(u + 1u) * UNIT_LINES] : lookahead;
            (void)scaler_scale_block(SCALE_GEOM, SCALER_MODE_BLEND,
                                     row_ptrs + u * UNIT_LINES, la,
                                     out + (size_t)u * UNIT_ROWS * GAME_W,
                                     scratch);
        }
        scale_us += (uint32_t)(esp_timer_get_time() - t1);
    }
    arm_add(a, lut_us, scale_us);
}

// Column-major: colourize the whole frame transposed, then walk it in column
// blocks. The transpose is one pass rather than one per block because a column
// cannot be scaled until all 144 of its pixels exist, which is the frame of
// added latency the phase trades for the artefact.
static void bench_col(arm_t* a)
{
    unsigned blocks = SCALER_SRC_W / UNIT_LINES;
    unsigned x;
    unsigned y;
    unsigned b;
    int64_t t0;
    int64_t t1;

    t0 = esp_timer_get_time();
    for (y = 0; y < GB_SCREEN_H; y++) {
        const uint8_t* src = idx_frame + (size_t)y * SCALER_SRC_W;
        for (x = 0; x < SCALER_SRC_W; x++) {
            col_src[(size_t)x * GB_SCREEN_H + y] = lut[src[x]];
        }
    }
    t1 = esp_timer_get_time();

    for (b = 0; b < blocks; b++) {
        unsigned first = b * UNIT_LINES;
        const uint16_t* lookahead = (first + UNIT_LINES < SCALER_SRC_W)
            ? col_ptrs[first + UNIT_LINES] : nullptr;
        (void)scaler_scale_col_block(SCALE_GEOM, SCALER_MODE_BLEND,
                                     col_ptrs + first, lookahead, out,
                                     scratch);
    }
    (void)scaler_scale_col_tail(SCALE_GEOM, SCALER_MODE_BLEND,
                                col_ptrs + blocks * UNIT_LINES, out);
    arm_add(a, (uint32_t)(t1 - t0),
            (uint32_t)(esp_timer_get_time() - t1));
}

static void print_arm(const char* name, const arm_t* a)
{
    uint32_t lut_mean = a->lut_sum / BENCH_FRAMES;
    uint32_t scale_mean = a->scale_sum / BENCH_FRAMES;

    Serial.printf("[SBENCH] %-3s lut=%lu us (max %lu)  scale=%lu us (max %lu)"
                  "  total=%lu us\n",
                  name, (unsigned long)lut_mean, (unsigned long)a->lut_max,
                  (unsigned long)scale_mean, (unsigned long)a->scale_max,
                  (unsigned long)(lut_mean + scale_mean));
}

static void bench_task(void* arg)
{
    (void)arg;
    for (;;) {
        arm_t row = { 0, 0, 0, 0 };
        arm_t col = { 0, 0, 0, 0 };
        long row_total;
        long col_total;
        unsigned f;

        // Interleaved rather than one arm then the other, so a thermal or
        // clock drift over the round lands on both equally.
        for (f = 0; f < BENCH_FRAMES; f++) {
            bench_row(&row);
            bench_col(&col);
        }
        print_arm("row", &row);
        print_arm("col", &col);
        row_total = (long)((row.lut_sum + row.scale_sum) / BENCH_FRAMES);
        col_total = (long)((col.lut_sum + col.scale_sum) / BENCH_FRAMES);
        Serial.printf("[SBENCH] delta=%+ld us  (%+ld %% of row)  "
                      "free heap %lu\n",
                      col_total - row_total,
                      row_total ? (col_total - row_total) * 100 / row_total : 0,
                      (unsigned long)ESP.getFreeHeap());
        vTaskDelay(pdMS_TO_TICKS(500));
    }
}

void scale_bench_run()
{
    uint32_t seed = 0x5EED1234u;
    unsigned i;

    Serial.printf("[SBENCH] geom %ux%u, block %u units, free heap %lu\n",
                  (unsigned)GAME_W, (unsigned)GAME_H, (unsigned)BLOCK_UNITS,
                  (unsigned long)ESP.getFreeHeap());

    geom = scaler_geom_info(SCALE_GEOM);
    idx_frame = (uint8_t*)malloc((size_t)GB_SCREEN_H * SCALER_SRC_W);
    lut = (uint16_t*)malloc(PALETTE_LUT_SIZE * sizeof(uint16_t));
    row_lines = (uint16_t*)malloc((size_t)(BLOCK_LINES + 1) * SCALER_SRC_W
                                  * sizeof(uint16_t));
    col_src = (uint16_t*)malloc((size_t)SCALER_SRC_W * GB_SCREEN_H
                                * sizeof(uint16_t));
    out = (uint16_t*)malloc(OUT_PX * sizeof(uint16_t));
    scratch = (uint16_t*)malloc(SCRATCH_PX * sizeof(uint16_t));

    if (geom == nullptr || idx_frame == nullptr || lut == nullptr
        || row_lines == nullptr || col_src == nullptr || out == nullptr
        || scratch == nullptr) {
        // No number is better than a number measured on buffers that are not
        // the real sizes.
        Serial.printf("[SBENCH] out of heap (%lu free); no measurement\n",
                      (unsigned long)ESP.getFreeHeap());
        for (;;) {
            delay(1000);
        }
    }

    palette_build_lut(0, lut);
    for (i = 0; i < (unsigned)GB_SCREEN_H * SCALER_SRC_W; i++) {
        // gnuboy's DMG path bounds the raw byte at 39, so stay inside it: the
        // LUT covers all 64 values but only the low 40 ever arrive.
        seed = seed * 1664525u + 1013904223u;
        idx_frame[i] = (uint8_t)((seed >> 16) % 40u);
    }
    for (i = 0; i < BLOCK_LINES + 1; i++) {
        row_ptrs[i] = row_lines + (size_t)i * SCALER_SRC_W;
    }
    for (i = 0; i < SCALER_SRC_W; i++) {
        col_ptrs[i] = col_src + (size_t)i * GB_SCREEN_H;
    }

    // Core 0 at the push task's priority: the scaler's real home, and core 0's
    // budget is the question being asked.
    xTaskCreatePinnedToCore(bench_task, "sbench", 4096, nullptr, 3, nullptr, 0);
    for (;;) {
        delay(1000);
    }
}

#endif // SCALE_BENCH
