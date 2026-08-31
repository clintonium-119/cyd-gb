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
