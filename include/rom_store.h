#pragma once
#include <stdint.h>
#include <stdbool.h>
#include <FS.h>  // fs::File — qualified below, see the note on rom_store_write()

#include "cart/rom_store.h"

// ROM store, device side: binds the host-tested store logic to the raw
// `romdata` flash partition.
//
// The ROM is written to flash exactly once — the first time a given file
// name and size is seen — and read for the rest of the session through the
// memory map, so ROM access costs a pointer dereference through the flash
// cache instead of an SD read.
//
// Ordering is a hard constraint, not a preference: a flash write stalls the
// other core's instruction fetch, so rom_store_write() must complete before
// any emulation task exists. There is deliberately no erase, format, or
// second write entry point — the only thing that can reach this partition is
// a ROM streamed from the SD card during boot.

// Find the romdata partition. Safe to call more than once.
bool rom_store_init();

// Stream `f` into the partition under `filename`, unless the stored header
// already describes exactly that name and size. The parameter is spelled
// fs::File rather than File on purpose: TFT_eSPI defines FS_NO_GLOBALS, so
// the unqualified alias does not exist in a translation unit that reaches
// display.h before <SD.h>. Nothing is erased or written on the unchanged
// path. True when the partition holds this ROM on return.
bool rom_store_write(fs::File& f, const char* filename);

// Map the stored ROM and return a pointer to its first byte, or NULL. The
// map lives for the rest of the session; there is no unmap, because there is
// no path that stops using the ROM short of a power cycle. Repeat calls
// return the same pointer.
const uint8_t* rom_store_mmap(uint32_t* out_len);

// Peek at the stored header without mapping anything. False when nothing
// valid is stored.
bool rom_store_stored(rom_store_hdr_t* out);
