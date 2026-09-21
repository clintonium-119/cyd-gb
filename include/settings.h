#pragma once
#include <stdint.h>
#include <stdbool.h>

#include "cart/boot.h"

// Per-unit settings persisted in NVS. Ten hand-built units each need their own
// game_x / game_y nudge, so these are stored per device, never compiled in.
struct settings_t {
    uint8_t palette;
    uint8_t frameskip;
    uint8_t brightness;
    uint8_t volume;
    int16_t game_x;
    int16_t game_y;
    // The panel rate trim: PORCTRL's front porch in whole lines, and the
    // 64ths of a line the fractional divider dithers on top of it. Per unit
    // for the same reason the nudge is — it measures this panel's own
    // oscillator error against the emulator's audio-paced cadence, and that
    // number does not travel between boards.
    uint8_t trim_fpa;
    uint8_t trim_ratio;
};

// volume is an index, not a level: design §4's vol_lut is {high, med, low} and
// 3 means off: the mixer writes mid-scale and the DAC sits at 128, because no
// hardware mute exists. Louder therefore counts down towards
// SETTINGS_VOL_HIGH. The mixer reads the setting with this same meaning, so
// changing the encoding means changing both.
#define SETTINGS_VOL_HIGH 0
#define SETTINGS_VOL_OFF  3

void settings_defaults(settings_t* s);

// Reads NVS over *s, leaving any field the store does not carry at whatever
// value it already held. Returns false when nothing was stored. A volume index
// past SETTINGS_VOL_OFF is clamped here rather than at the point of use, so no
// consumer has to defend against an out-of-range index.
bool settings_load(settings_t* s);

void settings_save(const settings_t* s);

// Coalesced save. A held brightness or volume combo emits an event every
// 200 ms, and each one would otherwise be an NVS write; instead the adjusted
// struct is parked here and written once the adjusting stops.
//
// Call settings_save_coalesced() whenever a value changes, then
// settings_flush(now_ms, false) once per frame. force = true writes a pending
// save immediately, which is what the menu path needs: the settings menu
// writes NVS itself, and a save still parked here would land on top of it.
void settings_save_coalesced(const settings_t* s, uint32_t now_ms);
void settings_flush(uint32_t now_ms, bool force);

// ─── Cartridge boot records ─────────────────────────────────────────────────
// The two records the cartridge boot flow needs to survive a power cycle,
// stored as exactly the gbcore types the decision table consumes so a
// recorded selection needs no translation on the way back out.
//
// The pending record is the single write the user lined up in the writer and
// has not carried out yet: which ROM, and what kind of cart it is aimed at.
// There is one record, not a queue — a later selection overwrites it — and it
// carries no timestamp on purpose, because millis() does not survive the
// power cycle the record exists to span, and an age would only invite a
// staleness rule the flow does not want.
//
// The three wizard flags record how far first-boot setup got: the menu cart
// written or adopted, the wildcard written or adopted, and setup finished.
// Nothing else about the wizard is kept — no UID, and no list of which game
// carts it wrote.
//
// A full erase_flash drops a pending write silently, with no trace on the
// next boot. That is an assembly-day note rather than a firmware concern, and
// the flashing-station docs carry the warning.

// False when no record is stored, in which case *out is left untouched.
bool settings_pending_load(boot_selection_t* out);

void settings_pending_save(const boot_selection_t* s);

void settings_pending_clear();

// ─── Setup-progress record ──────────────────────────────────────────────────
// Which carts the first-boot wizard has written this setup, so the picker can
// mark the rows that are already done. One blob under the key "made", in the
// same "settings" namespace as everything else here, holding exactly the
// gbcore boot_made_t the picker consumes.
//
// The mark is a convenience, nothing more: it carries no routing, protection
// or ordering meaning, and Finish setup clears it. A stored blob whose length
// is not sizeof(boot_made_t) is treated as absent rather than as a partial
// record — a shorter blob would be a record from a different build, and a
// filename list is not worth a migration.

// False when nothing is stored or the stored blob is the wrong length, in
// which case *out is left as the empty record rather than untouched: every
// caller wants a usable record either way.
bool settings_made_load(boot_made_t* out);

void settings_made_save(const boot_made_t* m);

void settings_made_clear();

// Every flag reads false when nothing is stored, so an unset store and a
// wizard that has not started are the same state.
void settings_wizard_load(boot_flags_t* f);

void settings_wizard_save(const boot_flags_t* f);
