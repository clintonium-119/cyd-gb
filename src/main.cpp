#include <Arduino.h>
#include "hw_config.h"
#include "display.h"
#include "button_input.h"
#include "i2c_bus.h"
#include "input/combo.h"
#include "sd_manager.h"
#include "ui_launcher.h"
#include "emulator_bridge.h"
#include "rom_store.h"
#include "settings.h"
#include <SD.h>

// Interim boot ROM. WS-06 replaces this with the NFC cartridge match; until then
// there is exactly one path and no way to choose another.
#define DEV_TEST_ROM_PATH "/roms/gb/test.gb"

static char cur_path[80] = {0};
static bool emu_on = false, menu_req = false;
static settings_t settings;
static combo_state_t combo;

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

static void save_ram() {
    if(!cur_path[0]) return;
    uint32_t sz=0; uint8_t* r=emu_get_cart_ram(&sz);
    if(sz>0) {
        bool ok = sd_save_state(cur_path,r,sz);
        Serial.printf("[SAVE] %u bytes (%s)\n",sz, ok ? "ok" : "fail");
    }
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

        if (menu_req) {
            menu_req = false;

            // Between frames, so nothing is half produced: stop the producer,
            // wait for the queue to drain and take the bus before anything
            // draws through `tft` directly. The quit case returns while still
            // paused, which is what keeps the loading screen off the bus.
            emu_pause_pipeline();

            // The settings menu writes NVS itself, so a coalesced save still
            // parked here has to land first or it would overwrite what the
            // menu stores.
            settings_flush(millis(), true);

            int c = launcher_ingame_menu();
            switch(c) {
                case 0: break;  // resume
                case 1:  // save
                    save_ram();
                    tft.fillRect(80,80,160,40,TFT_BLACK);
                    tft.setTextDatum(MC_DATUM); tft.setTextColor(TFT_GREEN);
                    tft.drawString("SAVED!",SCREEN_W/2,100,4);
                    delay(700);
                    break;
                case 2:  // load
                    load_ram(); emu_reset(); load_ram();
                    tft.fillRect(80,80,160,40,TFT_BLACK);
                    tft.setTextDatum(MC_DATUM); tft.setTextColor(0x07FF);
                    tft.drawString("LOADED!",SCREEN_W/2,100,4);
                    delay(700);
                    break;
                case 3:  // quit
                    emu_on=false; save_ram(); return;
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

    display_init();
    i2c_bus_init();
    button_init();

    if(!sd_init()) {
        tft.fillScreen(TFT_BLACK); tft.setTextDatum(MC_DATUM);
        tft.setTextColor(TFT_RED); tft.drawString("SD Card Error!",SCREEN_W/2,100,4);
        tft.setTextColor(0x7BEF); tft.drawString("Insert FAT32 SD & reset",SCREEN_W/2,140,2);
        while(true) delay(1000);
    }

    // Splash
    tft.fillScreen(TFT_BLACK); tft.setTextDatum(MC_DATUM);
    tft.setTextColor(0x07E0); tft.drawString("CYD-GB",SCREEN_W/2,70,4);
    tft.setTextColor(0x7BEF); tft.drawString("Game Boy Emulator",SCREEN_W/2,110,2);
    delay(1200);

    // Load saved settings from NVS
    settings_defaults(&settings);
    bool stored = settings_load(&settings);
    emu_set_palette(settings.palette);
    emu_set_frame_skip(settings.frameskip);
    emu_set_viewport(settings.game_x, settings.game_y);
    display_set_backlight(settings.brightness);
    Serial.printf("[INIT] Settings (%s): pal=%d fs=%d bl=%d vol=%d gx=%d gy=%d\n",
                  stored ? "NVS" : "defaults",
                  settings.palette, settings.frameskip, settings.brightness,
                  settings.volume, settings.game_x, settings.game_y);

    Serial.printf("[INIT] Heap: %u\n",ESP.getFreeHeap());
}

// ─── Loop ───────────────────────────────────────────────────────────────────
void loop() {
    strncpy(cur_path, DEV_TEST_ROM_PATH, sizeof cur_path - 1);
    cur_path[sizeof cur_path - 1] = 0;

    // Loading screen
    tft.fillScreen(TFT_BLACK); tft.setTextDatum(MC_DATUM);
    tft.setTextColor(0x07E0); tft.drawString("Loading...",SCREEN_W/2,90,4);

    // Basename, not the path: it is what the store records and compares, and
    // deriving it here keeps it in step with cur_path instead of repeating the
    // file name as a second literal that could drift out of agreement.
    const char* rom_name = strrchr(cur_path, '/');
    rom_name = rom_name ? rom_name + 1 : cur_path;

    File rom_file = SD.open(cur_path, FILE_READ);
    if (!rom_file) {
        tft.setTextColor(TFT_RED); tft.drawString("Open failed!",SCREEN_W/2,170,2); delay(2000); return;
    }

    // Order is load-bearing, not incidental: a flash write stalls the other
    // core's instruction fetch, so the ROM must be in the partition before any
    // emulation task exists. Whatever replaces this boot flow has to keep the
    // write here, ahead of run_emu().
    bool in_flash = rom_store_init() && rom_store_write(rom_file, rom_name);
    rom_file.close();
    if (!in_flash) {
        tft.setTextColor(TFT_RED); tft.drawString("ROM store failed!",SCREEN_W/2,170,2); delay(2000); return;
    }

    uint32_t rom_len = 0;
    const uint8_t* rom = rom_store_mmap(&rom_len);
    if (!rom) {
        tft.setTextColor(TFT_RED); tft.drawString("Map failed!",SCREEN_W/2,170,2); delay(2000); return;
    }
    if (!emu_init(rom, rom_len)) {
        tft.setTextColor(TFT_RED); tft.drawString("Init failed!",SCREEN_W/2,170,2); delay(2000); return;
    }

    // After the ROM is in flash, never before: the push task makes core 0 a
    // second consumer of the instruction cache, and a flash write stalls both
    // cores' fetch. Started once; a later game reuses the same task.
    emu_start_push_task();

    load_ram();
    if (LED_G_PIN >= 0) digitalWrite(LED_G_PIN, LOW);
    run_emu();
    if (LED_G_PIN >= 0) digitalWrite(LED_G_PIN, HIGH);
}
