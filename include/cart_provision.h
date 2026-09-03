#pragma once

#include "cart/boot.h"

// Cartridge provisioning — the one and only place in the firmware that
// writes a tag.
//
// Every tag-write symbol in the gbcore tag layer is referenced from
// src/cart_provision.cpp and from nowhere else. That is not a style
// preference: it is the property a guard test pins, so that "can this
// firmware write a cartridge, and from where" has one auditable answer
// instead of a search. Anything that needs a tag written asks one of the
// verbs below.
//
// These verbs are executors, not deciders. The boot decision table in
// lib/gbcore/cart/boot.c has already worked out which action applies; each
// function here carries out exactly the action it is handed and neither
// re-examines the tag's class nor consults the setup flags to second-guess
// it. Nothing here draws on the display or holds a user-facing string —
// every function returns a result code and main.cpp owns the wording.
//
// The build-wide tag password lives in hw_config.h, which explains at length
// why it is not a secret.

// The authentication state of the tag currently selected, for the decision
// table's BOOT_NEED_AUTH re-entry. An unprotected tag is known from its
// configuration alone and costs no authentication attempt; otherwise the
// password decides between "ours" and "foreign".
//
// Fails closed: a transport error reports BOOT_AUTH_FOREIGN, logged, because
// every caller treats foreign as "do not write" and a bus glitch must never
// be the reason a stranger's tag gets overwritten.
enum boot_auth_e provision_auth_state();

// ─── Wizard steps ───────────────────────────────────────────────────────────
// Split into one function per action rather than one function taking both a
// boot action and a pick action, because in the combined form exactly one of
// the two enums would be meaningful per call and the type could not say
// which.
//
// Each write is verified before its flag is recorded, so a failed write
// leaves the wizard where it was and the step simply runs again on the next
// boot. Every one of these returns 0 on success, or a negative tag-layer
// error code.

// Write the menu cart, then record menu_done.
int provision_wizard_menu(boot_flags_t* flags);

// Record that an already-written cart of class `which` (BOOT_CLASS_MENU or
// BOOT_CLASS_WILD) has been adopted as this device's. No tag traffic: the
// tag already carries what it needs.
int provision_wizard_adopt(enum boot_class_e which, boot_flags_t* flags);

// Carry out the wizard's pick: BOOT_PICK_WRITE_WILD writes the wildcard and
// records wild_done, BOOT_PICK_WRITE_GAME writes an ordinary game cart and
// records nothing — which cart the wizard wrote is not persisted.
int provision_wizard_write(enum boot_pick_action_e pick_action,
                           const boot_selection_t* pick, boot_flags_t* flags);

// Record setup_done. No tag traffic.
int provision_wizard_finish(boot_flags_t* flags);

// ─── Pending writes ─────────────────────────────────────────────────────────

// Carry out the write the user lined up earlier against the tag now
// presented, whose class is `presented`. The pending record is cleared only
// once the tag has been written, verified and protected; a failure leaves it
// in place so the next boot simply tries again, which is what makes an
// interrupted write safe to repeat.
int provision_execute_pending(const boot_selection_t* pending,
                              enum boot_class_e presented);

// Apply protection to a tag that carries valid content but was found
// unprotected — one that lost power between its write and its protect. A
// configuration-only write: no content is touched.
int provision_heal();
