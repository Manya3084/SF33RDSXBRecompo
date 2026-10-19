#ifndef SF_XWB_H
#define SF_XWB_H

#include <stddef.h>
#include <stdint.h>

/* One wave from an XACT 1 wave bank (WBND version 2 or 3). pcm is
   interleaved s16. This reads 8-bit and 16-bit PCM and Xbox ADPCM
   entries; WMA is refused. loop_len is 0 when the entry has no loop
   region; otherwise the region is loop_start and loop_len frames. */

typedef struct SfWave {
    int channels;
    int rate;
    int32_t samples;
    int16_t *pcm;
    int32_t loop_start;
    int32_t loop_len;
} SfWave;

int sf_xwb_count(const uint8_t *file, uint32_t size, int *count, char *err, size_t err_n);
int sf_xwb_entry(const uint8_t *file, uint32_t size, int index, SfWave *out, char *err, size_t err_n);
void sf_wave_free(SfWave *wave);

/* The bank's name (its WAVEBANKDATA szBankName), which sound banks use to
   refer to it. out holds 17 bytes. */
int sf_xwb_name(const uint8_t *file, uint32_t size, char *out);

#endif
