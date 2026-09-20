#include "settings.h"
#include "render_config.h"
#include "hw_config.h"
#include <Preferences.h>

static Preferences prefs;

// The ladder is BL_MIN + n*BL_STEP for n in 0..7, and the combo clamps at
// 255. These two constants only give 8 evenly spaced levels if the top rung
// lands exactly on the clamp; move one without the other and the count
// silently drifts or the last step goes stubby. Nothing else links them.
static_assert(BL_MIN + 7 * BL_STEP == 255,
              "backlight ladder must reach 255 in exactly 8 steps");
static_assert(BL_MIN > 0, "a backlight floor of 0 looks like a dead unit");

void settings_defaults(settings_t* s) {
    s->palette = 0;
    // 0 (bench, 2026-09-17): with the scaler on core 0 and the fixed 3/2
    // kernel, Black Castle at 80 MHz SPI plays at 60 fps with the audio
    // underrun counter flat — core 1 at 14.8 ms median, 15.6 ms worst, in a
    // 16.7 ms budget — so every frame is drawn and one-frame hit flashes show
    // again. The setting stays per unit for a heavier title that needs it.
    s->frameskip = 0;
    // 6th of the 8 backlight levels, not the top: a fresh unit that boots
    // at maximum is the one the bench found too bright. This is
    // BL_MIN + 5*BL_STEP -- derived, so it follows the ladder if those
    // constants move.
    s->brightness = BL_MIN + 5 * BL_STEP;
    // MED, not HIGH. Louder counts *down* toward SETTINGS_VOL_HIGH (0),
    // so medium is 1.
    s->volume = SETTINGS_VOL_HIGH + 1;
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

    // The nudge is stored in panel pixels, but GAME_W / GAME_H are compile
    // time: flip RENDER_GEOM and every nudge saved under the old geometry is
    // suddenly out of range. A 24/16 unit stores gy=12, which at 26/16 puts
    // the last 6 of 234 rows off the bottom of the panel -- and silently, as
    // the panel simply drops the rows the address window runs past. The
    // diagnostics page already clamps to exactly these bounds on entry and on
    // every step (diag.c:99-104, 121-141), so before this it would display a
    // corrected origin while the game drew from the stored one. Clamped here
    // so both read the same value and no stored nudge can push the window off
    // the panel, whatever geometry wrote it.
    if (s->game_x < 0) {
        s->game_x = 0;
    } else if (s->game_x > (int16_t)(SCREEN_W - GAME_W)) {
        s->game_x = (int16_t)(SCREEN_W - GAME_W);
    }
    if (s->game_y < 0) {
        s->game_y = 0;
    } else if (s->game_y > (int16_t)(SCREEN_H - GAME_H)) {
        s->game_y = (int16_t)(SCREEN_H - GAME_H);
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

// ─── Cartridge boot records ─────────────────────────────────────────────────
// Same "settings" namespace as the per-unit values above, deliberately: a
// factory reset clears the device with one nvs_flash_erase whatever the
// layout, and keeping one namespace leaves settings_load's isKey("pal") probe
// meaningful as the "has this device ever been configured" test.

bool settings_pending_load(boot_selection_t* out) {
    prefs.begin("settings", true);
    // Both halves or neither: a record with a ROM and no target, or the
    // reverse, is not a selection anything could carry out.
    bool has = prefs.isKey("p_rom") && prefs.isKey("p_tgt");
    if (has) {
        prefs.getString("p_rom", out->rom, ROM_STORE_NAME_MAX);
        out->rom[ROM_STORE_NAME_MAX - 1] = '\0';
        out->target = prefs.getUChar("p_tgt", BOOT_TARGET_WILDCARD);
    }
    prefs.end();

    // Clamped on the way in, the same stance as volume: a target past the
    // last enumerator would fall through boot_target_class's switch, so the
    // one place that knows the encoding is the only place that has to.
    if (has && out->target > BOOT_TARGET_REWRITE) {
        out->target = BOOT_TARGET_WILDCARD;
    }
    return has;
}

void settings_pending_save(const boot_selection_t* s) {
    prefs.begin("settings", false);
    prefs.putString("p_rom", s->rom);
    prefs.putUChar("p_tgt", s->target);
    prefs.end();
}

void settings_pending_clear() {
    prefs.begin("settings", false);
    prefs.remove("p_rom");
    prefs.remove("p_tgt");
    prefs.end();
}

bool settings_made_load(boot_made_t* out) {
    if (!out) {
        return false;
    }
    boot_made_clear(out);
    prefs.begin("settings", true);
    // A blob of the wrong length is a record from another build, not a
    // partial one: the caller gets the empty record and starts again.
    bool has = prefs.getBytesLength("made") == sizeof(boot_made_t);
    if (has) {
        prefs.getBytes("made", out, sizeof(boot_made_t));
    }
    prefs.end();

    // Clamped on the way in, the same stance as the pending target: a count
    // past the array's end could only come from a corrupt blob, and every
    // reader would otherwise have to defend against it.
    if (has && out->count > BOOT_MADE_MAX) {
        boot_made_clear(out);
    }
    return has;
}

void settings_made_save(const boot_made_t* m) {
    prefs.begin("settings", false);
    prefs.putBytes("made", m, sizeof(boot_made_t));
    prefs.end();
}

void settings_made_clear() {
    prefs.begin("settings", false);
    prefs.remove("made");
    prefs.end();
}

void settings_wizard_load(boot_flags_t* f) {
    prefs.begin("settings", true);
    f->menu_done = prefs.getBool("wz_menu", false);
    f->wild_done = prefs.getBool("wz_wild", false);
    f->setup_done = prefs.getBool("wz_done", false);
    prefs.end();
}

void settings_wizard_save(const boot_flags_t* f) {
    prefs.begin("settings", false);
    prefs.putBool("wz_menu", f->menu_done);
    prefs.putBool("wz_wild", f->wild_done);
    prefs.putBool("wz_done", f->setup_done);
    prefs.end();
}
