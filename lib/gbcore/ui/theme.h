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
#define UI_COL_TEXT      0xFFFF /* primary text, hint labels */
/* Prose at #DDDDDD and the hint pill at #555555, as chosen on glass; RGB565
 * holds them as #D8DCD8 and #505450, the nearest it has. */
#define UI_COL_SUB       0xDEFB /* help lines, descriptions, secondary text */
#define UI_COL_DIM       0x7BEF /* a disabled row */
#define UI_COL_PILL      0xFFFF /* the selected row's pill */
#define UI_COL_PILL_TEXT 0x0000 /* text on the pill */
#define UI_COL_PILL_DIM  0x6B4D /* a disabled row's text on the pill */
#define UI_COL_HINT_BG   0x52AA /* the hint footer's pill */
#define UI_COL_GLYPH     0xFFFF /* a button's circle in the hint footer */
#define UI_COL_GLYPH_FG  0x0000 /* the button's letter in it */
#define UI_COL_OK        0x07E0 /* a check that passed */
#define UI_COL_WARN      0xF800 /* errors only */
#define UI_COL_SLOT      0x1082 /* an empty image slot */

/* Chrome geometry, in pixels. */
#define UI_PAD         4  /* inset from the window edge */
#define UI_PILL_PAD    8  /* text inset inside a pill */
#define UI_TEXT_X      (UI_PAD + UI_PILL_PAD) /* where row, header and help
                                                 text all start */
#define UI_PILL_H_ROW  22 /* a menu row's pill */
#define UI_PILL_H_LIST 20 /* a list row's pill */
#define UI_PILL_R(h)   ((h) / 2)
#define UI_IMG_R       4  /* corner radius of a cover, screenshot or thumbnail */
#define UI_HEADER_H    24
#define UI_HELP_H      18 /* one line of prose and the gap under it */
#define UI_FOOT_H      20
#define UI_GLYPH_D     12 /* a button glyph's circle */
#define UI_HINT_GAP    6  /* between hint pairs, and glyph to label */

/* Font roles, as the font ids canvas.h names. The hints stay the 8 px GLCD
 * font, whose 7 px capitals sit in a 12 px circle with room to spare. */
#define UI_FONT_HEADER UI_FONT_HEAD
#define UI_FONT_LIST   UI_FONT_BOLD
#define UI_FONT_VAL    UI_FONT_VALUE
#define UI_FONT_HELP   UI_FONT_TEXT
#define UI_FONT_DESC   UI_FONT_TEXT
#define UI_FONT_HINT   UI_FONT_SMALL
#define UI_FONT_NOTICE UI_FONT_HEAD
