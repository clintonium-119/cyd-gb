#pragma once
#include <stdint.h>

// Every pin below appears in the ESP32-2432S024 pin map (design §1.2 / §1.4).
// Each line carries that table's status and the §11 bench item that settles it.
// Render geometry lives in render_config.h, not here.

// ─── Display (onboard) ──────────────────────────────────────────────────────
#define TFT_PIN_BL     21   // Backlight PWM (§1.2, confirmed). Panel pins are set in platformio.ini.

// Landscape: the panel is 320 px horizontal x 240 px vertical once rotated
// (§2.1). TFT_WIDTH / TFT_HEIGHT in platformio.ini still describe the
// controller's native portrait orientation — rotation, not those, decides
// which way round the image sits.
#define SCREEN_W      320
#define SCREEN_H      240
#define TFT_ROTATION_LANDSCAPE 1   // §11 confirms 1 vs 3 once a board sits in a shell;
                                   // 3 is the same landscape flipped end for end, so this
                                   // flips if the USB socket lands on the wrong side.

// ─── SD card (onboard, VSPI, now exclusive to SD) ───────────────────────────
#define SD_PIN_CS       5   // §1.2, confirmed
#define SD_PIN_MOSI    23   // §1.2, confirmed
#define SD_PIN_MISO    19   // §1.2, confirmed
#define SD_PIN_SCK     18   // §1.2, confirmed

// ─── I²C (buttons, NFC) ─────────────────────────────────────────────────────
// The whole bus lives on the CN1 plug: GND / IO22 / IO27 / 3.3V (§1.2, §1.3,
// verified — wiring PDF rev C). Neither pin is shared with the UART, so no
// bus recovery is needed.
#define I2C_SDA        22   // CN1 (verified, rev C)
#define I2C_SCL        27   // CN1 (verified, rev C)
#define BTN_I2C_ADDR 0x20   // MCP23017, A0-A2 to GND (§1.4)

// ─── Backlight adjustment ───────────────────────────────────────────────────
// The Select+Left/Right combo steps the backlight by BL_STEP and clamps to
// [BL_MIN, 255], giving 8 levels. The floor is not 0 on purpose: a unit
// mounted in a shell at brightness 0 looks dead, and the operators cannot
// recover it by sight. Whether BL_MIN is visible in daylight is §11 bench
// work; raising it is a one-line change here.
#define BL_STEP        32
#define BL_MIN         32

// ─── Audio ──────────────────────────────────────────────────────────────────
// No amp-enable pin exists. The vendor datasheet calls IO4 the amp enable, but
// the bench proved it is not (§1.6, wiring PDF rev C); its real function is
// unknown, so leave IO4 unused (§13). With no hardware mute, WS-08's volume
// "off" holds the DAC at 128 (mid-scale) instead.

// ─── Power ──────────────────────────────────────────────────────────────────
#define BAT_ADC_PIN    34   // Battery sense (§1.2). Divider ratio undocumented — §11 item 6.

// ─── Status LEDs ────────────────────────────────────────────────────────────
// §1.2 gives no LED pin on this board. The fork drove IO4, which must stay
// unused (§13); all three are -1 and every use site guards on >= 0.
#define LED_R_PIN      -1
#define LED_G_PIN      -1
#define LED_B_PIN      -1

// ─── Game Boy ───────────────────────────────────────────────────────────────
#define GB_SCREEN_W   160
#define GB_SCREEN_H   144
