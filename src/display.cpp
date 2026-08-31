#include "display.h"
#include "hw_config.h"
#include "render_config.h"
#include <Arduino.h>

TFT_eSPI tft = TFT_eSPI();

void display_init()
{
    pinMode(TFT_PIN_BL, OUTPUT);
    digitalWrite(TFT_PIN_BL, HIGH);
    tft.init();
    tft.invertDisplay(true);
    // Palette values are native RGB565 and the scaler blends them before
    // anything swaps bytes, so the driver does the swap at push time — this is
    // the ordering design §2.3 requires to avoid colour fringing.
    tft.setSwapBytes(true);
    tft.setRotation(TFT_ROTATION_LANDSCAPE);
    tft.fillScreen(TFT_BLACK);
    ledcSetup(0, 5000, 8);
    ledcAttachPin(TFT_PIN_BL, 0);
    ledcWrite(0, 255);
    Serial.printf("[TFT] %dx%d OK\n", tft.width(), tft.height());
}

void display_set_backlight(uint8_t level)
{
    ledcWrite(0, level);
}

void display_clear(uint16_t color)
{
    tft.fillScreen(color);
}

void display_frame_begin(int16_t x, int16_t y)
{
    tft.startWrite();
    tft.setAddrWindow(x, y, GAME_W, GAME_H);
}

void display_push_rows(const uint16_t* px, size_t n)
{
    tft.pushPixels(px, (uint32_t)n);
}

void display_frame_end()
{
    tft.endWrite();
}
