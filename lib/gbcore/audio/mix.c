#include "mix.h"

int mix_mono(const int16_t* left, const int16_t* right, size_t n_samples,
             uint8_t volume, int16_t* out)
{
    (void)left;
    (void)right;
    (void)n_samples;
    (void)volume;
    (void)out;
    return MIX_ERR_NOT_IMPLEMENTED;
}
