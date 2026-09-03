#include "match.h"

#include <string.h>

#include "rom_store.h"

/* ASCII only, and deliberately not <ctype.h>: those take an int whose value
 * must be representable as unsigned char, and a file name byte above 0x7F
 * would be passed as a negative char. */
static int is_space(char c)
{
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\v' ||
           c == '\f';
}

static char to_lower(char c)
{
    if (c >= 'A' && c <= 'Z') {
        return (char)(c + ('a' - 'A'));
    }
    return c;
}

int match_normalise(const char* in, char* out, size_t out_sz)
{
    size_t begin;
    size_t end;
    size_t len;
    size_t need;
    size_t i;
    int has_suffix;

    if (in == NULL || out == NULL || out_sz == 0) {
        return MATCH_ERR_ARGS;
    }
    out[0] = '\0';

    begin = 0;
    while (in[begin] != '\0' && is_space(in[begin])) {
        begin++;
    }
    end = begin;
    while (in[end] != '\0') {
        end++;
    }
    while (end > begin && is_space(in[end - 1])) {
        end--;
    }
    len = end - begin;
    if (len == 0) {
        return MATCH_ERR_ARGS;
    }

    /* The suffix, not a dot anywhere: "Snow Bros. Jr." must still gain ".gb",
     * and "tetris.GB" must not gain a second one. */
    has_suffix = len >= 3 && in[end - 3] == '.' &&
                 to_lower(in[end - 2]) == 'g' && to_lower(in[end - 1]) == 'b';
    need = has_suffix ? len : len + 3;
    if (need + 1 > out_sz) {
        return MATCH_ERR_ARGS;
    }

    for (i = 0; i < len; i++) {
        out[i] = to_lower(in[begin + i]);
    }
    if (!has_suffix) {
        memcpy(out + len, ".gb", 3);
    }
    out[need] = '\0';
    return MATCH_OK;
}

bool match_legacy(const char* norm_title, const char* entry)
{
    char norm_entry[ROM_STORE_NAME_MAX];

    if (norm_title == NULL || entry == NULL || norm_title[0] == '\0') {
        return false;
    }
    if (match_normalise(entry, norm_entry, sizeof(norm_entry)) != MATCH_OK) {
        return false;
    }
    return strstr(norm_entry, norm_title) != NULL;
}

int match_find(const char* title, const char* const* entries,
               size_t n_entries, size_t* out_index)
{
    char norm_title[ROM_STORE_NAME_MAX];
    size_t i;

    if (title == NULL || out_index == NULL) {
        return MATCH_ERR_ARGS;
    }
    if (entries == NULL && n_entries > 0) {
        return MATCH_ERR_ARGS;
    }

    /* Exact first, and case-sensitively: the tag was written from the
     * catalog's file name, so anything else is a legacy tag. */
    for (i = 0; i < n_entries; i++) {
        if (entries[i] != NULL && strcmp(title, entries[i]) == 0) {
            *out_index = i;
            return MATCH_OK;
        }
    }

    if (match_normalise(title, norm_title, sizeof(norm_title)) != MATCH_OK) {
        return MATCH_NOT_FOUND;
    }
    for (i = 0; i < n_entries; i++) {
        if (match_legacy(norm_title, entries[i])) {
            *out_index = i;
            return MATCH_OK;
        }
    }
    return MATCH_NOT_FOUND;
}
