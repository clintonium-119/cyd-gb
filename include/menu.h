#pragma once
#include <stdint.h>
#include <stdbool.h>

#include "settings.h"
#include "cart/ndef.h"

// ─── Cartridge snapshot ─────────────────────────────────────────────────────
// Everything the Cart Info page shows, gathered once during boot and handed
// to the menu by value. The menu never asks the cartridge anything itself:
// there is one read per power cycle and this is what it left behind.
//
// `valid` false means no cartridge was read at all — the bench build — and
// the page says so rather than showing empty fields. `uid_hex` is up to seven
// bytes as fourteen hex digits; it lives in RAM for display only and is never
// persisted anywhere. `cls` and `auth` are the boot table's own enums, not a
// re-derivation, and `auth0` is the raw configuration byte behind `auth`.
typedef struct menu_cart_info_s {
    bool valid;
    char uid_hex[15];
    char payload[NDEF_TEXT_MAX + 1];
    enum boot_class_e cls;
    uint8_t auth0;
    enum boot_auth_e auth;
    char path[80];
    char title[17];
    uint8_t colour_hash;
} menu_cart_info_t;

// ─── In-game menu ───────────────────────────────────────────────────────────
// What the caller has to do next. There is no third answer: the menu is a
// pause screen with a restart on it, and it leads nowhere else.
enum menu_result_e {
    MENU_RESUME = 0,
    MENU_RESET,
};

// Six rows — Resume, Volume, Brightness, Palette, Cart Info, Reset — drawn
// inside the game window, driven by the D-pad, with A to act and B to go
// back.
//
// Calling contract: pause the pipeline and take the display bus first, and
// give them back afterwards; this draws through the driver directly for as
// long as it is open. It edits s->volume, s->brightness and s->palette in
// place, and applies backlight and palette as they change so the effect is
// visible while adjusting. It writes nothing to NVS and touches nothing else:
// persisting the struct and acting on the result are the caller's, and every
// screen it draws stays inside the game window.
enum menu_result_e menu_open(settings_t* s, const menu_cart_info_t* info);
