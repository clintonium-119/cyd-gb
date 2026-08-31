#pragma once
// NDEF Text-record parser seam — WS-06 fills the real logic.
//
// Placeholder semantics (this workstream): ndef_parse_text() rejects every
// input with NDEF_ERR_NOT_IMPLEMENTED. When out is non-NULL and out_sz > 0
// it NUL-terminates out[0]; it never writes beyond out_sz.
//
// Pure C, no Arduino/ESP-IDF headers, no allocation.

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

enum ndef_result_e {
    NDEF_OK = 0,
    NDEF_ERR_NOT_IMPLEMENTED = -1, /* stub: parsing not implemented yet */
    NDEF_ERR_ARGS = -2,            /* NULL buffer or zero-sized output */
};

/*
 * Parse the text payload of an NDEF Text record from buf/len into out
 * (NUL-terminated, at most out_sz bytes including the NUL). WS-06 fills;
 * language-code/length handling per the design's NFC section.
 */
int ndef_parse_text(const uint8_t* buf, size_t len, char* out, size_t out_sz);

#ifdef __cplusplus
}
#endif
