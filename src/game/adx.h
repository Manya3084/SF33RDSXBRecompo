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

/* The same decode in parts, for a file still being read: sf_adx_open
   reads the header (the first audio_off bytes), sf_adx_step decodes
   blocks from the bytes read so far (size, and size_final once that is
   all of the file) into pcm, which holds total * channels samples, at
   most max more samples a call; it returns the samples decoded so far.
   finished is set when the stream ends (its samples, its end block, the
   file's end). Until then total and the loop are the header's, and they
   are an upper bound: sf_adx_final gives what sf_adx_decode returns. */
typedef struct SfAdxStream {
    int channels;
    int rate;
    int version;
    int high_pass;
    int16_t coef1;
    int16_t coef2;
    int32_t total;
    int loop;
    int32_t loop_start;
    int32_t loop_end;
    uint32_t audio_off;
    int16_t hist1[2];
    int16_t hist2[2];
    int state;
    uint32_t off;
    uint32_t produced;
    int finished;
} SfAdxStream;

int sf_adx_open(const uint8_t *file, uint32_t size, SfAdxStream *s, char *err, size_t err_n);
uint32_t sf_adx_step(SfAdxStream *s, const uint8_t *file, uint32_t size, int size_final, int16_t *pcm, uint32_t max);
void sf_adx_final(const SfAdxStream *s, int32_t *total, int *loop, int32_t *loop_start, int32_t *loop_end);
int sf_adx_decode(const uint8_t *file, uint32_t size, SfAdx *out, char *err, size_t err_n);
void sf_adx_free(SfAdx *adx);

#endif
