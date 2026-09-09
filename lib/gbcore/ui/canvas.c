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
    default:
        return 0;
    }
}
