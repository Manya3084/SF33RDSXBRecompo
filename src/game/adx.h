#ifndef SF_ADX_H
#define SF_ADX_H

#include <stddef.h>
#include <stdint.h>

/* One ADX cue. pcm is interleaved s16, channels samples wide, total
   samples per channel. loop_start is the first looped sample. Playback
   stops before loop_end. A flags byte of 8 or more is rejected: that
   path uses the scale key, and this slice does not. */

typedef struct SfAdx {
    int channels;
    int rate;
    int high_pass;
    int16_t coef1;
    int16_t coef2;
    int32_t total;
    int loop;
    int32_t loop_start;
    int32_t loop_end;
    int16_t *pcm;
} SfAdx;

int sf_adx_coef(int high_pass, int rate, int16_t *coef1, int16_t *coef2);
int sf_adx_decode(const uint8_t *file, uint32_t size, SfAdx *out, char *err, size_t err_n);
void sf_adx_free(SfAdx *adx);

#endif
