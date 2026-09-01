#include "button_input.h"
#include "hw_config.h"
#include "i2c_bus.h"
#include <Wire.h>

// MCP23017 button expander (design §1.4): the button PCB's eight switches are
// on GPA0-GPA7, active LOW against the expander's internal pull-ups. Only
// three registers are needed, so this is raw Wire traffic rather than a
// library. Port B is unused and its registers are never touched.
//
// Register addresses below are the BANK=0 map (the power-on default, which
// nothing here changes).
static const uint8_t MCP_IODIRA = 0x00;   // 1 = input
static const uint8_t MCP_GPPUA  = 0x0C;   // 1 = 100k pull-up enabled
static const uint8_t MCP_GPIOA  = 0x12;   // port A pin states

// No debounce here: it belongs to the host-tested combo state machine, which
// sees this raw word.
static volatile uint16_t cur_btns = 0;

static void mcp_write_reg(uint8_t reg, uint8_t val) {
    Wire.beginTransmission((uint8_t)BTN_I2C_ADDR);
    Wire.write(reg);
    Wire.write(val);
    Wire.endTransmission();
}

void button_init() {
    i2c_bus_init();

    // Both values are the power-on defaults for IODIRA; written anyway so a
    // warm reset that left the expander configured for output lands in a
    // known state.
    mcp_write_reg(MCP_IODIRA, 0xFF);
    mcp_write_reg(MCP_GPPUA, 0xFF);
}

void button_update() {
    Wire.beginTransmission((uint8_t)BTN_I2C_ADDR);
    Wire.write(MCP_GPIOA);
    Wire.endTransmission();

    Wire.requestFrom((uint8_t)BTN_I2C_ADDR, (uint8_t)1);
    if (Wire.available() < 1) {
        // Hold the last good word. Reporting 0 here would look like every
        // button releasing and then re-pressing on the next good read, which
        // a combo state machine reads as real edges.
        return;
    }

    uint8_t raw = Wire.read();
    raw = ~raw;  // active LOW: a pressed switch pulls its GPA pin to ground.

    uint16_t buttons = 0;
    if (raw & (1 << 0)) buttons |= GB_BTN_UP;
    if (raw & (1 << 1)) buttons |= GB_BTN_DOWN;
    if (raw & (1 << 2)) buttons |= GB_BTN_LEFT;
    if (raw & (1 << 3)) buttons |= GB_BTN_RIGHT;
    if (raw & (1 << 4)) buttons |= GB_BTN_A;
    if (raw & (1 << 5)) buttons |= GB_BTN_B;
    if (raw & (1 << 6)) buttons |= GB_BTN_START;
    if (raw & (1 << 7)) buttons |= GB_BTN_SELECT;
    cur_btns = buttons;
}

uint16_t button_get_buttons() {
    return cur_btns;
}
