#pragma once
#include "hw_config.h"

// Render geometry for the landscape k/16 scale. 160 and 144 are both divisible
// by 16, so any scale of the form k/16 gives integer output on both axes;
// vertical extent caps it at k <= 26 (design §2.1).

// ─── Scale ──────────────────────────────────────────────────────────────────
// Compile-time constant, deliberately NOT a runtime setting: the scaler's
// block geometry and every buffer size derive from it. 24/16 (3/2) ships as
// the default per §2.1's "build 24/16 first"; §11 item 8 (emulation-alone
// frame time) and §2.1's SPI comparison settle whether 26/16 is affordable,
// and flipping this one line is the whole experiment.
#define SCALE_K 24

// The gbcore geometry that matches SCALE_K, so Arduino-side callers never
// repeat the number. Expands to an enum name — the consumer includes
// render/scaler.h itself; this header stays free of gbcore.
#if SCALE_K == 24
#define SCALE_GEOM SCALER_GEOM_24_16   // 3/2:  240 x 216, 90% of the rows
#elif SCALE_K == 26
#define SCALE_GEOM SCALER_GEOM_26_16   // 13/8: 260 x 234, 97.5% of the rows
#else
#error "SCALE_K must be 24 or 26 — design §2.1 tabulates only those two, and 9k <= 240 caps k at 26."
#endif

// ─── Game area ──────────────────────────────────────────────────────────────
// Derived from SCALE_K, so 24 and 26 both land on §2.1's table: 240 x 216 at
// GAME_X/Y 40,12 and 260 x 234 at 30,3. GAME_X / GAME_Y are compile-time
// defaults only: NVS overrides them at runtime through settings_t.game_x /
// settings_t.game_y, because ten hand-built units each land slightly
// differently behind the bezel (design §2.2).
#define GAME_W   (GB_SCREEN_W * SCALE_K / 16)   // 160 x k/16 (design §2.1)
#define GAME_H   (GB_SCREEN_H * SCALE_K / 16)   // 144 x k/16 (design §2.1)
#define GAME_X   ((SCREEN_W - GAME_W) / 2)      // Centred (design §2.2); §11 item 5 (pixel pitch) may move it.
#define GAME_Y   ((SCREEN_H - GAME_H) / 2)      // Centred (design §2.2); §11 item 5 (pixel pitch) may move it.
