#pragma once
#include <stdint.h>

// Button masks. Bits 0-7 mirror the Game Boy joypad order the emulator
// expects, so the debounced word can go straight to emu_set_joypad(). Menu
// entry is not a button — it is a Start+Select combo event from
// lib/gbcore/input/combo.h, and nothing out-of-band appears here.
#define GB_BTN_RIGHT   0x01
#define GB_BTN_LEFT    0x02
#define GB_BTN_UP      0x04
#define GB_BTN_DOWN    0x08
#define GB_BTN_A       0x10
#define GB_BTN_B       0x20
#define GB_BTN_SELECT  0x40
#define GB_BTN_START   0x80

void button_init();
void button_update();
uint16_t button_get_buttons();
