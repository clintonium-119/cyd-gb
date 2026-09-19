#pragma once
// Force-included into every translation unit of the gnuboy environments (see
// platformio.ini), because the vendored lib/gnuboy/lcd.c is its own
// translation unit and cannot see anything the bridge's .cpp defines.
//
// The per-line hook in that file is a macro so it costs nothing when nobody
// defines it. Pointing it at a function needs the function declared where
// lcd.c is compiled, and this is the smallest thing that does it without a
// second modification to the vendored source.

#ifdef __cplusplus
extern "C" {
#endif

/* The finished 160-byte line and its number, straight from lcd_renderline().
 * Implemented by whoever is driving gnuboy: src/emulator_bridge_gnuboy.cpp on
 * the device, lib/gnuboy_runner/ on the host. */
void emu_gnuboy_line(const unsigned char* line, int index);

#ifdef __cplusplus
}
#endif

#define GNUBOY_DRAW_LINE(line, index) emu_gnuboy_line((line), (index))
