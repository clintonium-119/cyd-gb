#pragma once
#include <stdint.h>

// Every pin below appears in the ESP32-2432S024 pin map (design §1.2 / §1.4).
// Each line carries that table's status and the §11 bench item that settles it.
// Render geometry lives in render_config.h, not here.

// ─── Display (onboard) ──────────────────────────────────────────────────────
#define TFT_PIN_BL     21   // Backlight PWM (§1.2, confirmed). Panel pins are set in platformio.ini.
#define SCREEN_W       240
#define SCREEN_H       320

// ─── SD card (onboard, VSPI, now exclusive to SD) ───────────────────────────
#define SD_PIN_CS       5   // §1.2, confirmed
#define SD_PIN_MOSI    23   // §1.2, confirmed
#define SD_PIN_MISO    19   // §1.2, confirmed
#define SD_PIN_SCK     18   // §1.2, confirmed

// ─── I²C (buttons, NFC) ─────────────────────────────────────────────────────
#define I2C_SDA        27   // SPI header pin 1 (§1.2, proposed)
#define I2C_SCL         1   // UART TX0 (§1.2, proposed — §1.3). If §11 item 2 meters the
                            // 4th EXP pad as IO22, move SCL there and drop bus recovery.
#define BTN_I2C_ADDR 0x20   // MCP23017, A0-A2 to GND (§1.4)

// ─── Audio ──────────────────────────────────────────────────────────────────
#define AMP_EN_PIN      4   // Amplifier enable, HIGH = on (§1.2). Never drive this for
                            // anything else — §13. §11 item 3 rates the onboard amp.

// ─── Power ──────────────────────────────────────────────────────────────────
#define BAT_ADC_PIN    34   // Battery sense (§1.2). Divider ratio undocumented — §11 item 6.

// ─── Status LEDs ────────────────────────────────────────────────────────────
// §1.2 gives no LED pin on this board. The fork drove IO4, which is AMP_EN_PIN
// here; all three are -1 and every use site guards on >= 0.
#define LED_R_PIN      -1
#define LED_G_PIN      -1
#define LED_B_PIN      -1

// ─── Game Boy ───────────────────────────────────────────────────────────────
#define GB_SCREEN_W   160
#define GB_SCREEN_H   144
