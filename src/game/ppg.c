#include "game/ppg.h"

#include "common.h"
#include "mem/heap.h"

#include <string.h>
#include <zlib.h>

/* compress 2 is decLZ77withSizeCheck at 0x1811B. That body is zlib inflate:
   inflateInit, inflate until Z_STREAM_END, inflateEnd, then total_out.
   The PlayStation 2 bitstream in Lz77Dec.c is a different decoder. */

enum {
    SF_PTEX = 0x70544558u,
    SF_PEND = 0x70454E44u
};

static uint32_t be32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

static int bitdepth(uint8_t pixel)
{
    switch (pixel & 3) {
    case 0: return 0;
    case 1: return 1;
    case 2: return 2;
    default: return 4;
    }
}

static uint32_t texel_pitch(uint8_t pixel, int width)
{
    switch (pixel & 3) {
    case 0: return (uint32_t)width / 2u;
    case 1: return (uint32_t)width;
    case 2: return (uint32_t)width * 2u;
    default: return (uint32_t)width * 4u;
    }
}

static uint32_t inflate_checked(const uint8_t *src, uint32_t src_size, uint8_t *dst, uint32_t dst_size)
{
    z_stream strm;
    int z;
    memset(&strm, 0, sizeof strm);
    strm.next_in = (Bytef *)src;
    strm.avail_in = src_size;
    strm.next_out = dst;
    strm.avail_out = dst_size;
    if (inflateInit(&strm) != Z_OK) {
        return 0;
    }
    for (;;) {
        z = inflate(&strm, Z_NO_FLUSH);
        if (z == Z_STREAM_END) {
            break;
        }
        if (z != Z_OK) {
            inflateEnd(&strm);
            return 0;
        }
    }
    if (inflateEnd(&strm) != Z_OK) {
        return 0;
    }
    return (uint32_t)strm.total_out;
}

static uint32_t ppg_decompress(int how, const uint8_t *src, uint32_t src_size, uint8_t *dst, uint32_t dst_size)
{
    uint32_t i;
    if (how == 2) {
        return inflate_checked(src, src_size, dst, dst_size);
    }
    if (how == 1) {
        return 0;
    }
    if (src_size < dst_size) {
        return 0;
    }
    for (i = 0; i < dst_size; i++) {
        dst[i] = src[i];
    }
    return src_size;
}

/* ppgChangeDataEndian at 0xF25C4. Depth 1 returns. A texture passes excdot 0,
   so 4-bit pixels are left packed. dendL clear swaps 32-bit words when the
   raw form word is 0x8888, and 16-bit words otherwise. */
static void change_endian(uint8_t *adrs, uint32_t size, int dend_l, int col4, int depth)
{
    uint32_t i;
    if (depth == 1) {
        return;
    }
    if (depth == 0) {
        return;
    }
    if (dend_l != 0) {
        return;
    }
    if (col4) {
        for (i = 0; i + 4 <= size; i += 4) {
            uint8_t b0 = adrs[i];
            uint8_t b1 = adrs[i + 1];
            uint8_t b2 = adrs[i + 2];
            uint8_t b3 = adrs[i + 3];
            adrs[i] = b3;
            adrs[i + 1] = b2;
            adrs[i + 2] = b1;
            adrs[i + 3] = b0;
        }
        return;
    }
    for (i = 0; i + 2 <= size; i += 2) {
        uint8_t b0 = adrs[i];
        adrs[i] = adrs[i + 1];
        adrs[i + 1] = b0;
    }
}

static uint8_t expand5(uint32_t channel)
{
    channel &= 31u;
    return (uint8_t)((channel << 3) | (channel >> 2));
}

static void write_1555(uint16_t v, uint8_t *rgba)
{
    rgba[0] = expand5(v >> 10);
    rgba[1] = expand5(v >> 5);
    rgba[2] = expand5(v);
    rgba[3] = (v & 0x8000u) ? 255 : 0;
}

static void expand_1555(const uint8_t *texels, int count, uint8_t *rgba)
{
    int i;
    for (i = 0; i < count; i++) {
        uint16_t v = (uint16_t)(texels[i * 2] | (texels[i * 2 + 1] << 8));
        write_1555(v, rgba + i * 4);
    }
}

static int find_tex(const uint8_t *file, uint32_t size, int index, uint32_t *at, uint32_t *chunk, char *err, size_t err_n)
{
    uint32_t off = 0;
    int seen = 0;
    if (index < 0) {
        sf_set_err(err, err_n, "texture index is negative");
        return -1;
    }
    while (off + 4 <= size) {
        uint32_t tag = be32(file + off);
        uint32_t file_size;
        uint32_t step;
        if (tag == SF_PEND) {
            break;
        }
        if (off + 16 > size) {
            sf_set_err(err, err_n, "texture chunk header is short");
            return -1;
        }
        file_size = be32(file + off + 4);
        if (file_size < 16 || file_size > size - off) {
            sf_set_err(err, err_n, "texture chunk size is wrong");
            return -1;
        }
        step = (file_size + 3u) & ~3u;
        if (step < file_size || step > size - off) {
            sf_set_err(err, err_n, "texture chunk size is wrong");
            return -1;
        }
        if (tag == SF_PTEX) {
            if (seen == index) {
                *at = off;
                *chunk = file_size;
                return 0;
            }
            seen++;
        }
        if (step == 0) {
            sf_set_err(err, err_n, "texture chunk size is wrong");
            return -1;
        }
        off += step;
    }
    sf_set_err(err, err_n, "texture %d is not in the file", index);
    return -1;
}

int sf_ppg_tex(const uint8_t *file, uint32_t size, int index, SfPpgTex *out, char *err, size_t err_n)
{
    uint32_t at = 0;
    uint32_t chunk = 0;
    const uint8_t *hdr;
    int width;
    int height;
    uint32_t trans;
    uint32_t payload;
    uint32_t src_size;
    uint32_t raw_size;
    uint8_t *texels;
    int how;
    int depth;
    uint16_t form;
    uint16_t raw_form;
    if (out) {
        memset(out, 0, sizeof *out);
    }
    if (!file || !out) {
        sf_set_err(err, err_n, "missing texture arguments");
        return -1;
    }
    if (find_tex(file, size, index, &at, &chunk, err, err_n) != 0) {
        return -1;
    }
    hdr = file + at;
    width = (int)hdr[8] * 16;
    height = (int)hdr[9] * 16;
    if (width <= 0 || height <= 0) {
        sf_set_err(err, err_n, "texture size is zero");
        return -1;
    }
    trans = (uint32_t)hdr[14] << 8 | hdr[15];
    payload = 16u + trans * 3u;
    if (payload > chunk) {
        sf_set_err(err, err_n, "texture rectangles do not fit");
        return -1;
    }
    src_size = chunk - payload;
    raw_size = texel_pitch(hdr[11], width) * (uint32_t)height;
    if (raw_size == 0 || raw_size > 16u * 1024u * 1024u) {
        sf_set_err(err, err_n, "texture pitch is wrong");
        return -1;
    }
    texels = sf_alloc(raw_size);
    if (!texels) {
        sf_set_err(err, err_n, "out of memory");
        return -1;
    }
    how = hdr[10] & 3;
    if (how == 1) {
        sf_free(texels);
        sf_set_err(err, err_n, "texture compress 1 is not decoded");
        return -1;
    }
    if (ppg_decompress(how, hdr + payload, src_size, texels, raw_size) != raw_size) {
        sf_free(texels);
        sf_set_err(err, err_n, "texture decompress failed");
        return -1;
    }
    form = (uint16_t)(((uint16_t)hdr[12] << 8) | hdr[13]);
    raw_form = (uint16_t)((uint16_t)hdr[12] | ((uint16_t)hdr[13] << 8));
    depth = bitdepth(hdr[11]);
    change_endian(texels, raw_size, hdr[11] & 4, raw_form == 0x8888, depth);
    out->width = width;
    out->height = height;
    out->compress = hdr[10];
    out->pixel = hdr[11];
    out->form = form;
    out->rects = (uint16_t)trans;
    out->pal = trans > 0 ? hdr[16] : 0;
    out->raw_size = raw_size;
    out->texels = texels;
    if (form == 0x1555 && depth == 2 && raw_size == (uint32_t)width * (uint32_t)height * 2u) {
        size_t pixels = (size_t)width * (size_t)height;
        out->rgba = sf_alloc(pixels * 4u);
        if (!out->rgba) {
            sf_ppg_tex_free(out);
            sf_set_err(err, err_n, "out of memory");
            return -1;
        }
        expand_1555(out->texels, (int)pixels, out->rgba);
    }
    return 0;
}

int sf_ppg_rect(const uint8_t *file, uint32_t size, int index, int n, int *x, int *y, int *w, int *h, int *pal)
{
    uint32_t at = 0;
    uint32_t chunk = 0;
    const uint8_t *hdr;
    const uint8_t *rec;
    int cells;
    int trans;
    int ip;
    int cofs;
    if (!file || !x || !y || !w || !h || !pal || n < 0) {
        return -1;
    }
    if (find_tex(file, size, index, &at, &chunk, NULL, 0) != 0) {
        return -1;
    }
    hdr = file + at;
    trans = (int)(((unsigned)hdr[14] << 8) | hdr[15]);
    if (n >= trans || 16 + (n + 1) * 3 > (int)chunk) {
        return -1;
    }
    cells = hdr[8];
    if (cells <= 0) {
        return -1;
    }
    rec = hdr + 16 + n * 3;
    ip = rec[1];
    cofs = rec[2];
    *pal = rec[0];
    *x = (ip % cells) * 16;
    *y = (ip / cells) * 16;
    *w = ((cofs >> 4) + 1) * 16;
    *h = ((cofs & 0xF) + 1) * 16;
    return 0;
}

int sf_ppg_expand_ci(SfPpgTex *tex, const uint16_t *pal, int colors, char *err, size_t err_n)
{
    size_t pixels;
    size_t i;
    uint8_t *rgba;
    if (!tex || !tex->texels || !pal || colors <= 0 || colors > 256) {
        sf_set_err(err, err_n, "missing indexed texture arguments");
        return -1;
    }
    if ((tex->pixel & 3) != 1 || tex->form != 0) {
        sf_set_err(err, err_n, "texture is not 8-bit indexed");
        return -1;
    }
    pixels = (size_t)tex->width * (size_t)tex->height;
    if (tex->width <= 0 || tex->height <= 0 || tex->raw_size != pixels) {
        sf_set_err(err, err_n, "indexed texture size is wrong");
        return -1;
    }
    for (i = 0; i < pixels; i++) {
        if (tex->texels[i] >= colors) {
            sf_set_err(err, err_n, "palette index is past the row");
            return -1;
        }
    }
    rgba = sf_alloc(pixels * 4u);
    if (!rgba) {
        sf_set_err(err, err_n, "out of memory");
        return -1;
    }
    for (i = 0; i < pixels; i++) {
        write_1555(pal[tex->texels[i]], rgba + i * 4u);
    }
    sf_free(tex->rgba);
    tex->rgba = rgba;
    return 0;
}

void sf_ppg_tex_free(SfPpgTex *tex)
{
    if (!tex) {
        return;
    }
    sf_free(tex->texels);
    sf_free(tex->rgba);
    memset(tex, 0, sizeof *tex);
}
