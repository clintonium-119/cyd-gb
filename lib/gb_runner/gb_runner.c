#include "gb_runner.h"

#include "peanut_gb.h"

/* Mirrors the firmware's callback wiring in src/emulator_bridge.cpp
 * (memory-backed gb_rom_read, plain-array cart RAM, per-scanline lcd
 * capture) with none of the SD/SPIFFS paging. */

#define GB_RUNNER_MAXRAM (32 * 1024)

static struct gb_s gb;
static const uint8_t* rom_buf;
static size_t rom_len;
static uint8_t cram[GB_RUNNER_MAXRAM];
static uint8_t frame[GB_RUNNER_H * GB_RUNNER_W];
static unsigned error_count;
static int booted;

static uint8_t rom_read(struct gb_s* g, const uint_fast32_t addr)
{
    (void)g;
    return (addr < rom_len) ? rom_buf[addr] : 0xFF;
}

static uint8_t cram_read(struct gb_s* g, const uint_fast32_t addr)
{
    (void)g;
    return (addr < GB_RUNNER_MAXRAM) ? cram[addr] : 0xFF;
}

static void cram_write(struct gb_s* g, const uint_fast32_t addr,
                       const uint8_t val)
{
    (void)g;
    if (addr < GB_RUNNER_MAXRAM) {
        cram[addr] = val;
    }
}

static void on_error(struct gb_s* g, const enum gb_error_e err,
                     const uint16_t addr)
{
    (void)g;
    (void)err;
    (void)addr;
    error_count++;
}

static void lcd_line(struct gb_s* g, const uint8_t* pixels,
                     const uint_fast8_t line)
{
    (void)g;
    if (line < GB_RUNNER_H) {
        size_t x;
        for (x = 0; x < GB_RUNNER_W; x++) {
            frame[(size_t)line * GB_RUNNER_W + x] = pixels[x];
        }
    }
}

int gb_runner_init(const uint8_t* rom, size_t len)
{
    size_t i;
    if (rom == NULL || len == 0) {
        return GB_RUNNER_ERR_ARGS;
    }
    rom_buf = rom;
    rom_len = len;
    error_count = 0;
    booted = 0;
    for (i = 0; i < sizeof(cram); i++) {
        cram[i] = 0;
    }
    for (i = 0; i < sizeof(frame); i++) {
        frame[i] = 0;
    }
    if (gb_init(&gb, rom_read, cram_read, cram_write, on_error, NULL)
        != GB_INIT_NO_ERROR) {
        return GB_RUNNER_ERR_INIT;
    }
    gb_init_lcd(&gb, lcd_line);
    booted = 1;
    return GB_RUNNER_OK;
}

int gb_runner_run_frames(unsigned n)
{
    unsigned i;
    if (!booted) {
        return GB_RUNNER_ERR_INIT;
    }
    for (i = 0; i < n; i++) {
        gb_run_frame(&gb);
    }
    return GB_RUNNER_OK;
}

const uint8_t* gb_runner_frame(void)
{
    return booted ? frame : NULL;
}

unsigned gb_runner_error_count(void)
{
    return error_count;
}
