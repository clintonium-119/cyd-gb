#include "i2c_bus.h"
#include "hw_config.h"
#include <Arduino.h>
#include <Wire.h>

static bool bus_up = false;

void i2c_bus_init() {
    if (bus_up) {
        return;
    }
    bus_up = true;

    // The whole bus lives on the CN1 plug (design §1.3): SDA on IO22, SCL on
    // IO21 as the board's silkscreen prints it, 3.3V and GND on the same
    // connector. The wiring PDF rev C says IO27 for SCL; the silkscreen wins. Neither
    // pin is shared with the UART, so the old nine-pulse recovery for
    // SCL-on-TX0 is gone with the pin.
    Wire.begin(I2C_SDA, I2C_SCL);
    Wire.setClock(400000);   // design §1.3: 400 kHz is fine on these pins

    // Bench aid: log every address that ACKs, so a silent peripheral reads
    // as wiring (absent here) or protocol (present, but no answer). Two
    // passes: a sleeping PN532 may miss the first address match.
    for (int pass = 0; pass < 2; pass++) {
        Serial.print("[i2c] ack:");
        for (uint8_t addr = 1; addr < 0x78; addr++) {
            Wire.beginTransmission(addr);
            if (Wire.endTransmission() == 0) {
                Serial.printf(" 0x%02X", addr);
            }
        }
        Serial.println();
    }
}
