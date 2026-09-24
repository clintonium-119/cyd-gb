#pragma once
// The one place the UI's colours and chrome geometry are named.
//
// Every surface — the setup notices, the diagnostic pages, the in-game menu,
// the manual view and the cartridge writer — draws from these. One fixed
// theme: plain white text on black, a white rounded pill behind the selected
// row only, grey for secondary text and hints, red for errors and nothing
// else.
//
// The sizes are NextUI's 640x480 constants scaled by about 0.4 to the game
// window, then tuned on glass. They are macros, not a struct: nothing switches
// theme at run time.
//
// Pure C, no includes beyond <stdint.h>.

#include <stdint.h>

/* RGB565 colours. */
#define UI_COL_BG        0x0000 /* screen background */
#define UI_COL_TEXT      0xFFFF /* primary text */
#define UI_COL_DIM       0x7BEF /* secondary text, the help line, a disabled row */
#define UI_COL_HINT      0x9CD3 /* hint labels in the button-hint footer */
#define UI_COL_PILL      0xFFFF /* the selected row's pill */
#define UI_COL_PILL_TEXT 0x0000 /* text on the pill */
#define UI_COL_PILL_DIM  0x6B4D /* a disabled row's text on the pill */
#define UI_COL_GLYPH     0x2124 /* a button glyph's pill in the hint bar */
#define UI_COL_OK        0x07E0 /* a check that passed */
#define UI_COL_WARN      0xF800 /* errors only */
#define UI_COL_SLOT      0x1082 /* an empty image slot */

/* Chrome geometry, in pixels. */
#define UI_PAD         4  /* inset from the window edge */
#define UI_PILL_PAD    8  /* text inset inside a pill */
#define UI_PILL_H_ROW  22 /* a menu row's pill, font 2 */
#define UI_PILL_H_LIST 18 /* a list row's pill, font 2 */
#define UI_PILL_R(h)   ((h) / 2)
#define UI_IMG_R       4  /* corner radius of a cover, screenshot or thumbnail */
#define UI_HEADER_H    18
#define UI_HELP_H      10
#define UI_FOOT_H      18
#define UI_GLYPH_D     12 /* a button glyph's pill height */
#define UI_HINT_GAP    6  /* between hint pairs, and glyph to label */

/* Font roles, as the built-in font ids canvas.h names. */
#define UI_FONT_HEADER UI_FONT_ROW
#define UI_FONT_HELP   UI_FONT_SMALL
#define UI_FONT_HINT   UI_FONT_SMALL
#define UI_FONT_NOTICE UI_FONT_TITLE
