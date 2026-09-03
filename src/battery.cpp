#include "battery.h"
#include "hw_config.h"
#include <Arduino.h>

// When the next sample is due. Compared with a signed difference so the
// schedule survives the millis() rollover, the idiom the coalesced settings
// save uses.
static uint32_t next_ms = 0;

void battery_init() {
    // A full 4.2 V cell through the assumed 2:1 divider is 2.1 V at the pin,
    // which needs the widest attenuation the ADC offers. 11 dB tops out
    // around 2.5 V, so it covers the whole cell range with headroom.
    analogSetPinAttenuation(BAT_ADC_PIN, ADC_11db);

    // One reading in the boot log, so §11 item 6 has a printed number to
    // meter the cell against without instrumenting a running unit.
    Serial.printf("[BAT] %u mV\n", battery_read_mv());
}

uint16_t battery_read_mv() {
    // analogReadMilliVolts() applies the module's eFuse ADC calibration, so
    // the pin voltage needs no correction of its own — only the divider.
    uint32_t pin_mv = analogReadMilliVolts(BAT_ADC_PIN);
    uint32_t cell_mv = (uint32_t)((float)pin_mv * BAT_DIVIDER);
    if (cell_mv > UINT16_MAX) {
        cell_mv = UINT16_MAX;
    }
    return (uint16_t)cell_mv;
}

bool battery_poll(uint32_t now_ms, uint16_t* mv) {
    if (mv == nullptr) {
        return false;
    }
    if ((int32_t)(now_ms - next_ms) < 0) {
        return false;
    }
    next_ms = now_ms + BAT_SAMPLE_MS;
    *mv = battery_read_mv();
    return true;
}
