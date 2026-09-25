#pragma once

#include "cart/boot.h"
#include "cart/catalog.h"
#include "ui/picker.h"

// The picker screen: the list of games, each game's page, and the 1 s hold
// that picks one. The cart writer opens it in pending or immediate mode, and
// the boot executor's games list opens it in launch mode. Three rules:
//
//   1. It returns a pick. It never writes a tag and never loads a game; the
//      caller's decision table works out what the pick means.
//   2. It reaches for nothing below itself. No tag, reader, emulator, ROM
//      storage or provisioner symbol appears in its sources — guard-tested by
//      substring, so the rule holds in comments too.
//   3. Nothing runs after it in the same boot. Its big buffers are allocated
//      on entry and freed on exit, not static: static storage is paid in
//      every boot, including every boot that plays a game.
//
// `flags` drives the wizard's starter filter and Finish setup, and
// `pending_set` whether pending mode offers Cancel; launch mode ignores both.
// On BOOT_PICK_ROM, *out carries the selection. BOOT_PICK_NONE, with *out
// untouched, when there is no catalog, no heap, or no row to show.
enum boot_pick_e picker_screen_run(enum picker_mode_e mode,
                                   const catalog_reader_t* cat,
                                   const boot_flags_t* flags, bool pending_set,
                                   boot_selection_t* out);
