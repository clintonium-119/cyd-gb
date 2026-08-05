#pragma once

void bt_scanner_enter();
// Returns true when scanner requests exit back to ROM menu.
bool bt_scanner_loop();
// Frees scanner-side resources (especially BLE heap) before launching emulator.
void bt_scanner_shutdown();
