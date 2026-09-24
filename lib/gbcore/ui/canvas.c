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
    case UI_FONT_BOLD:
        return 18; /* 13 above the baseline, 5 below */
    case UI_FONT_HEAD:
        return 23; /* 17 above, 6 below */
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
    case UI_FONT_BOLD:
        return 12;
    case UI_FONT_HEAD:
        return 17;
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
        return 1;
    default:
        return 0;
    }
}
