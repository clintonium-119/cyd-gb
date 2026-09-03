#include "display.h"
#include "hw_config.h"
#include "render_config.h"
#include <Arduino.h>
#include <string.h>

TFT_eSPI tft = TFT_eSPI();

// Set once by display_init(). Nothing falls back to the blocking path when
// this is false - see display_push_rows_dma().
static bool dma_ready = false;

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
    // The frame path is DMA-only, so a failed DMA init is a boot-level fact,
    // not something to paper over at push time: the numbers the bench items
    // collect would silently describe the blocking path instead.
    dma_ready = tft.initDMA();
    if (!dma_ready) {
        Serial.println("[TFT] DMA init FAILED - frame path unavailable");
    }
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

int16_t display_draw_wrapped(const char* s, int16_t cx, int16_t top,
                             int16_t max_w, uint8_t max_rows, uint8_t font)
{
    char line[96];
    size_t at = 0;
    size_t len;
    uint8_t row = 0;
    int16_t row_h = tft.fontHeight(font) + 2;

    if (!s) {
        return top;
    }
    len = strlen(s);
    while (row < max_rows && at < len) {
        size_t n = 0;
        // Grow the row one character at a time and keep the last one that
        // still measured inside max_w. Measuring is the only way to know:
        // font 2 is proportional, so a character count says nothing.
        while (at + n < len && n < sizeof(line) - 1) {
            line[n] = s[at + n];
            line[n + 1] = '\0';
            if (tft.textWidth(line, font) > max_w) {
                line[n] = '\0';
                break;
            }
            n++;
        }
        if (n == 0) {
            break;
        }
        line[n] = '\0';
        tft.drawString(line, cx, top + row * row_h, font);
        at += n;
        row++;
    }
    return (int16_t)(top + row * row_h);
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

void display_push_rows_dma(uint16_t* px, size_t n)
{
    static bool complained = false;

    if (!dma_ready) {
        if (!complained) {
            complained = true;
            Serial.println("[TFT] DMA push refused - no DMA, frame path dead");
        }
        return;
    }
    // The previous transfer must finish before the producer may refill its
    // buffer, and pushPixelsDMA byte-swaps this one in place before queueing
    // it, so the wait has to happen first either way. The driver waits
    // internally too; saying it here is what makes the ordering readable.
    tft.dmaWait();
    tft.pushPixelsDMA(px, (uint32_t)n);
}

void display_dma_wait()
{
    tft.dmaWait();
}

void display_bus_acquire()
{
    // Close whatever the frame path left open. endWrite() is safe unbalanced -
    // it clears the transaction flags and releases the bus either way - so the
    // menu never has to know whether a frame was in progress.
    tft.dmaWait();
    tft.endWrite();
}

void display_bus_release()
{
    // Nothing to undo: the next display_frame_begin() opens its own
    // transaction and address window. The function exists so the call sites
    // read as a matched pair and so a future handover that does need teardown
    // has one place to live.
}
