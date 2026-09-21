#include "render/panel_rate.h"

/* Total lines a porch setting puts in a frame, the term everything here
 * scales against. */
static double total_lines(uint8_t fpa, uint8_t ratio)
{
    return PANEL_RATE_BASE_LINES + (double)fpa
         + (double)ratio / (double)PANEL_RATE_RATIO_DEN;
}

double panel_rate_hz(const panel_anchor_t* a, uint8_t fpa, uint8_t ratio)
{
    return a->hz * total_lines(a->fpa, a->ratio) / total_lines(fpa, ratio);
}

bool panel_rate_porch_for(const panel_anchor_t* a, double hz, uint8_t* fpa,
                          uint8_t* ratio)
{
    double lines;
    long sixtyfourths;

    if (hz < 1.0) {
        return false;
    }
    lines = a->hz * total_lines(a->fpa, a->ratio) / hz - PANEL_RATE_BASE_LINES;
    if (lines < 1.0) {
        return false;
    }
    sixtyfourths = (long)(lines * (double)PANEL_RATE_RATIO_DEN + 0.5);
    /* Bounded after rounding, not before: PORCTRL's front porch is 7 bits, so
     * 127 is the largest value it holds, and the divider programs fpa + 1 on
     * its long frames. 126 + 63/64 is therefore the last expressible setting
     * and 127 + 0/64 is one past the end — a distinction the pre-rounded
     * number cannot make. */
    if (sixtyfourths / PANEL_RATE_RATIO_DEN > 126) {
        return false;
    }
    *fpa = (uint8_t)(sixtyfourths / PANEL_RATE_RATIO_DEN);
    *ratio = (uint8_t)(sixtyfourths % PANEL_RATE_RATIO_DEN);
    return true;
}

uint8_t panel_rate_next_porch(uint8_t* acc, uint8_t fpa, uint8_t ratio)
{
    *acc = (uint8_t)(*acc + ratio);
    if (*acc >= PANEL_RATE_RATIO_DEN) {
        *acc = (uint8_t)(*acc - PANEL_RATE_RATIO_DEN);
        return (uint8_t)(fpa + 1);
    }
    return fpa;
}
