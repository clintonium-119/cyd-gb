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

    // A stored volume past the off step would index past the end of the
    // audio path's lookup table. Clamped on the way in, so that table stays
    // the only place the encoding is known and every reader is safe.
    if (s->volume > SETTINGS_VOL_OFF) {
        s->volume = SETTINGS_VOL_OFF;
    }
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

// ─── Coalesced save ─────────────────────────────────────────────────────────
// Long enough to outlast a repeat train at one event per 200 ms, so a sweep
// from one end of the brightness range to the other is a single write; short
// enough that a power yank shortly after letting go still keeps the change.
#define SETTINGS_SAVE_DELAY_MS 1000

static settings_t pending;
static uint32_t pending_due_ms = 0;
static bool pending_valid = false;

void settings_save_coalesced(const settings_t* s, uint32_t now_ms) {
    pending = *s;
    pending_due_ms = now_ms + SETTINGS_SAVE_DELAY_MS;
    pending_valid = true;
}

void settings_flush(uint32_t now_ms, bool force) {
    if (!pending_valid) {
        return;
    }
    // Signed difference so the comparison survives the millis() rollover
    // rather than parking a save for another 49 days.
    if (!force && (int32_t)(now_ms - pending_due_ms) < 0) {
        return;
    }
    pending_valid = false;
    settings_save(&pending);
}
