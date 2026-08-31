#include "combo.h"

#include <stddef.h>

int combo_init(combo_state_t* s)
{
    if (s == NULL) {
        return COMBO_ERR_ARGS;
    }
    s->stable_word = 0;
    s->raw_word = 0;
    s->last_change_ms = 0;
    return COMBO_OK;
}

int combo_update(combo_state_t* s, uint16_t raw_word, uint32_t now_ms,
                 uint8_t* out_event)
{
    (void)raw_word;
    (void)now_ms;
    if (s == NULL || out_event == NULL) {
        return COMBO_ERR_ARGS;
    }
    *out_event = COMBO_EVENT_NONE;
    return COMBO_ERR_NOT_IMPLEMENTED;
}
