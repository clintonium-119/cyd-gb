#pragma once

#include "cart/boot.h"
#include "cart/catalog.h"

// The cartridge writer's interface — the picker the boot state machine opens
// when the user is allowed to choose what goes on a tag.
//
// This header is the contract the full writer inherits; the body shipping
// today is a stub. Five rules define it, and none of them are incidental:
//
//   1. One call site. The boot state machine's executor opens the writer and
//      nothing else does — a guard test pins the count at exactly one call
//      plus this declaration, so "when can this device write a cart" has a
//      single answer.
//   2. It returns a pick. It never writes a tag and never launches a game;
//      it reports what the user chose and the decision table works out what
//      that means. A picker that wrote would put a second write site in the
//      firmware, which is the thing the whole arrangement prevents.
//   3. It reaches for nothing below itself. No tag, reader, emulator or ROM
//      storage symbol appears in the writer's sources — also guard-tested,
//      by substring, so the rule holds in comments too.
//   4. When it renders, it renders inside the game window, like every other
//      screen on this device.
//   5. Every exit is a halt or a power-off prompt. Nothing runs after the
//      writer in the same boot, so its buffers may be static and generous.
//
// `pending_set` drives whether a Cancel entry is offered; `flags` drives the
// starter filter and whether Finish setup is available.

enum writer_mode_e {
    // Reached by booting with a menu cart: the pick is recorded and carried
    // out on a later boot, against whichever tag is presented then.
    WRITER_MODE_PENDING,
    // The first-boot wizard: the tag to write is the one in the field now.
    WRITER_MODE_IMMEDIATE,
};

// Open the picker and return what the user chose. On BOOT_PICK_ROM, *out
// carries the selection; `target` defaults to BOOT_TARGET_WILDCARD, as the
// New cart and Rewrite entries that would set anything else are menu entries
// the full writer adds. *out is untouched for every other result.
enum boot_pick_e writer_open(enum writer_mode_e mode,
                             const catalog_reader_t* cat,
                             const boot_flags_t* flags, bool pending_set,
                             boot_selection_t* out);
