#include "display.h"
#include "hw_config.h"
#include <Arduino.h>

TFT_eSPI tft = TFT_eSPI();
static uint16_t scaled[SCREEN_W];

void display_init() {
    pinMode(TFT_PIN_BL, OUTPUT);
    digitalWrite(TFT_PIN_BL, HIGH);
    tft.init();
    // Some ST77xx displays expect swapped byte order (RGB/BGR). Enable
    // swap here to match palette byte-order when needed.
    tft.setSwapBytes(true);
    // Use rotation 2 so the USB connector is at the top in portrait mode.
    tft.setRotation(2);
    tft.fillScreen(TFT_BLACK);
    ledcSetup(0, 5000, 8);
    ledcAttachPin(TFT_PIN_BL, 0);
    ledcWrite(0, 255);
    Serial.printf("[TFT] %dx%d OK\n", tft.width(), tft.height());
}

void display_set_backlight(uint8_t level) { ledcWrite(0, level); }
void display_clear(uint16_t color) { tft.fillScreen(color); }

// Game scanline -> top 192px (2x horiz, ~1.33x vert)
void display_push_gb_line(uint8_t y, uint16_t* buf) {
    if (y >= GB_SCREEN_H) return;
    // Scale 160 -> SCREEN_W (240) horizontally. We approximate 1.5x scaling
    // by duplicating every even pixel (pattern: 2,1,2,1...) to reach 240.
    int idx = 0;
    for (int x = 0; x < GB_SCREEN_W && idx < SCREEN_W; x++) {
        scaled[idx++] = buf[x];
        if ((x & 1) == 0 && idx < SCREEN_W) scaled[idx++] = buf[x];
    }
    while (idx < SCREEN_W) scaled[idx++] = buf[GB_SCREEN_W-1];
    int y0 = y * GAME_H / GB_SCREEN_H;
    int y1 = (y+1) * GAME_H / GB_SCREEN_H;
    if (y1 == y0) y1 = y0 + 1;

    // No setSwapBytes - palette values are pre-swapped in emulator_bridge
    for (int sy = y0; sy < y1 && sy < GAME_H; sy++)
        tft.pushImage(0, sy, SCREEN_W, 1, scaled);
}

// ─── Control bar (y=192..240) ───────────────────────────────────────────────
void display_draw_controls() {
    // Touch GUI removed: we use physical I2C buttons instead.
    (void)CTRL_Y; (void)CTRL_H; // keep unused vars quiet
}
