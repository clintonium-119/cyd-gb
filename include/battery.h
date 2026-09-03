#pragma once
#include <stdint.h>
#include <stdbool.h>

// Battery sense on IO34 (design §1.5). IO34 is ADC1 channel 6: input-only,
// with no pull-up or output driver to disable, and on ADC1 rather than ADC2
// so a reading is unaffected by Wi-Fi being up.
//
// This module reads a voltage and nothing else — it decides nothing, keeps no
// history and averages nothing. One sample per BAT_SAMPLE_MS is enough
// because the only consumer is the low-battery save latch, and that latch
// carries its own hysteresis: a faster sample would give it nothing to catch.
//
// Both numbers this module depends on, BAT_DIVIDER and BAT_LOW_MV, are
// placeholders in hw_config.h until §11 item 6 meters a cell. battery_init()
// prints one reading so that bench run has something to compare against.
void battery_init();

// Cell millivolts: the calibrated pin voltage scaled back up through the
// board's divider. 0 if the reading failed.
uint16_t battery_read_mv();

// One reading per BAT_SAMPLE_MS. Returns true with fresh cell millivolts in
// *mv when a sample was due, and false with *mv untouched otherwise, so the
// emulation loop can call it every frame and pay for one ADC read a second.
bool battery_poll(uint32_t now_ms, uint16_t* mv);
