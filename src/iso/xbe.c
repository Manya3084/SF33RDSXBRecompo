#include "iso/xbe.h"

#include "common.h"
#include <stdlib.h>

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

/* An XACT sound bank begins with a 56-byte header (xactengd.lib's
 * _XACT_SOUNDBANK_FILE_HEADER): "SDBK", version 11, a CRC, five table
 * offsets, flags and six counts, then a 16-byte name. Its last table is
 * the RPC table, so a bank with no RPC entries ends at that table's
 * offset. */
#define SDBK_HEADER 0x38u
#define SDBK_VERSION 11u
#define SDBK_MAX (256u * 1024u)

static uint32_t u32le(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static uint16_t u16le(const uint8_t *p)
{
    return (uint16_t)(p[0] | (p[1] << 8));
}

/* The bank number for a name: "SE" is 0, "plNN" is NN + 1. */
static int bank_of(const char *name)
{
    if (strcmp(name, "SE") == 0) {
        return 0;
    }
    if (name[0] == 'p' && name[1] == 'l' && name[2] >= '0' && name[2] <= '9' && name[3] >= '0' && name[3] <= '9' && name[4] == 0) {
        int n = (name[2] - '0') * 10 + (name[3] - '0');
        return n < SF_SOUND_BANKS - 1 ? n + 1 : -1;
    }
    return -1;
}

static int write_bank(const char *out_dir, int bank, const uint8_t *p, uint32_t size, char *err, size_t err_n)
{
    char path[4096];
    char name[16];
    FILE *fp;
    snprintf(name, sizeof name, "%02d.sdbk", bank);
    if (sf_join(path, sizeof path, out_dir, name) != 0) {
        sf_set_err(err, err_n, "sound bank path is too long");
        return -1;
    }
    fp = fopen(path, "wb");
    if (!fp || fwrite(p, 1, size, fp) != size) {
        sf_set_err(err, err_n, "cannot write %s", path);
        if (fp) {
            fclose(fp);
        }
        return -1;
    }
    if (fclose(fp) != 0) {
        sf_set_err(err, err_n, "cannot write %s", path);
        return -1;
    }
    return 0;
}

int sf_xbe_sound_banks(const char *xbe_path, const char *out_dir, char *err, size_t err_n)
{
    FILE *fp;
    long n;
    uint8_t *x;
    uint32_t size;
    uint32_t i;
    int found[SF_SOUND_BANKS] = { 0 };
    int count = 0;
    int rc = -1;
    fp = fopen(xbe_path, "rb");
    if (!fp) {
        sf_set_err(err, err_n, "cannot read %s", xbe_path);
        return -1;
    }
    if (fseek(fp, 0, SEEK_END) != 0 || (n = ftell(fp)) < 0x1000 || n > 64L * 1024 * 1024 || fseek(fp, 0, SEEK_SET) != 0) {
        fclose(fp);
        sf_set_err(err, err_n, "%s is not an XBE", xbe_path);
        return -1;
    }
    size = (uint32_t)n;
    x = malloc(size);
    if (!x || fread(x, 1, size, fp) != size) {
        fclose(fp);
        free(x);
        sf_set_err(err, err_n, "cannot read %s", xbe_path);
        return -1;
    }
    fclose(fp);
    if (memcmp(x, "XBEH", 4) != 0) {
        sf_set_err(err, err_n, "%s is not an XBE", xbe_path);
        goto done;
    }
    if (mkdir(out_dir, 0755) != 0 && errno != EEXIST) {
        sf_set_err(err, err_n, "cannot create %s", out_dir);
        goto done;
    }
    for (i = 0; i + SDBK_HEADER <= size; i++) {
        const uint8_t *p = x + i;
        char name[17];
        uint32_t bank_size;
        int bank;
        if (memcmp(p, "SDBK", 4) != 0 || u16le(p + 4) != SDBK_VERSION) {
            continue;
        }
        memcpy(name, p + 0x28, 16);
        name[16] = 0;
        bank = bank_of(name);
        bank_size = u32le(p + 0x14);
        if (bank < 0 || u16le(p + 0x20) != 0 || bank_size < SDBK_HEADER || bank_size > SDBK_MAX || bank_size > size - i) {
            continue;
        }
        if (found[bank]) {
            sf_set_err(err, err_n, "%s holds sound bank %s twice", xbe_path, name);
            goto done;
        }
        if (write_bank(out_dir, bank, p, bank_size, err, err_n) != 0) {
            goto done;
        }
        found[bank] = 1;
        count++;
    }
    if (count != SF_SOUND_BANKS) {
        sf_set_err(err, err_n, "%s has %d of the game's %d sound banks", xbe_path, count, SF_SOUND_BANKS);
        goto done;
    }
    rc = 0;
done:
    free(x);
    return rc;
}

int sf_xbe_image(const char *xbe_path, const char *out_path, char *err, size_t err_n)
{
    FILE *fp;
    long n;
    uint8_t *x;
    uint32_t size;
    uint32_t base;
    uint32_t nsec;
    uint32_t hdr;
    uint32_t i;
    uint32_t at;
    int rc = -1;
    fp = fopen(xbe_path, "rb");
    if (!fp) {
        sf_set_err(err, err_n, "cannot read %s", xbe_path);
        return -1;
    }
    if (fseek(fp, 0, SEEK_END) != 0 || (n = ftell(fp)) < 0x1000 || n > 64L * 1024 * 1024 || fseek(fp, 0, SEEK_SET) != 0) {
        fclose(fp);
        sf_set_err(err, err_n, "%s is not an XBE", xbe_path);
        return -1;
    }
    size = (uint32_t)n;
    x = malloc(size);
    if (!x || fread(x, 1, size, fp) != size) {
        fclose(fp);
        free(x);
        sf_set_err(err, err_n, "cannot read %s", xbe_path);
        return -1;
    }
    fclose(fp);
    if (memcmp(x, "XBEH", 4) != 0) {
        sf_set_err(err, err_n, "%s is not an XBE", xbe_path);
        goto done;
    }
    base = u32le(x + 0x104);
    nsec = u32le(x + 0x11C);
    hdr = u32le(x + 0x120) - base;
    if (nsec == 0 || nsec > 64 || hdr > size || (uint64_t)hdr + (uint64_t)nsec * 0x38 > size) {
        sf_set_err(err, err_n, "%s has no sections", xbe_path);
        goto done;
    }
    fp = fopen(out_path, "wb");
    if (!fp) {
        sf_set_err(err, err_n, "cannot write %s", out_path);
        goto done;
    }
    {
        uint8_t head[12] = { 'S', 'F', '3', 'I', 'M', 'G', '1', 0 };
        head[8] = (uint8_t)nsec;
        fwrite(head, 1, sizeof head, fp);
    }
    /* the table: each section's address, size and offset in this file */
    at = 12 + nsec * 12;
    for (i = 0; i < nsec; i++) {
        const uint8_t *s = x + hdr + i * 0x38;
        uint32_t entry[3];
        entry[0] = u32le(s + 4);
        entry[1] = u32le(s + 8);
        entry[2] = at;
        fwrite(entry, sizeof entry, 1, fp);
        at += entry[1];
    }
    for (i = 0; i < nsec; i++) {
        const uint8_t *s = x + hdr + i * 0x38;
        uint32_t vsize = u32le(s + 8);
        uint32_t raw = u32le(s + 12);
        uint32_t rsize = u32le(s + 16);
        uint32_t k;
        if (raw > size || rsize > size - raw) {
            rsize = 0;
        }
        if (rsize > vsize) {
            rsize = vsize;
        }
        fwrite(x + raw, 1, rsize, fp);
        for (k = rsize; k < vsize; k++) {
            fputc(0, fp);
        }
    }
    rc = fclose(fp) == 0 ? 0 : -1;
    if (rc != 0) {
        sf_set_err(err, err_n, "cannot write %s", out_path);
    }
done:
    free(x);
    return rc;
}
