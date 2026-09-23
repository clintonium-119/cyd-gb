#include "settings.h"
#include "render_config.h"
#include "render/palette.h"
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
    // Auto: the colours the Game Boy Color's own table gives this cartridge,
    // and the muted DMG green for a cartridge it does not know. A palette is per
    // game, so this is the value every game starts at until the builder picks
    // one for it in the menu.
    s->palette = PALETTE_AUTO;
    // 0 (bench, 2026-09-17): with the scaler on core 0 and the fixed 3/2
    // kernel, Black Castle at 80 MHz SPI plays at 60 fps with the audio
    // underrun counter flat — core 1 at 14.8 ms median, 15.6 ms worst, in a
    // 16.7 ms budget — so every frame is drawn and one-frame hit flashes show
    // again. The setting stays per unit for a heavier title that needs it.
    s->frameskip = 0;
    // 4th of the 8 backlight levels, 4/8 in the menu: a fresh unit that
    // boots at maximum is the one the bench found too bright, and 6/8 was
    // still brighter than wanted. This is BL_MIN + 3*BL_STEP -- derived, so
    // it follows the ladder if those constants move.
    s->brightness = BL_MIN + 3 * BL_STEP;
    // 5 of 8, not the top: a gain of 64/256, about 12 dB below full scale.
    s->volume = 5;
    s->game_x = GAME_X;
    s->game_y = GAME_Y;
    // The batch-typical null, not the panel's power-on porch. An
    // uncalibrated unit is then mediocre rather than bad; a calibrated one
    // overwrites both from the store.
    s->trim_fpa = PANEL_TRIM_FPA;
    s->trim_ratio = PANEL_TRIM_RATIO;
    // Shorten. A unit with no calibration behind it has no measured
    // direction either, so this is the trim page's own opening guess, held
    // here so the two cannot drift apart.
    s->trim_dir = +1;
}

bool settings_load(settings_t* s) {
    prefs.begin("settings", true);
    // "bright", not "pal": the palette moved out of this record when it became
    // per cartridge, and this probe still has to mean "has this device ever
    // been configured". Every key settings_save() writes would answer that
    // equally well, including on a unit configured before the move.
    bool has = prefs.isKey("bright");
    if (has) {
        s->frameskip = prefs.getUChar("fskip", s->frameskip);
        s->brightness = prefs.getUChar("bright", s->brightness);
        // "vol8", not "vol": the old key held a High/Med/Low/Off index that
        // counted down towards louder, and read as a level it would come out
        // backwards. Nothing reads "vol" any more.
        s->volume = prefs.getUChar("vol8", s->volume);
        s->game_x = prefs.getShort("gx", s->game_x);
        s->game_y = prefs.getShort("gy", s->game_y);
        s->trim_fpa = prefs.getUChar("tfpa", s->trim_fpa);
        s->trim_ratio = prefs.getUChar("trat", s->trim_ratio);
        s->trim_dir = prefs.getChar("tdir", s->trim_dir);
    }
    prefs.end();

    // A stored volume past the top level would index past the end of the
    // audio path's lookup table. Clamped on the way in, so that table stays
    // the only place the encoding is known and every reader is safe.
    if (s->volume > SETTINGS_VOL_MAX) {
        s->volume = SETTINGS_VOL_MAX;
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
    // A porch of 0 would be a frame the panel cannot scan and a ratio past
    // 63 would carry into the whole-line count on the wrong frame, so both
    // are clamped on the way in — the same stance as volume and the nudge,
    // and for the same reason: one place knows the encoding.
    if (s->trim_fpa == 0) {
        s->trim_fpa = 1;
    } else if (s->trim_fpa > 126) {
        s->trim_fpa = 126;
    }
    if (s->trim_ratio > 63) {
        s->trim_ratio = 63;
    }
    // The only two values the loop can act on. A store written by an older
    // firmware carries no key at all and lands on the default; anything else
    // is a corrupt read, and shortening is what an uncalibrated unit does.
    if (s->trim_dir >= 0) {
        s->trim_dir = +1;
    } else {
        s->trim_dir = -1;
    }
    return has;
}

void settings_save(const settings_t* s) {
    prefs.begin("settings", false);
    prefs.putUChar("fskip", s->frameskip);
    prefs.putUChar("bright", s->brightness);
    prefs.putUChar("vol8", s->volume);
    prefs.putShort("gx", s->game_x);
    prefs.putShort("gy", s->game_y);
    prefs.putUChar("tfpa", s->trim_fpa);
    prefs.putUChar("trat", s->trim_ratio);
    prefs.putChar("tdir", s->trim_dir);
    prefs.end();
}

// ─── Per-cartridge palette ──────────────────────────────────────────────────
// FNV-1a over the printable title. Thirty-two bits because eight would not do:
// the Game Boy Color's own table needs a fourth-letter tiebreak to tell
// ninety-four games apart by an eight-bit title sum, and a library of a
// hundred-odd ROMs would collide the same way. Eight hex digits plus the
// prefix is nine characters, inside the fifteen an NVS key allows.
//
// The prefix is "p", not the original "g": the palette table changed shape
// from 20 entries to 14, and an index stored against the old one would name a
// different palette in the new one. Moving the key puts every cartridge with
// an old override back on Auto. The old "g" keys are left where they are;
// nothing reads them.
static void game_key(const char* title, char* out, size_t out_sz) {
    uint32_t h = 2166136261u;

    for (; title && *title; title++) {
        h = (h ^ (uint8_t)*title) * 16777619u;
    }
    snprintf(out, out_sz, "p%08lX", (unsigned long)h);
}

bool settings_game_palette_load(const char* title, uint8_t* out) {
    char key[16];

    if (!out) {
        return false;
    }
    game_key(title, key, sizeof(key));
    prefs.begin("settings", true);
    bool has = prefs.isKey(key);
    uint8_t pal = has ? prefs.getUChar(key, PALETTE_AUTO) : PALETTE_AUTO;
    prefs.end();

    // A stored index the table cannot name is a record from a build with a
    // different palette list, so it is absent rather than clamped.
    if (!has || pal >= PALETTE_COUNT) {
        return false;
    }
    *out = pal;
    return true;
}

void settings_game_palette_save(const char* title, uint8_t palette) {
    char key[16];

    game_key(title, key, sizeof(key));
    prefs.begin("settings", false);
    if (palette < PALETTE_COUNT) {
        prefs.putUChar(key, palette);
    } else {
        // Auto, or anything out of range: no override, which is the state a
        // unit that has never been told otherwise is already in.
        prefs.remove(key);
    }
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
// layout, and keeping one namespace leaves settings_load's isKey("bright") probe
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
