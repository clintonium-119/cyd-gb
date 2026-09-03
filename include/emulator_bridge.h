#pragma once
#include <stdint.h>
#include <stdbool.h>

#include "render/palette.h"

// The ROM is not opened, copied or closed any more: the caller hands over a
// pointer to the memory-mapped ROM partition, which must stay valid for the
// whole session. rom_store's map guarantees that — there is no unmap.
bool emu_init(const uint8_t* rom_data, uint32_t rom_size);
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

// ─── Automatic saves ───────────────────────────────────────────────────────
// Call emu_autosave_tick() once per frame, after emu_run_frame(). The
// cartridge-RAM write callback only sets a flag — it is IRAM resident and
// runs per access, so it may not read a clock — and the tick is what turns
// that flag into the dirty state. So the dirty stamp is the frame's
// timestamp, not the write's, which is at worst one frame stale and is what
// emu_autosave_idle_due() measures from.
//
// The battery thresholds are passed in rather than read here, which keeps
// this header free of hw_config.h: the constants live there and are applied
// from main.cpp, alongside the ADC reading they are compared against.
void emu_autosave_tick(uint32_t now_ms);

// Whether cartridge RAM is dirty and has gone unwritten long enough to be
// worth saving.
bool emu_autosave_idle_due(uint32_t now_ms);

// After a save that failed: the RAM stays dirty, but the idle clock restarts
// so the retry waits a full idle period instead of firing every frame.
void emu_autosave_defer(uint32_t now_ms);

// True exactly once per crossing below low_mv; re-arms only above
// low_mv + hyst_mv, so a cell sagging under load does not save repeatedly.
bool emu_autosave_battery(uint16_t mv, uint16_t low_mv, uint16_t hyst_mv);

// ─── Pipeline ───────────────────────────────────────────────────────────────
// Emulation and display transfer run on different cores so they overlap.
// Peanut-GB, the scaler and this module's frame walk stay on the Arduino
// loopTask (core 1); emu_start_push_task() creates a task on core 0 that pops
// scaled blocks from an internal two-slot queue and pushes them over DMA.
// Slot buffers belong to this module, and each is the producer's or the
// consumer's exclusively, never both — the queue is the arbiter and its rules
// are host-tested.
//
// Call once, after emu_init() succeeds and after anything that writes flash:
// two cores executing from flash means a flash write stalls both.
void emu_start_push_task();

// Menu handover. Pause stops the producer, waits for the queue to drain and
// takes the display bus, so `tft` may be drawn on directly; resume gives it
// back. Both must be called from the emulation task, between frames — pausing
// mid-frame would abandon the frame in progress. Every direct `tft` draw
// during emulation belongs between them.
void emu_pause_pipeline();
void emu_resume_pipeline();

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
