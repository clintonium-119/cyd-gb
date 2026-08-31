#include "match.h"

#include <stddef.h>

int match_normalise(const char* in, char* out, size_t out_sz)
{
    (void)in;
    if (out != NULL && out_sz > 0) {
        out[0] = '\0';
    }
    return MATCH_ERR_NOT_IMPLEMENTED;
}

int match_find(const char* title, const char* const* entries,
               size_t n_entries, size_t* out_index)
{
    (void)title;
    (void)entries;
    (void)n_entries;
    (void)out_index;
    return MATCH_NOT_FOUND;
}
