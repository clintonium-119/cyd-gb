#pragma once
#include <TFT_eSPI.h>
#include <stddef.h>

// Boot screens, the in-game menu and the save toast draw through the driver
// directly, between display_bus_acquire() and display_bus_release().
extern TFT_eSPI tft;

void display_init();
void display_set_backlight(uint8_t level);
void display_clear(uint16_t color = TFT_BLACK);

// ─── Wrapped text ───────────────────────────────────────────────────────────
// Draws s across up to max_rows rows of `font`, breaking wherever max_w runs
// out rather than at word boundaries — a file name has no useful break
// points. The row pitch is the font's height plus two, so font 2 gives the
// 18-px rows the boot screens have always used and font 1 gives 10.
//
// The text datum and colour are the caller's: cx is whatever x that datum
// makes it (a centre for MC_DATUM, a left edge for TL_DATUM). Returns the y
// below the last row drawn, so consecutive calls stack.
int16_t display_draw_wrapped(const char* s, int16_t cx, int16_t top,
                             int16_t max_w, uint8_t max_rows, uint8_t font);

// ─── Frame path ─────────────────────────────────────────────────────────────
// One address window per frame, then a pushPixels per scaled row block
// (design §2.5), replacing the fork's per-destination-line pushImage. The
// caller supplies the viewport origin so the per-unit NVS nudge applies;
// GAME_W x GAME_H comes from render_config.h. Bracket every frame:
//
//   display_frame_begin(x, y);
//   ... display_push_rows(block, rows * width) per block ...
//   display_frame_end();
//
// The display module stays geometry-agnostic: it pushes whatever pixel count
// it is handed, and the total across a frame must be exactly GAME_W * GAME_H.
void display_frame_begin(int16_t x, int16_t y);
void display_push_rows(const uint16_t* px, size_t n);
void display_frame_end();

// ─── DMA push ───────────────────────────────────────────────────────────────
// The variant the push task uses, so the SPI transfer of one block overlaps
// the emulation and scaling of the next (design §3.3). The blocking
// display_push_rows above stays for the splash and interim menu paths, which
// are not on the frame path and are not worth a DMA transaction.
//
// Two contract points, both consequences of how the driver implements this:
//
//   * The pushed buffer belongs to the driver until display_dma_wait()
//     returns. Refilling it before then corrupts the transfer in flight.
//   * setSwapBytes(true) is in force, and the DMA path byte-swaps the buffer
//     IN PLACE on the CPU before starting the transfer. A pushed buffer is
//     therefore consumed, not merely read: its contents are no longer the
//     native-order pixels the scaler wrote. Never push the same buffer twice,
//     and never read one back expecting the scaler's values.
//
// The pointer is non-const for exactly that reason.
void display_push_rows_dma(uint16_t* px, size_t n);

// Block until every queued transfer has completed. The push task calls this
// at frame end so display_frame_end()'s endWrite cannot truncate a transfer
// still in flight.
void display_dma_wait();

// ─── Bus handover ───────────────────────────────────────────────────────────
// Menu and diagnostic drawing goes through `tft` directly, which means it
// cannot share the bus with an open frame window. Acquire waits out any
// transfer and closes the window; release is the documented counterpart —
// the next display_frame_begin() reopens one, so it has nothing to undo.
//
// Calling contract: pause the producer and wait for the queue to report
// drained BEFORE calling display_bus_acquire(), or blocks committed but not
// yet pushed will be drawn over the menu.
void display_bus_acquire();
void display_bus_release();
