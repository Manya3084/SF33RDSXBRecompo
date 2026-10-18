#include "iso/afs.h"

#include "common.h"
#include "iso/archive.h"

#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static uint32_t ru32(const unsigned char *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static int clean_name(const unsigned char *raw, char *out, size_t n)
{
    size_t i;
    for (i = 0; i < 32 && raw[i] != 0; i++) {
        unsigned char c = raw[i];
        if (c < 0x20 || c == 0x7F || c == '/' || c == '\\' || c == ':') {
            return -1;
        }
    }
    if (i == 0 || i >= n || (i == 1 && raw[0] == '.') || (i == 2 && raw[0] == '.' && raw[1] == '.')) {
        return -1;
    }
    memcpy(out, raw, i);
    out[i] = 0;
    return 0;
}

static int copy_span(FILE *in, FILE *out, uint32_t left, char *err, size_t err_n)
{
    char buf[8192];
    while (left) {
        size_t chunk = left > sizeof buf ? sizeof buf : left;
        if (fread(buf, 1, chunk, in) != chunk) {
            sf_set_err(err, err_n, "AFS read ended early");
            return -1;
        }
        if (fwrite(buf, 1, chunk, out) != chunk) {
            sf_set_err(err, err_n, "cannot write an AFS file");
            return -1;
        }
        left -= (uint32_t)chunk;
    }
    return 0;
}

int sf_afs_unpack(const char *afs_path, const char *dest_dir, const char *index_path,
                  void (*progress)(const char *name, void *user), void *user,
                  int *written, char *err, size_t err_n)
{
    FILE *in;
    FILE *index = NULL;
    unsigned char hdr[8];
    struct stat st;
    uint64_t file_size;
    uint32_t count;
    uint32_t i;
    unsigned char toc_tail[8];
    uint32_t name_off;
    uint32_t name_size;
    if (written) {
        *written = 0;
    }
    if (!afs_path || !dest_dir || !index_path) {
        sf_set_err(err, err_n, "missing AFS path");
        return -1;
    }
    in = fopen(afs_path, "rb");
    if (!in) {
        sf_set_err(err, err_n, "cannot open the AFS");
        return -1;
    }
    if (fstat(fileno(in), &st) != 0 || st.st_size < 16) {
        fclose(in);
        return 1;
    }
    file_size = (uint64_t)st.st_size;
    if (fread(hdr, 1, 8, in) != 8 || memcmp(hdr, "AFS", 3) != 0 || hdr[3] != 0) {
        fclose(in);
        return 1;
    }
    count = ru32(hdr + 4);
    if (count == 0 || count > 8192 || file_size > 0xFFFFFFFFu || (uint64_t)8 + (uint64_t)count * 8 + 8 > file_size) {
        fclose(in);
        sf_set_err(err, err_n, "AFS directory is not usable");
        return -1;
    }
    if (fseek(in, 8 + (long)count * 8, SEEK_SET) != 0 || fread(toc_tail, 1, 8, in) != 8) {
        fclose(in);
        sf_set_err(err, err_n, "AFS directory is not usable");
        return -1;
    }
    name_off = ru32(toc_tail);
    name_size = ru32(toc_tail + 4);
    if ((uint64_t)name_off + (uint64_t)count * 48 > file_size || name_size < count * 48) {
        fclose(in);
        sf_set_err(err, err_n, "AFS filename table is not usable");
        return -1;
    }
    if (sf_mkdir_p(dest_dir, err, err_n) != 0) {
        fclose(in);
        return -1;
    }
    index = fopen(index_path, "wb");
    if (!index) {
        fclose(in);
        sf_set_err(err, err_n, "cannot write the AFS index");
        return -1;
    }
    if (fprintf(index, "sf33rd-afs 1\nfiles %u\n", count) < 0) {
        fclose(index);
        unlink(index_path);
        fclose(in);
        sf_set_err(err, err_n, "cannot write the AFS index");
        return -1;
    }
    for (i = 0; i < count; i++) {
        unsigned char ent[8];
        unsigned char raw[32];
        char name[40];
        char dest[4096];
        uint32_t offset;
        uint32_t size;
        FILE *out;
        if (fseek(in, 8 + (long)i * 8, SEEK_SET) != 0 || fread(ent, 1, 8, in) != 8) {
            sf_set_err(err, err_n, "AFS directory is not usable");
            goto bad;
        }
        offset = ru32(ent);
        size = ru32(ent + 4);
        if ((uint64_t)offset + size > file_size) {
            sf_set_err(err, err_n, "AFS file %u is outside the archive", i);
            goto bad;
        }
        if (fseek(in, (long)name_off + (long)i * 48, SEEK_SET) != 0 || fread(raw, 1, 32, in) != 32 || clean_name(raw, name, sizeof name) != 0) {
            sf_set_err(err, err_n, "AFS file %u has no usable name", i);
            goto bad;
        }
        if (sf_join(dest, sizeof dest, dest_dir, name) != 0) {
            sf_set_err(err, err_n, "AFS path is too long");
            goto bad;
        }
        out = fopen(dest, "wb");
        if (!out) {
            sf_set_err(err, err_n, "cannot write %s", name);
            goto bad;
        }
        if (fseek(in, (long)offset, SEEK_SET) != 0 || copy_span(in, out, size, err, err_n) != 0) {
            fclose(out);
            unlink(dest);
            goto bad;
        }
        if (fclose(out) != 0) {
            unlink(dest);
            sf_set_err(err, err_n, "cannot write %s", name);
            goto bad;
        }
        if (fprintf(index, "%u %s %u\n", i, name, size) < 0) {
            unlink(dest);
            sf_set_err(err, err_n, "cannot write the AFS index");
            goto bad;
        }
        if (written) {
            (*written)++;
        }
        if (progress) {
            progress(name, user);
        }
    }
    if (fclose(index) != 0) {
        unlink(index_path);
        fclose(in);
        sf_set_err(err, err_n, "cannot write the AFS index");
        return -1;
    }
    fclose(in);
    return 0;

bad:
    if (index) {
        fclose(index);
        unlink(index_path);
    }
    fclose(in);
    return -1;
}
