#pragma once
#include <stdint.h>
#include <stdbool.h>

#include "render/palette.h"

bool emu_open_rom(const char* path);
void emu_close_rom();
bool emu_init(uint8_t* rom_data, uint32_t rom_size);
void emu_run_frame();
void emu_set_joypad(uint8_t buttons);
uint8_t* emu_get_cart_ram(uint32_t* size);
void emu_set_cart_ram(const uint8_t* data, uint32_t size);
bool emu_cart_ram_dirty();
uint32_t emu_get_cart_ram_last_write_ms();
void emu_clear_cart_ram_dirty();
void emu_set_frame_skip(uint8_t skip);
uint8_t emu_get_frame_skip();
uint32_t emu_get_fps();
void emu_reset();

// Viewport origin of the game image, per-unit nudgeable from NVS.
void emu_set_viewport(int16_t x, int16_t y);

// Microseconds spent in the last completed frame, split three ways. Any
// pointer may be NULL, so a caller can ask for just one.
void emu_get_frame_times(uint32_t* emu_us, uint32_t* scale_us,
                         uint32_t* push_us);

// Palette
// Alias kept so existing callers (the settings menu) compile unchanged; the
// count itself belongs to the gbcore palette module.
#define NUM_PALETTES PALETTE_COUNT
void emu_set_palette(uint8_t idx);
uint8_t emu_get_palette();
const char* emu_get_palette_name(uint8_t idx);
