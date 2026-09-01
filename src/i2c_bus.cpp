#include "i2c_bus.h"
#include "hw_config.h"
#include <Arduino.h>
#include <Wire.h>

static bool bus_up = false;

#if I2C_SCL == 1
// SCL shares TX0 (design §1.3): the ROM bootloader clocks that pin at every
// power-up, and a device left mid-transaction by a warm reset can still be
// holding SDA low. Nine clocks let any such device finish the byte it thinks
// it is sending, and the STOP that follows returns the bus to idle. Gated at
// compile time because the design's contingency — SCL on IO22, see
// hw_config.h — removes the need entirely rather than making it conditional.
static void i2c_bus_recover() {
    pinMode(I2C_SDA, INPUT_PULLUP);           // release SDA; the slave drives it
    pinMode(I2C_SCL, OUTPUT_OPEN_DRAIN);
    digitalWrite(I2C_SCL, HIGH);
    delayMicroseconds(5);

    for (int i = 0; i < 9; i++) {
        digitalWrite(I2C_SCL, LOW);
        delayMicroseconds(5);
        digitalWrite(I2C_SCL, HIGH);
        delayMicroseconds(5);
    }

    // STOP: SDA low while SCL is high, then SDA released.
    pinMode(I2C_SDA, OUTPUT_OPEN_DRAIN);
    digitalWrite(I2C_SDA, LOW);
    delayMicroseconds(5);
    digitalWrite(I2C_SDA, HIGH);
    delayMicroseconds(5);

    pinMode(I2C_SDA, INPUT);
    pinMode(I2C_SCL, INPUT);
}
#endif

void i2c_bus_init() {
    if (bus_up) {
        return;
    }
    bus_up = true;

#if I2C_SCL == 1
    i2c_bus_recover();
#endif

    Wire.begin(I2C_SDA, I2C_SCL);
    Wire.setClock(400000);   // design §1.3: 400 kHz is fine on these pins
}
