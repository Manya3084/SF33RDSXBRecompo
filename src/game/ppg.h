#ifndef SF_PPG_H
#define SF_PPG_H

#include <stddef.h>
#include <stdint.h>

/* One pTEX from a .ppg chunk list. texels are the bytes after the Xbox
   endian fix. rgba is set for A1R5G5B5 (form 0x1555). */

typedef struct SfPpgTex {
    int width;
    int height;
    uint8_t compress;
    uint8_t pixel;
    uint16_t form;
    uint32_t raw_size;
    uint8_t *texels;
    uint8_t *rgba;
} SfPpgTex;

int sf_ppg_tex(const uint8_t *file, uint32_t size, int index, SfPpgTex *out, char *err, size_t err_n);
void sf_ppg_tex_free(SfPpgTex *tex);

#endif
