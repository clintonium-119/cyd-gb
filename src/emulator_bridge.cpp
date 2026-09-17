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
// module and valid for the whole session. What used to be here — a sixteen
// entry 4 KB page cache with its own hash table and LRU, plus a 32 KB copy of
// bank 0 — is gone: the hardware flash cache does that job, in silicon, for
// free (§3.2).
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
// because the blend runs before the byte swap and setSwapBytes(true) handles
// wire order once at push time (§2.3).
static uint16_t lut[PALETTE_LUT_SIZE];
static uint8_t curpal = 0;
#if SCALER_VARIANT_LUT
// The pair table the bench variant scales from: 24 KB, rebuilt with the LUT,
// only from init and the menu, both with the consumer parked. Heap, not
// static: the static DRAM segment has about 15 KB to spare after the frame
// buffers, and the table does not fit there. Allocated once in emu_init().
static uint16_t* pair_lut = nullptr;
#endif

void emu_set_palette(uint8_t idx)
{
    if (idx >= PALETTE_COUNT) {
        return;
    }
    curpal = idx;
    palette_build_lut(curpal, lut);
#if SCALER_VARIANT_LUT
    /* Timed and reported because a palette change from the menu must not
     * visibly hitch; 4096 entries, expected well under a millisecond. Before
     * emu_init() there is no table yet and nothing to fill. */
    if (pair_lut) {
        int64_t t0 = esp_timer_get_time();
        palette_build_pair_lut(lut, pair_lut);
        Serial.printf("[EMU] pair table %uus\n",
                      (unsigned)(esp_timer_get_time() - t0));
    }
#endif
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
// Peanut-GB hands us one 160-px index line at a time, in order. Core 1 does
// nothing with it but copy the 160 raw bytes into the slot the current
// geometry block owns. A slot is src_lines_per_block lines plus room for one
// lookahead line — the first line of the NEXT block — which is copied only
// when the geometry table says a blend row reads it (26/16 does; 24/16 never
// does). Where it is needed and missing, the scaler's frame-end rule would
// fire at every block boundary and band the picture. The frame's final block
// has no lookahead, and its last_in_frame flag is what tells the consumer to
// pass NULL.
//
// Colour never enters a slot: the raw byte is what the queue carries, and the
// LUT, the scaler and the scaled DMA buffer all belong to the consumer on
// core 0 (see the pipeline note below). Buffers are sized for the larger
// geometry and every count comes from the geometry table, so flipping SCALE_K
// changes the output with no edit here.
static uint8_t slot_src[FRAMEQUEUE_SLOTS][SCALER_SRC_LINES_MAX + 1][SCALER_SRC_W];
#if !SCALER_VARIANT_LUT
static uint16_t lut_lines[SCALER_SRC_LINES_MAX + 1][SCALER_SRC_W];
static uint16_t scratch_row[SCALER_DST_W_MAX];
#endif
static const scaler_geom_info_t* geom = nullptr;
static int16_t vp_x = GAME_X;
static int16_t vp_y = GAME_Y;

// ─── Pipeline ───────────────────────────────────────────────────────────────
// Threading model, stated once so nothing else has to guess:
//
//   core 1, loopTask   emulation and audio. Peanut-GB calls lcd_line, which
//                      copies each raw line into the open slot, acquiring a
//                      slot at a block's first line and committing the block
//                      once its lookahead line is in hand: 144 copies of 160
//                      bytes per frame, and nothing else for the display.
//   core 0, gbpush     emu_push_task. Pops committed slots, LUTs the raw
//                      lines into RGB565, scales the block into the DMA
//                      buffer, drives the DMA display path, and releases each
//                      slot only after its transfer has completed.
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
// paused; and the palette LUT (and the pair table, on the bench variant),
// which the menu rebuilds from core 1 while the pipeline is paused and the
// consumer is parked on an empty queue.
//
// The slot array is 2.9 KB of raw lines at the larger geometry; the scaled
// block lives in dma_buf, 6.8 KB, static so it lands in internal DRAM: the SPI
// DMA engine cannot read from flash, and this board has no PSRAM to get wrong.
//
// The mapped ROM is read-only for the whole session and must stay that way now
// that two cores execute from flash: a flash write stalls the other core's
// instruction fetch, so anything that writes the ROM partition has to happen
// before the push task exists.
static uint16_t dma_buf[SCALER_DST_ROWS_MAX * SCALER_DST_W_MAX];
static framequeue_t fq;
static TaskHandle_t push_task = nullptr;
static uint16_t frame_seq = 0;
static bool frame_dropped = false;
// The slot lcd_line is filling, or -1 between blocks. Producer-owned state.
static int open_slot = -1;

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

// ─── Audio ──────────────────────────────────────────────────────────────────
// One APU context, one frame of its interleaved stereo output, one frame of
// mixed 8-bit mono, and the dither state that carries across frames. All
// static, so all internal DRAM and none of it allocated per frame: 2192 B for
// the stereo frame, 548 B for the mono one, and the context itself.
static struct minigb_apu_ctx apu;
static audio_sample_t apu_buf[AUDIO_SAMPLES_TOTAL];
static uint8_t mono_buf[AUDIO_SAMPLES];
static mix_state_t mix;
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

/* Blocks one full frame is made of, from the geometry table. */
static uint8_t blocks_per_frame()
{
    return (uint8_t)(GB_SCREEN_H / geom->src_lines_per_block);
}

/*
 * Producer half: take the slot for the block about to start. The only thing
 * core 1 waits for is a free slot, and waiting there is the overlap working:
 * core 1 is ahead of core 0 and the two-slot backpressure is what keeps them
 * in step. Backpressure is the whole rate control; there is no catch-up path
 * that drops a block to get ahead, because that is how tearing gets in. The
 * one abandonment below is the menu taking the bus, which is a different
 * thing: the frame is not being raced, it is being cancelled.
 *
 * Deliberately not IRAM_ATTR: it calls straight into flash-resident gbcore.
 */
static void acquire_block()
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
        /* Paused: the menu has the bus. Abandon the rest of this frame rather
         * than committing a hole in the middle of it; resume starts clean. */
        frame_dropped = true;
        return;
    }
    open_slot = slot;
}

/*
 * Producer half: hand the open slot, holding the block that starts at source
 * line `first`, to the push task. Its lines and lookahead are already in
 * place, so this is metadata, a commit and a wake-up.
 */
static void push_block(uint_fast8_t first)
{
    framequeue_meta_t meta;

    meta.block_idx = (uint8_t)(first / geom->src_lines_per_block);
    meta.frame_seq = frame_seq;
    meta.last_in_frame = (meta.block_idx == (uint8_t)(blocks_per_frame() - 1u));
    if (framequeue_commit(&fq, open_slot, &meta) != FRAMEQUEUE_OK) {
        /* Unreachable while the frame walk in lcd_line is the only producer,
         * so if it ever fires the sequencing assumption has been broken and
         * the rest of the frame is not worth pushing. */
        Serial.println("[EMU] frame queue rejected a block");
        frame_dropped = true;
    }
    open_slot = -1;
    if (!frame_dropped && push_task) {
        xTaskNotifyGive(push_task);
    }
}

/*
 * Consumer half, pinned to core 0. Every display transform lives here: the
 * raw lines are LUT'd, the block is scaled into dma_buf and pushed, in that
 * order and one block at a time. Frame bracketing is driven entirely by the
 * metadata the producer committed: block 0 opens the address window,
 * last_in_frame closes it.
 *
 * A slot is released only after the block's transfer has completed. Its raw
 * bytes are finished with sooner than that, but framequeue_drained() is the
 * menu's cue to take the bus, and it must not fire while a transfer is still
 * in flight.
 *
 * The wait when the queue is empty is a task notification rather than a
 * spin: this is the only task core 0 hosts — input is polled per frame from
 * the emulation loop on core 1 — and a busy loop here would starve that
 * core's idle task into a watchdog reset. The timeout is the belt to that
 * braces — a lost wakeup costs one late block, not a stalled pipeline.
 */
static void emu_push_task(void* arg)
{
#if !SCALER_VARIANT_LUT
    const uint16_t* src_lines[SCALER_SRC_LINES_MAX];
    const uint16_t* lookahead;
    unsigned lines;
    unsigned i;
    unsigned x;
#endif
    framequeue_meta_t meta;
    uint32_t scale_acc = 0;
    uint32_t push_acc = 0;
    int slot = 0;
    int64_t t0;
    int64_t t1;

    (void)arg;
#if !SCALER_VARIANT_LUT
    for (i = 0; i < SCALER_SRC_LINES_MAX; i++) {
        src_lines[i] = lut_lines[i];
    }
#endif
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
#if SCALER_VARIANT_LUT
        /* Bench variant: palette lookup and horizontal blend are one table
         * read per source pair, straight from the raw bytes. */
        (void)scaler_scale_block_24_16_lut(slot_src[slot][0], slot_src[slot][1],
                                           pair_lut, dma_buf);
#else
        /* The lookahead rides in the slot after the block's own lines, when
         * the geometry reads one at all; the frame's final block has none.
         * No mask on the raw byte: the 12-colour path bounds it at 0x23 and
         * the LUT covers all 64 values, which is what removes 23,040 ANDs per
         * frame (§2.4). */
        lines = (unsigned)geom->src_lines_per_block;
        lookahead = nullptr;
        if (geom->uses_lookahead && !meta.last_in_frame) {
            lookahead = lut_lines[lines];
            lines++;
        }
        for (i = 0; i < lines; i++) {
            for (x = 0; x < SCALER_SRC_W; x++) {
                lut_lines[i][x] = lut[slot_src[slot][i][x]];
            }
        }
        /* The only failure is a NULL buffer or a bad enum, and every argument
         * here is a static or a compile-time constant. */
        (void)scaler_scale_block(SCALE_GEOM, SCALER_MODE_BLEND, src_lines,
                                 lookahead, dma_buf, scratch_row);
#endif
        t1 = esp_timer_get_time();
        scale_acc += (uint32_t)(t1 - t0);

        if (meta.block_idx == 0) {
            display_frame_begin(vp_x, vp_y);
        }
        display_push_rows_dma(dma_buf,
                              (size_t)geom->dst_rows_per_block * geom->dst_w);
        display_dma_wait();
        framequeue_release(&fq, slot);
        if (meta.last_in_frame) {
            display_frame_end();
        }
        push_acc += (uint32_t)(esp_timer_get_time() - t1);
        if (meta.last_in_frame) {
            scale_us = scale_acc;
            push_us = push_acc;
        }
    }
}

// ─── Callbacks ──────────────────────────────────────────────────────────────
static uint8_t IRAM_ATTR gb_rom_read(struct gb_s* g, const uint_fast32_t a)
{
    (void)g;
    /* One compare more than a bare rom[a]: an out-of-range bank read from a
     * corrupt ROM would otherwise fault through the flash cache, and this
     * branch predicts perfectly. */
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
static void IRAM_ATTR lcd_line(struct gb_s* g, const uint8_t px[160], const uint_fast8_t ln)
{
    unsigned lpb;
    unsigned in_block;

    (void)g;
    /* Frameskip is Peanut-GB's own (gb->direct.frame_skip): on a skipped
     * frame it skips the PPU line draw as well, so this callback is never
     * entered and neither the scaler nor the queue sees the frame. The frame
     * sequence therefore counts rendered frames and simply jumps over skipped
     * ones, which is exactly what the queue's ordering rule allows. */
    if (ln == 0) {
        q_stall_acc = 0;
        frame_dropped = false;
        frame_seq++;
    }
    if (frame_dropped) {
        return;
    }

    lpb = geom->src_lines_per_block;
    in_block = ln % lpb;
    if (in_block == 0) {
        /* This line is the previous block's lookahead as well as this block's
         * first line. 144 divides by both geometries' block heights, so a
         * block boundary is never also the last line. The previous block is
         * committed before the next slot is acquired, so core 0 has work in
         * hand while core 1 waits for the other slot to come free. */
        if (ln != 0) {
            if (geom->uses_lookahead) {
                memcpy(slot_src[open_slot][lpb], px, SCALER_SRC_W);
            }
            push_block((uint_fast8_t)(ln - lpb));
            if (frame_dropped) {
                return;
            }
        }
        acquire_block();
        if (frame_dropped) {
            return;
        }
    }
    memcpy(slot_src[open_slot][in_block], px, SCALER_SRC_W);

    if (ln == GB_SCREEN_H - 1) {
        /* Final block: no next line, so its trailing blend rows stay pure. Its
         * last_in_frame flag is what closes the window, over on core 0. */
        push_block((uint_fast8_t)(GB_SCREEN_H - lpb));
        q_stall_us = q_stall_acc;
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

    enum gb_init_error_e r = gb_init(gb, gb_rom_read, gb_cram_r, gb_cram_w,
                                     gb_err, nullptr);
    if (r != GB_INIT_NO_ERROR) {
        Serial.printf("[EMU] init fail %d\n", (int)r);
        return false;
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

    gb_init_lcd(gb, lcd_line);
    gb->direct.frame_skip = (fskip > 0);
#if SCALER_VARIANT_LUT
    if (!pair_lut) {
        pair_lut = (uint16_t*)malloc(PALETTE_PAIR_LUT_SIZE * sizeof(uint16_t));
    }
    if (!pair_lut) {
        return false;
    }
#endif
    /* Build the LUT here too: main() may never call emu_set_palette. */
    emu_set_palette(curpal);
    geom = scaler_geom_info(SCALE_GEOM);
    if (!geom) {
        return false;
    }
    frame_seq = 0;
    frame_dropped = false;
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
    gb_run_frame(gb);
    emu_us = (uint32_t)(esp_timer_get_time() - t);

    /* Every frame, skipped display frame or not: the sound has to stay
     * continuous, and the write is also what paces emulation — it blocks
     * only while the DMA queue is full, which happens only when the emulator
     * is ahead of real time. */
    t = esp_timer_get_time();
    minigb_apu_audio_callback(&apu, apu_buf);
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
         * the split, those two are what say which core is the bottleneck. */
        uint32_t aunder = 0, aover = 0, await_us = 0;
        speaker_get_stats(&aunder, &aover, &await_us);
        Serial.printf("[PERF] emu=%uus scale=%uus push=%uus qstall=%uus "
                      "qovf=%u apu=%uus await=%uus aunder=%u aover=%u "
                      "fps=%u\n",
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

bool emu_autosave_idle_due(uint32_t now_ms)
{
    return autosave_idle_due(&autosave, now_ms);
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
