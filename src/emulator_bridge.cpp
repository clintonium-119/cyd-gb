#include "emulator_bridge.h"
#include "display.h"
#include "hw_config.h"
#include "render_config.h"
#include "render/palette.h"
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
static uint16_t block_buf[SCALER_DST_ROWS_MAX * SCALER_DST_W_MAX];
static uint16_t scratch_row[SCALER_DST_W_MAX];
static const scaler_geom_info_t* geom = nullptr;
static int16_t vp_x = GAME_X;
static int16_t vp_y = GAME_Y;

// ─── Frame timing ───────────────────────────────────────────────────────────
// Microseconds of the last COMPLETED frame, from esp_timer_get_time(): emu is
// the Peanut-GB frame itself, scale is the accumulated scaler time and push the
// accumulated display time. The *_acc pair accumulates the frame in progress.
// Reported once a second, never per frame — serial writes cost frame time.
static uint32_t emu_us = 0;
static uint32_t scale_us = 0;
static uint32_t push_us = 0;
static uint32_t scale_acc = 0;
static uint32_t push_acc = 0;

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

/*
 * Scale the block starting at source line `first` and push its rows.
 * `lookahead` is the next block's first line, or nullptr at frame end.
 * Deliberately not IRAM_ATTR: it calls straight into flash-resident gbcore.
 */
static void push_block(uint_fast8_t first, const uint16_t* lookahead)
{
    const uint16_t* src_lines[SCALER_SRC_LINES_MAX];
    unsigned slots = (unsigned)geom->src_lines_per_block + 1u;
    unsigned i;
    int64_t t0;
    int64_t t1;

    for (i = 0; i < geom->src_lines_per_block; i++) {
        src_lines[i] = line_ring[(first + i) % slots];
    }
    /* Three timestamps, not four: the instant the scaler finishes is also the
     * instant the push begins, so it is read once and used for both. Timer
     * reads are not free and they land inside the very interval they measure. */
    t0 = esp_timer_get_time();
    if (scaler_scale_block(SCALE_GEOM, SCALER_MODE_BLEND, src_lines, lookahead,
                           block_buf, scratch_row) != SCALER_OK) {
        return;
    }
    t1 = esp_timer_get_time();
    display_push_rows(block_buf,
                      (size_t)geom->dst_rows_per_block * geom->dst_w);
    scale_acc += (uint32_t)(t1 - t0);
    push_acc += (uint32_t)(esp_timer_get_time() - t1);
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
    int64_t t;

    (void)g;
    /* Frameskip first: a skipped frame must never open an address window. */
    if (fskip > 0 && (fcnt % (fskip + 1)) != 0) {
        return;
    }
    if (ln == 0) {
        scale_acc = 0;
        push_acc = 0;
        t = esp_timer_get_time();
        display_frame_begin(vp_x, vp_y);
        push_acc += (uint32_t)(esp_timer_get_time() - t);
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
        /* Final block: no next line, so its trailing blend rows stay pure. */
        push_block((uint_fast8_t)(GB_SCREEN_H - geom->src_lines_per_block),
                   nullptr);
        t = esp_timer_get_time();
        display_frame_end();
        push_acc += (uint32_t)(esp_timer_get_time() - t);
        scale_us = scale_acc;
        push_us = push_acc;
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
        /* Last completed frame, once a second. */
        Serial.printf("[PERF] emu=%uus scale=%uus push=%uus fps=%u\n",
                      emu_us, scale_us, push_us, cfps);
    }
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
