#pragma once
#include <stdint.h>
#include <stdbool.h>

#include "settings.h"

// ─── Cartridge snapshot ─────────────────────────────────────────────────────
// What the menu needs to know about the running game, gathered once at boot
// and handed to it by value: the ROM path, whose file name keys the catalog
// entry, the cover, the snapshot and the manual; and the cartridge header's
// title, shown when the catalog has no entry. Nothing about the tag — the
// menu is for the player, and the tag means nothing to them.
typedef struct menu_cart_info_s {
    char path[80];
    char title[17];
} menu_cart_info_t;

// ─── In-game menu ───────────────────────────────────────────────────────────
// What the caller has to do next. There is no third answer: the menu is a
// pause screen with a restart on it, and it leads nowhere else.
enum menu_result_e {
    MENU_RESUME = 0,
    MENU_RESET,
};

// Nine rows — Resume, Save State, Game Manual, Game Details, Color Palette,
// Volume, Brightness, Hotkeys, Reset — drawn inside the game window seven at
// a time and scrolled, driven by the D-pad, with A to act and B to go back.
// Hotkeys opens a view-only page listing the fixed button combos. Game Manual
// reads "Game Manual (Unavailable)", dimmed, and does nothing when the
// running cartridge has no manual on the card; Save State does the same on a
// core without save states.
//
// Save State opens the game's one state: its snapshot, and Save, Load and
// Back. Load always asks first, and so does a Save that would replace a
// state. A load that succeeds returns MENU_RESUME at once, with the machine
// already at the loaded moment and its cartridge RAM marked dirty.
//
// Calling contract: pause the pipeline and take the display bus first, and
// give them back afterwards; this draws through the display's canvas for as
// long as it is open. It edits s->volume, s->brightness and s->palette in
// place, and applies backlight and palette as they change so the effect is
// visible while adjusting. It writes nothing to NVS and touches nothing else:
// persisting the struct and acting on the result are the caller's, and every
// screen it draws stays inside the game window.
enum menu_result_e menu_open(settings_t* s, const menu_cart_info_t* info);
