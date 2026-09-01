#pragma once

// Shared I²C bring-up. Both I²C peripherals — the MCP23017 button expander
// (0x20) and the PN532 tag reader (0x24) — sit behind this one call; nothing
// else may call Wire.begin().
//
// Sharing contract: the buttons are polled once per frame from the emulation
// loop, and the tag read happens at boot before emulation starts. The two
// therefore never contend for the bus, and no arbitration or mutex exists
// here. A future reader that polls during emulation would need one.
//
// Idempotent: the second and later calls are no-ops, so every user can call
// it in its own init without ordering rules.
void i2c_bus_init();
