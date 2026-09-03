#include <Arduino.h>
#include "hw_config.h"
#include "render_config.h"
#include "display.h"
#include "button_input.h"
#include "battery.h"
#include "i2c_bus.h"
#include "input/combo.h"
#include "sd_manager.h"
#include "ui_launcher.h"
#include "emulator_bridge.h"
#include "rom_store.h"
#include "settings.h"
#include "nfc_cart.h"
#include "cart_provision.h"
#include "cart_writer.h"
#include "cart/boot.h"
#include "cart/catalog.h"
#include "cart/ndef.h"
#include "cart/ntag.h"
#include <SD.h>

static char cur_path[80] = {0};
static bool emu_on = false, menu_req = false;
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

// ─── Boot screens ───────────────────────────────────────────────────────────
// All boot drawing lands inside the game window. The printed bezel masks
// everything outside GAME_X/GAME_Y x GAME_W/GAME_H, so a screen centred on
// the 320x240 panel is partly hidden behind plastic on every unit.

// A 63-character file name at font 2 runs to roughly 500-750 px depending on
// which glyphs it uses, against a 240-px window, so the detail line wraps
// instead of running under the bezel. Four rows covers the longest name the
// store accepts even in all-wide glyphs, and still ends above the window's
// bottom edge.
#define HALT_WRAP_LINES 4
#define HALT_WRAP_ROW_H 18

// Draws s across up to HALT_WRAP_LINES rows of font 2, breaking wherever the
// window runs out rather than at word boundaries — a file name has no useful
// break points. Returns the y below the last row drawn.
static int16_t draw_wrapped(const char* s, int16_t cx, int16_t top) {
    char line[96];
    size_t at = 0;
    size_t len = strlen(s);
    int row = 0;

    while (row < HALT_WRAP_LINES && at < len) {
        size_t n = 0;
        while (at + n < len && n < sizeof(line) - 1) {
            line[n] = s[at + n];
            line[n + 1] = '\0';
            if (tft.textWidth(line, 2) > GAME_W - 8) {
                line[n] = '\0';
                break;
            }
            n++;
        }
        if (n == 0) {
            break;
        }
        line[n] = '\0';
        tft.drawString(line, cx, top + row * HALT_WRAP_ROW_H, 2);
        at += n;
        row++;
    }
    return top + row * HALT_WRAP_ROW_H;
}

// l1 is the condition, l2 the detail. l1 drops from font 4 to the wrapped
// small font when the large one would not fit the window.
static void draw_boot_screen(const char* l1, uint16_t c1, const char* l2) {
    int16_t cx = settings.game_x + GAME_W / 2;
    int16_t cy = settings.game_y + GAME_H / 2;

    tft.fillScreen(TFT_BLACK);
    tft.setTextDatum(MC_DATUM);
    tft.setTextColor(c1, TFT_BLACK);

    int16_t l2_top = cy + 10;
    if (tft.textWidth(l1, 4) <= GAME_W - 8) {
        tft.drawString(l1, cx, cy - 20, 4);
    } else {
        l2_top = draw_wrapped(l1, cx, cy - 28) + 6;
    }

    if (l2 && l2[0]) {
        tft.setTextColor(0x7BEF, TFT_BLACK);
        draw_wrapped(l2, cx, l2_top);
    }
}

// Halt means halt: no retry loop and no fallback browser. The DMG's
// mechanical interlock already forces a power-off to change carts, so the
// power cycle is the retry.
static void halt_screen(const char* l1, const char* l2) {
    Serial.printf("[BOOT] halt: %s %s\n", l1, l2 ? l2 : "");
    draw_boot_screen(l1, TFT_RED, l2);
    for (;;) {
        delay(1000);
    }
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

    draw_boot_screen(l1, 0x07E0, l2);
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
            return;
        case COMBO_EVENT_VOL_UP:      // louder counts the index down towards 0
            volume = combo_step_u8(volume, -1, SETTINGS_VOL_HIGH, SETTINGS_VOL_OFF, 1);
            break;
        case COMBO_EVENT_VOL_DOWN:
            volume = combo_step_u8(volume, +1, SETTINGS_VOL_HIGH, SETTINGS_VOL_OFF, 1);
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

    settings.volume = volume;
    if (brightness != settings.brightness) {
        settings.brightness = brightness;
        display_set_backlight(settings.brightness);
    }
    settings_save_coalesced(&settings, now_ms);
}

// ─── Automatic saves ───────────────────────────────────────────────────────
// The minimum the toast stays up, measured from the draw rather than from the
// write, so a fast card does not flash it too briefly to read. The game is
// paused for this long: a save is the one moment the player is told about, and
// a confirmation nobody can read is not a confirmation.
#define SAVE_TOAST_MS 400

// A strip across the bottom of the game window, never outside it. The next
// emulated frame repaints the whole window, so there is nothing to clear.
static void draw_toast(const char* text, uint16_t colour) {
    int16_t top = settings.game_y + GAME_H - 32;

    tft.fillRect(settings.game_x, top, GAME_W, 32, TFT_BLACK);
    tft.setTextDatum(MC_DATUM);
    tft.setTextColor(colour, TFT_BLACK);
    tft.drawString(text, settings.game_x + GAME_W / 2,
                   settings.game_y + GAME_H - 16, 4);
}

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

    emu_pause_pipeline();

    uint32_t t0 = millis();
    draw_toast("SAVED", 0x07E0);

    bool ok = sd_save_state(cur_path, ram, sz);
    if (ok) {
        emu_clear_cart_ram_dirty();
    } else {
        draw_toast("SAVE FAILED", TFT_RED);
        emu_autosave_defer(millis());
    }
    Serial.printf("[SAVE] %s: %u bytes (%s)\n", why, sz, ok ? "ok" : "fail");

    // Signed difference so the hold survives the millis() rollover instead of
    // parking the game for 49 days.
    while ((int32_t)(millis() - t0) < SAVE_TOAST_MS) {
        delay(10);
    }

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
void run_emu() {
    emu_on = true; menu_req = false;
    combo_init(&combo);
    display_clear(TFT_BLACK);

    // loopTask is core 1 on arduino-esp32, which is what leaves core 0 to the
    // push task. Input is polled here too, so core 0 hosts nothing but the
    // push task. Logged once rather than assumed.
    Serial.printf("[EMU] emulation on core %d\n", xPortGetCoreID());

    while(emu_on) {
        uint32_t now = millis();
        poll_input(now);
        settings_flush(now, false);

        emu_run_frame();

        // The write flag the cartridge-RAM callback set becomes the dirty
        // state here, stamped with this frame's time: the callback is IRAM
        // resident and may not read a clock.
        emu_autosave_tick(now);
        if (emu_autosave_idle_due(now)) {
            flush_save("idle");
        }

        // One ADC read a second, and one save per crossing below the
        // threshold — the latch in gbcore is what makes the second true.
        uint16_t mv = 0;
        if (battery_poll(now, &mv) && emu_autosave_battery(mv, BAT_LOW_MV, BAT_HYST_MV)) {
            flush_save("battery");
        }

        if (menu_req) {
            menu_req = false;

            // Before the pause, because flush_save takes the bus itself.
            flush_save("menu");

            // Between frames, so nothing is half produced: stop the producer,
            // wait for the queue to drain and take the bus before anything
            // draws through `tft` directly. The quit case returns while still
            // paused, which is what keeps the loading screen off the bus.
            emu_pause_pipeline();

            // The settings menu writes NVS itself, so a coalesced save still
            // parked here has to land first or it would overwrite what the
            // menu stores.
            settings_flush(millis(), true);

            // Saving and loading are not the player's job any more, so the
            // menu has no arm for either: opening it has already saved.
            int c = launcher_ingame_menu();
            switch(c) {
                case 0: break;  // resume
                case 3:  // quit
                    emu_on = false;
                    flush_save("quit");
                    return;
                case 5:  // settings
                    launcher_settings_menu(&settings); break;
            }
            display_clear(TFT_BLACK);
            emu_resume_pipeline();

            // The menu polled the buttons on its own, so the state machine's
            // view of them is stale; start it clean rather than reporting the
            // release of whatever exited the menu as fresh input.
            combo_init(&combo);
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

    draw_boot_screen("Loading...", 0x07E0, "");

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
    if (!emu_init(rom, rom_len)) {
        halt_screen("Init failed", "");
    }

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
    if (LED_G_PIN >= 0) digitalWrite(LED_G_PIN, HIGH);

    // The only thing after a finished game is a halt. There is no path from a
    // running game back to cart selection, and adding one would defeat the
    // cartridge scheme.
    halt_screen("Power off", "");
}

// ─── Setup ──────────────────────────────────────────────────────────────────
void setup() {
    Serial.begin(115200); delay(200);
    Serial.println("\n=== CYD-GB ===");
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
#ifndef DEV_ROM_PATH
    if (!nfc_init()) {
        Serial.println("[BOOT] NFC reader did not answer");
    }
    read_tag(&in);
#endif

    settings_defaults(&settings);
    bool stored = settings_load(&settings);
    settings_wizard_load(&in.flags);
    in.pending_set = settings_pending_load(&in.pending);

    display_init();
    display_set_backlight(settings.brightness);
    emu_set_palette(settings.palette);
    emu_set_frame_skip(settings.frameskip);
    emu_set_viewport(settings.game_x, settings.game_y);
    Serial.printf("[INIT] Settings (%s): pal=%d fs=%d bl=%d vol=%d gx=%d gy=%d\n",
                  stored ? "NVS" : "defaults",
                  settings.palette, settings.frameskip, settings.brightness,
                  settings.volume, settings.game_x, settings.game_y);

#ifndef DEV_ROM_PATH
    // Exactly one retry, and only now that there is a screen to report the
    // outcome on. One, not a loop: a tag that does not read twice is a halt.
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
            halt_screen("Blank cart. Use your MENU cart.", "");
            break;
        case BOOT_HALT_INSERT_WILDCARD:
            halt_screen("Insert your wildcard", "");
            break;
        case BOOT_HALT_INSERT_BLANK:
            halt_screen("Insert a blank cart", "");
            break;
        case BOOT_HALT_INSERT_GAME_CART:
            halt_screen("Insert a game cart", "");
            break;
        case BOOT_HALT_SETUP_INSERT_BLANK:
            halt_screen("Setup: insert a blank cart", "");
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
            halt_screen("MENU cart made. Power off.", "");
            break;
        case BOOT_WIZARD_ADOPT_MENU:
            rc = provision_wizard_adopt(BOOT_CLASS_MENU, &in.flags);
            if (rc != 0) {
                snprintf(detail, sizeof(detail), "code %d", rc);
                halt_screen("Write failed", detail);
            }
            halt_screen("Menu cart adopted. Power off.", "");
            break;
        case BOOT_WIZARD_ADOPT_WILD:
            rc = provision_wizard_adopt(BOOT_CLASS_WILD, &in.flags);
            if (rc != 0) {
                snprintf(detail, sizeof(detail), "code %d", rc);
                halt_screen("Write failed", detail);
            }
            halt_screen("Wildcard adopted. Power off.", "");
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
                    halt_screen(pa == BOOT_PICK_WRITE_WILD
                                    ? "Wildcard made. Power off."
                                    : "Game cart made. Power off.", "");
                    break;
                case BOOT_PICK_FINISH_SETUP:
                    rc = provision_wizard_finish(&in.flags);
                    if (rc != 0) {
                        snprintf(detail, sizeof(detail), "code %d", rc);
                        halt_screen("Write failed", detail);
                    }
                    halt_screen("Setup finished. Power off.", "");
                    break;
                case BOOT_PICK_RECORD_PENDING:
                    settings_pending_save(&sel);
                    halt_screen("Power off, insert your wildcard, power on.", "");
                    break;
                case BOOT_PICK_CLEAR_PENDING:
                    settings_pending_clear();
                    halt_screen("Pending write cancelled. Power off.", "");
                    break;
                case BOOT_PICK_HALT_MENU_CART:
                    halt_screen("Menu cart", "");
                    break;
                case BOOT_PICK_HALT_NO_SELECTION:
                    halt_screen("Setup: insert a blank cart", "");
                    break;
                case BOOT_PICK_INVALID:
                    halt_screen("Write failed", "");
                    break;
            }
            break;

        case BOOT_EXECUTE_PENDING:
            draw_boot_screen("Writing cart...", 0x07E0, "");
            rc = provision_execute_pending(&in.pending, in.cls);
            if (rc == NTAG_ERR_AUTH) {
                // Someone else's tag. The record stays for the right one.
                halt_screen("Insert your wildcard", "");
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
