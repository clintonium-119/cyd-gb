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

    // The whole bus lives on the CN1 plug (design §1.3, wiring PDF rev C):
    // SDA on IO22, SCL on IO27, 3.3V and GND on the same connector. Neither
    // pin is shared with the UART, so the old nine-pulse recovery for
    // SCL-on-TX0 is gone with the pin.
    Wire.begin(I2C_SDA, I2C_SCL);
    Wire.setClock(400000);   // design §1.3: 400 kHz is fine on these pins
}
