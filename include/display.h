#pragma once
#include <TFT_eSPI.h>
#include <stddef.h>

#include "render_config.h"   // PUSH_ORDER, GAME_W / GAME_H
#include "ui/canvas.h"

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

// ─── Canvas ─────────────────────────────────────────────────────────────────
// The injected draw seam bound to `tft`: window-relative coordinates in,
// panel coordinates out. Both layout modules in gbcore paint through it, and
// this is the only implementation of it — the origin is stored rather than
// passed per primitive, because the seam's signature is fixed by two layout
// modules and a host test.
//
// A later call with a new origin re-points the same canvas, so a caller that
// has one pointer never has a stale origin. Valid between
// display_bus_acquire() and display_bus_release(), or before the frame path
// exists.
const ui_canvas_t* display_canvas(int16_t ox, int16_t oy);

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
//
// Orientation is the frame path's, not the UI's, and which one depends on
// PUSH_ORDER:
//
//   PUSH_ROW  landscape, TFT_ROTATION_LANDSCAPE. A window row is GAME_W
//             pixels of the screen's horizontal, which is the panel's
//             gate-line axis, so one row crosses every gate line — the reason
//             the tear reads as a diagonal staircase.
//   PUSH_COL  portrait, TFT_ROTATION_PORTRAIT. The window is transposed —
//             GAME_H along a gate line by GAME_W across them — so filling it
//             advances along the scan. The viewport origin still arrives in
//             LANDSCAPE space, because it is the per-unit NVS nudge and the
//             D-pad that sets it means landscape; display_frame_begin() maps
//             it through the rotation pair.
//
// Under PUSH_COL the caller must hand its output columns over in the order
// the window fills, which FRAME_COLS_DESCENDING states: descending landscape
// x with the default rotation pair. Each push is a whole number of output
// columns of GAME_H pixels.
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
//   * The DMA path byte-swaps the buffer IN PLACE on the CPU before starting
//     the transfer. A pushed buffer is therefore consumed, not merely read:
//     its contents are no longer the native-order pixels the scaler wrote.
//     Never push the same buffer twice, and never read one back expecting the
//     scaler's values. The swap happens before the wait for the previous
//     transfer, so it costs transfer time rather than idle bus time; the
//     driver's own setSwapBytes(true) — which the menu's cover path needs —
//     is cleared across the queueing call and restored after it.
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
// Acquire also owns the orientation. Under PUSH_COL the frame path leaves the
// panel in portrait, and every UI surface draws in landscape and knows
// nothing of that, so acquire restores landscape and the next
// display_frame_begin() takes portrait back. The switch is a single MADCTL
// write, and it happens at most twice per menu round-trip rather than once a
// frame: display_frame_begin() asks every frame and gets a no-op unless the
// orientation actually moved.
//
// Calling contract: pause the producer and wait for the queue to report
// drained BEFORE calling display_bus_acquire(), or blocks committed but not
// yet pushed will be drawn over the menu.
void display_bus_acquire();
void display_bus_release();

// ─── Fill-direction probe (bench only) ──────────────────────────────────────
// Pushes a pattern of known push ORDER through the real column-major frame
// window, so the panel reports which way the window fills in each axis — the
// one thing about the rotation pairing that cannot be inferred. Runs instead
// of the boot and never returns; see src/display.cpp for how to read it.
#ifdef PANEL_FILL_PROBE
void display_fill_probe();
#endif

// ─── Panel probe (bench only) ───────────────────────────────────────────────
// Answers the one question a software vsync depends on: does this panel drive
// MISO, so the firmware can read its scan position (ST7789 GSCAN, 0x45) and
// know where the refresh is? Prints its findings and returns; the boot carries
// on, so the same image can be built with DEV_ROM_PATH and played afterwards.
//
//   PLATFORMIO_BUILD_FLAGS='-DPANEL_PROBE -DDEV_ROM_PATH="..."' pio run -e cyd-gnuboy
#ifdef PANEL_PROBE
void display_panel_probe();
#endif
