#pragma once

#include "cart/boot.h"
#include "cart/catalog.h"

// The cartridge writer's interface — the picker the boot state machine opens
// when the user is allowed to choose what goes on a tag.
//
// The body is src/cart_writer.cpp, a mode mapping over the shared picker
// screen (picker_screen.h), itself a thin binding over the pure picker and
// layout modules in lib/gbcore/ui/. Five rules define it, and none of them are
// incidental:
//
//   1. One call site. The boot state machine's executor opens the writer and
//      nothing else does — a guard test pins the count at exactly one call
//      plus this declaration, so "when can this device write a cart" has a
//      single answer. There is no button combo and no settings entry: the
//      cart is the key.
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
//      writer in the same boot, so its buffers may be generous. The big ones
//      belong to the shared picker screen (picker_screen.h), which allocates
//      them on entry and frees them on exit, not static: static storage is
//      paid in every boot, including every boot that plays a game.
//
// The games list is a separate caller of the same shared picker screen, in a
// launch mode that only reports a pick; it is not a second writer.
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
// carries the selection, and `target` is always BOOT_TARGET_WILDCARD: the
// writer's one job from a menu cart is choosing the wildcard's next game, and
// writing blank carts belongs to the first-boot wizard. *out is untouched for
// every other result.
enum boot_pick_e writer_open(enum writer_mode_e mode,
                             const catalog_reader_t* cat,
                             const boot_flags_t* flags, bool pending_set,
                             boot_selection_t* out);
