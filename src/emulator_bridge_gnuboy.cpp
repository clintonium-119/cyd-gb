/*
 * gnuboy's headers come first, alone, and their macros are dismantled before
 * anything else is included.
 *
 * cpu.h spells the emulated registers as one-letter macros — A, B, C, D, E,
 * F, H, L — and hw.h spells the global state `host`. In a C++ translation
 * unit that also pulls in Arduino.h those are a minefield: F() alone is
 * Arduino's flash-string macro. So the core is included first, the three
 * values this bridge actually reads are captured in inline accessors while
 * the macros still mean something, and then the whole set is undefined.
 *
 * gnuboy.h also defines IRAM_ATTR to nothing, which is how it builds without
 * retro-go. That goes the same way, so the Arduino definition is the one that
 * lands and the callbacks below keep their placement.
 */
extern "C" {
#include "gnuboy.h"
#include "hw.h"
}

/* The three DMG palette registers, read here because this is the last point
 * at which their names exist. */
static inline uint8_t reg_bgp()  { return R_BGP; }
static inline uint8_t reg_obp0() { return R_OBP0; }
static inline uint8_t reg_obp1() { return R_OBP1; }

/* Interleaved int16s the sound unit wrote during the frame just run. gnuboy
 * zeroes this at the top of every gnuboy_run(), so it is that frame's count
 * and not a running total. */
static inline size_t gnuboy_audio_samples() { return GB.audio.pos; }

#undef A
#undef B
#undef C
#undef D
#undef E
#undef F
#undef H
#undef L
#undef W
#undef LB
#undef HB
#undef AF
#undef BC
#undef DE
#undef HL
#undef PC
#undef SP
#undef IMA
#undef IME
#undef FZ
#undef FN
#undef FH
#undef FC
#undef FL
#undef host
#undef IRAM_ATTR

#include "emulator_bridge.h"
#include "display.h"
#include "hw_config.h"
#include "render_config.h"
#include "render/palette.h"
#include "cart/cgb_palette.h"
#include "render/framequeue.h"
#include "render/scaler.h"
#include "save/autosave.h"
#include "speaker.h"
#include "audio/mix.h"
#include "gnuboy_hook.h"
#include <Arduino.h>
#include <esp_timer.h>
#include <string.h>

/*
 * The gnuboy core behind the same interface as the Peanut-GB bridge, selected
 * per environment by build_src_filter so exactly one of the two is ever
 * linked. Everything outside this file — main, the menu, saves, the scaler,
 * the push task's consumers — is untouched and cannot tell which core it has.
 *
 * The pipeline is deliberately identical to the other bridge's, block for
 * block, so that an A/B between the two images has the core as its only
 * variable. Where the code below reads as a copy of src/emulator_bridge.cpp,
 * that is the point, not an oversight.
 */

// ─── ROM ────────────────────────────────────────────────────────────────────
// A pointer into memory-mapped flash, owned by rom_store and valid for the
// whole session. gnuboy_load_rom() points its bank table straight at it — it
// stores `data + pos` per bank and copies nothing — so this is the same
// direct-read shape the other core gets from its rom_direct pointer, and the
// mapped ROM is read exactly as the interpreter needs it.
static const uint8_t* rom = nullptr;
static uint32_t romlen = 0;

// ─── State ──────────────────────────────────────────────────────────────────
static autosave_state_t autosave;
// The cartridge's real save size, from the ROM header, and the bytes gnuboy
// actually allocated. The first is what reaches the card; the second is only
// the bound on a restore. See save_size_from_header().
static uint32_t save_size = 0;
static uint32_t cram_alloc = 0;
static bool emu_up = false;
/* Defined with the rest of the cartridge-RAM surface, below; emu_init() needs
 * it before that. */
static uint32_t save_size_from_header();
static uint8_t fskip = 0, fcnt = 0;
static bool ffwd = false;
static uint32_t fpsc = 0, fpst = 0, cfps = 0;
static uint8_t jpad = 0;

// ─── Palette ────────────────────────────────────────────────────────────────
// The tables live in gbcore, as they do for the other core, but the fill rule
// is the gnuboy one: that core writes the tile's raw two bits and identifies
// the source in the high bits, applying BGP / OBP0 / OBP1 when it builds its
// own colour table rather than when it draws the pixel. Peanut-GB bakes the
// register into the pixel byte instead. So the LUT here is a function of the
// three palette registers as well as the chosen palette, and it is rebuilt
// whenever any of them moves — otherwise every fade and every inverted screen
// would simply not happen.
//
// Sixteen stores on the frames where a register changed, and nothing at all on
// the frames where none did. The per-pixel cost stays one lookup, which is
// what keeps the scaler and the rest of gbcore out of this entirely.
//
// Core 0 reads the LUT while core 1 may be rebuilding it. Each entry is an
// aligned 16-bit store, so a reader sees either the old colour or the new one,
// never half of each; the worst a race can do is give one block of one frame
// of a fade the previous frame's shade.
static uint16_t lut[PALETTE_LUT_SIZE];
static uint8_t curpal = PALETTE_AUTO;
/* The colours the Game Boy Color's table gives this cartridge, looked up once
 * per ROM because the header cannot change underneath us. Not valid until
 * emu_init() has run, which is why curpal == PALETTE_AUTO before then still
 * builds the fallback rather than reading this. */
static uint16_t auto_ramps[3][4];
static bool auto_ok = false;
static uint8_t pal_bgp = 0, pal_obp0 = 0, pal_obp1 = 0;
static bool pal_valid = false;

/* The one place that knows what PALETTE_AUTO means at LUT-build time: the
 * cartridge's own ramps when the table knew it, and the shipped default when
 * it did not. Every other palette is a straight index into the table. */
static void build_lut(uint8_t bgp, uint8_t obp0, uint8_t obp1)
{
    if (curpal != PALETTE_AUTO) {
        palette_build_lut_gnuboy(curpal, bgp, obp0, obp1, lut);
    } else if (auto_ok) {
        palette_build_lut_gnuboy_ramps(auto_ramps, bgp, obp0, obp1, lut);
    } else {
        palette_build_lut_gnuboy(PALETTE_FALLBACK, bgp, obp0, obp1, lut);
    }
}

static void palette_refresh(bool force)
{
#ifdef TEAR_DEMO
    /* The test pattern's four shades are the palette's four shades, whatever
     * ROM is running underneath and whatever it does to BGP. 0xE4 is the
     * identity mapping: colour c is shade c. */
    if (force || !pal_valid) {
        pal_bgp = pal_obp0 = pal_obp1 = 0xE4;
        pal_valid = true;
        build_lut(0xE4, 0xE4, 0xE4);
    }
    return;
#endif
    if (!force && pal_valid && pal_bgp == reg_bgp() && pal_obp0 == reg_obp0() &&
        pal_obp1 == reg_obp1()) {
        return;
    }
    pal_bgp = reg_bgp();
    pal_obp0 = reg_obp0();
    pal_obp1 = reg_obp1();
    pal_valid = true;
    build_lut(pal_bgp, pal_obp0, pal_obp1);
}

void emu_set_palette(uint8_t idx)
{
    /* PALETTE_AUTO is accepted as well as the table's own indices: it is a
     * real choice the menu offers, not an out-of-range value. */
    if (idx > PALETTE_AUTO) {
        return;
    }
    curpal = idx;
    palette_refresh(true);
}

uint8_t emu_get_palette()
{
    return curpal;
}

const char* emu_get_palette_name(uint8_t idx)
{
    /* The gbcore table has no name for Auto and should not: it is a choice
     * this bridge resolves, not a twenty-first set of colours. */
    if (idx == PALETTE_AUTO) {
        return "Auto";
    }
    return palette_name(idx);
}

// ─── Frame path ─────────────────────────────────────────────────────────────
// gnuboy keeps its own persistent 160x144 index buffer and writes each
// finished line into it, so it is handed `fb` as its framebuffer and the two
// are the same allocation. That is the whole reason the per-line hook carries
// no copy: by the time it fires, gnuboy has already written the line into fb,
// and the hook's only job is to notice which block the frame has reached and
// commit the finished one. The other bridge copies each line because its core
// hands over a transient line buffer instead.
//
// Everything downstream of fb is shared with the other bridge: a block is
// BLOCK_LINES raw lines plus room for the lookahead line the 26/16 geometry
// reads, blocks are committed in order into the two-slot queue, and colour
// never enters a slot.
//
// The persistent buffer is what lets a short frame still reach the consumer
// whole. gnuboy renders no line at all while the LCD is off — its frame loop
// returns early before the line state machine runs — so a frame can commit
// far fewer than 144 lines. Those blocks go out made of the previous frame's
// pixels, and the queue never sticks on half a frame.
#if PUSH_ORDER == PUSH_ROW
static uint8_t slot_src[FRAMEQUEUE_SLOTS][BLOCK_LINES + 1][SCALER_SRC_W];
static uint16_t lut_lines[BLOCK_LINES + 1][SCALER_SRC_W];
#else
// The column order's counterpart: a block's source COLUMNS, colourized. One
// more than the block consumes, for the lookahead column the geometry reads
// across its own boundary — the same shape lut_lines has, an axis over.
static uint16_t lut_cols[COL_BLOCK_SRC + 1][GB_SCREEN_H];
#endif
// dst_w pixels for the row order, dst_h for the column order, and the row
// order's is the larger at every geometry, so one size covers both. The packed
// walk wants four units where the 565 one wants one: it keeps every scaled
// source unit of the block alive so the blend units can be built without a
// second pass over the block.
#if PIXEL_PACKED
static uint16_t scratch_row[SCALER_SCRATCH_444_MAX];
#else
static uint16_t scratch_row[SCALER_DST_W_MAX];
#endif
static const scaler_geom_info_t* geom = nullptr;
static int16_t vp_x = GAME_X;
static int16_t vp_y = GAME_Y;

// ─── Pipeline ───────────────────────────────────────────────────────────────
// Same two-core split as the other bridge, and the same reasons: core 1 runs
// emulation and the block copies, core 0 runs the LUT, the scaler and the DMA
// push. See include/emulator_bridge.h's Pipeline section for the contract and
// src/emulator_bridge.cpp for the long-form rationale; nothing here diverges
// from it, because an A/B whose pipeline also changed would measure two
// things at once.
//
// A packed buffer is bytes rather than pixels, and three quarters the size.
// DMA_ELEMS() turns a pixel count into the buffer's own units so the walks
// below index one buffer the same way whichever format is compiled, and the
// alignment is stated because the driver hands the pointer to the DMA engine
// as words.
#if PIXEL_PACKED
typedef uint8_t dma_px_t;
#define DMA_ELEMS(px) SCALER_PACKED_BYTES(px)
#else
typedef uint16_t dma_px_t;
#define DMA_ELEMS(px) ((size_t)(px))
#endif

#if PUSH_TRANSPOSED
static dma_px_t dma_buf[2][DMA_ELEMS(COL_BLOCK_COLS * GAME_H)]
    __attribute__((aligned(4)));
#else
static dma_px_t dma_buf[2][DMA_ELEMS(BLOCK_ROWS * GAME_W)]
    __attribute__((aligned(4)));
#endif

/* One transfer, in whichever format is compiled: the walks below count in
 * pixels and this is the one place that knows what a pixel costs. */
static inline void push_dma(dma_px_t* at, size_t px)
{
#if PIXEL_PACKED
    display_push_packed_dma(at, SCALER_PACKED_BYTES(px));
#else
    display_push_rows_dma(at, px);
#endif
}
static framequeue_t fq;
static TaskHandle_t push_task = nullptr;
static uint16_t frame_seq = 0;
static bool frame_dropped = false;
// The raw frame, GB_SCREEN_H x SCALER_SRC_W index bytes, and gnuboy's own
// framebuffer. Heap, not static: 23 KB, and the static DRAM segment is nearly
// full.
static uint8_t* fb = nullptr;
#if PUSH_TRANSPOSED
// The same frame as columns: GB_SCREEN_H index bytes per column, one buffer
// per queue slot. An output column needs every source row, so the consumer
// cannot start on a frame until it is whole, and the producer must therefore
// be writing a buffer nobody is reading. The queue's slot ownership already
// IS that double-buffering — a slot is an index and the wrapper attaches the
// real buffer to it — so the pipeline shift costs no second handshake, and
// the producer's acquire is the same backpressure it always was.
//
// Heap, not static: 23 KB each, and the static DRAM segment is nearly full.
static uint8_t* tfb[FRAMEQUEUE_SLOTS] = { nullptr, nullptr };
// The slot this frame is being transposed into, -1 when no frame is open, and
// the first line of the frame not yet transposed.
static int tpose_slot = -1;
static int tpose_next = 0;
#endif
static int cur_block = -1;
static bool frame_open = false;
// The last line the hook was handed. A line number that does not advance is
// how a frame boundary is recognised — see emu_gnuboy_line().
static int last_line = -1;

// ─── Frame timing ───────────────────────────────────────────────────────────
static uint32_t emu_us = 0;
static volatile uint32_t scale_us = 0;
static volatile uint32_t push_us = 0;
static uint32_t q_stall_us = 0;
static uint32_t q_stall_acc = 0;

// ─── Audio ──────────────────────────────────────────────────────────────────
// gnuboy's sound unit is bound to its own hardware state and cannot be swapped
// for MiniGB APU, so this core generates its own samples. Everything after
// that is the other bridge's path unchanged: gnuboy is asked for interleaved
// stereo int16, which is exactly what mix_mono() takes, so the volume table,
// the rounding and the mid-scale bias are the same tested code on both cores
// rather than a second conversion written here. The alternative — gnuboy's
// mono format and a hand-rolled shift and bias — would have had to re-derive
// the volume encoding and the silence rule that mix_mono already pins.
//
#define GNUBOY_AUDIO_HEADROOM 64
// The sample count is NOT fixed. gnuboy emits a sample every snd.rate cycles,
// so the count follows the frame's real emulated length: at 32768 Hz it
// alternates 548 and 549 and averages 548.62, which is a DMG's true 59.727 Hz.
//
// Every one of them is handed over. An earlier version truncated to the
// speaker's nominal 548 and discarded the rest, on the reasoning that a
// dropped sample is a 30 us slip and inaudible. The bench disagreed: that is
// about 37 discontinuities a second, the builder heard crackle, and the
// underflow counter does not measure it at all — over the capture the stream
// was in net surplus while 3,317 samples were being thrown away. The speaker
// now accepts up to SPEAKER_SAMPLES_MAX and the surplus goes to the DAC.
//
// Delivering all of it is also what makes the pacing correct rather than
// approximately correct. The write blocks when the DMA queue is full, so in
// steady state delivery equals the DAC's 32768 samples a second, which puts
// the emulator at 32768 / 548.62 = 59.727 fps — a Game Boy's real frame rate.
// Truncating had it running about 0.5 % fast.
//
// The buffer still carries headroom above one frame because gnuboy wraps and
// loses samples if a frame fills it: this bridge passes no audio callback for
// it to flush through, and the first frame after a reset emits 572.
static int16_t apu_buf[2 * (SPEAKER_SAMPLES_PER_FRAME + GNUBOY_AUDIO_HEADROOM)];
static uint8_t mono_buf[SPEAKER_SAMPLES_MAX];
// The head of a fast-forward frame's skipped run, for the seam's crossfade.
static int16_t ff_head[2 * MIX_XFADE_FRAMES];
// Off until main() applies the stored setting, so a unit is never loud before
// its own volume is read.
static uint8_t vol_idx = MIX_VOL_OFF;
// The mix, for the [PERF] line. NOT the same quantity the other bridge reports
// under this name: there the APU runs after the frame and is timed with the
// mix, here it runs inside gnuboy_run() and its cost is inside emu_us. The
// sum of the two is what compares across cores; neither half does.
static uint32_t apu_us = 0;

void emu_get_frame_times(uint32_t* out_emu_us, uint32_t* out_scale_us,
                         uint32_t* out_push_us)
{
    if (out_emu_us) {
        *out_emu_us = emu_us;
    }
    if (out_scale_us) {
        *out_scale_us = scale_us;
    }
    if (out_push_us) {
        *out_push_us = push_us;
    }
}

void emu_set_viewport(int16_t x, int16_t y)
{
    vp_x = x;
    vp_y = y;
}

/* Queue blocks one full frame is made of. The column order hands the frame
 * over in one piece — the consumer walks its own column blocks out of a buffer
 * that is stable for the whole frame — so there is exactly one. */
static uint8_t blocks_per_frame()
{
#if PUSH_TRANSPOSED
    return 1u;
#else
    return (uint8_t)(GB_SCREEN_H / BLOCK_LINES);
#endif
}

#if PUSH_TRANSPOSED
/*
 * Source lines [from, to] into the frame's transposed buffer: 160 stores of
 * stride GB_SCREEN_H each, on core 1, which has about 3.3 ms spare. The
 * consumer then reads a source column as GB_SCREEN_H contiguous bytes. The
 * strided side has to be somewhere, and this is the core that can afford it.
 *
 * Read out of fb rather than the hook's line pointer, and that is what makes
 * a short frame come out whole. gnuboy draws no line at all while the LCD is
 * off, so the lines this frame never reached are transposed from fb holding
 * the previous frame's pixels — exactly what the row order's blocks are made
 * of in the same case.
 */
static void transpose_lines(unsigned from, unsigned to)
{
    uint8_t* base;
    unsigned y;

    if (tpose_slot < 0 || from > to || to >= GB_SCREEN_H) {
        return;
    }
    base = tfb[tpose_slot];
    for (y = from; y <= to; y++) {
        const uint8_t* src = fb + (size_t)y * SCALER_SRC_W;
        uint8_t* dst = base + y;
        unsigned x;

        for (x = 0; x < SCALER_SRC_W; x++) {
            dst[(size_t)x * GB_SCREEN_H] = src[x];
        }
    }
}
#endif

#if PUSH_ORDER == PUSH_ROW
/*
 * Producer half: copy block `blk` out of the frame buffer into a queue slot
 * and commit it. Waiting for a free slot is the overlap working, and the
 * two-slot backpressure is the whole rate control. A PAUSED acquire is the
 * menu taking the bus, so the rest of the frame is abandoned and resume
 * starts clean.
 */
static void commit_block(uint_fast8_t blk)
{
    framequeue_meta_t meta;
    const uint8_t* first;
    int slot = 0;
    int r;
    int64_t t0;

    t0 = esp_timer_get_time();
    while ((r = framequeue_acquire(&fq, &slot)) == FRAMEQUEUE_FULL) {
        taskYIELD();
    }
    q_stall_acc += (uint32_t)(esp_timer_get_time() - t0);
    if (r != FRAMEQUEUE_OK) {
        frame_dropped = true;
        return;
    }

    meta.block_idx = (uint8_t)blk;
    meta.frame_seq = frame_seq;
    meta.last_in_frame = (blk == (uint_fast8_t)(blocks_per_frame() - 1u));

    first = fb + (size_t)blk * BLOCK_LINES * SCALER_SRC_W;
    memcpy(slot_src[slot][0], first, (size_t)BLOCK_LINES * SCALER_SRC_W);
    if (geom->uses_lookahead && !meta.last_in_frame) {
        memcpy(slot_src[slot][BLOCK_LINES],
               first + (size_t)BLOCK_LINES * SCALER_SRC_W, SCALER_SRC_W);
    }

    if (framequeue_commit(&fq, slot, &meta) != FRAMEQUEUE_OK) {
        Serial.println("[EMU] frame queue rejected a block");
        frame_dropped = true;
        return;
    }
    if (push_task) {
        xTaskNotifyGive(push_task);
    }
}

/* Blocks [from, to] in order, stopping at the first pause. */
static void commit_blocks(int from, int to)
{
    int b;

    for (b = from; b <= to && !frame_dropped; b++) {
        commit_block((uint_fast8_t)b);
    }
}
#endif /* PUSH_ORDER == PUSH_ROW */

#if PUSH_TRANSPOSED
/*
 * Producer half, column order: take the slot this frame will be transposed
 * into. Called on the frame's first drawn line rather than at its end, so the
 * transpose can go straight into the slot as the lines arrive instead of
 * bursting 23 KB at frame end. Waiting here is the overlap working; a PAUSED
 * acquire is the menu taking the bus, and the rest of the frame is abandoned.
 */
static void open_frame_slot()
{
    int slot = 0;
    int r;
    int64_t t0;

    t0 = esp_timer_get_time();
    while ((r = framequeue_acquire(&fq, &slot)) == FRAMEQUEUE_FULL) {
        taskYIELD();
    }
    q_stall_acc += (uint32_t)(esp_timer_get_time() - t0);
    if (r != FRAMEQUEUE_OK) {
        frame_dropped = true;
        return;
    }
    tpose_slot = slot;
    tpose_next = 0;
}

/* Hand the finished frame over: one block, which is the whole frame. */
static void commit_frame()
{
    framequeue_meta_t meta;

    if (tpose_slot < 0) {
        return;
    }
    meta.block_idx = 0;
    meta.frame_seq = frame_seq;
    meta.last_in_frame = 1;
    if (framequeue_commit(&fq, tpose_slot, &meta) != FRAMEQUEUE_OK) {
        Serial.println("[EMU] frame queue rejected a frame");
        frame_dropped = true;
        return;
    }
    if (push_task) {
        xTaskNotifyGive(push_task);
    }
}
#endif /* PUSH_TRANSPOSED */

/*
 * Close whatever frame is open. Whatever block was still collecting lines goes
 * out now, and so does anything after it, made of the previous frame's lines,
 * so a frame the LCD cut short still reaches the consumer whole. A frame that
 * drew nothing commits nothing and the sequence number simply jumps, which the
 * queue's ordering rule allows.
 *
 * Called from two places, and it has to be both: the line hook when the line
 * number wraps, and emu_run_frame() when the run returns.
 */
static void frame_flush()
{
    if (frame_open) {
        if (!frame_dropped) {
#if PUSH_TRANSPOSED
            /* Whatever the frame never drew comes out of fb, which still
             * holds the previous frame's pixels there. */
            transpose_lines((unsigned)tpose_next, GB_SCREEN_H - 1u);
            commit_frame();
#else
            commit_blocks(cur_block, (int)blocks_per_frame() - 1);
#endif
        }
        frame_open = false;
        cur_block = -1;
        last_line = -1;
#if PUSH_TRANSPOSED
        tpose_slot = -1;
        tpose_next = 0;
#endif
    }
    frame_dropped = false;
}

/* Frame end, from emu_run_frame() once gnuboy_run() has returned. */
static void frame_end()
{
    frame_flush();
    q_stall_us = q_stall_acc;
    q_stall_acc = 0;
}

/*
 * Consumer half, pinned to core 0. Identical to the other bridge's: the raw
 * lines are LUT'd, the block is scaled unit by unit into whichever DMA buffer
 * the bus is not reading, and queued. Frame bracketing comes from the
 * metadata the producer committed — block 0 opens the address window,
 * last_in_frame closes it — and a slot is released as soon as its raw lines
 * are consumed, except the frame's last, which waits for the transfer so that
 * framequeue_drained() cannot fire mid-push.
 */
#if PUSH_TRANSPOSED
/* Whole scaler column-blocks a frame is made of, and the leftover columns
 * past them. 53 and 1 at 5/3; 20 and 0 at 26/16. */
#define COL_BLOCKS (SCALER_SRC_W / UNIT_LINES)
#define COL_UNITS  (COL_BLOCKS + (COL_TAIL_COLS ? 1u : 0u))

#if FRAME_COLS_DESCENDING
#define COL_ORDER SCALER_COLS_DESCENDING
#else
#define COL_ORDER SCALER_COLS_ASCENDING
#endif

/*
 * Colourize `count` source columns starting at `base` out of the frame's
 * transposed buffer. Both sides are sequential: a source column is
 * GB_SCREEN_H contiguous index bytes because the producer already transposed
 * it, and an output column is GB_SCREEN_H contiguous pixels because that is
 * what the scaler reads. No mask on the raw byte — gnuboy's DMG path bounds
 * it at 39 and the LUT covers all 64 values.
 */
static void colourize_cols(const uint8_t* tframe, unsigned base,
                           unsigned count)
{
    unsigned i;

    for (i = 0; i < count; i++) {
        const uint8_t* src = tframe + (size_t)(base + i) * GB_SCREEN_H;
        uint16_t* dst = lut_cols[i];
        unsigned y;

        for (y = 0; y < GB_SCREEN_H; y++) {
            dst[y] = lut[src[y]];
        }
    }
}

/*
 * One unit — a whole scaler block, or the frame's tail — colourized out of the
 * transposed frame and scaled into `at`. Returns the output columns it wrote.
 * Shared by both transposed consumers, which differ only in what order they
 * ask for the units and where they put them.
 */
static unsigned scale_unit(const uint8_t* tframe, unsigned u,
                           const uint16_t* const* src_cols, dma_px_t* at)
{
    unsigned base = u * UNIT_LINES;

    if (u == COL_BLOCKS) {
        colourize_cols(tframe, base, COL_TAIL_COLS);
#if PIXEL_PACKED
        (void)scaler_scale_col_tail_444(SCALE_GEOM, SCALER_MODE_BLEND,
                                        src_cols, at, scratch_row);
#else
        (void)scaler_scale_col_tail(SCALE_GEOM, SCALER_MODE_BLEND,
                                    src_cols, at);
#endif
        return COL_TAIL_COLS;
    }
    {
        const uint16_t* la = nullptr;
        unsigned count = UNIT_LINES;

        if (geom->uses_lookahead && base + UNIT_LINES < SCALER_SRC_W) {
            count++;
        }
        colourize_cols(tframe, base, count);
        if (count > UNIT_LINES) {
            la = src_cols[UNIT_LINES];
        }
#if PIXEL_PACKED
        (void)scaler_scale_col_block_444(SCALE_GEOM, SCALER_MODE_BLEND,
                                         src_cols, la, at, scratch_row,
                                         COL_ORDER);
#else
        (void)scaler_scale_col_block(SCALE_GEOM, SCALER_MODE_BLEND,
                                     src_cols, la, at, scratch_row,
                                     COL_ORDER);
#endif
    }
    return UNIT_ROWS;
}

/*
 * Consumer half, column order, pinned to core 0. The frame arrives as one
 * queue block whose buffer is stable for the whole frame, so unlike the row
 * order this walks its own blocks: it fills a DMA buffer with COL_BLOCK_COLS
 * output columns and pushes, which is what keeps the transfer size near the
 * row order's measured one.
 *
 * The walk follows the panel, not the image. The address window fills in one
 * fixed direction and FRAME_COLS_DESCENDING says which, so when that runs
 * against the image the units come out last-to-first and each block's own
 * columns come out reversed with them — which the scaler does for the cost of
 * a sign on a stride, rather than anything here moving a pixel twice.
 */
static void emu_push_task(void* arg)
{
    const uint16_t* src_cols[COL_BLOCK_SRC + 1];
    framequeue_meta_t meta;
    unsigned i;
    int slot = 0;

    (void)arg;
    for (i = 0; i < COL_BLOCK_SRC + 1u; i++) {
        src_cols[i] = lut_cols[i];
    }
    for (;;) {
        const uint8_t* tframe;
        uint32_t scale_acc = 0;
        uint32_t push_acc = 0;
        unsigned in_buf = 0;
        unsigned buf = 0;
        unsigned k;
        int64_t t0;
        int64_t t1;

        if (framequeue_pop(&fq, &slot, &meta) != FRAMEQUEUE_OK) {
            ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(2));
            continue;
        }
        tframe = tfb[slot];
        display_frame_begin(vp_x, vp_y);

        for (k = 0; k < COL_UNITS; k++) {
            /* The unit this step of the walk emits, and how many output
             * columns it is worth: a whole block, or the tail. */
            unsigned u = (COL_ORDER == SCALER_COLS_DESCENDING)
                ? (COL_UNITS - 1u - k) : k;
            unsigned ncols = (u == COL_BLOCKS) ? COL_TAIL_COLS : UNIT_ROWS;
            dma_px_t* at;

            t0 = esp_timer_get_time();
            if (in_buf + ncols > COL_BLOCK_COLS) {
                /* This unit will not fit, so the buffer goes now. */
                t1 = esp_timer_get_time();
                scale_acc += (uint32_t)(t1 - t0);
                push_dma(dma_buf[buf], (size_t)in_buf * GAME_H);
                push_acc += (uint32_t)(esp_timer_get_time() - t1);
                buf ^= 1u;
                in_buf = 0;
                t0 = esp_timer_get_time();
            }
            at = dma_buf[buf] + DMA_ELEMS((size_t)in_buf * GAME_H);
            in_buf += scale_unit(tframe, u, src_cols, at);
            scale_acc += (uint32_t)(esp_timer_get_time() - t0);
        }

        t1 = esp_timer_get_time();
        if (in_buf) {
            push_dma(dma_buf[buf], (size_t)in_buf * GAME_H);
        }
        display_dma_wait();
        display_frame_end();
        push_acc += (uint32_t)(esp_timer_get_time() - t1);

        framequeue_release(&fq, slot);
        scale_us = scale_acc;
        push_us = push_acc;
    }
}
#else
static void emu_push_task(void* arg)
{
    const uint16_t* src_lines[BLOCK_LINES + 1];
    const uint16_t* lookahead;
    unsigned lines;
    unsigned x;
    framequeue_meta_t meta;
    uint32_t scale_acc = 0;
    uint32_t push_acc = 0;
    unsigned buf = 0;
    unsigned u;
    unsigned i;
    int slot = 0;
    int64_t t0;
    int64_t t1;

    (void)arg;
    for (i = 0; i < BLOCK_LINES + 1; i++) {
        src_lines[i] = lut_lines[i];
    }
    for (;;) {
        if (framequeue_pop(&fq, &slot, &meta) != FRAMEQUEUE_OK) {
            ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(2));
            continue;
        }
        t0 = esp_timer_get_time();
        if (meta.block_idx == 0) {
            scale_acc = 0;
            push_acc = 0;
        }
        /* No mask on the raw byte: gnuboy's DMG path bounds it at 39 and the
         * LUT covers all 64 values, so every byte indexes a real colour. */
        lines = BLOCK_LINES;
        lookahead = nullptr;
        if (geom->uses_lookahead && !meta.last_in_frame) {
            lookahead = lut_lines[BLOCK_LINES];
            lines++;
        }
        for (i = 0; i < lines; i++) {
            for (x = 0; x < SCALER_SRC_W; x++) {
                lut_lines[i][x] = lut[slot_src[slot][i][x]];
            }
        }
        for (u = 0; u < BLOCK_UNITS; u++) {
            const uint16_t* la = (u + 1u < BLOCK_UNITS)
                ? lut_lines[(u + 1u) * UNIT_LINES] : lookahead;
#if PIXEL_PACKED
            (void)scaler_scale_block_444(SCALE_GEOM, SCALER_MODE_BLEND,
                                         src_lines + u * UNIT_LINES, la,
                                         dma_buf[buf]
                                         + DMA_ELEMS((size_t)u * UNIT_ROWS
                                                     * GAME_W),
                                         scratch_row);
#else
            (void)scaler_scale_block(SCALE_GEOM, SCALER_MODE_BLEND,
                                     src_lines + u * UNIT_LINES, la,
                                     dma_buf[buf] + (size_t)u * UNIT_ROWS * GAME_W,
                                     scratch_row);
#endif
        }
        t1 = esp_timer_get_time();
        scale_acc += (uint32_t)(t1 - t0);

        if (!meta.last_in_frame) {
            framequeue_release(&fq, slot);
        }
        if (meta.block_idx == 0) {
            display_frame_begin(vp_x, vp_y);
        }
        push_dma(dma_buf[buf], (size_t)BLOCK_ROWS * GAME_W);
        if (meta.last_in_frame) {
            display_dma_wait();
            display_frame_end();
            framequeue_release(&fq, slot);
        }
        push_acc += (uint32_t)(esp_timer_get_time() - t1);
        if (meta.last_in_frame) {
            scale_us = scale_acc;
            push_us = push_acc;
        }
        buf ^= 1u;
    }
}
#endif /* PUSH_ORDER */

// ─── Per-line hook ──────────────────────────────────────────────────────────
/*
 * Called from the tail of the vendored lcd_renderline(), once per drawn line.
 * `line` is gnuboy's working buffer and is deliberately unused: gnuboy has
 * already written this line into fb, which is its framebuffer, so the bytes
 * are where the block copy will look for them. The parameter stays in the
 * hook's signature because the hook is the vendored core's, not this file's.
 *
 * Not IRAM_ATTR: it calls straight into flash-resident gbcore, exactly as the
 * other bridge's line callback does.
 */
#ifdef TEAR_DEMO
// ─── Tear demo (bench only) ─────────────────────────────────────────────────
// A scrolling test pattern in place of the emulated picture, for judging the
// tear without playing a game to find something that moves.
//
// It replaces the PICTURE and nothing else. gnuboy still runs, the audio still
// paces the frame, the producer still transposes, the queue still hands over
// and the consumer still walks and pushes — so what this shows is what a game
// shows, and a trim calibrated here is a trim calibrated for a game. That is
// the whole reason it lives inside the bridge rather than beside it: a fixture
// that drives its own pipeline measures its own pipeline.
//
// The cadence comes free for the same reason. The frame rate is set by
// speaker_write_frame() blocking on the DMA queue, which is untouched here, so
// the beat against the panel's oscillator is the one a game beats at — and
// that beat is the entire subject of the rate trim.
//
// Any ROM will do and none of it is seen. Boot with one that loads fast;
// nothing about this depends on what the cartridge is doing.
//
//   Up / Down     vertical scroll -1 / +1 px per frame
//   Left / Right  horizontal scroll -1 / +1 px per frame
//   A             stop
//   B             next pattern: noise, checkerboard, stripes, grid
//   Select        print the current pattern and rate
//
// Vertical scroll is the one that matters for the column-major push: its seam
// is a vertical line with a vertical displacement across it, and the
// horizontal rules below are what make a one-pixel step in that displacement
// impossible to miss.
//
//   PLATFORMIO_BUILD_FLAGS="-DDEV_ROM_PATH='\"Black Castle.gb\"' -DTEAR_DEMO" \
//     pio run -e cyd-gnuboy -t upload
#define DEMO_RATE_MAX 6

static int16_t demo_sx = 0;
static int16_t demo_sy = 0;
static int8_t demo_vx = 0;
/* 2 px/frame on boot, not 1: the bench found that the sharpest rate for the
 * periodic patterns, and a static pattern has no tear to show at all. */
static int8_t demo_vy = 2;
static uint8_t demo_prev_pad = 0;

/*
 * One pixel of the endless background, in one of three patterns.
 *
 * What reveals a seam is spatial frequency along the axis the two sides are
 * displaced on, and plenty of it. A sparse grid on an empty field is easy to
 * read but weak: the mismatch appears only where a rule crosses the seam, and
 * one broken thin line does not catch the eye. The checkerboard and the
 * stripes put an edge on almost every row instead, so a one-pixel vertical
 * step misaligns the whole length of the seam at once.
 *
 *   DEMO_PAT_NOISE   4x4 blocks of pseudo-random shade. The honest one and the
 *                    default. Every other pattern here is singly periodic, so
 *                    its sensitivity OSCILLATES with the scroll rate: the
 *                    displacement across a seam is one frame of motion, and
 *                    when that equals a whole period the two sides line up
 *                    and a real seam becomes invisible. Measured on the
 *                    bench: the stripes show a seam at 2 px/frame and hide it
 *                    completely at 4. Random blocks have no period to line up
 *                    with, so a displacement of any size decorrelates them.
 *
 *                    Share of pixels that change under a one-frame
 *                    displacement, by scroll rate, counted over a frame:
 *
 *                      rate        1    2    3    4    5    6    7    8
 *                      noise 4x2  38%  76%  75%  73%  75%  76%  76%  76%
 *                      stripe     50% 100%  50%   0%  50% 100%  50%   0%
 *                      check      25%  50%  75% 100%  75%  50%  25%   0%
 *
 *                    Blocks 4 wide by 2 tall rather than 4 by 1: one pixel of
 *                    vertical detail scores better on that table and then
 *                    loses most of it to the 5/3 vertical blend, which
 *                    averages adjacent source rows. Two rows keeps a pure row
 *                    per block. Pixels changed is not the same as seen.
 *   DEMO_PAT_CHECK   4x4 checkerboard. Frequency on both axes, so it shows a
 *                    displacement whichever way it runs — but blind wherever
 *                    the rate hits a multiple of its 8px period.
 *   DEMO_PAT_STRIPE  Horizontal bands, two rows on and two off. All of the
 *                    frequency on the vertical axis, which is the one the
 *                    column-major seam displaces, and the sharpest of these
 *                    AT 2 px/frame — a half-period shift inverts it — and
 *                    blind at 4.
 *   DEMO_PAT_GRID    8px rules with a diagonal. The least sensitive and the
 *                    only one that lets the step be COUNTED in pixels rather
 *                    than just seen.
 *
 * All three are periodic on a power of two, so they stay endless under a
 * negative offset: the masks below work on the wrapped value.
 */
#define DEMO_PAT_NOISE 0
#define DEMO_PAT_CHECK 1
#define DEMO_PAT_STRIPE 2
#define DEMO_PAT_GRID 3
#define DEMO_PAT_COUNT 4

static uint8_t demo_pat = DEMO_PAT_NOISE;

/* One 4x4 block's shade. Any decent integer hash does; this is the mix from
 * the xorshift family, over the block
 * coordinates rather than a sequence, so the field is stable in space and
 * scrolls with the offset instead of fizzing. */
static uint8_t demo_noise(unsigned u, unsigned v)
{
    uint32_t h = (u >> 2) * 0x9E3779B1u ^ (v >> 1) * 0x85EBCA77u;

    h ^= h >> 15;
    h *= 0x2545F491u;
    h ^= h >> 13;
    return (uint8_t)(h & 3u);
}

static uint8_t demo_shade(unsigned u, unsigned v)
{
    switch (demo_pat) {
    case DEMO_PAT_NOISE:
        return demo_noise(u, v);
    case DEMO_PAT_STRIPE:
        return ((v >> 1) & 1u) ? 3u : 0u;
    case DEMO_PAT_GRID:
        if ((v & 7u) == 0u) {
            return 3u;
        }
        if ((u & 7u) == 0u) {
            return 2u;
        }
        if (((u + v) & 15u) < 2u) {
            return 1u;
        }
        return 0u;
    default:
        return (((u >> 2) ^ (v >> 2)) & 1u) ? 3u : 0u;
    }
}

static const char* demo_pat_name()
{
    switch (demo_pat) {
    case DEMO_PAT_CHECK:  return "check";
    case DEMO_PAT_STRIPE: return "stripe";
    case DEMO_PAT_GRID:   return "grid";
    default:              return "noise";
    }
}

/* Line `y` of the pattern into fb, where gnuboy's own line would have gone,
 * and before the hook below reads it. */
static void demo_line(unsigned y)
{
    uint8_t* dst = fb + (size_t)y * SCALER_SRC_W;
    unsigned v = (unsigned)(int)((int)y + demo_sy);
    unsigned x;

    for (x = 0; x < SCALER_SRC_W; x++) {
        dst[x] = demo_shade((unsigned)(int)((int)x + demo_sx), v);
    }
}

/* Once per frame, at the first drawn line. Reads the joypad the emulator has
 * already debounced for this frame, so this costs no I2C of its own. */
static void demo_advance()
{
    /* Same order and values as the firmware's GB_BTN_* masks. */
    const uint8_t RIGHT = 0x01, LEFT = 0x02, UP = 0x04, DOWN = 0x08;
    const uint8_t A = 0x10, B = 0x20, SELECT = 0x40, START = 0x80;
    uint8_t pressed = (uint8_t)(jpad & ~demo_prev_pad);
    int8_t was_x = demo_vx;
    int8_t was_y = demo_vy;

    demo_prev_pad = jpad;
    if (pressed & UP) {
        demo_vy--;
    }
    if (pressed & DOWN) {
        demo_vy++;
    }
    if (pressed & LEFT) {
        demo_vx--;
    }
    if (pressed & RIGHT) {
        demo_vx++;
    }
    if (pressed & A) {
        demo_vx = 0;
        demo_vy = 0;
    }
    if (pressed & B) {
        demo_pat = (uint8_t)((demo_pat + 1u) % DEMO_PAT_COUNT);
        Serial.printf("[DEMO] pattern %s\n", demo_pat_name());
    }
    (void)START;

    if (demo_vx > DEMO_RATE_MAX) {
        demo_vx = DEMO_RATE_MAX;
    }
    if (demo_vx < -DEMO_RATE_MAX) {
        demo_vx = -DEMO_RATE_MAX;
    }
    if (demo_vy > DEMO_RATE_MAX) {
        demo_vy = DEMO_RATE_MAX;
    }
    if (demo_vy < -DEMO_RATE_MAX) {
        demo_vy = -DEMO_RATE_MAX;
    }
    if ((pressed & SELECT) || demo_vx != was_x || demo_vy != was_y) {
        /* Once, on a change, and never on the frame path's own account: a
         * print every frame would itself cost most of one. */
        Serial.printf("[DEMO] %s scroll %+d,%+d px/frame\n",
                      demo_pat_name(), (int)demo_vx, (int)demo_vy);
    }
    demo_sx = (int16_t)(demo_sx + demo_vx);
    demo_sy = (int16_t)(demo_sy + demo_vy);
}
#endif /* TEAR_DEMO */

void emu_gnuboy_line(const unsigned char* line, int index)
{
    int blk;

    (void)line;
    if (index < 0 || index >= GB_SCREEN_H) {
        return;
    }
    /*
     * The frame boundary is the line number wrapping, NOT gnuboy_run()
     * returning, and the difference is load-bearing rather than pedantic.
     * gnuboy's run loop tests R_LY between CPU steps, so a step that carries
     * the LCD past the last line and around to the top is not noticed and the
     * run keeps going into the next frame: the first run after a reset draws
     * 287 lines, two frames' worth, before it returns.
     *
     * Missing that wrap commits block 0 of the second frame while the queue
     * is still expecting block 17 of the first, which it rejects as out of
     * order — and a rejected commit leaves its slot producer-owned, so two of
     * them strand both slots and the producer waits for a free one forever.
     */
    if (frame_open && index <= last_line) {
        frame_flush();
    }
    blk = index / BLOCK_LINES;
#ifdef TEAR_DEMO
    if (!frame_open) {
        demo_advance();
    }
    /* Before the transpose and before the block copy, which is exactly where
     * gnuboy's own pixels for this line already sit. */
    demo_line((unsigned)index);
#endif
    if (!frame_open) {
        /* First drawn line of this frame. The sequence counts drawn frames
         * and jumps over skipped ones. Lines before this one — the LCD
         * enabled partway down — go out with the previous frame's pixels so
         * the frame is whole. */
        frame_open = true;
        frame_seq++;
#if PUSH_TRANSPOSED
        open_frame_slot();
#else
        commit_blocks(0, blk - 1);
#endif
        cur_block = blk;
    }
    last_line = index;
    if (frame_dropped) {
        return;
    }
#if PUSH_TRANSPOSED
    /* Everything up to and including this line, which on the frame's first
     * drawn line covers the undrawn ones above it. gnuboy wrote the line into
     * fb before the hook fired, so it is there before this reads it. Nothing
     * is committed per block here: the frame goes over in one piece once it
     * is whole, because a column of it needs every row. */
    transpose_lines((unsigned)tpose_next, (unsigned)index);
    tpose_next = index + 1;
#else
    if (blk != cur_block) {
        /* This line is the next block's first, and under 26/16 the previous
         * block's lookahead. gnuboy wrote it into fb before the hook fired,
         * so it is there before the commit reads it. */
        commit_blocks(cur_block, blk - 1);
        cur_block = blk;
    }
#endif
}

// ─── API ────────────────────────────────────────────────────────────────────
/*
 * The cartridge title out of the mapped header bytes — the ROM's, not the
 * core's, so this is the other bridge's rule verbatim. A byte outside the
 * printable range is padding or part of the manufacturer and CGB fields that
 * overlap the tail of the field, never part of a name, so it terminates the
 * string. out_sz wants to be 17 for the whole field.
 */
static void rom_title(char* out, size_t out_sz)
{
    size_t n;
    size_t i;

    if (!out || out_sz == 0) {
        return;
    }
    memset(out, 0, out_sz);
    if (!rom || romlen <= 0x143) {
        return;
    }
    n = (out_sz > 17) ? 16 : (out_sz - 1);
    for (i = 0; i < n; i++) {
        char ch = (char)rom[0x134 + i];
        out[i] = (ch >= 32 && ch < 127) ? ch : 0;
    }
}

bool emu_init(const uint8_t* rom_data, uint32_t rom_size)
{
    char title[17] = {0};

    if (!rom_data || rom_size == 0) {
        return false;
    }
    rom = rom_data;
    romlen = rom_size;

    if (!fb) {
        fb = (uint8_t*)malloc((size_t)GB_SCREEN_H * SCALER_SRC_W);
    }
    if (!fb) {
        return false;
    }
#if PUSH_TRANSPOSED
    for (unsigned i = 0; i < FRAMEQUEUE_SLOTS; i++) {
        if (!tfb[i]) {
            tfb[i] = (uint8_t*)malloc((size_t)GB_SCREEN_H * SCALER_SRC_W);
        }
        if (!tfb[i]) {
            Serial.println("[EMU] no heap for the transposed frame");
            return false;
        }
        /* Index 0 is the background palette's first shade, the blank the
         * panel shows until the first frame lands. */
        memset(tfb[i], 0, (size_t)GB_SCREEN_H * SCALER_SRC_W);
    }
#endif
    /* Index 0 is the background palette's first shade: the blank the panel
     * shows until the first frame lands, and what the blocks of a frame the
     * LCD cut short are made of. */
    memset(fb, 0, (size_t)GB_SCREEN_H * SCALER_SRC_W);

    /* The speaker's rate, not a number of this file's own: gnuboy derives its
     * sample counter from it, and a zero there leaves the counter unable to
     * advance and hangs the core outright. No audio callback is passed —
     * gnuboy fills the buffer and this bridge reads it at frame end, which is
     * where the other core's mix happens too. */
    if (gnuboy_init(SPEAKER_SAMPLE_RATE, GB_AUDIO_STEREO_S16,
                    GB_PIXEL_PALETTED, nullptr, nullptr) != 0) {
        Serial.println("[EMU] gnuboy init failed");
        return false;
    }
    if (gnuboy_load_rom(rom, romlen) != 0) {
        Serial.println("[EMU] gnuboy rejected the ROM");
        return false;
    }
    /* DMG only, forced rather than asked for, and AFTER the load.
     *
     * gnuboy_set_hwtype() is a stub in this vendored version — its body is
     * the comment "nothing for now" — so the type is whatever
     * gnuboy_load_rom() read out of the cartridge header a line above. A
     * CGB-aware cartridge sets header byte 0x143 to 0x80, which is most of
     * the library and includes Pokemon Yellow, and that would run the colour
     * scanline renderers and the CGB pixel encoding. Neither is wired: this
     * phase is DMG only, and the bridge's LUT assumes the DMG index layout.
     *
     * Writing the field is what actually takes effect, because IS_CGB reads
     * it directly. */
    GB.hwtype = GB_HW_DMG;
    /* gnuboy's framebuffer IS fb, which is what makes the per-line hook a
     * bookkeeping call with no copy in it. */
    gnuboy_set_framebuffer(fb);
    gnuboy_set_soundbuffer(apu_buf, sizeof(apu_buf) / sizeof(apu_buf[0]));
    gnuboy_reset(true);
    emu_up = true;

    save_size = save_size_from_header();
    cram_alloc = (uint32_t)cart.ramsize * 8192u;
    if (!save_size) {
        Serial.println("[EMU] no cartridge RAM declared, autosave off");
    }
    autosave_init(&autosave, save_size);

    palette_refresh(true);
    geom = scaler_geom_info(SCALE_GEOM);
    if (!geom || geom->src_lines_per_block != UNIT_LINES ||
        geom->dst_rows_per_block != UNIT_ROWS || geom->dst_w != GAME_W) {
        /* render_config.h repeats the table's numbers for the static buffer
         * sizes; if they ever disagree, refuse rather than scale into the
         * wrong-sized buffer. */
        Serial.println("[EMU] render_config.h disagrees with the scaler geometry");
        return false;
    }
    frame_seq = 0;
    frame_dropped = false;
    frame_open = false;
    cur_block = -1;
    last_line = -1;
#if PUSH_TRANSPOSED
    tpose_slot = -1;
    tpose_next = 0;
#endif
    if (framequeue_init(&fq, blocks_per_frame()) != FRAMEQUEUE_OK) {
        return false;
    }
    fcnt = fpsc = cfps = 0;
    fpst = millis();

    /* Before the palette is built, and once per ROM: the header is the only
     * input and it cannot change while a cartridge is running. */
    auto_ok = cgb_palette_lookup(rom, romlen, auto_ramps);
    palette_refresh(true);

    rom_title(title, sizeof(title));
#ifdef TEAR_DEMO
    Serial.println("[DEMO] tear demo: Up/Down vertical, Left/Right "
                   "horizontal, A stop, Select print");
#endif
    /* auto: whether the Game Boy Color's table knew this cartridge, which is
     * the one fact about the palette that cannot be read off the screen —
     * a cart it does not know looks like any other DMG Green boot. */
    Serial.printf("[EMU] gnuboy '%s' %uKB push:%s auto:%s heap:%u\n", title,
                  romlen / 1024,
                  PUSH_TRANSPOSED ? "col" : "row",
                  auto_ok ? "yes" : "no",
                  ESP.getFreeHeap());
    return true;
}

void emu_run_frame()
{
    bool draw;
    size_t n_samples;
    size_t head_n = 0;
    int64_t t;

    /* The two cores' pad bits happen to agree exactly — right, left, up,
     * down, A, B, select, start from bit 0 up — so the byte goes straight
     * across with no translation. */
    gnuboy_set_pad(jpad);

    /* gnuboy has no interlace and none is added: skipping alternate
     * background lines is what combs a scrolling map, and a renderer that
     * does not need the trick is the reason this core is here at all. */
    draw = (fskip == 0) || ((fcnt & 1u) == 0u);

    t = esp_timer_get_time();
    if (ffwd) {
        /* Fast-forward: an undrawn run first, then the frame proper, so game
         * time runs at twice the panel's rate. The skipped run's audio is
         * dropped, which keeps the pitch, all but its head: that carries on
         * from the previous output, and the kept run's audio is faded in
         * from it below so the seam does not click. Kept here because the
         * next run starts gnuboy's buffer over. The frame end and palette
         * refresh run here too, so a register the skipped run wrote is not
         * lost. */
        gnuboy_run(false);
        frame_end();
        palette_refresh(false);
        head_n = gnuboy_audio_samples() / 2u;
        if (head_n > MIX_XFADE_FRAMES) {
            head_n = MIX_XFADE_FRAMES;
        }
        memcpy(ff_head, apu_buf, head_n * 2u * sizeof(apu_buf[0]));
    }
    gnuboy_run(draw);
    frame_end();
    emu_us = (uint32_t)(esp_timer_get_time() - t);

    /* After the frame, so a register the frame wrote is picked up before the
     * next one is drawn with it. */
    palette_refresh(false);

    /* Every frame, drawn or skipped: the sound has to stay continuous, and
     * the write is also what paces emulation — it blocks only while the DMA
     * queue is full, which happens only when the emulator is ahead of real
     * time. gnuboy reports its samples per channel in audio.pos counting
     * interleaved int16s, so the frame length is half of it, clamped to what
     * the speaker takes. */
    t = esp_timer_get_time();
    n_samples = gnuboy_audio_samples() / 2u;
    if (n_samples > SPEAKER_SAMPLES_MAX) {
        /* Only a frame that ran long enough to overshoot the speaker's
         * headroom, which the reset frame's 572 does not. Clamping here
         * discards audio, so it is the last resort rather than the rule. */
        n_samples = SPEAKER_SAMPLES_MAX;
    }
    mix_crossfade_in(apu_buf, ff_head, head_n < n_samples ? head_n : n_samples);
    mix_mono(apu_buf, n_samples, vol_idx, mono_buf);
    apu_us = (uint32_t)(esp_timer_get_time() - t);
    speaker_write_frame(mono_buf, n_samples);

    fcnt++; fpsc++;
    uint32_t n = millis();
    if (n - fpst >= 1000) {
        cfps = fpsc;
        fpsc = 0;
        fpst = n;
        uint32_t aunder = 0, aover = 0, await_us = 0;
        speaker_get_stats(&aunder, &aover, &await_us);
        /* Same key and same field names as the other bridge's line, so one
         * capture tool reads both cores. core=gnuboy is what tells the two
         * captures apart. */
#ifndef QUIET_PERF
        Serial.printf("[PERF] emu=%uus scale=%uus push=%uus qstall=%uus "
                      "qovf=%u apu=%uus await=%uus aunder=%u aover=%u "
                      "fps=%u split=c0 core=gnuboy ff=%u\n",
                      emu_us, scale_us, push_us, q_stall_us,
                      framequeue_overflows(&fq), apu_us, await_us, aunder,
                      aover, cfps, ffwd ? 1u : 0u);
#endif
    }
}

void emu_start_push_task()
{
    if (push_task) {
        return;
    }
    xTaskCreatePinnedToCore(emu_push_task, "gbpush", 4096, nullptr, 3,
                            &push_task, 0);
}

void emu_pause_pipeline()
{
    framequeue_pause(&fq);
    while (!framequeue_drained(&fq)) {
        taskYIELD();
    }
    display_bus_acquire();
    speaker_silence();
}

void emu_resume_pipeline()
{
    display_bus_release();
    frame_dropped = false;
    framequeue_resume(&fq);
}

void emu_set_joypad(uint8_t b) { jpad = b; }

// ─── Cartridge RAM ──────────────────────────────────────────────────────────
// gnuboy allocates cartridge RAM as one contiguous calloc and views it as
// banks, so the flat buffer the header's contract promises is that allocation
// with no gather.
//
// The length handed out is the ROM header's save size, NOT what gnuboy
// allocated, and the difference matters: gnuboy rounds every cartridge up to
// whole 8 KB banks and gives a cartridge that declares no RAM a bank anyway,
// so its allocation is 8192 where the header says 0 or 2048. Writing that to
// the card would change the .sav length and break every file already on the
// cards. The header is the core-independent answer and it is the one the
// other bridge writes, so both cores produce the same file for the same ROM.
//
// The allocation is always at least the header size — 8 KB banks rounded up —
// so handing out the smaller number can never run off the end.

/*
 * The cartridge's save size from the mapped header, by the same rule the
 * other core applies: byte 0x149 into the standard table, with MBC2 as the
 * exception it always is. An unrecognised code is 0 — autosave off — rather
 * than a guess at how much RAM to write to a card.
 */
static uint32_t save_size_from_header()
{
    static const uint32_t sizes[] = {
        0x0u, 0x800u, 0x2000u, 0x8000u, 0x20000u, 0x10000u
    };
    uint8_t code;

    if (!rom || romlen <= 0x149) {
        return 0;
    }
    /* MBC2 carries 512 half-bytes of its own and declares no RAM in the
     * header, so the table would answer 0 for it. */
    if (cart.mbc == MBC_MBC2) {
        return 0x200u;
    }
    code = rom[0x149];
    if (code >= (uint8_t)(sizeof(sizes) / sizeof(sizes[0]))) {
        return 0;
    }
    return sizes[code];
}

uint8_t* emu_get_cart_ram(uint32_t* s)
{
    if (s) {
        *s = save_size;
    }
    return (uint8_t*)cart.rambanks;
}

void emu_set_cart_ram(const uint8_t* d, uint32_t s)
{
    if (!d || !cart.rambanks) {
        return;
    }
    if (s > cram_alloc) {
        s = cram_alloc;
    }
    memcpy(cart.rambanks, d, s);
}

bool emu_cart_ram_dirty()
{
    return autosave_dirty(&autosave);
}

uint32_t emu_get_cart_ram_last_write_ms()
{
    return autosave.last_write_ms;
}

void emu_clear_cart_ram_dirty()
{
    autosave_flushed(&autosave);
}

void emu_autosave_tick(uint32_t now_ms)
{
    /* The other core notes each write from its IRAM-resident RAM callback.
     * gnuboy has no such callback to hand out; it sets a per-bank bit in
     * cart.sram_dirty instead, and only when the byte actually changed. So
     * the notice is collected here, once a frame, and the bits are consumed
     * the way gnuboy's own save path consumes them.
     *
     * The flag-then-tick split survives intact: the stamp is still the
     * frame's, still at worst one frame stale, and autosave still owns the
     * dirty state — which is what lets emu_clear_cart_ram_dirty() work at
     * all, since gnuboy's own flag has no public way to be cleared.
     *
     * Consuming the bits is what keeps a save from firing on every menu open
     * forever after the first write. */
    if (save_size && cart.sram_dirty) {
        cart.sram_dirty = 0;
        autosave_note_write(&autosave, 0);
    }
    autosave_tick(&autosave, now_ms);
}

void emu_autosave_defer(uint32_t now_ms)
{
    autosave_defer(&autosave, now_ms);
}

bool emu_autosave_battery(uint16_t mv, uint16_t low_mv, uint16_t hyst_mv)
{
    return autosave_battery(&autosave, mv, low_mv, hyst_mv);
}

void emu_set_volume(uint8_t idx)
{
    vol_idx = (idx > MIX_VOL_HIGH) ? MIX_VOL_HIGH : idx;
}

uint8_t emu_get_volume()
{
    return vol_idx;
}

void emu_get_audio_times(uint32_t* out_apu_us, uint32_t* out_wait_us)
{
    if (out_apu_us) {
        *out_apu_us = apu_us;
    }
    if (out_wait_us) {
        speaker_get_stats(nullptr, nullptr, out_wait_us);
    }
}

void emu_set_frame_skip(uint8_t s)
{
    fskip = s;
}

uint8_t emu_get_frame_skip() { return fskip; }

void emu_set_fast_forward(bool on)
{
    ffwd = on;
}

bool emu_get_fast_forward() { return ffwd; }
uint32_t emu_get_fps() { return cfps; }

void emu_reset()
{
    if (!emu_up) {
        return;
    }
    /* Soft, and that is load-bearing: gnuboy's hard reset memsets cartridge
     * RAM to 0xFF, which would throw away the save the boot flow had just
     * restored. A soft reset still puts the CPU, the LCD, the sound unit and
     * every IO register back to their power-up values. */
    gnuboy_reset(false);
    palette_refresh(true);
    fcnt = 0;
}

void emu_get_rom_title(char* out, size_t out_sz)
{
    rom_title(out, out_sz);
}
