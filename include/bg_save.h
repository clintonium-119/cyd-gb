#pragma once
#include <stdint.h>
#include <stdbool.h>

// Background save of cartridge RAM (BUG-0025).
//
// The slide switch removes power with no warning, so a save the player makes
// in-game has to reach the card on its own, during play, without the game or
// its audio pausing. This module keeps a copy of cartridge RAM and a task on
// core 0 that writes the copy to the card; the emulation task takes the copy
// between frames and carries on. It decides nothing about *when* — that is
// the autosave rule — and keeps no dirty state of its own.
//
// The copy lives in 32-bit-only instruction RAM when it can: during play the
// byte-addressable heap is too small for a 32 KB save (Pokemon leaves ~27 KB
// free, 10 KB in its largest block), so the copy is pieces of up to 8 KB, and
// every access to them is a whole word.
//
// Ownership: bg_save_take() and bg_save_start() run on the emulation task
// only, and only while bg_save_idle(); the writer task touches nothing but
// the copy, the path it was handed, and the result it publishes.

enum bg_save_result_e {
    BG_SAVE_NONE = 0,   // nothing finished since the last bg_save_result()
    BG_SAVE_OK,
    BG_SAVE_FAILED,
};

// Reserve the copy for a cartridge whose save is `size` bytes, and start the
// writer task the first time. False when the copy cannot be had or the size
// is not a whole number of words — the caller then saves the old way, from
// the menu, reset and games list only. Never call it twice.
bool bg_save_init(uint32_t size);

// Whether bg_save_init() succeeded.
bool bg_save_ready();

// No write in flight.
bool bg_save_idle();

// Copy `ram` (bg_save_init()'s size, word-aligned) into the copy. Between
// frames, on the emulation task, while idle.
void bg_save_take(const uint8_t* ram);

// Hand the copy to the writer for the save beside `rom_path`.
void bg_save_start(const char* rom_path);

// Block until the writer is idle.
void bg_save_wait();

// The outcome of the last finished write, once; BG_SAVE_NONE after that.
uint8_t bg_save_result();
