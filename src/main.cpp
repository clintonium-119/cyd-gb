#include <Arduino.h>
#include "hw_config.h"
#include "render_config.h"
#include "display.h"
#include "scale_bench.h"
#include "button_input.h"
#include "battery.h"
#include "i2c_bus.h"
#include "input/combo.h"
#include "sd_manager.h"
#include "menu.h"
#include "emulator_bridge.h"
#include "rom_store.h"
#include "settings.h"
#include "speaker.h"
#include "audio/mix.h"
#include "nfc_cart.h"
#include "cart_provision.h"
#include "cart_writer.h"
#include "diag.h"
#include "cart/boot.h"
#include "cart/catalog.h"
#include "cart/ndef.h"
#include "cart/ntag.h"
#include "ui/theme_draw.h"
#include <SD.h>

// The stored volume index IS the mixer's index; the two lists are declared in
// different modules and nothing links them but this.
static_assert(SETTINGS_VOL_OFF == MIX_VOL_OFF && SETTINGS_VOL_LOW == MIX_VOL_LOW &&
                  SETTINGS_VOL_MED == MIX_VOL_MED && SETTINGS_VOL_HIGH == MIX_VOL_HIGH,
              "volume index encodings must agree");

static char cur_path[80] = {0};
static bool menu_req = false;
static settings_t settings;
static combo_state_t combo;

// Everything the decision table needs, gathered once at boot and never
// re-derived. main.cpp gathers inputs and executes actions; which action
// applies is decided in lib/gbcore/cart/boot.c and nowhere else.
static boot_input_t in;
static catalog_reader_t cat;
static bool cat_ok = false;

// The string the tag actually carried, kept for the Not found and Unreadable
// screens: what the person holding the cart can compare against is what was
// read, not a normalised form of it.
static char tag_payload[NDEF_TEXT_MAX + 1];

// Reads only. Every tag write in this firmware goes through cart_provision.
static const ntag_dev_t tag_dev = { NULL, nfc_transceive };

// Everything the Cart Info page shows, gathered once as the boot flow learns
// it and never re-derived: nothing here re-reads the cartridge or re-decides
// what it is. The UID lives in this struct for display and nowhere else — it
// is not persisted, and nothing outside the menu reads it.
static menu_cart_info_t cart_info;

// ─── Boot screens ───────────────────────────────────────────────────────────
// All boot drawing lands inside the game window. The printed bezel masks
// everything outside GAME_X/GAME_Y x GAME_W/GAME_H, so a screen centred on
// the 320x240 panel is partly hidden behind plastic on every unit.

// A 63-character file name at font 2 runs to roughly 500-750 px depending on
// which glyphs it uses, against a 240-px window, so a detail line wraps
// instead of running under the bezel. Four rows covers the longest name the
// store accepts even in all-wide glyphs, and still ends above the window's
// bottom edge — which is what the arguments to the shared helper say below.

// l1 is the condition, l2 the detail: the theme's notice, white on black
// with a grey detail, and red only when it is an error. l1 drops from font 4
// to the wrapped small font when the large one would not fit the window.
static void notice(const char* l1, const char* l2, bool is_error) {
    display_clear(UI_COL_BG);
    ui_notice(display_canvas(settings.game_x, settings.game_y), GAME_W, GAME_H,
              l1, l2, is_error, NULL, 0);
}

// Halt means halt: no retry loop and no fallback browser. The DMG's
// mechanical interlock already forces a power-off to change carts, so the
// power cycle is the retry.
static void halt(const char* l1, const char* l2, bool is_error) {
    Serial.printf("[BOOT] halt: %s %s\n", l1, l2 ? l2 : "");
    notice(l1, l2, is_error);
    for (;;) {
        delay(1000);
    }
}

// Something went wrong: the card, the tag, the file or a write.
static void halt_screen(const char* l1, const char* l2) {
    halt(l1, l2, true);
}

// Nothing went wrong: a write that worked, or what to insert next.
static void halt_notice(const char* l1, const char* l2) {
    halt(l1, l2, false);
}

// ─── Tag read ───────────────────────────────────────────────────────────────

// Fills only the tag-derived fields, deliberately: the setup flags and the
// pending record are loaded separately and the post-display retry calls this
// again, so clearing the whole struct here would discard them.
static void read_tag(boot_input_t* b) {
    b->tag = BOOT_TAG_NONE;
    b->cls = BOOT_CLASS_BLANK;
    b->rom[0] = '\0';
    b->auth = BOOT_AUTH_UNKNOWN;
    tag_payload[0] = '\0';

    uint8_t uid[7] = {0};
    uint8_t uid_len = 0;
    switch (nfc_detect(uid, &uid_len)) {
        case NFC_DETECT_NONE:
            return;
        case NFC_DETECT_MULTI:
            // Two targets means the shielding is still in the DMG, or a
            // second tag is in the field. Never pick one of them.
            b->tag = BOOT_TAG_MULTI;
            return;
        case NFC_DETECT_ERR:
            b->tag = BOOT_TAG_UNREADABLE;
            return;
        case NFC_DETECT_ONE:
            break;
    }

    uint8_t raw[NDEF_BUF_MAX];
    if (ntag_read_pages(&tag_dev, NTAG215_PAGE_USER_FIRST,
                        NDEF_BUF_MAX / NTAG_PAGE_SIZE, raw) != NTAG_OK) {
        b->tag = BOOT_TAG_UNREADABLE;
        return;
    }

    int rc = ndef_parse_text(raw, sizeof(raw), tag_payload, sizeof(tag_payload));
    if (rc == NDEF_BLANK) {
        b->cls = BOOT_CLASS_BLANK;
    } else if (rc == NDEF_OK) {
        if (boot_classify(tag_payload, &b->cls, b->rom, sizeof(b->rom))
            != BOOT_CLASSIFY_OK) {
            b->tag = BOOT_TAG_UNREADABLE;
            return;
        }
    } else {
        b->tag = BOOT_TAG_UNREADABLE;
        return;
    }

    // The configuration read doubles as the part check: a foreign
    // read-protected tag, or something that is not an NTAG215, refuses it,
    // and that shows the unreadable screen rather than a guess.
    uint8_t auth0 = 0;
    if (ntag_read_auth0(&tag_dev, &auth0) != NTAG_OK) {
        b->tag = BOOT_TAG_UNREADABLE;
        return;
    }
    b->auth = (auth0 == NTAG215_AUTH0_OPEN) ? BOOT_AUTH_OPEN : BOOT_AUTH_UNKNOWN;
    b->tag = BOOT_TAG_OK;
    Serial.printf("[BOOT] tag cls=%d rom='%s' auth0=0x%02X\n", (int)b->cls,
                  b->rom, auth0);
}

// Long enough to read, short enough not to feel like a delay in the boot.
#define PENDING_BANNER_MS 1500

static void show_pending_banner() {
    char title[CATALOG_TITLE_MAX];
    catalog_entry_t e;

    // The catalog's title when it has one, the file name otherwise: a
    // missing catalog means "no title", never a failure.
    const char* src = in.pending.rom;
    if (cat_ok && catalog_find(&cat, in.pending.rom, &e) == CATALOG_OK) {
        src = e.title;
    }
    strncpy(title, src, sizeof(title) - 1);
    title[sizeof(title) - 1] = '\0';

    char l1[CATALOG_TITLE_MAX + 16];
    snprintf(l1, sizeof(l1), "Pending: %s", title);

    const char* l2 = "insert your wildcard";
    if (in.pending.target == BOOT_TARGET_NEW_CART) {
        l2 = "insert a blank cart";
    } else if (in.pending.target == BOOT_TARGET_REWRITE) {
        l2 = "insert the cart to rewrite";
    }

    notice(l1, l2, false);
    delay(PENDING_BANNER_MS);
}

/*
 * One expander read per frame, fed to the combo state machine, which hands
 * back the word the emulator sees and at most one combo event. This replaces
 * the fork's 12 ms input task: at 60 fps the poll is more frequent than that
 * task was, it costs one I2C byte (~60 us) on the core that is already
 * running the emulation, and it removes a second writer of the joypad word.
 *
 * Every side effect of an event lives here rather than in the state machine,
 * which is what keeps that machine host-testable.
 */
static void poll_input(uint32_t now_ms) {
    uint8_t event = COMBO_EVENT_NONE;
    uint8_t volume = settings.volume;
    uint8_t brightness = settings.brightness;

    button_update();
    combo_update(&combo, button_get_buttons(), now_ms, &event);
    emu_set_joypad(combo_joypad(&combo));

    switch (event) {
        case COMBO_EVENT_MENU:
            menu_req = true;
            emu_state_thumb_arm();
            return;
        case COMBO_EVENT_FAST_FORWARD:
            // Runtime only: never stored, so every boot starts at 1x.
            emu_set_fast_forward(!emu_get_fast_forward());
            return;
        case COMBO_EVENT_VOL_UP:
            volume = combo_step_u8(volume, +1, SETTINGS_VOL_OFF, SETTINGS_VOL_HIGH, 1);
            break;
        case COMBO_EVENT_VOL_DOWN:
            volume = combo_step_u8(volume, -1, SETTINGS_VOL_OFF, SETTINGS_VOL_HIGH, 1);
            break;
        case COMBO_EVENT_BRIGHT_UP:
            brightness = combo_step_u8(brightness, +1, BL_MIN, 255, BL_STEP);
            break;
        case COMBO_EVENT_BRIGHT_DOWN:
            brightness = combo_step_u8(brightness, -1, BL_MIN, 255, BL_STEP);
            break;
        default:
            return;
    }

    // Nothing moved means the combo is being held against an end of its
    // range, so there is nothing to apply and nothing to write.
    if (volume == settings.volume && brightness == settings.brightness) {
        return;
    }

    if (volume != settings.volume) {
        settings.volume = volume;
        emu_set_volume(settings.volume);
    }
    if (brightness != settings.brightness) {
        settings.brightness = brightness;
        display_set_backlight(settings.brightness);
    }
    settings_save_coalesced(&settings, now_ms);
}

// ─── Automatic saves ───────────────────────────────────────────────────────
// Silent by design. There was a toast across the bottom of the game window
// here, held 400 ms so it could be read; the builder's verdict is that it is
// jarring and ugly, and on a cartridge console the save is meant to be
// invisible — the player never asked for it and cannot decline it. Removing
// the hold also gives back 400 ms of paused emulation per save, which the
// audio queue was riding out as silence.
//
// A failed save now reports only over serial. The write is retried a full
// idle period later and the RAM stays dirty until it lands, so nothing is
// lost to a single failure; a card that fails every time loses saves without
// telling the player, and if that is worth surfacing the RGB LED is the
// unobtrusive place for it.

// Write cartridge RAM to the card, with the toast that confirms it. Self
// contained: it decides whether there is anything to do, takes the display
// bus itself and gives it back, so every call site is one line.
//
// A failed write leaves the RAM dirty — it has not reached the card — and
// defers the retry by a full idle period, so a bad card costs one toast every
// ten seconds rather than one every frame.
static void flush_save(const char* why) {
    if (!cur_path[0] || !emu_cart_ram_dirty()) {
        return;
    }

    uint32_t sz = 0;
    uint8_t* ram = emu_get_cart_ram(&sz);
    if (sz == 0 || !ram) {
        return;
    }

    // The pipeline still pauses: the card write blocks this core for long
    // enough that the DMA queue would run dry, and speaker_silence() holding
    // the pin at mid-scale is quieter than a starved chain repeating its last
    // buffer. Nothing is drawn, so the frozen frame is all the player sees.
    emu_pause_pipeline();

    bool ok = sd_save_state(cur_path, ram, sz);
    if (ok) {
        emu_clear_cart_ram_dirty();
    } else {
        emu_autosave_defer(millis());
    }
    Serial.printf("[SAVE] %s: %u bytes (%s)\n", why, sz, ok ? "ok" : "fail");

    emu_resume_pipeline();
}

static void load_ram() {
    if(!cur_path[0]) return;
    uint32_t sz=0;
    uint8_t* cart_ram = emu_get_cart_ram(&sz);
    if (sz == 0) {
        Serial.println("[SAVE] Load skipped: cart RAM size is 0");
        return;
    }

    if (!cart_ram) {
        Serial.println("[SAVE] Load failed: cart RAM pointer is null");
        return;
    }

    if (sd_load_state(cur_path, cart_ram, sz)) {
        Serial.printf("[SAVE] Loaded %u bytes\n", sz);
    } else {
        Serial.printf("[SAVE] No load for %s\n", cur_path);
    }
}

// ─── Emulation loop ─────────────────────────────────────────────────────────
// Runs the menu request may wait for the save state's snapshot: a run can
// start mid-frame, so a whole frame needs two, and the rest cover a skipped
// frame or the LCD being off. Past this the menu opens without one.
#define MENU_SNAPSHOT_RUNS 4

// Never returns. A running game leads to the menu and back, and nowhere else:
// there is no path from one back to cart selection, and adding one would
// defeat the cartridge scheme.
void run_emu() {
    menu_req = false;
    combo_init(&combo);
    display_clear(TFT_BLACK);

    // loopTask is core 1 on arduino-esp32, which is what leaves core 0 to the
    // push task. Input is polled here too, so core 0 hosts nothing but the
    // push task. Logged once rather than assumed.
    Serial.printf("[EMU] emulation on core %d\n", xPortGetCoreID());

    uint8_t menu_runs = 0;

    for (;;) {
        uint32_t now = millis();
        poll_input(now);
        settings_flush(now, false);

        emu_run_frame();

        // The write flag the cartridge-RAM callback set becomes the dirty
        // state here, stamped with this frame's time: the callback is IRAM
        // resident and may not read a clock.
        emu_autosave_tick(now);

        // One ADC read a second, and one save per crossing below the
        // threshold — the latch in gbcore is what makes the second true.
        uint16_t mv = 0;
        if (battery_poll(now, &mv) && emu_autosave_battery(mv, BAT_LOW_MV, BAT_HYST_MV)) {
            flush_save("battery");
        }

        // The combo armed the snapshot; the game runs on, the combo masked
        // from it, until that frame is drawn.
        if (menu_req && (emu_state_thumb_ready()
                         || ++menu_runs >= MENU_SNAPSHOT_RUNS)) {
            Serial.printf("[MENU] snapshot %s after %u runs\n",
                          emu_state_thumb_ready() ? "ready" : "missing",
                          (unsigned)menu_runs);
            menu_req = false;
            menu_runs = 0;

            // Before the pause, because flush_save takes the bus itself.
            flush_save("menu");

            // Between frames, so nothing is half produced: stop the producer,
            // wait for the queue to drain and take the bus before anything
            // draws through `tft` directly.
            emu_pause_pipeline();

            // A coalesced save still parked here has to land before the menu
            // reads the struct, or it would be written back over whatever the
            // menu leaves in it.
            settings_flush(millis(), true);

            // The menu edits the struct in place and applies backlight and
            // palette as they change, so one write on the way out is all the
            // persistence this path needs — and is the only NVS write in it.
            enum menu_result_e r = menu_open(&settings, &cart_info);
            settings_save(&settings);
            // Separately, and keyed by the cartridge: the palette row is the
            // one thing in the menu that belongs to the game rather than to
            // the unit. Auto clears the override rather than storing one.
            settings_game_palette_save(cart_info.title, settings.palette);
            // The menu's Volume row edits the struct without applying it, so
            // the new index reaches the mixer here, on the way back to play.
            emu_set_volume(settings.volume);

            display_clear(TFT_BLACK);
            emu_resume_pipeline();

            // The menu polled the buttons on its own, so the state machine's
            // view of them is stale; start it clean rather than reporting the
            // release of whatever exited the menu as fresh input.
            combo_init(&combo);

            // After the resume, because flush_save pauses the pipeline itself,
            // and a no-op unless something dirtied cartridge RAM since the
            // menu-open flush. Before the restart, not after: gb_reset leaves
            // cartridge RAM alone, so the progress the player keeps is
            // whatever reached the card here.
            if (r == MENU_RESET) {
                flush_save("reset");
                emu_reset();
            }
        }

        taskYIELD();
    }
}

// ─── Load ───────────────────────────────────────────────────────────────────
// `name` is a ROM file name, not a path: exact match is the rule, and the
// legacy walk is the fallback for tags hand-written before the device could
// write them.
static void load_and_run(const char* name) {
    if (!sd_rom_path(name, cur_path, sizeof(cur_path))
        && !sd_rom_find_legacy(name, cur_path, sizeof(cur_path))) {
        halt_screen("Not found:", tag_payload[0] ? tag_payload : name);
    }

    notice("Loading...", "", false);

    // Basename, not the path: it is what the store records and compares, and
    // deriving it here keeps it in step with cur_path instead of repeating the
    // file name as a second literal that could drift out of agreement.
    const char* rom_name = strrchr(cur_path, '/');
    rom_name = rom_name ? rom_name + 1 : cur_path;

    File rom_file = SD.open(cur_path, FILE_READ);
    if (!rom_file) {
        halt_screen("Open failed", cur_path);
    }

    // Order is load-bearing, not incidental: a flash write stalls the other
    // core's instruction fetch, so the ROM must be in the partition before any
    // emulation task exists.
    bool in_flash = rom_store_init() && rom_store_write(rom_file, rom_name);
    rom_file.close();
    if (!in_flash) {
        halt_screen("ROM store failed", "");
    }

    uint32_t rom_len = 0;
    const uint8_t* rom = rom_store_mmap(&rom_len);
    if (!rom) {
        halt_screen("Map failed", "");
    }
    // Read on the stack: emu_init() copies it into gnuboy's own buffer.
    uint8_t boot_rom[DMG_BOOT_ROM_SIZE];
    if (sd_boot_rom_read(boot_rom, sizeof(boot_rom))) {
        emu_set_boot_rom(boot_rom);
    }
    if (!emu_init(rom, rom_len)) {
        halt_screen("Init failed", "");
    }

    // The menu's snapshot: the path the tag's name matched, and the header
    // title the mapped ROM has just made answerable. Gathered once, here,
    // because nothing after this point changes either.
    strncpy(cart_info.path, cur_path, sizeof(cart_info.path) - 1);
    cart_info.path[sizeof(cart_info.path) - 1] = '\0';
    emu_get_rom_title(cart_info.title, sizeof(cart_info.title));

    // The palette is the cartridge's, not the device's, so it is resolved
    // here rather than with the rest of the settings in setup(): the title it
    // is keyed by does not exist until the ROM is mapped. No override leaves
    // it at the default settings_defaults() put there, PALETTE_AUTO, and the
    // bridge has already built that cartridge's own colours in emu_init().
    if (settings_game_palette_load(cart_info.title, &settings.palette)) {
        emu_set_palette(settings.palette);
    }
    Serial.printf("[INIT] Palette: %s\n", emu_get_palette_name(settings.palette));

    // Protection for a tag that carries valid content but lost power between
    // its write and its protect. Here on purpose: the buttons are not polled
    // until run_emu() and the push task does not exist yet, so the I2C bus is
    // uncontended. A later change that starts polling earlier has to keep
    // this order. Never a halt — the game still plays and the next boot heals
    // again.
    if (boot_should_heal(&in)) {
        Serial.printf("[BOOT] heal -> %d\n", provision_heal());
    }

    // After the ROM is in flash, never before: the push task makes core 0 a
    // second consumer of the instruction cache, and a flash write stalls both
    // cores' fetch. Started once; a later game reuses the same task.
    emu_start_push_task();

    load_ram();
    if (LED_G_PIN >= 0) digitalWrite(LED_G_PIN, LOW);
    run_emu();
}

// ─── Setup ──────────────────────────────────────────────────────────────────
void setup() {
    // Bench builds raise this. Serial.printf blocks once the TX buffer
    // fills, so a 150-character line costs 13 ms at 115200 - most of a
    // frame, on the frame path - and that is a tearing artefact of the
    // instrumentation rather than of the renderer (SERIAL_BAUD, 2026-09-21).
#ifndef SERIAL_BAUD
#define SERIAL_BAUD 115200
#endif
    Serial.begin(SERIAL_BAUD); delay(200);
    Serial.println("\n=== CYD-GB ===");

    // First, so the DAC is parked at mid-scale for the whole boot instead of
    // floating into an amplifier that is always live. Nothing about the DAC
    // or I2S0 touches the I2C bus, the panel or the card, so there is no
    // ordering cost. A refusal is logged and the unit plays silently — sound
    // is never a halt.
    if (!speaker_init()) {
        Serial.println("[SPK] no audio output");
    }
    if (LED_R_PIN >= 0) pinMode(LED_R_PIN, OUTPUT);
    if (LED_G_PIN >= 0) pinMode(LED_G_PIN, OUTPUT);
    if (LED_B_PIN >= 0) pinMode(LED_B_PIN, OUTPUT);
    if (LED_R_PIN >= 0) digitalWrite(LED_R_PIN, HIGH);
    if (LED_G_PIN >= 0) digitalWrite(LED_G_PIN, HIGH);
    if (LED_B_PIN >= 0) digitalWrite(LED_B_PIN, HIGH);

    // The tag read comes before the display and the SD card, and that order
    // is the point: the panel is dark and the card idle, which is the
    // quietest the RF environment gets, and the whole boot depends on this
    // one read.
    i2c_bus_init();
    button_init();
    battery_init();

    // Diagnostic mode: Start+Select held at power-on, sampled here because the
    // expander is up and the tag has not been read. Two samples 10 ms apart,
    // the same stability the combo module asks for, so a bounce on the first
    // I2C read of a cold boot cannot enter it. Entering skips the tag read
    // entirely and never returns; the power switch is the exit.
    //
    // Deliberately outside the DEV_ROM_PATH guard: a bench build needs the
    // diagnostic screen as much as a shipped one does.
    const uint16_t diag_combo = GB_BTN_START | GB_BTN_SELECT;
    button_update();
    bool diag = (button_get_buttons() & diag_combo) == diag_combo;
    if (diag) {
        delay(10);
        button_update();
        diag = (button_get_buttons() & diag_combo) == diag_combo;
    }
    if (diag) {
        Serial.println("[BOOT] diagnostics");
        // The reader comes up for the inspector's sake; no tag is read here.
        bool nfc_ok = nfc_init();
        settings_defaults(&settings);
        settings_load(&settings);
        display_init();
        display_set_backlight(settings.brightness);
        display_set_trim(settings.trim_fpa, settings.trim_ratio);
        // A missing card is a page result in this mode, not a halt: a unit
        // whose card is the fault is exactly the unit a builder is holding.
        bool sd_ok = sd_init();
        diag_run(&settings, nfc_ok, sd_ok);
    }

#ifdef SCALE_BENCH
    // Bench only: the transposed scale cost against the row-major one. Sits
    // ahead of the tag read because it needs no cartridge, no card and no
    // display, and it never returns.
    scale_bench_run();
#endif

#if defined(DEV_ROM_PATH) && defined(DEV_WRITER)
#error "DEV_ROM_PATH and DEV_WRITER are separate bench builds"
#endif

#if !defined(DEV_ROM_PATH) && !defined(DEV_WRITER)
    bool reader_ok = nfc_init();
    if (!reader_ok) {
        Serial.println("[BOOT] NFC reader did not answer");
    }
    read_tag(&in);
#endif

    settings_defaults(&settings);
    bool stored = settings_load(&settings);
    settings_wizard_load(&in.flags);
    in.pending_set = settings_pending_load(&in.pending);

    display_init();
#ifdef PANEL_FILL_PROBE
    // Bench only: which way does the portrait window fill? See display.h.
    // Needs the display up and nothing else.
    display_fill_probe();
#endif
#ifdef PANEL_PROBE
    // Bench only: does this panel answer reads? See display.h. Runs before the
    // frame path exists, so it has the bus to itself.
    display_panel_probe();
#endif
    display_set_backlight(settings.brightness);
#ifdef PANEL_TRIM_FORCE
    // Bench only: one arm of a blind trim trial. Applies the porch the build
    // was given and ignores the stored one, so neither arm depends on what
    // NVS happens to hold and the two differ in exactly two numbers.
    // scripts/trim_ab.sh picks the arm and is the only thing that knows it.
    display_set_trim(PANEL_TRIM_FPA, PANEL_TRIM_RATIO);
#else
    display_set_trim(settings.trim_fpa, settings.trim_ratio);
#endif
#ifdef DEV_FRAMESKIP
    // Bench only: force the frameskip setting for a measurement build.
    settings.frameskip = DEV_FRAMESKIP;
#endif
    emu_set_frame_skip(settings.frameskip);
    emu_set_viewport(settings.game_x, settings.game_y);
    emu_set_volume(settings.volume);
    // No palette here: it is the cartridge's, and load_and_run() reports it
    // once the ROM that decides it has been mapped.
    Serial.printf("[INIT] Settings (%s): fs=%d bl=%d vol=%d gx=%d gy=%d\n",
                  stored ? "NVS" : "defaults",
                  settings.frameskip, settings.brightness,
                  settings.volume, settings.game_x, settings.game_y);

#if !defined(DEV_ROM_PATH) && !defined(DEV_WRITER)
    // Exactly one retry, and only now that there is a screen to report the
    // outcome on. One, not a loop: a tag that does not read twice is a halt.
    if (!reader_ok) {
        reader_ok = nfc_init();
    }
    // A reader that does not answer twice is the fault, not the tag: say so
    // rather than let the tag read fail and blame the cartridge.
    if (!reader_ok) {
        halt_screen("Reader not responding", "");
    }
    if (in.tag != BOOT_TAG_OK) {
        read_tag(&in);
    }
#endif

    if (!sd_init()) {
        halt_screen("SD Card Error!", "Insert FAT32 SD & reset");
    }
    cat_ok = sd_catalog_reader(&cat);

    if (in.pending_set) {
        show_pending_banner();
    }

    Serial.printf("[INIT] Heap: %u\n",ESP.getFreeHeap());
}

// ─── Loop ───────────────────────────────────────────────────────────────────
// Runs once. Every exit is a halt.
void loop() {
#ifdef DEV_ROM_PATH
    // Bench bypass: loads one fixed ROM by file name and issues no tag
    // command at all. The guards are in setup(), not here — skipping this
    // switch alone would still leave the reader polled and, worse, still hand
    // a real tag to boot_should_heal() in load_and_run(), so a bench build
    // would write protection to any unprotected cart put in front of it.
    // With the acquisition guarded, `in` keeps its static zero-initialised
    // value: in.tag is BOOT_TAG_NONE, which boot_should_heal() refuses, so
    // the heal cannot fire and needs no guard of its own. Anything that moves
    // `in` off file scope, or relaxes that predicate, has to guard the heal
    // explicitly.
    //
    // A build flag only — platformio.ini does not configure it and a
    // guard test asserts it never will, so no default build can acquire it.
    // Pass it per invocation, naming a file under /roms/gb:
    //
    //   PLATFORMIO_BUILD_FLAGS='-DDEV_ROM_PATH=\"mygame.gb\"' pio run -e cyd
    //
    load_and_run(DEV_ROM_PATH);
    return;
#endif

#ifdef DEV_WRITER
    // Bench only: the MENU cart's writer screen with no reader wired. The tag
    // read is guarded out in setup(), and the pick is logged and dropped: no
    // pending record, no tag command, so the build cannot make a cart.
    //
    //   PLATFORMIO_BUILD_FLAGS=-DDEV_WRITER pio run -e cyd
    //
    // Add -DDEV_WRITER_SETUP for the first-boot wizard's view of it instead:
    // the starters only, with the wildcard counted as done so Finish setup is
    // on the list too.
    {
        boot_selection_t dev_sel = {};
#ifdef DEV_WRITER_SETUP
        const enum writer_mode_e dev_mode = WRITER_MODE_IMMEDIATE;
        in.flags.wild_done = true;
#else
        const enum writer_mode_e dev_mode = WRITER_MODE_PENDING;
#endif
        enum boot_pick_e dev_pick = writer_open(dev_mode,
                                                cat_ok ? &cat : NULL,
                                                &in.flags, in.pending_set,
                                                &dev_sel);
        Serial.printf("[DEV] writer pick=%d rom=%s target=%u\n",
                      (int)dev_pick, dev_sel.rom, (unsigned)dev_sel.target);
        halt_notice("Dev writer", "Nothing written. Power off");
    }
#endif

    enum boot_action_e action = boot_decide(&in);
    if (action == BOOT_NEED_AUTH) {
        // Asked for, never volunteered: the password goes to a tag only when
        // the outcome actually depends on the answer.
        in.auth = provision_auth_state();
        action = boot_decide(&in);
    }
    Serial.printf("[BOOT] action=%d\n", (int)action);

    boot_selection_t sel;
    enum boot_pick_e pick = BOOT_PICK_NONE;
    enum boot_pick_action_e pa = BOOT_PICK_INVALID;
    char detail[24];
    int rc = 0;

    switch (action) {
        case BOOT_HALT_NO_CART:
            halt_screen("No cartridge", "");
            break;
        case BOOT_HALT_SHIELDING:
            halt_screen("Shielding fault", "");
            break;
        case BOOT_HALT_UNREADABLE:
            halt_screen("Unreadable tag", tag_payload);
            break;
        case BOOT_HALT_BLANK:
            halt_notice("Blank cart. Use your MENU cart", "");
            break;
        case BOOT_HALT_INSERT_WILDCARD:
            halt_notice("Insert your wildcard", "");
            break;
        case BOOT_HALT_INSERT_BLANK:
            halt_notice("Insert a blank cart", "");
            break;
        case BOOT_HALT_INSERT_GAME_CART:
            halt_notice("Insert a game cart", "");
            break;
        case BOOT_HALT_SETUP_INSERT_BLANK:
            halt_notice("Setup: insert a blank cart", "");
            break;

        // Re-entry already happened above; a second request means the tag
        // stopped answering between the two decisions.
        case BOOT_NEED_AUTH:
            halt_screen("Unreadable tag", tag_payload);
            break;

        case BOOT_WIZARD_WRITE_MENU:
            rc = provision_wizard_menu(&in.flags);
            if (rc != 0) {
                snprintf(detail, sizeof(detail), "code %d", rc);
                halt_screen("Write failed", detail);
            }
            halt_notice("MENU cart made. Power off", "");
            break;
        case BOOT_WIZARD_ADOPT_MENU:
            rc = provision_wizard_adopt(BOOT_CLASS_MENU, &in.flags);
            if (rc != 0) {
                snprintf(detail, sizeof(detail), "code %d", rc);
                halt_screen("Write failed", detail);
            }
            halt_notice("Menu cart adopted. Power off", "");
            break;
        case BOOT_WIZARD_ADOPT_WILD:
            rc = provision_wizard_adopt(BOOT_CLASS_WILD, &in.flags);
            if (rc != 0) {
                snprintf(detail, sizeof(detail), "code %d", rc);
                halt_screen("Write failed", detail);
            }
            halt_notice("Wildcard adopted. Power off", "");
            break;

        // One call site for the writer, all three actions that open it.
        // boot_after_pick() takes the action that opened it precisely so the
        // caller does not need one entry point per mode: that is what keeps
        // "when can this device write a cart" a single auditable place.
        case BOOT_WIZARD_PICK_WILD:
        case BOOT_WIZARD_PICK_GAME:
        case BOOT_OPEN_WRITER:
            pick = writer_open(action == BOOT_OPEN_WRITER ? WRITER_MODE_PENDING
                                                         : WRITER_MODE_IMMEDIATE,
                               cat_ok ? &cat : NULL, &in.flags, in.pending_set,
                               &sel);
            pa = boot_after_pick(action, pick);
            switch (pa) {
                case BOOT_PICK_WRITE_WILD:
                case BOOT_PICK_WRITE_GAME:
                    rc = provision_wizard_write(pa, &sel, &in.flags);
                    if (rc != 0) {
                        snprintf(detail, sizeof(detail), "code %d", rc);
                        halt_screen("Write failed", detail);
                    }
                    halt_notice(pa == BOOT_PICK_WRITE_WILD
                                    ? "Wildcard made. Power off"
                                    : "Game cart made. Power off", "");
                    break;
                case BOOT_PICK_FINISH_SETUP:
                    rc = provision_wizard_finish(&in.flags);
                    if (rc != 0) {
                        snprintf(detail, sizeof(detail), "code %d", rc);
                        halt_screen("Write failed", detail);
                    }
                    halt_notice("Setup finished. Power off", "");
                    break;
                case BOOT_PICK_RECORD_PENDING:
                    settings_pending_save(&sel);
                    halt_notice("Power off, insert your wildcard, power on", "");
                    break;
                case BOOT_PICK_CLEAR_PENDING:
                    settings_pending_clear();
                    halt_notice("Pending write cancelled. Power off", "");
                    break;
                case BOOT_PICK_HALT_MENU_CART:
                    halt_notice("Menu cart", "");
                    break;
                case BOOT_PICK_HALT_NO_SELECTION:
                    halt_notice("Setup: insert a blank cart", "");
                    break;
                case BOOT_PICK_INVALID:
                    halt_screen("Write failed", "");
                    break;
            }
            break;

        case BOOT_EXECUTE_PENDING:
            notice("Writing cart...", "", false);
            rc = provision_execute_pending(&in.pending, in.cls);
            if (rc == NTAG_ERR_AUTH) {
                // Someone else's tag. The record stays for the right one.
                halt_notice("Insert your wildcard", "");
            }
            if (rc != 0) {
                snprintf(detail, sizeof(detail), "code %d", rc);
                halt_screen("Write failed", detail);
            }
            load_and_run(in.pending.rom);
            break;

        case BOOT_LOAD:
            load_and_run(in.rom);
            break;
    }

    // Unreachable: every arm above halts. Here so a future action added to
    // the table cannot silently fall through into a second loop() pass.
    halt_screen("Unreadable tag", tag_payload);
}
