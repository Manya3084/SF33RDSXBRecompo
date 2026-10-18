#include "game/fs.h"
#include "game/ppg.h"
#include "mem/heap.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <zlib.h>

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

static uint16_t texel_at(const SfPpgTex *tex, int i)
{
    return (uint16_t)(tex->texels[i * 2] | (tex->texels[i * 2 + 1] << 8));
}

static uint8_t expand5(unsigned channel)
{
    channel &= 31u;
    return (uint8_t)((channel << 3) | (channel >> 2));
}

static void put_be16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v >> 8);
    p[1] = (uint8_t)v;
}

static int write_file(const char *path, const void *data, size_t n)
{
    FILE *fp = fopen(path, "wb");
    if (!fp) {
        return -1;
    }
    if (n > 0 && fwrite(data, 1, n, fp) != n) {
        fclose(fp);
        return -1;
    }
    if (fclose(fp) != 0) {
        return -1;
    }
    return 0;
}

static int mkdir_at(const char *path)
{
    if (mkdir(path, 0700) != 0) {
        return -1;
    }
    return 0;
}

/* 16x16 A1R5G5B5, one compressed or raw chunk, one raw chunk, then pEND.
   The next chunk starts on the 4-byte boundary the loader uses. */
static uint8_t *make_ppg(size_t *out_size, int pixel, int compress, const uint16_t *logical, int count)
{
    uint8_t raw[16 * 16 * 2];
    uint8_t *packed;
    uLongf packed_len;
    uint8_t *file;
    uint32_t payload;
    uint32_t chunk;
    uint32_t step;
    uint32_t second;
    uint32_t total;
    int i;
    if (count != 16 * 16) {
        return NULL;
    }
    for (i = 0; i < count; i++) {
        if (pixel & 4) {
            raw[i * 2] = (uint8_t)logical[i];
            raw[i * 2 + 1] = (uint8_t)(logical[i] >> 8);
        } else {
            put_be16(raw + i * 2, logical[i]);
        }
    }
    packed_len = compressBound(sizeof raw);
    packed = sf_alloc(packed_len);
    if (!packed) {
        return NULL;
    }
    if (compress == 2) {
        if (compress2(packed, &packed_len, raw, sizeof raw, Z_DEFAULT_COMPRESSION) != Z_OK) {
            sf_free(packed);
            return NULL;
        }
        payload = (uint32_t)packed_len;
    } else {
        memcpy(packed, raw, sizeof raw);
        payload = (uint32_t)sizeof raw;
    }
    chunk = 16u + payload;
    step = (chunk + 3u) & ~3u;
    second = 16u + (uint32_t)sizeof raw;
    total = step + second + 4u;
    file = sf_alloc(total);
    if (!file) {
        sf_free(packed);
        return NULL;
    }
    memset(file, 0, total);
    memcpy(file, "pTEX", 4);
    file[4] = (uint8_t)(chunk >> 24);
    file[5] = (uint8_t)(chunk >> 16);
    file[6] = (uint8_t)(chunk >> 8);
    file[7] = (uint8_t)chunk;
    file[8] = 1;
    file[9] = 1;
    file[10] = (uint8_t)compress;
    file[11] = (uint8_t)pixel;
    file[12] = 0x15;
    file[13] = 0x55;
    memcpy(file + 16, packed, payload);
    memcpy(file + step, "pTEX", 4);
    file[step + 4] = 0;
    file[step + 5] = 0;
    file[step + 6] = (uint8_t)(second >> 8);
    file[step + 7] = (uint8_t)second;
    file[step + 8] = 1;
    file[step + 9] = 1;
    file[step + 10] = 0;
    file[step + 11] = 0x82;
    file[step + 12] = 0x15;
    file[step + 13] = 0x55;
    for (i = 0; i < count; i++) {
        put_be16(file + step + 16 + i * 2, 0xFFFF);
    }
    memcpy(file + total - 4, "pEND", 4);
    sf_free(packed);
    *out_size = total;
    return file;
}

static void test_synthetic(void)
{
    uint16_t logical[16 * 16];
    uint8_t *file;
    size_t size = 0;
    SfPpgTex tex;
    SfPpgTex second;
    char err[128];
    int i;
    memset(logical, 0, sizeof logical);
    logical[0] = 0x8000;
    logical[1] = 0x7C00;
    logical[2] = 0x03E0;
    logical[3] = 0x001F;
    logical[4] = 0xFFFF;
    memset(&tex, 0, sizeof tex);
    memset(&second, 0, sizeof second);
    file = make_ppg(&size, 0x82, 2, logical, 16 * 16);
    expect(file != NULL, "synthetic zlib pTEX builds");
    if (!file) {
        return;
    }
    err[0] = 0;
    expect(sf_ppg_tex(file, (uint32_t)size, 0, &tex, err, sizeof err) == 0, "synthetic texture decodes");
    if (tex.texels && tex.rgba) {
        expect(tex.width == 16 && tex.height == 16, "synthetic size is one cell");
        expect(tex.form == 0x1555 && tex.raw_size == 512 && (tex.compress & 3) == 2, "synthetic header is A1R5G5B5 inflate");
        expect(texel_at(&tex, 0) == 0x8000 && texel_at(&tex, 1) == 0x7C00, "inflate then byte-swap restores the texels");
        expect(tex.rgba[0] == 0 && tex.rgba[3] == 255, "pixel 0 is opaque black");
        expect(tex.rgba[4] == 255 && tex.rgba[7] == 0, "pixel 1 is red with alpha clear");
        expect(tex.rgba[9] == expand5(31) && tex.rgba[11] == 0, "pixel 2 is green");
        expect(tex.rgba[14] == expand5(31) && tex.rgba[15] == 0, "pixel 3 is blue");
        expect(tex.rgba[16] == 255 && tex.rgba[17] == 255 && tex.rgba[18] == 255 && tex.rgba[19] == 255, "pixel 4 is opaque white");
    }
    expect(sf_ppg_tex(file, (uint32_t)size, 1, &second, err, sizeof err) == 0, "the second chunk decodes");
    if (second.texels) {
        expect(second.width == 16 && texel_at(&second, 0) == 0xFFFF && (second.compress & 3) == 0, "the raw chunk swaps once");
    }
    sf_ppg_tex_free(&tex);
    file[18] ^= 0xFF;
    expect(sf_ppg_tex(file, (uint32_t)size, 0, &tex, err, sizeof err) != 0, "a broken zlib stream fails");
    sf_ppg_tex_free(&tex);
    sf_ppg_tex_free(&second);
    for (i = 0; i < 16 * 16; i++) {
        logical[i] = 0x8000;
    }
    logical[0] = 0x001F;
    sf_free(file);
    file = make_ppg(&size, 0x86, 0, logical, 16 * 16);
    expect(file != NULL && sf_ppg_tex(file, (uint32_t)size, 0, &tex, err, sizeof err) == 0, "pixel bit 2 skips the swap");
    if (tex.texels) {
        expect(texel_at(&tex, 0) == 0x001F, "little-endian texels stay little-endian");
    }
    sf_ppg_tex_free(&tex);
    sf_free(file);
    expect(sf_ppg_tex((const uint8_t *)"pEND", 4, 0, &tex, err, sizeof err) != 0, "pEND alone has no texture");
}

static void test_fs_fixture(void)
{
    char root[] = "/tmp/sf33rd-fsXXXXXX";
    char data[128];
    char afs[160];
    char files[180];
    char index_path[200];
    char member[200];
    char note_path[200];
    const char note[] = "hi";
    char index[128];
    SfFsFile file;
    char err[128];
    uint8_t *ppg;
    size_t ppg_size = 0;
    uint16_t logical[16 * 16];
    int n;
    if (!mkdtemp(root)) {
        expect(0, "temp archive directory");
        return;
    }
    snprintf(data, sizeof data, "%s/data", root);
    snprintf(afs, sizeof afs, "%s/afs", data);
    snprintf(files, sizeof files, "%s/files", afs);
    snprintf(index_path, sizeof index_path, "%s/index", afs);
    expect(mkdir_at(data) == 0 && mkdir_at(afs) == 0 && mkdir_at(files) == 0, "fixture directories");
    memset(logical, 0, sizeof logical);
    logical[0] = 0xFFFF;
    ppg = make_ppg(&ppg_size, 0x82, 2, logical, 16 * 16);
    snprintf(member, sizeof member, "%s/Tiny.ppg", files);
    snprintf(note_path, sizeof note_path, "%s/Note.bin", files);
    n = snprintf(index, sizeof index, "sf33rd-afs 1\nfiles 2\n0 Tiny.ppg %u\n1 Note.bin 2\n", (unsigned)ppg_size);
    expect(ppg && n > 0 && write_file(index_path, index, (size_t)n) == 0, "fixture index");
    expect(write_file(member, ppg, ppg_size) == 0 && write_file(note_path, note, 2) == 0, "fixture members");
    err[0] = 0;
    expect(fsOpen(root, 0, &file, err, sizeof err) == 0, "fsOpen reads file 0");
    expect(strcmp(file.name, "Tiny.ppg") == 0 && file.size == ppg_size, "file 0 name and size");
    expect(file.data && file.data[0] == 'p', "file 0 bytes");
    fsClose(&file);
    expect(fsOpen(root, 1, &file, err, sizeof err) == 0 && file.size == 2 && memcmp(file.data, "hi", 2) == 0, "fsOpen reads file 1");
    fsClose(&file);
    expect(fsOpen(root, 2, &file, err, sizeof err) != 0, "fsOpen rejects a number past the index");
    expect(fsOpen(root, 99, &file, err, sizeof err) != 0, "fsOpen rejects 99");
    unlink(member);
    unlink(note_path);
    unlink(index_path);
    rmdir(files);
    rmdir(afs);
    rmdir(data);
    rmdir(root);
    sf_free(ppg);
}

static void test_rank1(void)
{
    SfFsFile file;
    SfFsFile past;
    SfPpgTex none;
    char err[256];
    unsigned count = 0;
    FILE *fp;
    char line[64];
    int i;
    err[0] = 0;
    memset(&file, 0, sizeof file);
    memset(&past, 0, sizeof past);
    memset(&none, 0, sizeof none);
    fp = fopen("data/afs/index", "r");
    if (!fp) {
        printf("skip Rank1 (no archive index)\n");
        return;
    }
    if (!fgets(line, (int)sizeof line, fp) || !fgets(line, (int)sizeof line, fp) || sscanf(line, "files %u", &count) != 1) {
        fclose(fp);
        expect(0, "archive index count");
        return;
    }
    fclose(fp);
    expect(count == 1531, "the archive index lists 1531 files");
    expect(fsOpen(".", count, &past, err, sizeof err) != 0, "a file number past the archive fails");
    fsClose(&past);
    expect(fsOpen(".", 0, &file, err, sizeof err) == 0, "Rank1 opens as file 0");
    expect(file.data && strcmp(file.name, "Rank1.ppg") == 0 && file.size == 20732, "Rank1 name and size");
    if (file.data && strcmp(file.name, "Rank1.ppg") == 0) {
        for (i = 0; i < 8; i++) {
            SfPpgTex tex;
            char msg[64];
            int ok;
            memset(&tex, 0, sizeof tex);
            ok = sf_ppg_tex(file.data, file.size, i, &tex, err, sizeof err) == 0;
            snprintf(msg, sizeof msg, "Rank1 texture %d decodes", i);
            expect(ok, msg);
            if (!ok || !tex.texels) {
                sf_ppg_tex_free(&tex);
                continue;
            }
            snprintf(msg, sizeof msg, "Rank1 texture %d is 128x128 A1R5G5B5", i);
            expect(tex.width == 128 && tex.height == 128 && tex.form == 0x1555 && tex.raw_size == 32768, msg);
            snprintf(msg, sizeof msg, "Rank1 texture %d is inflate pixel 0x82", i);
            expect((tex.compress & 3) == 2 && tex.pixel == 0x82, msg);
            if (i == 0 && tex.rgba) {
                expect(texel_at(&tex, 0) == 0 && texel_at(&tex, 100) == 0x80ea && texel_at(&tex, 16383) == 0x8067, "Rank1 texture 0 texels");
                expect(tex.rgba[100 * 4] == 0 && tex.rgba[100 * 4 + 1] == 57 && tex.rgba[100 * 4 + 2] == 82 && tex.rgba[100 * 4 + 3] == 255, "Rank1 texture 0 pixel 100 is opaque teal");
            }
            if (i == 1) {
                expect(texel_at(&tex, 0) == 0x810b, "Rank1 texture 1 starts at 0x810b");
            }
            sf_ppg_tex_free(&tex);
        }
        expect(sf_ppg_tex(file.data, file.size, 8, &none, err, sizeof err) != 0, "Rank1 has eight textures");
        sf_ppg_tex_free(&none);
    }
    fsClose(&file);
}

int main(void)
{
    test_synthetic();
    test_fs_fixture();
    test_rank1();
    if (g_fail) {
        fprintf(stderr, "%d failed\n", g_fail);
        return 1;
    }
    printf("picture ok\n");
    return 0;
}
