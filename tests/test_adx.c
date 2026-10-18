#include "game/adx.h"
#include "game/fs.h"
#include "mem/heap.h"

#include <stdio.h>
#include <string.h>

static int g_fail;

static void expect(int cond, const char *msg)
{
    if (!cond) {
        fprintf(stderr, "FAIL %s\n", msg);
        g_fail++;
    } else {
        printf("ok %s\n", msg);
    }
}

static void put_be16(uint8_t *p, unsigned v)
{
    p[0] = (uint8_t)(v >> 8);
    p[1] = (uint8_t)v;
}

static void put_be32(uint8_t *p, unsigned v)
{
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
}

/* Version 3 header. Audio starts at byte 0x14. One 18-byte block follows. */
static void fill_hdr(uint8_t *h, unsigned samples, unsigned version, unsigned flags)
{
    memset(h, 0, 0x14);
    put_be16(h, 0x8000);
    put_be16(h + 2, 0x10);
    h[4] = 3;
    h[5] = 18;
    h[6] = 4;
    h[7] = 1;
    put_be32(h + 8, 48000);
    put_be32(h + 12, samples);
    put_be16(h + 16, 500);
    h[18] = (uint8_t)version;
    h[19] = (uint8_t)flags;
}

static void test_coef(void)
{
    int16_t c1 = 0;
    int16_t c2 = 0;
    expect(sf_adx_coef(500, 48000, &c1, &c2) == 0, "coefficient call");
    expect(c1 == 7400 && c2 == -3342, "500 Hz at 48000 is 7400, -3342");
}

static void test_scale(void)
{
    uint8_t file[0x14 + 18];
    SfAdx adx;
    char err[64];
    memset(file, 0, sizeof file);
    fill_hdr(file, 32, 3, 0);
    file[0x14] = 0x00;
    file[0x15] = 0x07;
    file[0x16] = 0x10;
    memset(&adx, 0, sizeof adx);
    err[0] = 0;
    expect(sf_adx_decode(file, sizeof file, &adx, err, sizeof err) == 0, "scale block decodes");
    expect(adx.channels == 1 && adx.total == 32 && adx.pcm != NULL, "scale block is 32 mono samples");
    if (adx.pcm && adx.total >= 2) {
        expect(adx.pcm[0] == 8, "nibble 1 times scale 8 is 8");
        expect(adx.pcm[1] == 14, "the next prediction is 14");
        if (adx.pcm[0] != 8 || adx.pcm[1] != 14) {
            fprintf(stderr, "pcm0 %d pcm1 %d coef %d %d\n", adx.pcm[0], adx.pcm[1], adx.coef1, adx.coef2);
        }
    }
    sf_adx_free(&adx);
}

static void test_zero(void)
{
    uint8_t file[0x14 + 18];
    SfAdx adx;
    char err[64];
    int i;
    memset(file, 0, sizeof file);
    fill_hdr(file, 32, 3, 0);
    memset(&adx, 0, sizeof adx);
    expect(sf_adx_decode(file, sizeof file, &adx, err, sizeof err) == 0 && adx.pcm != NULL, "zero block decodes");
    if (adx.pcm) {
        for (i = 0; i < adx.total; i++) {
            if (adx.pcm[i] != 0) {
                break;
            }
        }
        expect(i == adx.total, "a zero block is silence");
    }
    sf_adx_free(&adx);
}

static void test_end(void)
{
    uint8_t file[0x14 + 18];
    SfAdx adx;
    char err[64];
    memset(file, 0, sizeof file);
    fill_hdr(file, 32, 3, 0);
    file[0x14] = 0x80;
    file[0x15] = 0x01;
    memset(&adx, 0, sizeof adx);
    err[0] = 0;
    expect(sf_adx_decode(file, sizeof file, &adx, err, sizeof err) != 0, "a block with bit 15 set yields nothing");
    expect(adx.pcm == NULL, "an ended cue allocates no samples");
    sf_adx_free(&adx);
}

static void test_key(void)
{
    uint8_t file[0x14 + 18];
    SfAdx adx;
    char err[64];
    memset(file, 0, sizeof file);
    fill_hdr(file, 32, 4, 8);
    memset(&adx, 0, sizeof adx);
    err[0] = 0;
    expect(sf_adx_decode(file, sizeof file, &adx, err, sizeof err) != 0, "flags past 7 are refused");
    expect(strstr(err, "adx key") != NULL, "the refusal names the key");
    sf_adx_free(&adx);
}

static void test_nyc(void)
{
    SfFsFile file;
    SfAdx adx;
    char err[128];
    FILE *fp = fopen("data/afs/index", "r");
    if (!fp) {
        printf("skip o01_A_NYC (no archive index)\n");
        return;
    }
    fclose(fp);
    memset(&file, 0, sizeof file);
    memset(&adx, 0, sizeof adx);
    err[0] = 0;
    expect(fsOpen(".", 87, &file, err, sizeof err) == 0, "o01_A_NYC.adx opens");
    expect(file.data && strcmp(file.name, "o01_A_NYC.adx") == 0 && file.size == 4055040, "o01_A_NYC.adx name and size");
    if (!file.data) {
        fsClose(&file);
        return;
    }
    expect(sf_adx_decode(file.data, file.size, &adx, err, sizeof err) == 0, "o01_A_NYC.adx decodes");
    if (!adx.pcm) {
        fprintf(stderr, "%s\n", err[0] ? err : "no pcm");
        fsClose(&file);
        sf_adx_free(&adx);
        return;
    }
    expect(adx.channels == 2 && adx.rate == 48000 && adx.high_pass == 500, "o01 is stereo 48000 high-pass 500");
    expect(adx.coef1 == 7400 && adx.coef2 == -3342, "o01 uses the 500 Hz coefficients");
    expect(adx.total == 3601938, "o01 has 3601938 samples");
    expect(adx.loop == 1 && adx.loop_start == 191744 && adx.loop_end == 2875501, "o01 loops from 191744 to 2875501");
    if (adx.total > 41) {
        expect(adx.pcm[37] == 175 && adx.pcm[40] == 186 && adx.pcm[41] == 247, "o01 frame 0 matches the retail samples");
        if (adx.pcm[37] != 175 || adx.pcm[40] != 186 || adx.pcm[41] != 247) {
            fprintf(stderr, "pcm37 %d pcm40 %d pcm41 %d\n", adx.pcm[37], adx.pcm[40], adx.pcm[41]);
        }
    }
    sf_adx_free(&adx);
    fsClose(&file);
}

int main(void)
{
    test_coef();
    test_scale();
    test_zero();
    test_end();
    test_key();
    test_nyc();
    if (g_fail) {
        fprintf(stderr, "%d failed\n", g_fail);
        return 1;
    }
    printf("adx ok\n");
    return 0;
}
