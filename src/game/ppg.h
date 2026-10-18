#ifndef SF_PPG_H
#define SF_PPG_H

#include <stddef.h>
#include <stdint.h>

/* One pTEX from a .ppg chunk list. texels are the bytes after the Xbox
   endian fix. rgba is set for A1R5G5B5 (form 0x1555). An 8-bit texture
   leaves rgba empty until sf_ppg_expand_ci. rects is transNums. pal is
   the first rectangle's palette byte, or 0 when the texture has none. */

typedef struct SfPpgTex {
    int width;
    int height;
    uint8_t compress;
    uint8_t pixel;
    uint16_t form;
    uint16_t rects;
    uint8_t pal;
    uint32_t raw_size;
    uint8_t *texels;
    uint8_t *rgba;
} SfPpgTex;

int sf_ppg_tex(const uint8_t *file, uint32_t size, int index, SfPpgTex *out, char *err, size_t err_n);
int sf_ppg_expand_ci(SfPpgTex *tex, const uint16_t *pal, int colors, char *err, size_t err_n);
void sf_ppg_tex_free(SfPpgTex *tex);

#endif
