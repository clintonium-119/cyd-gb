#include "cart_writer.h"
#include <string.h>

// The writer stub.
//
// The full writer — the title list, the cover art, the confirmation screen
// and the New cart / Rewrite / Cancel entries — is a later workstream. It
// replaces this body and keeps the interface in cart_writer.h exactly as it
// stands, so the boot state machine needs no change when it lands.
//
// What the stub is for: it makes the wizard's exit reachable today. Booting a
// fresh device with a blank tag writes the menu cart, then the wildcard, then
// finishes setup — the whole first-boot path runs end to end without a UI.
// The starter it picks is read from the catalog rather than written in here,
// so the stub follows the real library instead of drifting from it.
//
// It draws nothing, polls nothing, and holds no cursor.

enum boot_pick_e writer_open(enum writer_mode_e mode,
                             const catalog_reader_t* cat,
                             const boot_flags_t* flags, bool pending_set,
                             boot_selection_t* out) {
    (void)pending_set;  // the Cancel entry it drives is the full writer's

    if (mode != WRITER_MODE_IMMEDIATE) {
        // Pending mode has no picker to show yet, so the caller halts on the
        // menu cart it booted from.
        return BOOT_PICK_NONE;
    }
    if (!cat || !flags || !out) {
        return BOOT_PICK_NONE;
    }
    if (flags->wild_done) {
        // The wildcard is written; there is nothing else the stub offers, so
        // the wizard's remaining step is to finish.
        return BOOT_PICK_FINISH;
    }

    catalog_entry_t e;
    if (catalog_first_flagged(cat, CATALOG_FLAG_STARTER, &e) != CATALOG_OK) {
        return BOOT_PICK_NONE;
    }
    strncpy(out->rom, e.filename, sizeof(out->rom) - 1);
    out->rom[sizeof(out->rom) - 1] = '\0';
    out->target = BOOT_TARGET_WILDCARD;
    return BOOT_PICK_ROM;
}
