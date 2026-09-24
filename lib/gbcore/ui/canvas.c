#include "ui/canvas.h"

uint8_t ui_font_height(uint8_t font)
{
    switch (font) {
    case UI_FONT_SMALL:
        return 8;
    case UI_FONT_ROW:
        return 16;
    case UI_FONT_TITLE:
        return 26;
    case UI_FONT_TEXT:
        return 13; /* 10 above the baseline, 3 below */
    case UI_FONT_BOLD:
    case UI_FONT_VALUE:
        return 15; /* 12 above, 3 below */
    case UI_FONT_HEAD:
        return 20; /* 16 above, 4 below */
    default:
        return 0;
    }
}

uint8_t ui_font_cap(uint8_t font)
{
    switch (font) {
    case UI_FONT_SMALL:
        return 7;
    case UI_FONT_ROW:
        return 11;
    case UI_FONT_TITLE:
        return 18;
    case UI_FONT_TEXT:
        return 9;
    case UI_FONT_BOLD:
    case UI_FONT_VALUE:
        return 11;
    case UI_FONT_HEAD:
        return 14;
    default:
        return 0;
    }
}

uint8_t ui_font_lead(uint8_t font)
{
    switch (font) {
    case UI_FONT_ROW:
        return 2;
    case UI_FONT_TITLE:
        return 4;
    case UI_FONT_TEXT:
    case UI_FONT_BOLD:
    case UI_FONT_VALUE:
        return 1;
    case UI_FONT_HEAD:
        return 2;
    default:
        return 0;
    }
}
