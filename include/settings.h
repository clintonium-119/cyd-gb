#pragma once
#include <stdint.h>
#include <stdbool.h>

// Per-unit settings persisted in NVS. Ten hand-built units each need their own
// game_x / game_y nudge, so these are stored per device, never compiled in.
struct settings_t {
    uint8_t palette;
    uint8_t frameskip;
    uint8_t brightness;
    uint8_t volume;
    int16_t game_x;
    int16_t game_y;
};

// volume is an index, not a level: design §4's vol_lut is {high, med, low} and
// 3 means off via the amplifier's hardware mute. Louder therefore counts down
// towards SETTINGS_VOL_HIGH. The audio workstream reads the setting with this
// same meaning, so changing the encoding means changing both.
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
