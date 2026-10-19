#include "game/adx.h"

#include "common.h"
#include "mem/heap.h"

#include <math.h>
#include <string.h>

/* ADX_DecodeMono4 is 0x1752E0. The frame word is big-endian. Bit 15 ends
   the stream. The scale is ((state XOR word) & 0x1FFF) + 1, then the
   state becomes (state * km + ka) & 0x7FFF. Nibbles are high then low,
   both signed 4-bit. The predictor is an arithmetic shift by 12.
   ADX_GetCoefficient is 0x170C60. The filter stays in double precision
   and truncates toward zero. */

#define SF_ADX_PI2 (3.14159265f * 2.0f)
#define SF_ADX_MAX_SAMPLES (64 * 1024 * 1024)

static uint16_t be16(const uint8_t *p)
{
    return (uint16_t)((p[0] << 8) | p[1]);
}

static uint32_t be32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}

int sf_adx_coef(int high_pass, int rate, int16_t *coef1, int16_t *coef2)
{
    double a;
    double b;
    double r;
    if (!coef1 || !coef2 || rate <= 0 || high_pass < 0) {
        return -1;
    }
    /* The 2*pi factor is the float at 0x219094. The rest stays double. */
    a = sqrt(2.0) - cos(SF_ADX_PI2 * (float)high_pass / (float)rate);
    b = sqrt(2.0) - 1.0f;
    r = (a - sqrt((a + b) * (a - b))) / b;
    *coef1 = (int16_t)(r * 8192.0f);
    *coef2 = (int16_t)(r * r * -4096.0f);
    return 0;
}

static int16_t predict(int16_t coef1, int16_t coef2, int16_t hist1, int16_t hist2, int nibble, int scale)
{
    int32_t a = (int32_t)coef1 * (int32_t)hist1;
    int32_t b = (int32_t)coef2 * (int32_t)hist2;
    int32_t sum = (int32_t)((uint32_t)a + (uint32_t)b);
    int32_t sample = (sum >> 12) + nibble * scale;
    if (sample > 32767) {
        sample = 32767;
    }
    if (sample < -32768) {
        sample = -32768;
    }
    return (int16_t)sample;
}

/* One 18-byte block, 32 samples. The end marker is refused.
   km and ka are the scale key. This slice only decodes a zero key. */
static int decode_block(const uint8_t *block, int16_t coef1, int16_t coef2, int16_t *hist1, int16_t *hist2, int *state, int km, int ka, int16_t *out)
{
    unsigned word = be16(block);
    int scale;
    int i;
    if (word & 0x8000u) {
        return -1;
    }
    scale = (int)(((unsigned)(*state) ^ word) & 0x1FFFu) + 1;
    *state = (*state * km + ka) & 0x7FFF;
    for (i = 0; i < 16; i++) {
        uint8_t byte = block[2 + i];
        int hi = (int)((int8_t)byte >> 4);
        int lo = (int)((int8_t)(byte << 4) >> 4);
        int16_t s0 = predict(coef1, coef2, *hist1, *hist2, hi, scale);
        int16_t s1;
        *hist2 = *hist1;
        *hist1 = s0;
        s1 = predict(coef1, coef2, *hist1, *hist2, lo, scale);
        *hist2 = *hist1;
        *hist1 = s1;
        out[i * 2] = s0;
        out[i * 2 + 1] = s1;
    }
    return 0;
}

static void read_loop(const uint8_t *file, uint32_t size, int version, int copyright, int *loop, int32_t *start, int32_t *end)
{
    unsigned base;
    int marker;
    *loop = 0;
    *start = 0;
    *end = 0;
    /* Version 4 stores the loop at 0x20. Every other version stores it
       at 0x14. The marker is the word two bytes in, and it must be 1.
       The sample range begins eight bytes after the block. */
    if (version == 4) {
        if (size < 0x3Cu || copyright < 0x38) {
            return;
        }
        base = 0x20;
    } else {
        if (size < 0x30u || copyright < 0x2C) {
            return;
        }
        base = 0x14;
    }
    marker = (int)(int16_t)be16(file + base + 2);
    if (marker != 1) {
        return;
    }
    *start = (int32_t)be32(file + base + 8);
    *end = (int32_t)be32(file + base + 16);
    if (*end <= *start || *start < 0) {
        *start = 0;
        *end = 0;
        return;
    }
    *loop = 1;
}

int sf_adx_decode(const uint8_t *file, uint32_t size, SfAdx *out, char *err, size_t err_n)
{
    unsigned copyright;
    unsigned audio_off;
    int encoding;
    int block;
    int bits;
    int channels;
    int rate;
    int version;
    int flags;
    int high_pass;
    int32_t total;
    int16_t coef1;
    int16_t coef2;
    int16_t hist1[2];
    int16_t hist2[2];
    int16_t tmp[2][32];
    int16_t *pcm;
    int state;
    int loop;
    int32_t loop_start;
    int32_t loop_end;
    uint32_t produced;
    uint32_t off;
    if (out) {
        memset(out, 0, sizeof *out);
    }
    if (!file || !out || size < 0x14u) {
        sf_set_err(err, err_n, "adx header");
        return -1;
    }
    if (be16(file) != 0x8000u) {
        sf_set_err(err, err_n, "adx header");
        return -1;
    }
    copyright = be16(file + 2);
    audio_off = copyright + 4u;
    encoding = file[4];
    block = file[5];
    bits = file[6];
    channels = file[7];
    rate = (int)be32(file + 8);
    total = (int32_t)be32(file + 12);
    if (copyright < 0x10u || audio_off > size || encoding != 3 || block != 18 || bits != 4 || (channels != 1 && channels != 2) || rate <= 0 || total <= 0 || total > SF_ADX_MAX_SAMPLES) {
        sf_set_err(err, err_n, "adx header");
        return -1;
    }
    version = file[0x12];
    flags = file[0x13];
    if (version >= 4 && flags >= 8) {
        sf_set_err(err, err_n, "adx key");
        return -1;
    }
    if (copyright < 0xEu) {
        sf_set_err(err, err_n, "adx header");
        return -1;
    }
    high_pass = (int)be16(file + 0x10);
    if (sf_adx_coef(high_pass, rate, &coef1, &coef2) != 0) {
        sf_set_err(err, err_n, "adx header");
        return -1;
    }
    hist1[0] = 0;
    hist2[0] = 0;
    hist1[1] = 0;
    hist2[1] = 0;
    if (version >= 4) {
        if (size < 0x20u || copyright < 0x1Cu) {
            sf_set_err(err, err_n, "adx header");
            return -1;
        }
        hist1[0] = (int16_t)be16(file + 0x18);
        hist2[0] = (int16_t)be16(file + 0x1A);
        hist1[1] = (int16_t)be16(file + 0x1C);
        hist2[1] = (int16_t)be16(file + 0x1E);
    }
    pcm = sf_alloc((size_t)total * (size_t)channels * sizeof(int16_t));
    if (!pcm) {
        sf_set_err(err, err_n, "adx memory");
        return -1;
    }
    state = 0;
    produced = 0;
    off = audio_off;
    while (produced < (uint32_t)total) {
        int n = 32;
        int c;
        int i;
        uint32_t frame = (uint32_t)(18 * channels);
        if (frame > size - off) {
            break;
        }
        for (c = 0; c < channels; c++) {
            if (be16(file + off + (uint32_t)(c * 18)) & 0x8000u) {
                goto stop;
            }
        }
        for (c = 0; c < channels; c++) {
            if (decode_block(file + off, coef1, coef2, &hist1[c], &hist2[c], &state, 0, 0, tmp[c]) != 0) {
                goto stop;
            }
            off += 18;
        }
        if ((uint32_t)n > (uint32_t)total - produced) {
            n = (int)((uint32_t)total - produced);
        }
        for (i = 0; i < n; i++) {
            for (c = 0; c < channels; c++) {
                pcm[((size_t)produced + (size_t)i) * (size_t)channels + (size_t)c] = tmp[c][i];
            }
        }
        produced += (uint32_t)n;
    }
stop:
    if (produced == 0) {
        sf_free(pcm);
        sf_set_err(err, err_n, "adx end");
        return -1;
    }
    read_loop(file, size, version, (int)copyright, &loop, &loop_start, &loop_end);
    if (loop && (loop_start >= (int32_t)produced || loop_end <= loop_start)) {
        loop = 0;
        loop_start = 0;
        loop_end = 0;
    }
    if (loop && loop_end > (int32_t)produced) {
        loop_end = (int32_t)produced;
    }
    out->channels = channels;
    out->rate = rate;
    out->high_pass = high_pass;
    out->coef1 = coef1;
    out->coef2 = coef2;
    out->total = (int32_t)produced;
    out->loop = loop;
    out->loop_start = loop_start;
    out->loop_end = loop_end;
    out->pcm = pcm;
    return 0;
}

void sf_adx_free(SfAdx *adx)
{
    if (!adx) {
        return;
    }
    sf_free(adx->pcm);
    memset(adx, 0, sizeof *adx);
}
