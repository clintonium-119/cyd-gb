#include "emulator_bridge.h"
#include "display.h"
#include "hw_config.h"
#include "render_config.h"
#include "render/palette.h"
#include "render/framequeue.h"
#include "render/scaler.h"
#include <Arduino.h>
#include <esp_timer.h>
#include <string.h>

#define ENABLE_LCD 1
#define ENABLE_SOUND 0
#define PEANUT_GB_HIGH_LCD_ACCURACY 0
#include "peanut_gb.h"

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

void emu_set_palette(uint8_t idx)
{
    if (idx >= PALETTE_COUNT) {
        return;
    }
    curpal = idx;
    palette_build_lut(curpal, lut);
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
// Peanut-GB hands us one 160-px index line at a time, in order. Each line is
// LUT'd into a ring of source lines; as soon as a geometry block's lines plus
// the first line of the NEXT block are in hand, the block is scaled and pushed.
// The ring holds one slot more than a block needs, which is what lets that
// lookahead line double as the next block's first line with no copying: by the
// time a new line reuses a slot, the block that owned it has already been
// pushed.
//
// Buffers are sized for the larger geometry and every count comes from the
// geometry table, so flipping SCALE_K changes the output with no edit here.
static uint16_t line_ring[SCALER_SRC_LINES_MAX + 1][SCALER_SRC_W];
static uint16_t scratch_row[SCALER_DST_W_MAX];
static const scaler_geom_info_t* geom = nullptr;
static int16_t vp_x = GAME_X;
static int16_t vp_y = GAME_Y;

// ─── Pipeline ───────────────────────────────────────────────────────────────
// Threading model, stated once so nothing else has to guess:
//
//   core 1, loopTask   emulation and scaling. Peanut-GB calls lcd_line, which
//                      LUTs lines into line_ring and hands finished blocks to
//                      push_block. push_block owns slot_buf while the queue
//                      says the slot is the producer's.
//   core 0, gbpush     emu_push_task. Pops committed slots, drives the DMA
//                      display path, releases each slot only after its
//                      transfer has completed.
//
// The two never touch the same slot at the same time; framequeue is what makes
// that a checked property rather than a convention, and it is host-tested. The
// only shared mutable state outside the queue is the timing counters, single
// writer each, and vp_x/vp_y, which change only from the menu with the
// pipeline paused.
//
// One block per slot, sized for the larger geometry, static so it lands in
// internal DRAM: the SPI DMA engine cannot read from flash, and this board has
// no PSRAM to get wrong. Two slots is 13.5 KB against the ~96 KB the mapped
// ROM gave back.
//
// The mapped ROM is read-only for the whole session and must stay that way now
// that two cores execute from flash: a flash write stalls the other core's
// instruction fetch, so anything that writes the ROM partition has to happen
// before the push task exists.
static uint16_t slot_buf[FRAMEQUEUE_SLOTS][SCALER_DST_ROWS_MAX * SCALER_DST_W_MAX];
static framequeue_t fq;
static TaskHandle_t push_task = nullptr;
static uint16_t frame_seq = 0;
static bool frame_dropped = false;

// ─── Frame timing ───────────────────────────────────────────────────────────
// Microseconds of the last COMPLETED frame, from esp_timer_get_time(): emu is
// the Peanut-GB frame itself, scale is the accumulated scaler time and push the
// accumulated display time. The *_acc pair accumulates the frame in progress.
// Reported once a second, never per frame — serial writes cost frame time.
static uint32_t emu_us = 0;
static uint32_t scale_us = 0;
static uint32_t scale_acc = 0;
// Written by the push task on core 0 and read by the [PERF] line on core 1.
// A 32-bit aligned volatile write is atomic on this part, so the worst a race
// can do is report the previous frame's figure in a once-a-second diagnostic.
static volatile uint32_t push_us = 0;
// Microseconds core 1 spent waiting for a free slot, and how often it found
// none. Both are the overlap's report card: stall time means the display is
// the bottleneck, zero stall with a full max_depth means emulation is.
static uint32_t q_stall_us = 0;
static uint32_t q_stall_acc = 0;

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
 * Scale the block starting at source line `first` into a queue slot and hand
 * it to the push task. `lookahead` is the next block's first line, or nullptr
 * at frame end. Deliberately not IRAM_ATTR: it calls straight into
 * flash-resident gbcore.
 *
 * This is the producer half of the split. It no longer touches the display —
 * the only thing it waits for is a free slot, and waiting there is the overlap
 * working: core 1 is ahead of core 0 and the two-slot backpressure is what
 * keeps them in step. Backpressure is the whole rate control; there is no
 * catch-up path that drops a block to get ahead, because that is how tearing
 * gets in. The one abandonment below is the menu taking the bus, which is a
 * different thing: the frame is not being raced, it is being cancelled.
 */
static void push_block(uint_fast8_t first, const uint16_t* lookahead)
{
    const uint16_t* src_lines[SCALER_SRC_LINES_MAX];
    unsigned slots = (unsigned)geom->src_lines_per_block + 1u;
    framequeue_meta_t meta;
    unsigned i;
    int slot = 0;
    int r;
    int64_t t0;
    int64_t t1;

    if (frame_dropped) {
        return;
    }
    meta.block_idx = (uint8_t)(first / geom->src_lines_per_block);
    meta.frame_seq = frame_seq;
    meta.last_in_frame = (meta.block_idx == (uint8_t)(blocks_per_frame() - 1u));

    t0 = esp_timer_get_time();
    while ((r = framequeue_acquire(&fq, &slot)) == FRAMEQUEUE_FULL) {
        taskYIELD();
    }
    t1 = esp_timer_get_time();
    q_stall_acc += (uint32_t)(t1 - t0);
    if (r != FRAMEQUEUE_OK) {
        /* Paused: the menu has the bus. Abandon the rest of this frame rather
         * than committing a hole in the middle of it; resume starts clean. */
        frame_dropped = true;
        return;
    }

    for (i = 0; i < geom->src_lines_per_block; i++) {
        src_lines[i] = line_ring[(first + i) % slots];
    }
    if (scaler_scale_block(SCALE_GEOM, SCALER_MODE_BLEND, src_lines, lookahead,
                           slot_buf[slot], scratch_row) != SCALER_OK) {
        frame_dropped = true;
        return;
    }
    scale_acc += (uint32_t)(esp_timer_get_time() - t1);
    if (framequeue_commit(&fq, slot, &meta) != FRAMEQUEUE_OK) {
        /* Unreachable while the frame walk above is the only producer, so if
         * it ever fires the sequencing assumption has been broken and the rest
         * of the frame is not worth pushing. */
        Serial.println("[EMU] frame queue rejected a block");
        frame_dropped = true;
        return;
    }
    if (push_task) {
        xTaskNotifyGive(push_task);
    }
}

/*
 * Consumer half, pinned to core 0. Frame bracketing lives here now, driven
 * entirely by the metadata the producer committed: block 0 opens the address
 * window, last_in_frame closes it.
 *
 * A slot is released only after its transfer has completed, so the producer
 * can never refill a buffer the DMA engine is still reading. The wait when the
 * queue is empty is a task notification rather than a spin: this is the only
 * task core 0 hosts — input is polled per frame from the emulation loop on
 * core 1 — and a busy loop here would starve that core's idle task into a
 * watchdog reset. The timeout is the belt to that braces — a lost wakeup costs
 * one late block, not a stalled pipeline.
 */
static void emu_push_task(void* arg)
{
    framequeue_meta_t meta;
    uint32_t push_acc = 0;
    int slot = 0;
    int64_t t;

    (void)arg;
    for (;;) {
        if (framequeue_pop(&fq, &slot, &meta) != FRAMEQUEUE_OK) {
            ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(2));
            continue;
        }
        t = esp_timer_get_time();
        if (meta.block_idx == 0) {
            push_acc = 0;
            display_frame_begin(vp_x, vp_y);
        }
        display_push_rows_dma(slot_buf[slot],
                              (size_t)geom->dst_rows_per_block * geom->dst_w);
        display_dma_wait();
        framequeue_release(&fq, slot);
        if (meta.last_in_frame) {
            display_frame_end();
        }
        push_acc += (uint32_t)(esp_timer_get_time() - t);
        if (meta.last_in_frame) {
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
    if(a<MAXRAM) cram[a]=v;
}
static void gb_err(struct gb_s* g, const enum gb_error_e e, const uint16_t a) {
    (void)g; Serial.printf("[EMU] Err %d @0x%04X\n",(int)e,a);
}
static void IRAM_ATTR lcd_line(struct gb_s* g, const uint8_t px[160], const uint_fast8_t ln)
{
    unsigned slots;
    uint16_t* dst;
    int x;

    (void)g;
    /* Frameskip first: a skipped frame must never reach the queue. The frame
     * sequence therefore counts rendered frames and simply jumps over skipped
     * ones, which is exactly what the queue's ordering rule allows. */
    if (fskip > 0 && (fcnt % (fskip + 1)) != 0) {
        return;
    }
    if (ln == 0) {
        scale_acc = 0;
        q_stall_acc = 0;
        frame_dropped = false;
        frame_seq++;
    }

    slots = (unsigned)geom->src_lines_per_block + 1u;
    dst = line_ring[ln % slots];
    /* No mask: the 12-colour path bounds px[x] at 0x23 and the LUT covers all
     * 64 possible bytes, which is what removes 23,040 ANDs per frame (§2.4). */
    for (x = 0; x < GB_SCREEN_W; x++) {
        dst[x] = lut[px[x]];
    }

    /* This line is the previous block's lookahead as well as this block's
     * first line. 144 divides by both geometries' block heights, so a block
     * boundary is never also the last line. */
    if (ln >= geom->src_lines_per_block &&
        (ln % geom->src_lines_per_block) == 0) {
        push_block((uint_fast8_t)(ln - geom->src_lines_per_block), dst);
    }
    if (ln == GB_SCREEN_H - 1) {
        /* Final block: no next line, so its trailing blend rows stay pure. Its
         * last_in_frame flag is what closes the window, over on core 0. */
        push_block((uint_fast8_t)(GB_SCREEN_H - geom->src_lines_per_block),
                   nullptr);
        scale_us = scale_acc;
        q_stall_us = q_stall_acc;
    }
}

// ─── API ────────────────────────────────────────────────────────────────────
bool emu_init(const uint8_t* rom_data, uint32_t rom_size)
{
    char title[17] = {0};
    int i;

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
    gb_init_lcd(gb, lcd_line);
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

    /* The cartridge title, read straight from the mapped bytes. */
    if (romlen > 0x143) {
        for (i = 0; i < 16; i++) {
            char c = (char)rom[0x134 + i];
            title[i] = (c >= 32 && c < 127) ? c : 0;
        }
    }
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
    fcnt++; fpsc++;
    uint32_t n=millis();
    if (n - fpst >= 1000) {
        cfps = fpsc;
        fpsc = 0;
        fpst = n;
        /* Last completed frame, once a second. qstall is core 1's wait for a
         * free slot and qovf the running count of times it found none: with
         * the split, those two are what say which core is the bottleneck. */
        Serial.printf("[PERF] emu=%uus scale=%uus push=%uus qstall=%uus "
                      "qovf=%u fps=%u\n",
                      emu_us, scale_us, push_us, q_stall_us,
                      framequeue_overflows(&fq), cfps);
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
bool emu_cart_ram_dirty(){return false;}
uint32_t emu_get_cart_ram_last_write_ms(){return 0;}
void emu_clear_cart_ram_dirty(){}
void emu_set_frame_skip(uint8_t s){fskip=s;}
uint8_t emu_get_frame_skip(){return fskip;}
uint32_t emu_get_fps(){return cfps;}
void emu_reset(){gb_reset(gb);fcnt=0;}
