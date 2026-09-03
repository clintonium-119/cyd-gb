#pragma once
// Cartridge title -> ROM file name matching.
//
// Tags are written by the device from the catalog, so the string on a tag is
// the ROM's file name and exact, byte-for-byte match is the rule. The
// normaliser and substring search below are a legacy path only, for tags
// hand-written from a phone before the device could write them.
//
// The normaliser tests for a ".gb" *suffix* rather than for the presence of a
// dot: "Snow Bros. Jr..gb", "Dr. Mario.gb", "Super R.C. Pro-Am.gb" and
// "Mr. Do!.gb" are all real library entries, and a dot test would mangle
// every one of them.
//
// The caller injects the directory listing; the module never touches the SD
// card or any filesystem. match_legacy() is the per-entry form of the same
// predicate, so the device side can walk a directory one entry at a time
// instead of holding a 132-entry listing in RAM.
//
// Pure C, no Arduino/ESP-IDF headers, no allocation.

#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

enum match_result_e {
    MATCH_OK = 0,
    MATCH_ERR_ARGS = -2,  /* NULL buffer, empty title, or output too small */
    MATCH_NOT_FOUND = -3, /* no listing entry matches the title            */
};

/*
 * Normalise a cartridge title into its canonical matching form in out
 * (NUL-terminated, at most out_sz bytes including the NUL): whitespace
 * trimmed from both ends, ASCII lowercased, and ".gb" appended unless the
 * trimmed name already ends in it. An empty result, or one that does not fit
 * out_sz, is MATCH_ERR_ARGS with out emptied — a truncated name must never
 * go on to match something.
 */
int match_normalise(const char* in, char* out, size_t out_sz);

/*
 * Find the listing entry matching title among entries[0..n_entries-1]. Exact
 * byte-for-byte matches are tried across the whole listing first; only then
 * does the legacy substring pass run. On MATCH_OK writes the winning index to
 * *out_index, which is left untouched otherwise.
 */
int match_find(const char* title, const char* const* entries,
               size_t n_entries, size_t* out_index);

/*
 * The legacy pass as a per-entry predicate: true when entry, normalised the
 * same way, contains the already-normalised norm_title as a substring.
 */
bool match_legacy(const char* norm_title, const char* entry);

#ifdef __cplusplus
}
#endif
