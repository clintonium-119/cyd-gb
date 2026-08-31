#pragma once
#include <TFT_eSPI.h>
#include <stddef.h>

// Splash screens and the interim menus still draw through the driver
// directly; WS-07's menu rewrite is what removes this.
extern TFT_eSPI tft;

void display_init();
void display_set_backlight(uint8_t level);
void display_clear(uint16_t color = TFT_BLACK);

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
