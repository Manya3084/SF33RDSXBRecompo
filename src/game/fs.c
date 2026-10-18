#include "game/fs.h"

#include "common.h"
#include "mem/heap.h"

#include <stdio.h>
#include <string.h>

static int name_ok(const char *name)
{
    size_t i;
    if (!name || name[0] == 0 || strcmp(name, ".") == 0 || strcmp(name, "..") == 0) {
        return 0;
    }
    for (i = 0; name[i] != 0; i++) {
        unsigned char c = (unsigned char)name[i];
        if (c < 0x20 || c == '/' || c == '\\' || c == ':') {
            return 0;
        }
    }
    return i < 48;
}

static int load_member(const char *install, const char *name, uint32_t expect, uint8_t **out, char *err, size_t err_n)
{
    char files[8192];
    char path[8192];
    FILE *fp;
    long n;
    uint8_t *data;
    if (sf_join(files, sizeof files, install, "data/afs/files") != 0 || sf_join(path, sizeof path, files, name) != 0) {
        sf_set_err(err, err_n, "archive path does not fit");
        return -1;
    }
    fp = fopen(path, "rb");
    if (!fp) {
        sf_set_err(err, err_n, "cannot open %s", name);
        return -1;
    }
    if (fseek(fp, 0, SEEK_END) != 0) {
        fclose(fp);
        sf_set_err(err, err_n, "cannot size %s", name);
        return -1;
    }
    n = ftell(fp);
    if (n < 0 || (unsigned long)n != expect) {
        fclose(fp);
        sf_set_err(err, err_n, "%s size does not match the index", name);
        return -1;
    }
    if (fseek(fp, 0, SEEK_SET) != 0) {
        fclose(fp);
        sf_set_err(err, err_n, "cannot read %s", name);
        return -1;
    }
    data = sf_alloc((size_t)n == 0 ? 1 : (size_t)n);
    if (!data) {
        fclose(fp);
        sf_set_err(err, err_n, "out of memory");
        return -1;
    }
    if (n > 0 && fread(data, 1, (size_t)n, fp) != (size_t)n) {
        sf_free(data);
        fclose(fp);
        sf_set_err(err, err_n, "short read of %s", name);
        return -1;
    }
    fclose(fp);
    *out = data;
    return 0;
}

int fsOpen(const char *install, uint32_t fnum, SfFsFile *out, char *err, size_t err_n)
{
    char index_path[8192];
    FILE *fp;
    char line[256];
    unsigned version = 0;
    unsigned count = 0;
    int found = 0;
    char name[48];
    unsigned bytes = 0;
    memset(name, 0, sizeof name);
    if (out) {
        memset(out, 0, sizeof *out);
    }
    if (!install || !out) {
        sf_set_err(err, err_n, "missing archive arguments");
        return -1;
    }
    if (sf_join(index_path, sizeof index_path, install, "data/afs/index") != 0) {
        sf_set_err(err, err_n, "archive index path does not fit");
        return -1;
    }
    fp = fopen(index_path, "r");
    if (!fp) {
        sf_set_err(err, err_n, "archive index is missing");
        return -1;
    }
    if (!fgets(line, (int)sizeof line, fp) || sscanf(line, "sf33rd-afs %u", &version) != 1 || version != 1) {
        fclose(fp);
        sf_set_err(err, err_n, "archive index header is wrong");
        return -1;
    }
    if (!fgets(line, (int)sizeof line, fp) || sscanf(line, "files %u", &count) != 1) {
        fclose(fp);
        sf_set_err(err, err_n, "archive index count is wrong");
        return -1;
    }
    if (fnum >= count) {
        fclose(fp);
        sf_set_err(err, err_n, "file number %u is past the archive", fnum);
        return -1;
    }
    while (fgets(line, (int)sizeof line, fp)) {
        unsigned num = 0;
        if (sscanf(line, "%u %47s %u", &num, name, &bytes) != 3) {
            continue;
        }
        if (num == fnum) {
            found = 1;
            break;
        }
    }
    fclose(fp);
    if (!found || bytes == 0 || !name_ok(name)) {
        sf_set_err(err, err_n, "file number %u has no archive member", fnum);
        return -1;
    }
    out->fnum = fnum;
    out->size = bytes;
    memcpy(out->name, name, sizeof out->name);
    if (load_member(install, name, bytes, &out->data, err, err_n) != 0) {
        memset(out, 0, sizeof *out);
        return -1;
    }
    return 0;
}

void fsClose(SfFsFile *file)
{
    if (!file) {
        return;
    }
    sf_free(file->data);
    memset(file, 0, sizeof *file);
}
