/* What retail's image held at an address: for the tables the game reads
 * past their ends (the decomp's XB_PAST: the computer's damage tables, an
 * effect's hit boxes). On the Xbox such a read took whatever followed the
 * table; a build of our own lays its globals out otherwise (and gcc and
 * clang each their own way), so the game went otherwise from one build to
 * the next. Here the bytes come from sf33rd.xbe's sections as they load
 * (data/xbe/image.bin, written at import: src/iso/xbe.c), and a live
 * variable that sits there in retail is read live.
 *
 * Without the image (data imported before 1.2) a read past a table reads
 * past our own table, as before. */

#include "host/paths.h"
#include "port/port.h"

#include "types.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct Section {
    uint32_t va;
    uint32_t size;
    uint32_t at; /* in the file */
} Section;

static struct {
    int tried;
    uint8_t *file;
    size_t file_n;
    Section sec[64];
    int n;
} s_img;

/* Retail's variables next to the tables read past (writable data: what
 * retail read there was their value at the time). */
extern u8 omSelObjNowOnMemoryType; /* 0x3330A4, after eff_hit_data */

static const struct {
    uint32_t va;
    uint32_t size;
    const void *live;
} k_live[] = {
    { 0x3330A4, 1, &omSelObjNowOnMemoryType },
};

static void load(void)
{
    char base[4096];
    char path[4200];
    FILE *f;
    long n;
    uint32_t count;
    int i;
    s_img.tried = 1;
    if (sf_exe_dir(base, sizeof base) != 0) {
        return;
    }
    snprintf(path, sizeof path, "%s/data/xbe/image.bin", base);
    f = fopen(path, "rb");
    if (!f) {
        fprintf(stderr, "retail: no %s (import the disc again): tables read past their ends read our own memory\n", path);
        return;
    }
    if (fseek(f, 0, SEEK_END) != 0 || (n = ftell(f)) < 12 || fseek(f, 0, SEEK_SET) != 0) {
        fclose(f);
        return;
    }
    s_img.file = malloc((size_t)n);
    if (!s_img.file || fread(s_img.file, 1, (size_t)n, f) != (size_t)n || memcmp(s_img.file, "SF3IMG1", 8) != 0) {
        fclose(f);
        free(s_img.file);
        s_img.file = NULL;
        fprintf(stderr, "retail: %s is not an image\n", path);
        return;
    }
    fclose(f);
    s_img.file_n = (size_t)n;
    count = s_img.file[8];
    for (i = 0; i < (int)count && i < 64 && 12 + (i + 1) * 12 <= n; i++) {
        memcpy(&s_img.sec[i], s_img.file + 12 + i * 12, sizeof s_img.sec[i]);
        if ((size_t)s_img.sec[i].at + s_img.sec[i].size > s_img.file_n) {
            s_img.sec[i].size = 0;
        }
    }
    s_img.n = i;
}

/* Retail's byte at va, 0 where retail had nothing loaded; 1 when found. */
static int byte_at(uint32_t va, uint8_t *out)
{
    int i;
    for (i = 0; i < (int)(sizeof k_live / sizeof k_live[0]); i++) {
        if (va >= k_live[i].va && va < k_live[i].va + k_live[i].size) {
            *out = ((const uint8_t *)k_live[i].live)[va - k_live[i].va];
            return 1;
        }
    }
    for (i = 0; i < s_img.n; i++) {
        if (va >= s_img.sec[i].va && va - s_img.sec[i].va < s_img.sec[i].size) {
            *out = s_img.file[s_img.sec[i].at + (va - s_img.sec[i].va)];
            return 1;
        }
    }
    *out = 0;
    return 0;
}

/* XB_PAST's read: what retail held at va + off, a window of it (a hit box
 * row is read whole); live + off without the image. One caller at a time
 * (the game thread), and the window holds until the next call. */
const void *xb_retail_or(const void *live, u32 va, s32 off)
{
    static uint8_t window[32];
    int i;
    if (!s_img.tried) {
        load();
    }
    if (!s_img.file) {
        return (const uint8_t *)live + off;
    }
    for (i = 0; i < (int)sizeof window; i++) {
        byte_at(va + (uint32_t)off + (uint32_t)i, &window[i]);
    }
    return window;
}

/* The image is there: what a netplay session asks both sides to have. */
int sf_retail_image_ok(void)
{
    if (!s_img.tried) {
        load();
    }
    return s_img.file != NULL;
}
