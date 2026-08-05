#pragma once
#include <stdint.h>

// ─── Pins ───────────────────────────────────────────────────────────────────
#define TFT_PIN_BL     21
#define SCREEN_W       240
#define SCREEN_H       320

#define TOUCH_PIN_CS    33
#define TOUCH_PIN_IRQ   36
#define TOUCH_PIN_MOSI  32
#define TOUCH_PIN_MISO  39
#define TOUCH_PIN_CLK   25

#define SD_PIN_CS 5
#define SD_PIN_MOSI 23
#define SD_PIN_MISO 19
#define SD_PIN_SCK 18

#define BUTTON_I2C_ADDR 0x20
#define BUTTON_I2C_SDA  16
#define BUTTON_I2C_SCL  17

#define LED_R_PIN 4
#define LED_G_PIN -1
#define LED_B_PIN -1

// ─── GameBoy ────────────────────────────────────────────────────────────────
#define GB_SCREEN_W 160
#define GB_SCREEN_H 144

// Game area: top of the screen (keeps same relative area)
// Previously 192 of 240 (0.8). For portrait (320px tall) use 0.8*320=256
#define GAME_H 256
#define CTRL_Y 256
#define CTRL_H 64

// ─── Touch Zones (y=192..240 control bar) ───────────────────────────────────
// D-pad left (mapped for portrait layout)
#define DPAD_CX    38
#define DPAD_CY   288
#define DPAD_R     17

// A = right-upper, B = right-lower (FIXED - was swapped)
#define BTN_A_X   214
#define BTN_A_Y   275
#define BTN_A_R    15

#define BTN_B_X   184
#define BTN_B_Y   301
#define BTN_B_R    15

// Start / Select center
#define BTN_ST_X  124
#define BTN_ST_Y  288
#define BTN_ST_W   29
#define BTN_ST_H   27

#define BTN_SE_X  86
#define BTN_SE_Y  288
#define BTN_SE_W   29
#define BTN_SE_H   27

// Menu top-right
#define BTN_M_X   229
#define BTN_M_Y    16
#define BTN_M_R    11
