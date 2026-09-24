#pragma once
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#include "render/palette.h"

// The ROM is not opened, copied or closed any more: the caller hands over a
// pointer to the memory-mapped ROM partition, which must stay valid for the
// whole session. rom_store's map guarantees that — there is no unmap.
bool emu_init(const uint8_t* rom_data, uint32_t rom_size);
// The 256-byte DMG boot ROM, run before the cartridge like a real power-on.
// Call before emu_init(); the bytes are copied there, so they need only live
// until it returns. Never called, or called with NULL, starts the cartridge
// at 0x100 as before.
#define DMG_BOOT_ROM_SIZE 256
void emu_set_boot_rom(const uint8_t* data);
void emu_run_frame();
void emu_set_joypad(uint8_t buttons);
uint8_t* emu_get_cart_ram(uint32_t* size);
void emu_set_cart_ram(const uint8_t* data, uint32_t size);
bool emu_cart_ram_dirty();
uint32_t emu_get_cart_ram_last_write_ms();
void emu_clear_cart_ram_dirty();
void emu_set_frame_skip(uint8_t skip);
uint8_t emu_get_frame_skip();
// Fast-forward: each emu_run_frame() runs two frames of game time and draws
// the second, so the game runs at up to 2x while the display holds its rate;
// a scene too heavy for that drops frames back to one run until it fits. The
// audio keeps its pitch: the two frames are spliced into one frame's worth.
// Runtime only, off at start.
void emu_set_fast_forward(bool on);
bool emu_get_fast_forward();
uint32_t emu_get_fps();
void emu_reset();

// ─── Cartridge header ───────────────────────────────────────────────────────
// The running cartridge's header title, so the boot flow can snapshot it once
// and never read the ROM again: the 16-character field filtered to printable
// characters and NUL-terminated, so out_sz wants to be at least 17.
void emu_get_rom_title(char* out, size_t out_sz);

// ─── Automatic saves ───────────────────────────────────────────────────────
// Call emu_autosave_tick() once per frame, after emu_run_frame(). The
// cartridge-RAM write callback only sets a flag — it is IRAM resident and
// runs per access, so it may not read a clock — and the tick is what turns
// that flag into the dirty state. So the dirty stamp is the frame's
// timestamp, not the write's, which is at worst one frame stale.
//
// There is deliberately no idle rule. A card write pauses emulation and
// audio for about 400 ms, and Pokemon uses cartridge RAM as scratch from
// the title screen on, so an idle save fired every ten seconds of play and
// every one was heard as a dropout (BUG-0011). Saves happen when the menu
// opens and once on a low battery.
//
// The battery thresholds are passed in rather than read here, which keeps
// this header free of hw_config.h: the constants live there and are applied
// from main.cpp, alongside the ADC reading they are compared against.
void emu_autosave_tick(uint32_t now_ms);

// After a save that failed: restamps the RAM so the next trigger's retry is
// not confused with a fresh write.
void emu_autosave_defer(uint32_t now_ms);

// True exactly once per crossing below low_mv; re-arms only above
// low_mv + hyst_mv, so a cell sagging under load does not save repeatedly.
bool emu_autosave_battery(uint16_t mv, uint16_t low_mv, uint16_t hyst_mv);

// ─── Save states ────────────────────────────────────────────────────────────
// The whole machine to and from one file, cartridge RAM included, through
// stdio: `path_vfs` is a VFS path, under the card's mount point. Save writes
// exactly the file it is given, so a caller that must never leave a
// half-written state saves to a temp name and renames it on success.
//
// Both must be called with the pipeline paused. A load that succeeded marks
// cartridge RAM dirty, so the loaded save reaches the .sav at the next
// flush. A load that failed may have overwritten part of the machine before
// it did; the caller should not resume as though nothing happened.
//
// A save is refused while the boot ROM is still running, because a load
// always resumes with it unmapped.
bool emu_state_save(const char* path_vfs);
bool emu_state_load(const char* path_vfs);

// The state's snapshot: the last drawn frame at half size, every other pixel
// of every other line, colourised through the palette as it stands now and
// written as raw little-endian RGB565, EMU_THUMB_W x EMU_THUMB_H with no
// header — the same format as the card's .565 art, so it is read the same
// way. A file that fails part way is removed. Pipeline paused, like the two
// above.
#define EMU_THUMB_W 80
#define EMU_THUMB_H 72
bool emu_state_thumb_save(const char* path_vfs);

// ─── Pipeline ───────────────────────────────────────────────────────────────
// Emulation and display transfer run on different cores so they overlap.
// The emulator core, the scaler and this module's frame walk stay on the
// Arduino loopTask (core 1); emu_start_push_task() creates a task on core 0
// that pops scaled blocks from an internal two-slot queue and pushes them
// over DMA.
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

// ─── Audio ──────────────────────────────────────────────────────────────────
// The volume index, in settings_t::volume's own encoding: Off, Low, Med,
// High, so a bigger number is louder. Applied from the next frame onward.
// The bridge starts at off, so a unit is silent until main() applies the
// stored setting; off is not a hardware mute — none exists — it holds the DAC
// at mid-scale. An index past High is clamped rather than rejected.
void emu_set_volume(uint8_t idx);
uint8_t emu_get_volume();

// Microseconds of the last completed frame's audio work: apu_us covers the
// APU callback and the mix, wait_us is what the speaker's write spent blocked
// on a full DMA queue — which happens only when emulation is ahead of real
// time. Any pointer may be NULL, like emu_get_frame_times().
void emu_get_audio_times(uint32_t* apu_us, uint32_t* wait_us);

// Palette
// Alias kept so existing callers (the settings menu) compile unchanged; the
// count itself belongs to the gbcore palette module.
#define NUM_PALETTES PALETTE_COUNT
void emu_set_palette(uint8_t idx);
uint8_t emu_get_palette();
const char* emu_get_palette_name(uint8_t idx);
