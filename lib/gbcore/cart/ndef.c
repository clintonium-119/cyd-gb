#include "ndef.h"

int ndef_parse_text(const uint8_t* buf, size_t len, char* out, size_t out_sz)
{
    (void)buf;
    (void)len;
    if (out != NULL && out_sz > 0) {
        out[0] = '\0';
    }
    return NDEF_ERR_NOT_IMPLEMENTED;
}
