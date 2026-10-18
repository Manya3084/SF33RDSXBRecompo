#ifndef SF_XWB_H
#define SF_XWB_H

#include <stddef.h>
#include <stdint.h>

/* One wave from an XACT 1 wave bank (WBND version 2 or 3). pcm is
   interleaved s16. This reads PCM entries. ADPCM and WMA are refused. */

typedef struct SfWave {
    int channels;
    int rate;
    int32_t samples;
    int16_t *pcm;
} SfWave;

int sf_xwb_count(const uint8_t *file, uint32_t size, int *count, char *err, size_t err_n);
int sf_xwb_entry(const uint8_t *file, uint32_t size, int index, SfWave *out, char *err, size_t err_n);
void sf_wave_free(SfWave *wave);

#endif
