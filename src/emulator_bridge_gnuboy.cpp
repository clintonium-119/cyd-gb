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
static uint8_t curpal = 0;
static uint8_t pal_bgp = 0, pal_obp0 = 0, pal_obp1 = 0;
static bool pal_valid = false;

static void palette_refresh(bool force)
{
    if (!force && pal_valid && pal_bgp == reg_bgp() && pal_obp0 == reg_obp0() &&
        pal_obp1 == reg_obp1()) {
        return;
    }
    pal_bgp = reg_bgp();
    pal_obp0 = reg_obp0();
    pal_obp1 = reg_obp1();
    pal_valid = true;
    palette_build_lut_gnuboy(curpal, pal_bgp, pal_obp0, pal_obp1, lut);
}

void emu_set_palette(uint8_t idx)
{
    if (idx >= PALETTE_COUNT) {
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
static uint8_t slot_src[FRAMEQUEUE_SLOTS][BLOCK_LINES + 1][SCALER_SRC_W];
static uint16_t lut_lines[BLOCK_LINES + 1][SCALER_SRC_W];
static uint16_t scratch_row[SCALER_DST_W_MAX];
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
static uint16_t dma_buf[2][BLOCK_ROWS * GAME_W];
static framequeue_t fq;
static TaskHandle_t push_task = nullptr;
static uint16_t frame_seq = 0;
static bool frame_dropped = false;
// The raw frame, GB_SCREEN_H x SCALER_SRC_W index bytes, and gnuboy's own
// framebuffer. Heap, not static: 23 KB, and the static DRAM segment is nearly
// full.
static uint8_t* fb = nullptr;
static int cur_block = -1;
static bool frame_open = false;

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
// the dither and the mid-scale bias are the same tested code on both cores
// rather than a second conversion written here. The alternative — gnuboy's
// mono format and a hand-rolled shift and bias — would have had to re-derive
// the volume encoding and the silence rule that mix_mono already pins.
//
// The sample count is NOT fixed, and it does not divide evenly into the
// speaker's frame. gnuboy emits a sample every snd.rate cycles, so the count
// follows the frame's real emulated length: at 32768 Hz it alternates 548 and
// 549 and averages 548.62, which is a DMG's true 59.727 Hz. The speaker takes
// a fixed SPEAKER_SAMPLES_PER_FRAME of 548, which is 59.796 Hz.
//
// So gnuboy produces about 0.6 samples a frame more than one write can carry,
// and the surplus is dropped. Carrying it instead does not work and was tried:
// the only way to drain a surplus is to hand the speaker more than one frame
// per emulated frame, which is the pacing rule itself, so a carry grows
// without bound until it is dropped anyway — in one audible 0.7 ms chunk
// rather than in single samples. Dropping one sample at the end of the 62 %
// of frames that run long is a 30 us slip, and it keeps the pacing rule and
// the A/B intact.
//
// The other core has the same 0.11 % discrepancy and spends it differently:
// MiniGB APU generates exactly 548 samples whatever the frame did, so there it
// shows up as the emulator being paced 0.11 % fast rather than as a dropped
// sample. Neither is a pitch error worth hearing; they are just not the same
// mechanism, which is worth knowing before reading an audio figure across the
// two.
//
// The buffer carries headroom above one frame because gnuboy wraps and loses
// samples if a frame fills it: this bridge passes no audio callback for it to
// flush through. The first frame after a reset emits 572, with the LCD off.
#define GNUBOY_AUDIO_HEADROOM 64
static int16_t apu_buf[2 * (SPEAKER_SAMPLES_PER_FRAME + GNUBOY_AUDIO_HEADROOM)];
static uint8_t mono_buf[SPEAKER_SAMPLES_PER_FRAME];
static mix_state_t mix;
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

/* Queue blocks one full frame is made of. */
static uint8_t blocks_per_frame()
{
    return (uint8_t)(GB_SCREEN_H / BLOCK_LINES);
}

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

/*
 * Frame end, from emu_run_frame() once gnuboy_run() has returned. Whatever
 * block was still collecting lines goes out now, and so does anything after
 * it, made of the previous frame's lines. A frame that drew nothing — the LCD
 * off, or a skipped frame — commits nothing and the sequence number simply
 * jumps, which the queue's ordering rule allows.
 */
static void frame_end()
{
    if (frame_open) {
        if (!frame_dropped) {
            commit_blocks(cur_block, (int)blocks_per_frame() - 1);
        }
        frame_open = false;
        cur_block = -1;
    }
    q_stall_us = q_stall_acc;
    q_stall_acc = 0;
    frame_dropped = false;
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
            (void)scaler_scale_block(SCALE_GEOM, SCALER_MODE_BLEND,
                                     src_lines + u * UNIT_LINES, la,
                                     dma_buf[buf] + (size_t)u * UNIT_ROWS * GAME_W,
                                     scratch_row);
        }
        t1 = esp_timer_get_time();
        scale_acc += (uint32_t)(t1 - t0);

        if (!meta.last_in_frame) {
            framequeue_release(&fq, slot);
        }
        if (meta.block_idx == 0) {
            display_frame_begin(vp_x, vp_y);
        }
        display_push_rows_dma(dma_buf[buf], (size_t)BLOCK_ROWS * GAME_W);
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
void emu_gnuboy_line(const unsigned char* line, int index)
{
    int blk;

    (void)line;
    if (index < 0 || index >= GB_SCREEN_H) {
        return;
    }
    blk = index / BLOCK_LINES;
    if (!frame_open) {
        /* First drawn line of this frame. The sequence counts drawn frames
         * and jumps over skipped ones. Blocks before this one — the LCD
         * enabled partway down — go out with the previous frame's lines so
         * the frame is whole. */
        frame_open = true;
        frame_seq++;
        commit_blocks(0, blk - 1);
        cur_block = blk;
    }
    if (frame_dropped) {
        return;
    }
    if (blk != cur_block) {
        /* This line is the next block's first, and under 26/16 the previous
         * block's lookahead. gnuboy wrote it into fb before the hook fired,
         * so it is there before the commit reads it. */
        commit_blocks(cur_block, blk - 1);
        cur_block = blk;
    }
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
    /* DMG only. No CGB path is wired: the colour scanline renderers and the
     * priority buffer are left as the vendored core has them. */
    gnuboy_set_hwtype(GB_HW_DMG);
    if (gnuboy_load_rom(rom, romlen) != 0) {
        Serial.println("[EMU] gnuboy rejected the ROM");
        return false;
    }
    /* gnuboy's framebuffer IS fb, which is what makes the per-line hook a
     * bookkeeping call with no copy in it. */
    gnuboy_set_framebuffer(fb);
    gnuboy_set_soundbuffer(apu_buf, sizeof(apu_buf) / sizeof(apu_buf[0]));
    gnuboy_reset(true);
    emu_up = true;

    mix_init(&mix, 0x2545F491u);

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
    if (framequeue_init(&fq, blocks_per_frame()) != FRAMEQUEUE_OK) {
        return false;
    }
    fcnt = fpsc = cfps = 0;
    fpst = millis();

    rom_title(title, sizeof(title));
    Serial.printf("[EMU] gnuboy '%s' %uKB heap:%u\n", title, romlen / 1024,
                  ESP.getFreeHeap());
    return true;
}

void emu_run_frame()
{
    bool draw;
    size_t n_samples;
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
    if (n_samples > SPEAKER_SAMPLES_PER_FRAME) {
        n_samples = SPEAKER_SAMPLES_PER_FRAME;
    }
    mix_mono(&mix, apu_buf, n_samples, vol_idx, mono_buf);
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
        Serial.printf("[PERF] emu=%uus scale=%uus push=%uus qstall=%uus "
                      "qovf=%u apu=%uus await=%uus aunder=%u aover=%u "
                      "fps=%u split=c0 core=gnuboy\n",
                      emu_us, scale_us, push_us, q_stall_us,
                      framequeue_overflows(&fq), apu_us, await_us, aunder,
                      aover, cfps);
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
    vol_idx = (idx > MIX_VOL_OFF) ? MIX_VOL_OFF : idx;
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

uint8_t emu_get_colour_hash()
{
    /* The sum of the title field's bytes, which is what the other bridge's
     * core computes from the same sixteen bytes. Zero before the ROM is
     * mapped, as the header promises. */
    uint8_t x = 0;
    uint16_t i;

    if (!rom || romlen <= 0x143) {
        return 0;
    }
    for (i = 0x134; i <= 0x143; i++) {
        x = (uint8_t)(x + rom[i]);
    }
    return x;
}
