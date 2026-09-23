#pragma once
#include <stdbool.h>

#include "settings.h"

// ─── Manual reader ──────────────────────────────────────────────────────────
// The running cartridge's scanned manual, drawn inside the game window. It
// opens on a whole-page overview, where Left and Right turn pages; A zooms
// into it a window-sized tile at a time, the D-pad walks the tiles and turns
// pages off their edges, A zooms back out, and B leaves. It reads only the
// manual it is handed the filename of — there is no route from here to any
// other game's.
//
// Pages stream from the card in bands of rows and go to the panel a row at a
// time, so no page is ever held in RAM: the heap cost is the page table and
// about 1.5 KB of row buffers, all freed on the way out.
//
// Calling contract, the same as menu_open(): pause the pipeline and take the
// display bus first. False, with one log line, when the manual will not open,
// fails validation or cannot get its buffers — nothing is drawn then. True
// once B has closed the reader, with every button already released so the
// same B cannot also act on the screen underneath.
bool manual_view_open(const settings_t* s, const char* rom_filename);
