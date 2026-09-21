#include "display.h"
#include "hw_config.h"
#include "render_config.h"
#include <Arduino.h>
#include <math.h>
#include <string.h>

TFT_eSPI tft = TFT_eSPI();

// Set once by display_init(). Nothing falls back to the blocking path when
// this is false - see display_push_rows_dma().
static bool dma_ready = false;

#ifdef PANEL_PORCH_FPA
// ─── Panel rate trim ────────────────────────────────────────────────────────
// The tear is the emulator's audio-locked 59.7275 fps beating against the
// panel's free-running refresh, and PANEL_PROBE established that this panel
// drives nothing on MISO, so the beat cannot be servoed. It can be nulled:
// PORCTRL's front porch lengthens the frame by whole lines, one line being
// about 0.17 Hz, and a fractional divider between two neighbouring values
// makes the average continuous. Both numbers are per unit, because the null
// measures that panel's own oscillator error.
//
// Trim values come from the PANEL_TRIM fixture. This is the verification
// build that applies one: a real game, the real frame path, the numbers the
// bench found.
//
//   PLATFORMIO_BUILD_FLAGS='-DPANEL_PORCH_FPA=11 -DPANEL_PORCH_RATIO=16 ...'
#ifndef PANEL_PORCH_RATIO
#define PANEL_PORCH_RATIO 0
#endif

// Live, because the fixture's null was measured against a timer set to the
// emulator's ASSUMED 59.7275 fps, and the game is paced by the audio DMA at
// whatever rate the I2S divider really produces. The two are not the same
// question, so the trim is driven over the serial port while a game runs and
// the real cadence is measured rather than assumed.
static uint8_t trim_fpa = PANEL_PORCH_FPA;
static uint8_t trim_ratio = PANEL_PORCH_RATIO;
// Any serial key hands control over for good: an auto-null that argued with
// the person turning the knob would be unusable.
static bool manual = false;

static void write_porch(uint8_t fpa)
{
    tft.writecommand(0xB2);
    tft.writedata(0x0C);
    tft.writedata(fpa);
    tft.writedata(0x00);
    tft.writedata(0x33);
    tft.writedata(0x33);
}

// One frame of the divider, from display_frame_end(): the panel spends
// PANEL_PORCH_RATIO frames in 64 on the longer porch, so the average line
// count is fractional even though each frame's is not. Six bytes of SPI on
// the frames where the value changes, with the bus already idle.
static void porch_advance()
{
    static uint8_t acc = 0;
    static uint8_t applied = 0xFF;
    uint8_t want = trim_fpa;

    acc = (uint8_t)(acc + trim_ratio);
    if (acc >= 64) {
        acc = (uint8_t)(acc - 64);
        want = (uint8_t)(trim_fpa + 1);
    }
    if (want != applied) {
        write_porch(want);
        applied = want;
    }
}

// The anchor: one porch setting whose true panel rate is known, from which
// every other setting follows, because the rate scales inversely with the
// total line count. It is per unit - it measures THIS panel's oscillator
// error, which the datasheet does not give and no register reports - and it
// is the one number in the whole scheme that needs a human, because a null is
// the only observable this hardware offers.
//
// Default: the stripe fixture nulled at 11 + 20/64 against its own 59.7275 Hz
// pacing on 2026-09-20. The eye placed that null to about +/-4/64, which is
// +/-0.011 Hz, which is the floor on how well any of this can park a seam.
#ifndef PANEL_ANCHOR_FPA
#define PANEL_ANCHOR_FPA 11
#endif
#ifndef PANEL_ANCHOR_RATIO
#define PANEL_ANCHOR_RATIO 20
#endif
#ifndef PANEL_ANCHOR_HZ
#define PANEL_ANCHOR_HZ 59.7275
#endif
#define PANEL_ANCHOR_LINES \
    (332.0 + (double)PANEL_ANCHOR_FPA + (double)PANEL_ANCHOR_RATIO / 64.0)

static double panel_hz(uint8_t fpa, uint8_t ratio)
{
    return PANEL_ANCHOR_HZ * PANEL_ANCHOR_LINES
                           / (332.0 + (double)fpa + (double)ratio / 64.0);
}

/* The porch that would put the panel on `hz`, in 64ths of a line. Returns
 * false if it lands outside what PORCTRL can express. */
static bool porch_for(double hz, uint8_t* fpa, uint8_t* ratio)
{
    double lines = PANEL_ANCHOR_HZ * PANEL_ANCHOR_LINES / hz - 332.0;
    long sixtyfourths;

    if (hz < 1.0 || lines < 1.0 || lines > 126.0) {
        return false;
    }
    sixtyfourths = (long)(lines * 64.0 + 0.5);
    *fpa = (uint8_t)(sixtyfourths / 64);
    *ratio = (uint8_t)(sixtyfourths % 64);
    return true;
}

static void trim_report(const char* why, double emu_hz)
{
    double p = panel_hz(trim_fpa, trim_ratio);

    Serial.printf("[TRIM] %s porch %u + %u/64  panel %.4f Hz  emu %.4f Hz  "
                  "beat %+.4f Hz", why, (unsigned)trim_fpa,
                  (unsigned)trim_ratio, p, emu_hz, p - emu_hz);
    if (emu_hz > 1.0) {
        double want = (332.0 + 11.0 + 20.0 / 64.0) * 59.7275 / emu_hz - 332.0;

        Serial.printf("  null at %u + %u/64", (unsigned)want,
                      (unsigned)((want - (double)(unsigned)want) * 64.0 + 0.5));
    }
    Serial.println();
}

/* Once a frame, from display_frame_end(). Measures the cadence the frame path
 * is actually running at over a long window - the whole point, since the
 * assumed rate is what the fixture got wrong - and takes trim keys off the
 * serial port so the builder can play rather than press buttons. */
static void trim_console()
{
    // Cumulative since a warm-up, not a fresh window each time. Counting
    // whole frames over ten seconds quantises to about 0.1 Hz, which is ten
    // times coarser than the trim needs; the same count over five minutes is
    // 0.003 Hz. The long-run rate is what a rate trim has to match anyway -
    // the short-term hunting around it is the audio buffer filling and
    // draining, and no porch value can follow that.
    static uint32_t frames = 0;
    static int64_t t0 = 0;
    static int64_t next_report = 0;
    static double emu_hz = 0.0;
    int64_t now = esp_timer_get_time();

    // Five seconds of warm-up discarded: the first frames carry the ROM load
    // and the splash, which are not the cadence being measured.
    if (t0 == 0) {
        if (now < 5000000) {
            return;
        }
        t0 = now;
        next_report = now + 10000000;
        return;
    }
    frames++;

    // Frame-to-frame spread, which is what decides whether a parked seam is
    // even possible. The seam's position is the phase between the emulator's
    // frame start and the panel's scan, so a frame that arrives 2 ms late
    // moves it an eighth of the way across. If the spread is wider than the
    // beat a trim could null, the trim is chasing the wrong term.
    {
        static int64_t last = 0;
        static int32_t lo = 0;
        static int32_t hi = 0;
        static int64_t sum = 0;
        static uint32_t n = 0;
        static bool skip_one = false;
        static uint32_t bucket[6];
        unsigned b;

        // The frame a report prints in is not a frame the renderer produced -
        // Serial.printf blocks - so it is dropped rather than measured.
        if (skip_one) {
            skip_one = false;
            last = now;
        } else if (last != 0) {
            int32_t dt = (int32_t)(now - last);

            if (n == 0 || dt < lo) {
                lo = dt;
            }
            if (n == 0 || dt > hi) {
                hi = dt;
            }
            sum += dt;
            n++;
            // A histogram, not a maximum: one 40 ms frame in ten seconds is
            // an event to go and find, and forty of them is the renderer's
            // normal behaviour. A maximum cannot tell those apart.
            b = (dt < 15000) ? 0 : (dt < 18000) ? 1 : (dt < 22000) ? 2
              : (dt < 28000) ? 3 : (dt < 35000) ? 4 : 5;
            bucket[b]++;
        }
        last = now;

        if (now >= next_report && n > 0) {
            Serial.printf("[TRIM] period mean %ld min %ld max %ld us | "
                          "<15:%u 15-18:%u 18-22:%u 22-28:%u 28-35:%u "
                          ">35:%u\n",
                          (long)(sum / n), (long)lo, (long)hi,
                          bucket[0], bucket[1], bucket[2], bucket[3],
                          bucket[4], bucket[5]);
            lo = hi = 0;
            sum = 0;
            n = 0;
            skip_one = true;
            for (b = 0; b < 6; b++) {
                bucket[b] = 0;
            }
        }
    }

    if (now >= next_report) {
        emu_hz = (double)frames * 1000000.0 / (double)(now - t0);
        Serial.printf("[TRIM] %u frames in %.1f s: ", (unsigned)frames,
                      (double)(now - t0) / 1000000.0);
        trim_report("cumulative", emu_hz);
        next_report = now + 10000000;

        // Apply the null the measurement implies, once the estimate has had
        // long enough to be worth applying and thereafter only when it has
        // actually moved. Sixty seconds of frames is about 0.006 Hz of
        // counting error, finer than the anchor itself, so waiting longer
        // buys nothing the anchor has not already lost.
        if (now - t0 >= 60000000 && !manual) {
            uint8_t want_fpa;
            uint8_t want_ratio;

            if (porch_for(emu_hz, &want_fpa, &want_ratio)
                && (want_fpa != trim_fpa || want_ratio != trim_ratio)) {
                trim_fpa = want_fpa;
                trim_ratio = want_ratio;
                trim_report("AUTO-NULL", emu_hz);
            }
        }
    }

    while (Serial.available() > 0) {
        int c = Serial.read();

        switch (c) {
            case '+':
                if (trim_ratio + 4 < 64) {
                    trim_ratio = (uint8_t)(trim_ratio + 4);
                } else {
                    trim_ratio = (uint8_t)(trim_ratio + 4 - 64);
                    trim_fpa++;
                }
                break;
            case '-':
                if (trim_ratio >= 4) {
                    trim_ratio = (uint8_t)(trim_ratio - 4);
                } else if (trim_fpa > 1) {
                    trim_ratio = (uint8_t)(trim_ratio + 60);
                    trim_fpa--;
                }
                break;
            case ']':
                trim_fpa++;
                break;
            case '[':
                if (trim_fpa > 1) {
                    trim_fpa--;
                }
                break;
            // Does the panel restart its scan on a display-on? If it does,
            // the seam jumps to the SAME place every time this is pressed,
            // and phase becomes something the firmware can set rather than
            // inherit at boot - which is the difference between a
            // column-major push being tear-free on some boots and on all of
            // them. If the seam lands somewhere different each press, it
            // does not, and phase stays unknowable on this hardware.
            case 'r':
                tft.writecommand(0x29);         // DISPON alone
                Serial.println("[TRIM] DISPON");
                continue;
            case 'R':
                tft.writecommand(0x28);         // DISPOFF, then on
                delayMicroseconds(200);
                tft.writecommand(0x29);
                Serial.println("[TRIM] DISPOFF/DISPON");
                continue;
            case 's':
                tft.writecommand(0x11);         // SLPOUT, the heavier reset
                Serial.println("[TRIM] SLPOUT");
                continue;
            default:
                continue;
        }
        manual = true;
        trim_report("set", emu_hz);
    }
}
#endif


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
#ifdef PANEL_PORCH_FPA
    write_porch(PANEL_PORCH_FPA);
    Serial.printf("[TFT] porch trim %d + %d/64\n", (int)PANEL_PORCH_FPA,
                  (int)PANEL_PORCH_RATIO);
#endif
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
#ifdef PANEL_PORCH_FPA
    porch_advance();
    trim_console();
#endif
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

#ifdef PANEL_PROBE
// ─── Panel probe ────────────────────────────────────────────────────────────
// Tearing is the emulator's 59.727 fps beating against the panel's own
// free-running refresh. Nothing in software can lock the two together without
// a phase reference, and this panel offers exactly one candidate: ST7789's
// GSCAN (0x45), which returns the line the refresh is currently on. That read
// only arrives if the panel's SDO is wired to TFT_MISO on this board, which
// the CYD's documentation does not say either way.
//
// The discriminator is a pull test, not a plausibility test on the bytes. An
// undriven MISO follows whatever the ESP32's internal pull does, so the same
// register read is taken twice, once with each pull. Identical results mean
// the panel is driving the line; results that follow the pull mean nothing is
// on the other end and the whole vsync programme is dead on this hardware.
#ifndef TFT_MISO
#error "PANEL_PROBE needs TFT_MISO"
#endif

static void probe_read(uint8_t cmd, uint8_t* out, size_t n, uint32_t hz)
{
    SPIClass& spi = tft.getSPIinstance();
    size_t i;

    // ST7789 reads want a slower clock than writes do; the datasheet's read
    // cycle is 150 ns, so anything above about 6 MHz is out of spec.
    spi.beginTransaction(SPISettings(hz, MSBFIRST, SPI_MODE0));
    digitalWrite(TFT_DC, LOW);
    digitalWrite(TFT_CS, LOW);
    spi.transfer(cmd);
    digitalWrite(TFT_DC, HIGH);
    // The first byte back is the dummy clock every ST7789 read begins with.
    for (i = 0; i < n; i++) {
        out[i] = spi.transfer(0x00);
    }
    digitalWrite(TFT_CS, HIGH);
    spi.endTransaction();
}

/* Same read under each internal pull. Returns true if the panel drove it. */
static bool probe_read_driven(uint8_t cmd, uint8_t* out, size_t n, uint32_t hz)
{
    uint8_t up[8];
    uint8_t dn[8];
    size_t i;

    if (n > sizeof(up)) {
        n = sizeof(up);
    }
    pinMode(TFT_MISO, INPUT_PULLUP);
    probe_read(cmd, up, n, hz);
    pinMode(TFT_MISO, INPUT_PULLDOWN);
    probe_read(cmd, dn, n, hz);
    pinMode(TFT_MISO, INPUT);
    for (i = 0; i < n; i++) {
        out[i] = up[i];
    }
    return memcmp(up, dn, n) == 0;
}

/* GSCAN's 9-bit line counter, from the two bytes after the dummy. */
static uint16_t probe_scanline(uint32_t hz)
{
    uint8_t b[3];

    probe_read(0x45, b, 3, hz);
    return (uint16_t)(((b[1] & 0x01) << 8) | b[2]);
}

void display_panel_probe()
{
    static const uint8_t cmds[] = { 0x04, 0x09, 0x0A, 0x0B, 0x0C, 0x45 };
    static const uint32_t rates[] = { 2000000, 6000000 };
    uint8_t b[6];
    unsigned r;
    unsigned i;
    bool any_driven = false;

    Serial.printf("[PROBE] panel readback on MISO=%d\n", (int)TFT_MISO);
    for (r = 0; r < sizeof(rates) / sizeof(rates[0]); r++) {
        for (i = 0; i < sizeof(cmds); i++) {
            bool driven = probe_read_driven(cmds[i], b, 6, rates[r]);
            any_driven |= driven;
            Serial.printf("[PROBE] %u MHz cmd %02X -> %02X %02X %02X %02X %02X %02X  %s\n",
                          (unsigned)(rates[r] / 1000000u), cmds[i],
                          b[0], b[1], b[2], b[3], b[4], b[5],
                          driven ? "DRIVEN" : "floating");
        }
    }
    if (!any_driven) {
        Serial.println("[PROBE] MISO follows the pull: the panel drives nothing.");
        Serial.println("[PROBE] No scan-position reference. Software vsync is out.");
        return;
    }

    // The panel is answering. Time its refresh against the ESP32's clock by
    // watching the line counter wrap: the emulator's own rate is a known
    // 32768/548.62 = 59.7275 fps, so the difference is the beat the tear
    // rides on, and the ratio says how far a porch trim would have to move it.
    {
        const uint32_t hz = 2000000;
        uint16_t prev = probe_scanline(hz);
        uint16_t top = prev;
        uint32_t wraps = 0;
        int64_t first = 0;
        int64_t last = 0;
        int64_t deadline = esp_timer_get_time() + 2000000;

        while (esp_timer_get_time() < deadline) {
            uint16_t now = probe_scanline(hz);
            if (now > top) {
                top = now;
            }
            // A wrap is the only large step backwards the counter takes.
            if (now + 16 < prev) {
                last = esp_timer_get_time();
                if (wraps == 0) {
                    first = last;
                }
                wraps++;
            }
            prev = now;
        }
        if (wraps < 2) {
            Serial.printf("[PROBE] GSCAN answers but never wrapped (top=%u, "
                          "wraps=%u): counter is not live.\n",
                          (unsigned)top, (unsigned)wraps);
            return;
        }
        {
            double us = (double)(last - first) / (double)(wraps - 1);
            Serial.printf("[PROBE] panel %.4f Hz (period %.1f us, top line %u, "
                          "%u wraps)\n", 1000000.0 / us, us, (unsigned)top,
                          (unsigned)wraps);
            Serial.printf("[PROBE] emulator 59.7275 Hz -> beat %.4f Hz, "
                          "seam crosses every %.1f s\n",
                          1000000.0 / us - 59.7275,
                          1.0 / fabs(1000000.0 / us - 59.7275));
        }
    }
}
#endif
