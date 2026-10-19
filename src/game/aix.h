#ifndef SF_AIX_H
#define SF_AIX_H

#include <stddef.h>
#include <stdint.h>

/* ST3_3_OP.aix is an AIXF container. Packets are AIXP, then AIXE.
   Track 0 is the front stereo ADX. The other pairs stay for later. */

typedef struct SfAix {
    int channels;
    int rate;
    int32_t total;
    int loop;
    int32_t loop_start;
    int32_t loop_end;
    int16_t *pcm;
} SfAix;

int sf_aix_decode(const uint8_t *file, uint32_t size, SfAix *out, char *err, size_t err_n);
void sf_aix_free(SfAix *aix);

#endif
