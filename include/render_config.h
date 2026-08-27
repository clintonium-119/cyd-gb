#pragma once

// Render geometry for the 24/16 (3/2) landscape scale. These are compile-time
// defaults only: NVS overrides GAME_X / GAME_Y at runtime through
// settings_t.game_x / settings_t.game_y, because ten hand-built units each land
// slightly differently behind the bezel.

// ─── Game area ──────────────────────────────────────────────────────────────
#define GAME_X    40   // Centred horizontally in 320 px (design §2.2); §11 item 5 (pixel pitch) may move it.
#define GAME_Y    12   // Centred vertically in 240 px (design §2.2); §11 item 5 (pixel pitch) may move it.
#define GAME_W   240   // 160 x 24/16 (design §2.1); §11 item 5 (pixel pitch) may move it.
#define GAME_H   216   // 144 x 24/16 (design §2.1); §11 item 5 (pixel pitch) may move it.
