#include "settings.h"
#include "render_config.h"
#include <Preferences.h>

static Preferences prefs;

void settings_defaults(settings_t* s) {
    s->palette = 0;
    // 0, not the fork's 2: the mapped ROM removed the cache-miss hitches and
    // the DMA push overlaps the transfer, so a skipped frame is no longer the
    // normal case. The setting stays for bench sweeps and the diagnostic
    // screen.
    s->frameskip = 0;
    s->brightness = 255;
    s->volume = 0;
    s->game_x = GAME_X;
    s->game_y = GAME_Y;
}

bool settings_load(settings_t* s) {
    prefs.begin("settings", true);
    bool has = prefs.isKey("pal");
    if (has) {
        s->palette = prefs.getUChar("pal", s->palette);
        s->frameskip = prefs.getUChar("fskip", s->frameskip);
        s->brightness = prefs.getUChar("bright", s->brightness);
        s->volume = prefs.getUChar("vol", s->volume);
        s->game_x = prefs.getShort("gx", s->game_x);
        s->game_y = prefs.getShort("gy", s->game_y);
    }
    prefs.end();
    return has;
}

void settings_save(const settings_t* s) {
    prefs.begin("settings", false);
    prefs.putUChar("pal", s->palette);
    prefs.putUChar("fskip", s->frameskip);
    prefs.putUChar("bright", s->brightness);
    prefs.putUChar("vol", s->volume);
    prefs.putShort("gx", s->game_x);
    prefs.putShort("gy", s->game_y);
    prefs.end();
}
