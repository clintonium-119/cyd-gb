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
    s->dirty_since_ms = 0;
    s->held = 0;
    s->hold_until_ms = 0;
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
    if (!s->dirty) {
        s->dirty_since_ms = now_ms;
    }
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

bool autosave_due(const autosave_state_t* s, uint32_t now_ms)
{
    if (s == NULL) {
        return false;
    }
    if (!s->dirty) {
        return false;
    }
    // Signed differences so the rule survives the caller's ms counter
    // rolling over, the idiom the coalesced settings save uses.
    if (s->held && (int32_t)(now_ms - s->hold_until_ms) < 0) {
        return false;
    }
    return (int32_t)(now_ms - s->last_write_ms) >= AUTOSAVE_QUIET_MS
           || (int32_t)(now_ms - s->dirty_since_ms) >= AUTOSAVE_MAX_AGE_MS;
}

void autosave_defer(autosave_state_t* s, uint32_t now_ms)
{
    if (s == NULL) {
        return;
    }
    if (!s->dirty) {
        s->dirty_since_ms = now_ms;
    }
    s->dirty = 1;
    s->held = 1;
    s->hold_until_ms = now_ms + AUTOSAVE_MAX_AGE_MS;
}

void autosave_flushed(autosave_state_t* s)
{
    if (s == NULL) {
        return;
    }
    s->dirty = 0;
    s->held = 0;
}
