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
    // This panel is not inverted and wants BGR, not RGB. Both were
    // wrong together and masked each other: inversion alone reads cyan,
    // the R/B swap alone reads blue on black, and the pair read yellow
    // on white. The colour order half is -DTFT_RGB_ORDER in
    // platformio.ini; this is the inversion half (BUG-0004).
    tft.invertDisplay(false);
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

// The scaler blends in native RGB565 and the panel wants the high byte first,
// so a block has to be byte-swapped between the two. pushPixelsDMA does that
// itself, but it does it AFTER its own dmaWait() — with the bus idle, which
// makes every swapped pixel dead transfer time. At 26/16 that is 60,840
// pixels a frame, and the 24/16 bench capture measured it directly: push
// accumulated 2,531 us across a frame whose blocks only cost 2 us each to
// queue (logs/perf26-04-pokered-24.log).
//
// This is the same swap, hoisted in front of the wait, where it overlaps the
// previous block's transfer instead of following it.
static void swap565(uint16_t* px, size_t n)
{
    size_t i;

    for (i = 0; i < n; i++) {
        px[i] = (uint16_t)(px[i] << 8 | px[i] >> 8);
    }
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
    // Before the wait, not after: this block's buffer is not the one in
    // flight, so swapping it is safe while the previous transfer runs.
    swap565(px, n);
    // Cleared only across the queueing call. setSwapBytes(true) is what the
    // menu's pushImage path relies on (the .565 covers), and the frame path
    // is the one place that has already done the swap for itself.
    tft.setSwapBytes(false);
    // The previous transfer must still finish before the producer may refill
    // its buffer. The driver waits internally too; saying it here is what
    // makes the ordering readable.
    tft.dmaWait();
    tft.pushPixelsDMA(px, (uint32_t)n);
    tft.setSwapBytes(true);
}

void display_dma_wait()
{
    tft.dmaWait();
}

// ─── Canvas ─────────────────────────────────────────────────────────────────
// The window origin, from NVS: ten hand-built units each land slightly
// differently behind the bezel. Stored rather than passed per primitive,
// because the seam's signature is fixed by two layout modules and a host test.
static int16_t ox;
static int16_t oy;

// Window-relative coordinates in, panel coordinates out. Neither layout
// module knows anything of the origin.

static void cv_fill(void* ctx, int16_t x, int16_t y, int16_t w, int16_t h,
                    uint16_t color) {
    (void)ctx;
    tft.fillRect(ox + x, oy + y, w, h, color);
}

static void cv_text(void* ctx, const char* s, int16_t x, int16_t y, int16_t w,
                    uint8_t rows, uint8_t font, uint8_t align, uint16_t fg,
                    uint16_t bg) {
    (void)ctx;
    int16_t anchor = x;

    tft.setTextColor(fg, bg);
    switch (align) {
        case UI_ALIGN_CENTER:
            tft.setTextDatum(TC_DATUM);
            anchor = (int16_t)(x + w / 2);
            break;
        case UI_ALIGN_RIGHT:
            tft.setTextDatum(TR_DATUM);
            anchor = (int16_t)(x + w);
            break;
        default:
            tft.setTextDatum(TL_DATUM);
            break;
    }
    display_draw_wrapped(s, (int16_t)(ox + anchor), (int16_t)(oy + y), w, rows,
                         font);
}

static void cv_image(void* ctx, int16_t x, int16_t y, int16_t w, int16_t h,
                     const uint16_t* px, int16_t row0, int16_t rows) {
    (void)ctx;
    (void)h;
    // The row range is how the scrolling band clips an image at its edge: the
    // driver is handed the first visible row and told how many follow.
    //
    // display_init() leaves setSwapBytes(true) in force and the .565 files are
    // little-endian, so there is no per-pixel swap to do here.
    tft.pushImage(ox + x, oy + y, w, rows, (uint16_t*)(px + (size_t)row0 * w));
}

static const ui_canvas_t canvas = { NULL, cv_fill, cv_text, cv_image };

const ui_canvas_t* display_canvas(int16_t x, int16_t y)
{
    ox = x;
    oy = y;
    return &canvas;
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
