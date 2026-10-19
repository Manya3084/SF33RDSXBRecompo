#include "game/xwb.h"

#include "common.h"
#include "mem/heap.h"

#include <string.h>

#define SF_XWB_COMPACT 0x00020000u

static int span(uint32_t size, uint32_t off, uint32_t n)
{
    return n <= size && off <= size - n;
}

static uint32_t u32le(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static int xwb_map(const uint8_t *file, uint32_t size, uint32_t *entry_off, uint32_t *elem, uint32_t *count, uint32_t *data_off, uint32_t *data_size, char *err, size_t err_n)
{
    uint32_t version;
    uint32_t base;
    uint32_t base_size;
    uint32_t flags;
    if (!file || size < 0x50u || u32le(file) != 0x444E4257u) {
        sf_set_err(err, err_n, "xwb header");
        return -1;
    }
    version = u32le(file + 4);
    if (version < 2 || version > 3) {
        sf_set_err(err, err_n, "xwb version");
        return -1;
    }
    base = u32le(file + 8);
    base_size = u32le(file + 12);
    *entry_off = u32le(file + 16);
    *data_off = u32le(file + 32);
    *data_size = u32le(file + 36);
    if (base_size < 0x28u || !span(size, base, base_size) || !span(size, *data_off, *data_size)) {
        sf_set_err(err, err_n, "xwb header");
        return -1;
    }
    flags = u32le(file + base);
    *count = u32le(file + base + 4);
    *elem = u32le(file + base + 0x18);
    if ((flags & SF_XWB_COMPACT) || *elem < 0x18u || *elem > size || *count > 4096u || *count > (size / *elem) || !span(size, *entry_off, *elem * *count)) {
        sf_set_err(err, err_n, "xwb header");
        return -1;
    }
    return 0;
}

/* Xbox ADPCM: IMA ADPCM in 36-byte blocks per channel, 64 samples each.
 * A block starts with each channel's 4-byte header (the predictor as a
 * little-endian s16, then the step index and a zero byte); the nibbles
 * follow in 4-byte words per channel, low nibble first. */
#define XBOX_ADPCM_BLOCK 36u
#define XBOX_ADPCM_SAMPLES 64u

static const int16_t k_ima_step[89] = {
    7,     8,     9,     10,    11,    12,    13,    14,    16,    17,    19,    21,    23,    25,    28,
    31,    34,    37,    41,    45,    50,    55,    60,    66,    73,    80,    88,    97,    107,   118,
    130,   143,   157,   173,   190,   209,   230,   253,   279,   307,   337,   371,   408,   449,   494,
    544,   598,   658,   724,   796,   876,   963,   1060,  1166,  1282,  1411,  1552,  1707,  1878,  2066,
    2272,  2499,  2749,  3024,  3327,  3660,  4026,  4428,  4871,  5358,  5894,  6484,  7132,  7845,  8630,
    9493,  10442, 11487, 12635, 13899, 15289, 16818, 18500, 20350, 22385, 24623, 27086, 29794, 32767,
};
static const int8_t k_ima_index[16] = { -1, -1, -1, -1, 2, 4, 6, 8, -1, -1, -1, -1, 2, 4, 6, 8 };

static int16_t ima_step(int *pred, int *index, int nib)
{
    int step = k_ima_step[*index];
    int diff = step >> 3;
    if (nib & 1) {
        diff += step >> 2;
    }
    if (nib & 2) {
        diff += step >> 1;
    }
    if (nib & 4) {
        diff += step;
    }
    *pred += (nib & 8) ? -diff : diff;
    if (*pred > 32767) {
        *pred = 32767;
    } else if (*pred < -32768) {
        *pred = -32768;
    }
    *index += k_ima_index[nib];
    if (*index < 0) {
        *index = 0;
    } else if (*index > 88) {
        *index = 88;
    }
    return (int16_t)*pred;
}

static void xbox_adpcm(const uint8_t *src, uint32_t blocks, int channels, int16_t *out)
{
    uint32_t b;
    int c;
    for (b = 0; b < blocks; b++) {
        const uint8_t *blk = src + b * XBOX_ADPCM_BLOCK * (uint32_t)channels;
        int16_t *dst = out + (size_t)b * XBOX_ADPCM_SAMPLES * (size_t)channels;
        for (c = 0; c < channels; c++) {
            int pred = (int16_t)(blk[c * 4] | (blk[c * 4 + 1] << 8));
            int index = blk[c * 4 + 2] > 88 ? 88 : blk[c * 4 + 2];
            uint32_t w;
            for (w = 0; w < XBOX_ADPCM_SAMPLES / 8u; w++) {
                const uint8_t *word = blk + 4 * channels + (w * (uint32_t)channels + (uint32_t)c) * 4u;
                int k;
                for (k = 0; k < 8; k++) {
                    int nib = (word[k >> 1] >> ((k & 1) * 4)) & 15;
                    dst[(w * 8u + (uint32_t)k) * (uint32_t)channels + (uint32_t)c] = ima_step(&pred, &index, nib);
                }
            }
        }
    }
}

int sf_xwb_name(const uint8_t *file, uint32_t size, char *out)
{
    uint32_t entry_off;
    uint32_t elem;
    uint32_t count;
    uint32_t data_off;
    uint32_t data_size;
    uint32_t base;
    out[0] = 0;
    if (xwb_map(file, size, &entry_off, &elem, &count, &data_off, &data_size, NULL, 0) != 0) {
        return -1;
    }
    base = u32le(file + 8);
    memcpy(out, file + base + 8, 16);
    out[16] = 0;
    return 0;
}

int sf_xwb_count(const uint8_t *file, uint32_t size, int *count, char *err, size_t err_n)
{
    uint32_t entry_off;
    uint32_t elem;
    uint32_t n;
    uint32_t data_off;
    uint32_t data_size;
    if (!count) {
        sf_set_err(err, err_n, "xwb header");
        return -1;
    }
    *count = 0;
    if (xwb_map(file, size, &entry_off, &elem, &n, &data_off, &data_size, err, err_n) != 0) {
        return -1;
    }
    *count = (int)n;
    return 0;
}

int sf_xwb_entry(const uint8_t *file, uint32_t size, int index, SfWave *out, char *err, size_t err_n)
{
    uint32_t entry_off;
    uint32_t elem;
    uint32_t count;
    uint32_t data_off;
    uint32_t data_size;
    uint32_t format;
    uint32_t rel;
    uint32_t bytes;
    uint32_t frames;
    int tag;
    int channels;
    int rate;
    int wide;
    int16_t *pcm;
    uint32_t i;
    const uint8_t *src;
    if (out) {
        memset(out, 0, sizeof *out);
    }
    if (!out || index < 0 || xwb_map(file, size, &entry_off, &elem, &count, &data_off, &data_size, err, err_n) != 0) {
        return -1;
    }
    if ((uint32_t)index >= count) {
        sf_set_err(err, err_n, "xwb entry");
        return -1;
    }
    format = u32le(file + entry_off + (uint32_t)index * elem + 4);
    rel = u32le(file + entry_off + (uint32_t)index * elem + 8);
    bytes = u32le(file + entry_off + (uint32_t)index * elem + 12);
    tag = (int)(format & 3u);
    channels = (int)((format >> 2) & 7u);
    rate = (int)((format >> 5) & 0x3FFFFFFu);
    wide = (format & 0x80000000u) != 0;
    if (tag > 1 || (channels != 1 && channels != 2) || rate <= 0 || bytes == 0 || rel > data_size || bytes > data_size - rel) {
        sf_set_err(err, err_n, tag <= 1 ? "xwb entry" : "xwb codec");
        return -1;
    }
    if (tag == 1) {
        if ((bytes % (XBOX_ADPCM_BLOCK * (uint32_t)channels)) != 0) {
            sf_set_err(err, err_n, "xwb entry");
            return -1;
        }
        frames = bytes / (XBOX_ADPCM_BLOCK * (uint32_t)channels) * XBOX_ADPCM_SAMPLES;
    } else if (wide) {
        if ((bytes % (uint32_t)(channels * 2)) != 0) {
            sf_set_err(err, err_n, "xwb entry");
            return -1;
        }
        frames = bytes / (uint32_t)(channels * 2);
    } else {
        if ((bytes % (uint32_t)channels) != 0) {
            sf_set_err(err, err_n, "xwb entry");
            return -1;
        }
        frames = bytes / (uint32_t)channels;
    }
    if (frames == 0 || frames > 16u * 1024u * 1024u) {
        sf_set_err(err, err_n, "xwb entry");
        return -1;
    }
    pcm = sf_alloc((size_t)frames * (size_t)channels * sizeof(int16_t));
    if (!pcm) {
        sf_set_err(err, err_n, "xwb memory");
        return -1;
    }
    src = file + data_off + rel;
    if (tag == 1) {
        xbox_adpcm(src, bytes / (XBOX_ADPCM_BLOCK * (uint32_t)channels), channels, pcm);
    } else if (wide) {
        for (i = 0; i < frames * (uint32_t)channels; i++) {
            pcm[i] = (int16_t)(src[i * 2] | (src[i * 2 + 1] << 8));
        }
    } else {
        for (i = 0; i < frames * (uint32_t)channels; i++) {
            pcm[i] = (int16_t)(((int)src[i] - 128) << 8);
        }
    }
    out->channels = channels;
    out->rate = rate;
    out->samples = (int32_t)frames;
    out->pcm = pcm;
    {
        /* the loop region, in bytes of this entry's data */
        uint32_t loop_off = u32le(file + entry_off + (uint32_t)index * elem + 16);
        uint32_t loop_bytes = u32le(file + entry_off + (uint32_t)index * elem + 20);
        uint32_t frame_bytes = tag == 1 ? 0 : (uint32_t)channels * (wide ? 2u : 1u);
        if (tag == 1 && loop_bytes != 0) {
            uint32_t blk = XBOX_ADPCM_BLOCK * (uint32_t)channels;
            out->loop_start = (int32_t)(loop_off / blk * XBOX_ADPCM_SAMPLES);
            out->loop_len = (int32_t)(loop_bytes / blk * XBOX_ADPCM_SAMPLES);
        } else if (loop_bytes != 0) {
            out->loop_start = (int32_t)(loop_off / frame_bytes);
            out->loop_len = (int32_t)(loop_bytes / frame_bytes);
        }
        if (out->loop_start < 0 || out->loop_start >= out->samples || out->loop_len <= 0) {
            out->loop_start = 0;
            out->loop_len = 0;
        } else if (out->loop_len > out->samples - out->loop_start) {
            out->loop_len = out->samples - out->loop_start;
        }
    }
    return 0;
}

void sf_wave_free(SfWave *wave)
{
    if (!wave) {
        return;
    }
    sf_free(wave->pcm);
    memset(wave, 0, sizeof *wave);
}
