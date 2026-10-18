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
    if (tag != 0 || (channels != 1 && channels != 2) || rate <= 0 || bytes == 0 || rel > data_size || bytes > data_size - rel) {
        sf_set_err(err, err_n, tag == 0 ? "xwb entry" : "xwb codec");
        return -1;
    }
    if (wide) {
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
    if (wide) {
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
