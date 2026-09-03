#include "autosave.h"

#include <stddef.h>

int autosave_init(autosave_state_t* s, uint32_t save_size)
{
    if (s == NULL) {
        return AUTOSAVE_ERR_ARGS;
    }
    s->save_size = save_size;
    s->wrote = 0;
    s->dirty = 0;
    s->last_write_ms = 0;
    s->batt_low = 0;
    return AUTOSAVE_OK;
}

void autosave_tick(autosave_state_t* s, uint32_t now_ms)
{
    if (s == NULL) {
        return;
    }
    if (!s->wrote) {
        return;
    }
    s->wrote = 0;
    s->dirty = 1;
    s->last_write_ms = now_ms;
}

bool autosave_dirty(const autosave_state_t* s)
{
    if (s == NULL) {
        return false;
    }
    return s->dirty != 0;
}

bool autosave_idle_due(const autosave_state_t* s, uint32_t now_ms)
{
    if (s == NULL) {
        return false;
    }
    if (!s->dirty) {
        return false;
    }
    // Signed difference so the rule survives the caller's ms counter
    // rolling over, the idiom the coalesced settings save uses.
    return (int32_t)(now_ms - s->last_write_ms) >= AUTOSAVE_IDLE_MS;
}

void autosave_defer(autosave_state_t* s, uint32_t now_ms)
{
    if (s == NULL) {
        return;
    }
    s->last_write_ms = now_ms;
}

void autosave_flushed(autosave_state_t* s)
{
    if (s == NULL) {
        return;
    }
    s->dirty = 0;
}

bool autosave_battery(autosave_state_t* s, uint16_t mv, uint16_t low_mv,
                      uint16_t hyst_mv)
{
    if (s == NULL) {
        return false;
    }
    if (s->batt_low) {
        // Re-arm only well clear of the threshold, so a cell that sags under
        // load and recovers between samples does not flush repeatedly. The
        // sum is taken in 32 bits: low_mv + hyst_mv can exceed a uint16_t.
        if ((uint32_t)mv >= (uint32_t)low_mv + (uint32_t)hyst_mv) {
            s->batt_low = 0;
        }
        return false;
    }
    if (mv < low_mv) {
        s->batt_low = 1;
        return true;
    }
    return false;
}
