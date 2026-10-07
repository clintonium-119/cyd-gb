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
