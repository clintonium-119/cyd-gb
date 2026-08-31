#pragma once
// Filename normaliser + matcher seam — WS-06 fills the real logic.
//
// The caller injects the directory listing (an array of entry names); the
// module never touches the SD card or any filesystem.
//
// Placeholder semantics (this workstream): match_normalise() rejects every
// input with MATCH_ERR_NOT_IMPLEMENTED, NUL-terminating out[0] when out is
// non-NULL and out_sz > 0, never writing beyond out_sz. match_find() never
// matches: it returns MATCH_NOT_FOUND for every title over every listing.
//
// Pure C, no Arduino/ESP-IDF headers, no allocation.

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

enum match_result_e {
    MATCH_OK = 0,
    MATCH_ERR_NOT_IMPLEMENTED = -1, /* stub: normalisation not implemented */
    MATCH_ERR_ARGS = -2,            /* NULL buffer or zero-sized output */
    MATCH_NOT_FOUND = -3,           /* no listing entry matches the title */
};

/*
 * Normalise a cartridge title / filename into its canonical matching form
 * in out (NUL-terminated, at most out_sz bytes including the NUL). WS-06
 * fills the normalisation rules.
 */
int match_normalise(const char* in, char* out, size_t out_sz);

/*
 * Find the listing entry matching title among entries[0..n_entries-1].
 * On MATCH_OK writes the winning index to *out_index. WS-06 fills.
 */
int match_find(const char* title, const char* const* entries,
               size_t n_entries, size_t* out_index);

#ifdef __cplusplus
}
#endif
