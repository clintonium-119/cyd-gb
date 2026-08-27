#pragma once
#include <stdint.h>

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

void settings_defaults(settings_t* s);
bool settings_load(settings_t* s);
void settings_save(const settings_t* s);
