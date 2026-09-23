#include "emulator_bridge.h"
#include "display.h"
#include "hw_config.h"
#include "render_config.h"
#include "render/palette.h"
#include "cart/cgb_palette.h"
#include "render/framequeue.h"
#include "render/scaler.h"

#if PIXEL_PACKED
#error "This bridge pushes 16-bit. PIXEL_FORMAT=PIXEL_444 is the gnuboy bridge's; build -e cyd-gnuboy."
#endif
#include "save/autosave.h"
#include "speaker.h"
#include "audio/mix.h"
#ifdef DEV_TONE_AUDIO
#include "audio/tone.h"
#endif
/* The vendored APU header is plain C with no linkage guard of its own, and it
 * is kept byte-identical to upstream, so the guard goes here. */
extern "C" {
#include "minigb_apu.h"
}
#include <Arduino.h>
#include <esp_timer.h>
#include <string.h>

/* Peanut-GB calls these when ENABLE_SOUND is non-zero and expects the
 * including translation unit to provide them. static is right here: the
 * header calls them by name from this same unit, and nothing else may link
 * to them. Declared ahead of the include because the header's use precedes
 * their definitions below. */
static uint8_t audio_read(uint16_t addr);
static void audio_write(uint16_t addr, uint8_t val);

#define ENABLE_LCD 1
#define ENABLE_SOUND 1
#define PEANUT_GB_HIGH_LCD_ACCURACY 0

/* Bench counters behind the two hooks the vendored header exposes.
 * emu_steps counts emulated instructions and is the workload proxy the
 * [PERF] line reports as steps/s: two builds compared at the same steps/s
 * saw the same work. draw_cycles is the line renderer's own cycle count for
 * the frame, read from the core's cycle counter around each __gb_draw_line,
 * which is what separates PPU time from interpreter time without a
 * regression (BUG-0011). About four instructions per emulated instruction
 * and four per scanline. */
#include <xtensa/hal.h>
static uint32_t emu_steps = 0;
static uint32_t draw_cycles = 0;
#define PEANUT_GB_STEP_HOOK() (emu_steps++)
#define PEANUT_GB_DRAW_LINE(g)                                            \
    do {                                                                  \
        uint32_t c0_ = xthal_get_ccount();                                \
        __gb_draw_line(g);                                                \
        draw_cycles += xthal_get_ccount() - c0_;                          \
    } while (0)
/* Sprite-preserving interlace (BUG-0011). The line renderer saves each drawn
 * line's background here before it composites sprites, and restores it on the
 * frames where that line's background is skipped, so sprites still move at the
 * full rate while only the background is interlaced. The saved copy has to be
 * pre-sprite: restoring the composited line would leave last frame's sprite
 * pixels on the line to be drawn over, i.e. a trail. 23 KB on the heap beside
 * fb, which the static DRAM segment has no room for (OBS-0012). */
static uint8_t* bgfb = nullptr;
static void bg_save(const uint8_t* px, unsigned ln);
static void bg_restore(uint8_t* px, unsigned ln);
#define PEANUT_GB_BG_SAVE(g, px)    bg_save((px), (g)->hram_io[IO_LY])
#define PEANUT_GB_BG_RESTORE(g, px) bg_restore((px), (g)->hram_io[IO_LY])
#include "peanut_gb.h"

/* The two-places rule, made a compile error rather than a comment: the APU
 * derives its rate and frame length from platformio.ini's AUDIO_SAMPLE_RATE,
 * and the speaker sizes its DMA buffers from hw_config.h. */
static_assert(AUDIO_SAMPLE_RATE == SPEAKER_SAMPLE_RATE,
              "AUDIO_SAMPLE_RATE in platformio.ini and SPEAKER_SAMPLE_RATE in "
              "hw_config.h disagree");
static_assert(AUDIO_SAMPLES == SPEAKER_SAMPLES_PER_FRAME,
              "the APU's samples per frame and SPEAKER_SAMPLES_PER_FRAME "
              "disagree");

// ─── ROM ────────────────────────────────────────────────────────────────────
// The ROM is a pointer into memory-mapped flash, owned by the rom_store
// module and valid for the whole session. The interpreter reads it directly
// through gb->rom_direct (a local modification to the vendored header): one
// load per byte, no callback, no bounds check. What used to be here — a
// sixteen-entry 4 KB page cache with its own hash table and LRU, plus a 32 KB
// copy of bank 0 — was removed on the grounds that the hardware flash cache
// does that job in silicon for free (§3.2), and the bench agreed: caching
// eliminated 224,000 flash reads a second and fps did not move (BUG-0011).
// The callback below still serves gb_init()'s header parse, and it is the
// fallback when a ROM file is shorter than its header claims.


static const uint8_t* rom = nullptr;
static uint32_t romlen = 0;

// ─── State ──────────────────────────────────────────────────────────────────
static struct gb_s* gb = nullptr;
#define MAXRAM (32*1024)
static uint8_t* cram = nullptr;
// When cartridge RAM is worth writing to the card is decided in gbcore,
// not here: this module only reports writes and relays the answers.
static autosave_state_t autosave;
static uint8_t fskip = 0, fcnt = 0;
static uint32_t fpsc = 0, fpst = 0, cfps = 0;
static uint8_t jpad = 0;

// ─── Palette ────────────────────────────────────────────────────────────────
// The tables and the fill rule live in gbcore (host-tested); this is the thin
// wrapper. Values stay NATIVE RGB565 — there is no pre-swap macro any more,
// because the blend runs before the byte swap and the display module handles
// wire order once at push time (§2.3). A pre-swapped LUT is not an option:
// avg565 needs each channel contiguous, and a byte swap splits green.
static uint16_t lut[PALETTE_LUT_SIZE];
static uint8_t curpal = PALETTE_AUTO;
/* The colours the Game Boy Color's table gives this cartridge, looked up once
 * per ROM in emu_init(). Before that the lookup has not run, so PALETTE_AUTO
 * builds the shipped default instead. */
static uint16_t auto_ramps[3][4];
static bool auto_ok = false;

void emu_set_palette(uint8_t idx)
{
    /* PALETTE_AUTO is accepted as well as the table's own indices: it is a
     * real choice the menu offers, not an out-of-range value. */
    if (idx > PALETTE_AUTO) {
        return;
    }
    curpal = idx;
    if (curpal != PALETTE_AUTO) {
        palette_build_lut(curpal, lut);
    } else if (auto_ok) {
        palette_build_lut_ramps(auto_ramps, lut);
    } else {
        palette_build_lut(PALETTE_FALLBACK, lut);
    }
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
// Peanut-GB hands us one 160-px index line at a time. Core 1 copies it into a
// persistent raw frame buffer (fb), and when the first line of the NEXT block
// arrives the finished block is copied out of fb into a queue slot and
// committed. The frame's final block is committed from emu_run_frame() once
// gb_run_frame() returns, because under interlace the last drawn line is 142
// on half the frames and no line number says "done".
//
// A block is BLOCK_LINES raw lines — BLOCK_UNITS scaler units — plus room for
// one lookahead line, the first line of the next block, copied only when the
// geometry table says a blend row reads it (26/16 does; 24/16 never does).
// Where it is needed and missing, the scaler's frame-end rule would fire at
// every block boundary and band the picture. The final block has no
// lookahead, and its last_in_frame flag is what tells the consumer to pass
// NULL.
//
// The persistent buffer is what makes interlace possible: a line Peanut-GB
// skips this frame keeps last frame's pixels, so every block still commits
// BLOCK_LINES valid lines. It also closes two gaps the per-line scheme could
// not: a frame that begins past block 0 (LCD enabled mid-frame) commits the
// blocks it missed from last frame's lines, and a frame cut short (LCD
// disabled mid-frame) commits the rest the same way — so the consumer always
// sees whole frames and the queue never sticks on a half one.
//
// Colour never enters a slot: the raw byte is what the queue carries, and the
// LUT, the scaler and the scaled DMA buffers all belong to the consumer on
// core 0 (see the pipeline note below). Every size here derives from
// RENDER_GEOM and BLOCK_UNITS, so flipping either changes the output with no
// edit here.
static uint8_t slot_src[FRAMEQUEUE_SLOTS][BLOCK_LINES + 1][SCALER_SRC_W];
static uint16_t lut_lines[BLOCK_LINES + 1][SCALER_SRC_W];
static uint16_t scratch_row[SCALER_DST_W_MAX];
static const scaler_geom_info_t* geom = nullptr;
static int16_t vp_x = GAME_X;
static int16_t vp_y = GAME_Y;

// ─── Pipeline ───────────────────────────────────────────────────────────────
// Threading model, stated once so nothing else has to guess:
//
//   core 1, loopTask   emulation and audio. Peanut-GB calls lcd_line, which
//                      copies each raw line into the persistent frame buffer
//                      and, once a block is complete, copies that block into
//                      a queue slot and commits it: about 46 KB of copying
//                      per frame, and nothing else for the display.
//   core 0, gbpush     emu_push_task. Pops committed slots, LUTs the raw
//                      lines into RGB565, scales the block into one of two
//                      DMA buffers while the other is still crossing the
//                      bus, hands the slot back as soon as its raw lines are
//                      consumed, then queues the transfer.
//
// The APU callback, the mixer and the speaker's DMA write all run on core 1
// inside emu_run_frame(), beside the emulation that feeds them: the APU's
// state is written by audio_write() from that same core, so moving the
// callback to core 0 would need a lock the design never asked for.
//
// The two never touch the same slot at the same time; framequeue is what makes
// that a checked property rather than a convention, and it is host-tested. The
// only shared mutable state outside the queue is the timing counters, single
// writer each; vp_x/vp_y, which change only from the menu with the pipeline
// paused; and the palette LUT, which the menu rebuilds from core 1 while the
// pipeline is paused and the consumer is parked on an empty queue.
//
// Two DMA buffers, one block each: while the bus reads buffer A, core 0
// scales the next block into buffer B, and the push of B waits for A to
// finish before queueing — that wait is the pipelining, and it is why the
// buffer being scaled is never the one in flight. Static so they land in
// internal DRAM: the SPI DMA engine cannot read from flash, and this board
// has no PSRAM to get wrong. At BLOCK_UNITS 4 and 24/16: 2 x 5.6 KB of
// buffers plus 2.9 KB of raw slot lines. The static DRAM segment has about
// 15 KB spare after this; anything larger goes on the heap.
//
// The mapped ROM is read-only for the whole session and must stay that way now
// that two cores execute from flash: a flash write stalls the other core's
// instruction fetch, so anything that writes the ROM partition has to happen
// before the push task exists.
static uint16_t dma_buf[2][BLOCK_ROWS * GAME_W];
static framequeue_t fq;
static TaskHandle_t push_task = nullptr;
static uint16_t frame_seq = 0;
static bool frame_dropped = false;
// The raw frame, GB_SCREEN_H x SCALER_SRC_W index bytes. Heap, not static:
// 23 KB, and the static DRAM segment is nearly full (OBS-0012).
static uint8_t* fb = nullptr;
// Block the incoming lines belong to, -1 before a frame's first line. The
// block is committed when a line from a later block arrives, or at frame end.
static int cur_block = -1;
// A line has arrived since emu_run_frame() began: frame_seq has been bumped
// and cur_block is meaningful.
static bool frame_open = false;

// ─── Frame timing ───────────────────────────────────────────────────────────
// Microseconds of the last COMPLETED frame, from esp_timer_get_time(): emu is
// the Peanut-GB frame itself, scale the accumulated LUT + scaler time and push
// the accumulated display time. Reported once a second, never per frame —
// serial writes cost frame time.
static uint32_t emu_us = 0;
// Written by the push task on core 0 and read by the [PERF] line on core 1.
// A 32-bit aligned volatile write is atomic on this part, so the worst a race
// can do is report the previous frame's figure in a once-a-second diagnostic.
static volatile uint32_t scale_us = 0;
static volatile uint32_t push_us = 0;
// Microseconds core 1 spent waiting for a free slot, and how often it found
// none. Both are the overlap's report card: stall time means the display is
// the bottleneck, zero stall with a full max_depth means emulation is.
static uint32_t q_stall_us = 0;
static uint32_t q_stall_acc = 0;
// Microseconds of the last frame spent inside __gb_draw_line, from the cycle
// counter (see PEANUT_GB_DRAW_LINE). Contained in emu_us.
static uint32_t draw_us = 0;

// ─── Audio ──────────────────────────────────────────────────────────────────
// One APU context, one frame of its interleaved stereo output, one frame of
// mixed 8-bit mono, and the dither state that carries across frames. All
// static, so all internal DRAM and none of it allocated per frame: 2192 B for
// the stereo frame, 548 B for the mono one, and the context itself.
static struct minigb_apu_ctx apu;
static audio_sample_t apu_buf[AUDIO_SAMPLES_TOTAL];
static uint8_t mono_buf[AUDIO_SAMPLES];
static mix_state_t mix;
#ifdef DEV_TONE_AUDIO
/* Bench A/B for BUG-0011: a known-perfect square replaces the APU's output
 * while everything else — the emulator, the frame pacing, the mixer, the DMA
 * write — stays exactly as it is. A tone that comes through clean puts the
 * fault in what the APU generates; a tone that crackles puts it in how the
 * samples are delivered. */
static tone_state_t tone_st;
#endif
// Off until main() applies the stored setting, so a unit is never loud before
// its own volume is read.
static uint8_t vol_idx = MIX_VOL_OFF;
// The APU callback plus the mix, for the [PERF] line.
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
 * and commit it. Waiting for a free slot is the only thing core 1 waits for,
 * and waiting there is the overlap working: core 1 is ahead of core 0 and the
 * two-slot backpressure is what keeps them in step. Backpressure is the whole
 * rate control; there is no catch-up path that drops a block to get ahead,
 * because that is how tearing gets in. A PAUSED acquire is the menu taking
 * the bus, which is a different thing: the frame is not being raced, it is
 * being cancelled, so the rest of it is abandoned and resume starts clean.
 *
 * Deliberately not IRAM_ATTR: it calls straight into flash-resident gbcore.
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
        /* Unreachable while commit_blocks is the only producer and walks the
         * blocks in order, so if it ever fires the sequencing assumption has
         * been broken and the rest of the frame is not worth pushing. */
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
 * Frame end, from emu_run_frame() once gb_run_frame() has returned. Whatever
 * block was still collecting lines goes out now, and so does anything after
 * it: a frame the LCD cut short still reaches the consumer whole, made of
 * last frame's lines. A frame that drew nothing (frameskip, LCD off) commits
 * nothing and the sequence number simply jumps, which is what the queue's
 * ordering rule allows.
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
 * Consumer half, pinned to core 0. Every display transform lives here: the
 * raw lines are LUT'd, the block is scaled unit by unit into the DMA buffer
 * the bus is NOT reading, and queued. Frame bracketing is driven entirely by
 * the metadata the producer committed: block 0 opens the address window,
 * last_in_frame closes it.
 *
 * The buffers alternate. dma_buf[buf] was queued two blocks ago, and the
 * push of the block in between waited for that transfer to finish before
 * queueing its own, so by the time this block is scaled into dma_buf[buf]
 * the bus has left it. Scaling block N therefore overlaps the transfer of
 * block N-1, which is the core split the design asked for.
 *
 * A slot is released as soon as its raw lines have been consumed, before its
 * pixels cross the bus, so core 1 gets it back a transfer early. The frame's
 * last block is the exception: framequeue_drained() is the menu's cue to
 * take the bus, and it must not fire while a transfer is in flight or the
 * frame's window is still open, so that slot is released after
 * display_frame_end().
 *
 * The wait when the queue is empty is a task notification rather than a
 * spin: this is the only task core 0 hosts — input is polled per frame from
 * the emulation loop on core 1 — and a busy loop here would starve that
 * core's idle task into a watchdog reset. The timeout is the belt to that
 * braces — a lost wakeup costs one late block, not a stalled pipeline.
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
        /* The lookahead rides in the slot after the block's own lines, when
         * the geometry reads one at all; the frame's final block has none.
         * No mask on the raw byte: the 12-colour path bounds it at 0x23 and
         * the LUT covers all 64 values, which is what removes 23,040 ANDs per
         * frame (§2.4). */
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
        /* One scaler call per unit. A unit's lookahead is the next unit's
         * first line; the block's last unit takes the slot's. The only
         * failure is a NULL buffer or a bad enum, and every argument here is
         * a static or a compile-time constant. */
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
        /* Waits for the previous block's transfer, then queues this one. */
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

// ─── Callbacks ──────────────────────────────────────────────────────────────
static uint8_t IRAM_ATTR gb_rom_read(struct gb_s* g, const uint_fast32_t a)
{
    (void)g;
    /* Header parse at init, and every read for a ROM whose file is shorter
     * than its header's bank count (emu_init leaves rom_direct NULL then):
     * an out-of-range bank read would otherwise fault through the flash
     * cache. */
    return (a < romlen) ? rom[a] : 0xFF;
}
static uint8_t IRAM_ATTR gb_cram_r(struct gb_s* g, const uint_fast32_t a) {
    (void)g; return (a<MAXRAM)?cram[a]:0xFF;
}
static void IRAM_ATTR gb_cram_w(struct gb_s* g, const uint_fast32_t a, const uint8_t v) {
    (void)g;
    /* Autosave costs one compare against the cartridge's real save size and
     * one byte store on the in-range path, and the compare alone on the
     * out-of-range path. autosave_note_write() is inline in the header so
     * this IRAM-resident callback never calls into flash, and it reads no
     * clock: the per-frame tick stamps the time. */
    if (a < MAXRAM) {
        cram[a] = v;
        autosave_note_write(&autosave, (uint32_t)a);
    }
}
/* Not IRAM_ATTR: these run only on an APU register access, a handful of times
 * per frame, and nothing writes flash during play. */
static uint8_t audio_read(uint16_t addr)
{
    return minigb_apu_audio_read(&apu, addr);
}

static void audio_write(uint16_t addr, uint8_t val)
{
    minigb_apu_audio_write(&apu, addr, val);
}

static void gb_err(struct gb_s* g, const enum gb_error_e e, const uint16_t a) {
    (void)g; Serial.printf("[EMU] Err %d @0x%04X\n",(int)e,a);
}
static void IRAM_ATTR bg_save(const uint8_t* px, unsigned ln)
{
    memcpy(bgfb + (size_t)ln * SCALER_SRC_W, px, SCALER_SRC_W);
}

static void IRAM_ATTR bg_restore(uint8_t* px, unsigned ln)
{
    memcpy(px, bgfb + (size_t)ln * SCALER_SRC_W, SCALER_SRC_W);
}

static void IRAM_ATTR lcd_line(struct gb_s* g, const uint8_t px[160], const uint_fast8_t ln)
{
    int blk;

    (void)g;
    /* Frameskip is Peanut-GB's own (gb->direct.frame_skip): on a skipped
     * frame it skips the PPU line draw as well, so this callback is never
     * entered and neither the scaler nor the queue sees the frame. Interlace
     * (gb->direct.interlace) skips alternate lines instead; those keep last
     * frame's bytes in fb. */
    blk = (int)(ln / BLOCK_LINES);
    if (!frame_open) {
        /* First drawn line of this frame. The sequence counts drawn frames
         * and jumps over skipped ones. Blocks before this one — an LCD
         * enabled mid-frame — go out with last frame's lines so the frame
         * is whole. */
        frame_open = true;
        frame_seq++;
        commit_blocks(0, blk - 1);
        cur_block = blk;
    }
    if (frame_dropped) {
        return;
    }
    memcpy(fb + (size_t)ln * SCALER_SRC_W, px, SCALER_SRC_W);
    if (blk != cur_block) {
        /* This line is the next block's first, and under 26/16 the previous
         * block's lookahead, so it is in fb before the commit reads it. */
        commit_blocks(cur_block, blk - 1);
        cur_block = blk;
    }
}

// ─── API ────────────────────────────────────────────────────────────────────
/*
 * The cartridge title out of the mapped header bytes. A byte outside the
 * printable range is padding or, on a Game Boy Color cartridge, part of the
 * manufacturer and CGB fields that overlap the tail of the field — never part
 * of a name — so it terminates the string rather than being kept. out_sz
 * wants to be 17 for the whole field; a shorter buffer truncates.
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

    if (!cram) {
        cram = (uint8_t*)malloc(MAXRAM);
    }
    if (!cram) {
        return false;
    }
    memset(cram, 0xFF, MAXRAM);

    if (!gb) {
        gb = (struct gb_s*)malloc(sizeof(struct gb_s));
    }
    if (!gb) {
        return false;
    }
    memset(gb, 0, sizeof(struct gb_s));

    if (!fb) {
        fb = (uint8_t*)malloc((size_t)GB_SCREEN_H * SCALER_SRC_W);
    }
    if (!fb) {
        return false;
    }
    /* Index 0 is shade 0 of the BG palette: the blank the panel shows until
     * the first frame lands, and what an interlaced first frame's skipped
     * lines are made of. */
    memset(fb, 0, (size_t)GB_SCREEN_H * SCALER_SRC_W);

    if (!bgfb) {
        bgfb = (uint8_t*)malloc((size_t)GB_SCREEN_H * SCALER_SRC_W);
    }
    if (!bgfb) {
        return false;
    }
    memset(bgfb, 0, (size_t)GB_SCREEN_H * SCALER_SRC_W);

    enum gb_init_error_e r = gb_init(gb, gb_rom_read, gb_cram_r, gb_cram_w,
                                     gb_err, nullptr);
    if (r != GB_INIT_NO_ERROR) {
        Serial.printf("[EMU] init fail %d\n", (int)r);
        return false;
    }
    /* The interpreter reads the ROM through this pointer, not the callback:
     * one load per byte instead of an indirect call and a bounds check on
     * every instruction fetch. Only when the file holds every bank the
     * header declares, so a bank index the MBC lets through cannot run off
     * the end of the map; a short file keeps the checked callback. */
    if ((uint32_t)(gb->num_rom_banks_mask + 1u) * ROM_BANK_SIZE <= romlen) {
        gb->rom_direct = rom;
    } else {
        Serial.println("[EMU] ROM shorter than its header claims; reads "
                       "go through the callback");
    }

    /* The cartridge's real save size, from the header gb_init just parsed.
     * An unrecognised RAM-size code is -1, which becomes 0 — autosave off —
     * rather than a guess at how much RAM to write to the card. */
    size_t save_sz = 0;
    if (gb_get_save_size_s(gb, &save_sz) != 0) {
        save_sz = 0;
        Serial.println("[EMU] unknown save size, autosave off");
    }
    autosave_init(&autosave, (uint32_t)save_sz);

    minigb_apu_audio_init(&apu);
    mix_init(&mix, 0x2545F491u);
#ifdef DEV_TONE_AUDIO
    tone_init(&tone_st, TONE_HZ, SPEAKER_SAMPLE_RATE);
    Serial.println("[EMU] DEV_TONE_AUDIO: APU output replaced by a test tone");
#endif

    gb_init_lcd(gb, lcd_line);
    gb->direct.frame_skip = (fskip > 0);
    /* Interlace the background on alternate lines: it is the largest and most
     * scene-dependent term in core 1's frame, and a background that scrolls
     * slowly or not at all hides a one-frame-old line. Sprites are still
     * composited on every line (see PEANUT_GB_BG_RESTORE in the vendored
     * header), because a sprite moving several pixels a frame combs visibly
     * when half its rows lag — measured on the bench 2026-09-18 as the one
     * artefact of plain interlace, on Black Castle. */
    gb->direct.interlace = 1;
    /* Before the LUT, and once per ROM: the header is the only input and it
     * cannot change while a cartridge is running. */
    auto_ok = cgb_palette_lookup(rom, romlen, auto_ramps);
    /* Build the LUT here too: main() may never call emu_set_palette. */
    emu_set_palette(curpal);
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
    Serial.printf("[EMU] '%s' %uKB heap:%u\n", title, romlen / 1024,
                  ESP.getFreeHeap());
    return true;
}

void emu_run_frame() {
    gb->direct.joypad_bits.a=!(jpad&0x10); gb->direct.joypad_bits.b=!(jpad&0x20);
    gb->direct.joypad_bits.select=!(jpad&0x40); gb->direct.joypad_bits.start=!(jpad&0x80);
    gb->direct.joypad_bits.right=!(jpad&0x01); gb->direct.joypad_bits.left=!(jpad&0x02);
    gb->direct.joypad_bits.up=!(jpad&0x04); gb->direct.joypad_bits.down=!(jpad&0x08);
    int64_t t = esp_timer_get_time();
    draw_cycles = 0;
    gb_run_frame(gb);
    frame_end();
    emu_us = (uint32_t)(esp_timer_get_time() - t);
    draw_us = draw_cycles / (F_CPU / 1000000UL);

    /* Every frame, skipped display frame or not: the sound has to stay
     * continuous, and the write is also what paces emulation — it blocks
     * only while the DMA queue is full, which happens only when the emulator
     * is ahead of real time. */
    t = esp_timer_get_time();
    minigb_apu_audio_callback(&apu, apu_buf);
#ifdef DEV_TONE_AUDIO
    /* Overwritten, not skipped: the callback's cost stays inside the frame so
     * the pacing under test is the real one. */
    tone_fill(&tone_st, TONE_AMPLITUDE, apu_buf, AUDIO_SAMPLES);
#endif
    mix_mono(&mix, apu_buf, AUDIO_SAMPLES, vol_idx, mono_buf);
    apu_us = (uint32_t)(esp_timer_get_time() - t);
    speaker_write_frame(mono_buf, AUDIO_SAMPLES);

    fcnt++; fpsc++;
    uint32_t n=millis();
    if (n - fpst >= 1000) {
        cfps = fpsc;
        fpsc = 0;
        fpst = n;
        /* Last completed frame, once a second. qstall is core 1's wait for a
         * free slot and qovf the running count of times it found none: with
         * the split, those two are what say which core is the bottleneck.
         * scale and push are measured on core 0 (see emu_push_task). */
        uint32_t aunder = 0, aover = 0, await_us = 0;
        speaker_get_stats(&aunder, &aover, &await_us);
        /* split=c0 marks the accounting: scale and push are core 0's, emu
         * contains only qstall. tools/perf_capture.py keys on it. draw is
         * the PPU's share of emu; steps is emulated instructions per
         * second, the workload proxy for comparing builds. */
        Serial.printf("[PERF] emu=%uus scale=%uus push=%uus qstall=%uus "
                      "qovf=%u apu=%uus await=%uus aunder=%u aover=%u "
                      "fps=%u split=c0 draw=%uus steps=%u\n",
                      emu_us, scale_us, push_us, q_stall_us,
                      framequeue_overflows(&fq), apu_us, await_us, aunder,
                      aover, cfps, draw_us, emu_steps);
        /* Per second, so the count reads as a rate beside fps. */
        emu_steps = 0;
    }
}

void emu_start_push_task()
{
    if (push_task) {
        return;
    }
    /* Core 0, which this task now has to itself: input is polled per frame
     * from the emulation loop on core 1, so priority 3 only has to beat that
     * core's idle task. 4096 bytes; the high-water mark is a bench item. */
    xTaskCreatePinnedToCore(emu_push_task, "gbpush", 4096, nullptr, 3,
                            &push_task, 0);
}

void emu_pause_pipeline()
{
    /* Called from the emulation task between frames, so no block is half
     * produced when the pause lands. Waiting for drained is what guarantees
     * nothing is still queued to be drawn over the menu, and framequeue
     * answers PAUSED rather than FULL so the producer cannot be stuck here. */
    framequeue_pause(&fq);
    while (!framequeue_drained(&fq)) {
        taskYIELD();
    }
    display_bus_acquire();
    /* The queue holds mid-scale for as long as the menu is up, so a starved
     * DMA chain repeats silence rather than the last fragment of music.
     * Resume needs nothing: the next frame's write refills. */
    speaker_silence();
}

void emu_resume_pipeline()
{
    display_bus_release();
    frame_dropped = false;
    framequeue_resume(&fq);
}

void emu_set_joypad(uint8_t b){jpad=b;}
uint8_t* emu_get_cart_ram(uint32_t* s){uint_fast32_t r=0;gb_get_save_size_s(gb,&r);if(s)*s=(uint32_t)r;return cram;}
void emu_set_cart_ram(const uint8_t* d,uint32_t s){if(s>MAXRAM)s=MAXRAM;memcpy(cram,d,s);}

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
    if (gb) {
        gb->direct.frame_skip = (fskip > 0);
    }
}
uint8_t emu_get_frame_skip(){return fskip;}
uint32_t emu_get_fps(){return cfps;}
void emu_reset()
{
    /* gb_reset() does not touch the APU, so a reset would otherwise resume
     * with whatever the previous game left in the sound registers. */
    gb_reset(gb);
    minigb_apu_audio_init(&apu);
    fcnt = 0;
}

void emu_get_rom_title(char* out, size_t out_sz)
{
    rom_title(out, out_sz);
}

uint8_t emu_get_colour_hash()
{
    /* Reads the header through gb->gb_rom_read, so there is nothing to sum
     * until the emulator has been initialised. */
    return gb ? gb_colour_hash(gb) : 0;
}
