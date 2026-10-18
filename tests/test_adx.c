#include "game/adx.h"
#include "game/fs.h"
#include "game/tracks.h"
#include "game/xwb.h"
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

static void put_le32(uint8_t *p, unsigned v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
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

/* Two PCM waves. Entry 0 is one unsigned 8-bit sample. Entry 1 is one
   little-endian 16-bit sample. */
static void fill_bank(uint8_t *b, size_t n)
{
    memset(b, 0, n);
    put_le32(b, 0x444E4257u);
    put_le32(b + 4, 3);
    put_le32(b + 8, 0x50);
    put_le32(b + 12, 0x28);
    put_le32(b + 16, 0x78);
    put_le32(b + 20, 48);
    put_le32(b + 32, 0xA8);
    put_le32(b + 36, 3);
    put_le32(b + 0x54, 2);
    put_le32(b + 0x50 + 0x18, 24);
    put_le32(b + 0x78 + 4, 0x000AC444u);
    put_le32(b + 0x78 + 12, 1);
    put_le32(b + 0x78 + 24 + 4, 0x800AC444u);
    put_le32(b + 0x78 + 24 + 8, 1);
    put_le32(b + 0x78 + 24 + 12, 2);
    b[0xA8] = 0xFF;
    b[0xA9] = 0x34;
    b[0xAA] = 0x12;
}

static void test_names(void)
{
    expect(sf_track_is_full("o01_A_NYC.adx") == 1, "o01_A_NYC.adx is a full mix");
    expect(sf_track_is_full("01_A_NYC.adx") == 1, "01_A_NYC.adx is a full mix");
    expect(sf_track_is_full("o01_A_NYC_00.adx") == 0, "o01_A_NYC_00.adx is a stem");
    expect(sf_track_is_full("o01_A_NYC_0G.adx") == 1, "a non-hex suffix stays in the list");
    expect(sf_track_is_full("Rank1.ppg") == 0, "a picture is not a mix");
    expect(sf_track_is_full(NULL) == 0, "a missing name is not a mix");
}

static void test_bank(void)
{
    uint8_t file[0xAB];
    SfWave wave;
    char err[64];
    int count = 0;
    fill_bank(file, sizeof file);
    err[0] = 0;
    expect(sf_xwb_count(file, sizeof file, &count, err, sizeof err) == 0 && count == 2, "the bank has two waves");
    memset(&wave, 0, sizeof wave);
    expect(sf_xwb_entry(file, sizeof file, 0, &wave, err, sizeof err) == 0, "the 8-bit wave decodes");
    expect(wave.channels == 1 && wave.rate == 22050 && wave.samples == 1 && wave.pcm && wave.pcm[0] == 32512, "byte 255 expands to 32512");
    sf_wave_free(&wave);
    expect(sf_xwb_entry(file, sizeof file, 1, &wave, err, sizeof err) == 0, "the 16-bit wave decodes");
    expect(wave.channels == 1 && wave.rate == 22050 && wave.samples == 1 && wave.pcm && wave.pcm[0] == 0x1234, "the 16-bit sample is 0x1234");
    sf_wave_free(&wave);
    file[4] = 1;
    expect(sf_xwb_count(file, sizeof file, &count, err, sizeof err) != 0 && strstr(err, "xwb version") != NULL, "version 1 is refused");
    fill_bank(file, sizeof file);
    file[0x78 + 4] = 0x45;
    memset(&wave, 0, sizeof wave);
    err[0] = 0;
    expect(sf_xwb_entry(file, sizeof file, 0, &wave, err, sizeof err) != 0, "a non-PCM tag is refused");
    expect(strstr(err, "xwb codec") != NULL && wave.pcm == NULL, "the refusal names the codec");
    sf_wave_free(&wave);
}

static void test_list(void)
{
    SfTrack *list = NULL;
    int count = 0;
    char err[128];
    int i;
    FILE *fp = fopen("data/afs/index", "r");
    if (!fp) {
        printf("skip track list (no archive index)\n");
        return;
    }
    fclose(fp);
    err[0] = 0;
    expect(sf_tracks_load(".", &list, &count, err, sizeof err) == 0, "the track list loads");
    expect(count == 116, "the archive has 116 full mixes");
    if (count >= 2 && list) {
        expect(list[0].fnum == 87 && strcmp(list[0].name, "o01_A_NYC.adx") == 0, "the list starts at o01_A_NYC.adx");
        expect(list[1].fnum == 103 && strcmp(list[1].name, "o02_B_NYC.adx") == 0, "the next mix is o02_B_NYC.adx");
    }
    for (i = 0; i < count; i++) {
        if (strcmp(list[i].name, "o01_A_NYC_00.adx") == 0) {
            break;
        }
    }
    expect(i == count, "o01_A_NYC_00.adx stays out of the list");
    sf_tracks_free(list);
}

static void test_se1(void)
{
    SfFsFile file;
    SfWave wave;
    char err[128];
    int count = 0;
    FILE *fp = fopen("data/afs/index", "r");
    if (!fp) {
        printf("skip SE1 (no archive index)\n");
        return;
    }
    fclose(fp);
    memset(&file, 0, sizeof file);
    memset(&wave, 0, sizeof wave);
    err[0] = 0;
    expect(fsOpen(".", 8, &file, err, sizeof err) == 0, "SE1.xwb opens");
    expect(file.data && strcmp(file.name, "SE1.xwb") == 0 && file.size == 767740, "SE1.xwb name and size");
    if (!file.data) {
        fsClose(&file);
        return;
    }
    expect(sf_xwb_count(file.data, file.size, &count, err, sizeof err) == 0 && count == 20, "SE1 has 20 waves");
    expect(sf_xwb_entry(file.data, file.size, 0, &wave, err, sizeof err) == 0, "SE1 entry 0 decodes");
    expect(wave.channels == 1 && wave.rate == 22050 && wave.samples == 30899 && wave.pcm != NULL, "SE1 entry 0 is mono 22050 with 30899 samples");
    if (wave.pcm && wave.samples > 16) {
        expect(wave.pcm[16] == 512, "SE1 entry 0 sample 16 is 512");
    }
    sf_wave_free(&wave);
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
    test_names();
    test_bank();
    test_list();
    test_se1();
    if (g_fail) {
        fprintf(stderr, "%d failed\n", g_fail);
        return 1;
    }
    printf("adx ok\n");
    return 0;
}
