#include "host/compat.h"
#include "iso/xdvdfs.h"

#include "common.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define SECTOR 2048u
#define DIR_MAX (1u << 20)
#define MAGIC "MICROSOFT*XBOX*MEDIA"
#define MAGIC_LEN 20

struct SfXdvdfs {
    int fd;
    uint64_t base;
    uint64_t file_size;
    uint32_t root_sector;
    uint32_t root_size;
};

static uint16_t ru16(const uint8_t *p)
{
    return (uint16_t)(p[0] | ((uint16_t)p[1] << 8));
}

static uint32_t ru32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static int read_at(int fd, uint64_t off, void *dst, size_t n)
{
    uint8_t *p = dst;
    size_t got = 0;
    while (got < n) {
        ssize_t r = pread(fd, p + got, n - got, (off_t)off + (off_t)got);
        if (r < 0 && errno == EINTR) {
            continue;
        }
        if (r <= 0) {
            return -1;
        }
        got += (size_t)r;
    }
    return 0;
}

static int no_child(uint16_t link)
{
    return link == 0 || link == 0xFFFF;
}

static int name_cmp(const char *a, size_t an, const char *b, size_t bn)
{
    size_t n = an < bn ? an : bn;
    size_t i;
    for (i = 0; i < n; i++) {
        int ca = tolower((unsigned char)a[i]);
        int cb = tolower((unsigned char)b[i]);
        if (ca != cb) {
            return (ca > cb) - (ca < cb);
        }
    }
    return (an > bn) - (an < bn);
}

typedef struct DirEnt {
    uint16_t left;
    uint16_t right;
    uint32_t sector;
    uint32_t size;
    uint8_t attr;
    uint8_t nlen;
    char name[256];
} DirEnt;

static int parse_ent(const uint8_t *dir, uint32_t dir_n, uint32_t off, DirEnt *e, char *err, size_t err_n)
{
    const uint8_t *p;
    if (off > dir_n || dir_n - off < 14) {
        sf_set_err(err, err_n, "truncated Xbox directory entry");
        return -1;
    }
    p = dir + off;
    e->left = ru16(p);
    e->right = ru16(p + 2);
    e->sector = ru32(p + 4);
    e->size = ru32(p + 8);
    e->attr = p[12];
    e->nlen = p[13];
    if (e->nlen == 0 || (uint32_t)14 + e->nlen > dir_n - off) {
        sf_set_err(err, err_n, "truncated Xbox file name");
        return -1;
    }
    memcpy(e->name, p + 14, e->nlen);
    e->name[e->nlen] = 0;
    return 0;
}

static int load_dir(SfXdvdfs *img, uint32_t sector, uint32_t size, uint8_t **out, char *err, size_t err_n)
{
    uint64_t off;
    uint8_t *buf;
    if (size == 0 || (size % SECTOR) != 0 || size > DIR_MAX) {
        sf_set_err(err, err_n, "Xbox directory size is not usable");
        return -1;
    }
    off = img->base + (uint64_t)sector * SECTOR;
    if (off + size > img->file_size) {
        sf_set_err(err, err_n, "Xbox directory runs past the end of the image");
        return -1;
    }
    buf = malloc(size);
    if (!buf) {
        sf_set_err(err, err_n, "out of memory");
        return -1;
    }
    if (read_at(img->fd, off, buf, size) != 0) {
        free(buf);
        sf_set_err(err, err_n, "failed to read an Xbox directory");
        return -1;
    }
    *out = buf;
    return 0;
}

static int accept_hit(int fd, uint64_t file_size, uint64_t hit, uint64_t *base, uint32_t *root_sector, uint32_t *root_size)
{
    uint8_t tmp[20];
    uint8_t fields[8];
    uint32_t sector;
    uint32_t dsize;
    uint64_t vol;
    if (hit < 0x10000 || hit + 0x7EC + MAGIC_LEN > file_size) {
        return 0;
    }
    if (read_at(fd, hit + 0x7EC, tmp, MAGIC_LEN) != 0 || memcmp(tmp, MAGIC, MAGIC_LEN) != 0) {
        return 0;
    }
    if (read_at(fd, hit + 0x14, fields, 8) != 0) {
        return 0;
    }
    sector = ru32(fields);
    dsize = ru32(fields + 4);
    if (sector == 0 || dsize == 0 || (dsize % SECTOR) != 0 || dsize > DIR_MAX) {
        return 0;
    }
    vol = hit - 0x10000;
    if (vol + (uint64_t)sector * SECTOR + dsize > file_size) {
        return 0;
    }
    *base = vol;
    *root_sector = sector;
    *root_size = dsize;
    return 1;
}

SfXdvdfs *sf_xdvdfs_open(const char *path, char *err, size_t err_n)
{
    SfXdvdfs *img;
    uint8_t *buf;
    uint64_t buf_off;
    size_t have;
    int fd;
    off_t end;
    const size_t cap = (1u << 20) + (MAGIC_LEN - 1);

    fd = sf_open_source(path);
    if (fd < 0) {
        sf_set_err(err, err_n, "cannot open %s", path);
        return NULL;
    }
    end = lseek(fd, 0, SEEK_END);
    if (end < 0) {
        close(fd);
        sf_set_err(err, err_n, "cannot size the disc image");
        return NULL;
    }
    img = calloc(1, sizeof(*img));
    buf = malloc(cap);
    if (!img || !buf) {
        free(img);
        free(buf);
        close(fd);
        sf_set_err(err, err_n, "out of memory");
        return NULL;
    }
    img->fd = fd;
    img->file_size = (uint64_t)end;
    buf_off = 0;
    have = 0;
    while (img->root_sector == 0) {
        size_t i;
        if (buf_off + have >= img->file_size && have < MAGIC_LEN) {
            break;
        }
        if (buf_off + have < img->file_size) {
            size_t want = cap - have;
            uint64_t at = buf_off + have;
            if (at + want > img->file_size) {
                want = (size_t)(img->file_size - at);
            }
            if (want > 0) {
                if (read_at(fd, at, buf + have, want) != 0) {
                    sf_set_err(err, err_n, "failed while scanning for the Xbox volume");
                    sf_xdvdfs_close(img);
                    free(buf);
                    return NULL;
                }
                have += want;
            }
        }
        for (i = 0; i + MAGIC_LEN <= have; i++) {
            uint64_t hit;
            if (memcmp(buf + i, MAGIC, MAGIC_LEN) != 0) {
                continue;
            }
            hit = buf_off + i;
            if (accept_hit(fd, img->file_size, hit, &img->base, &img->root_sector, &img->root_size)) {
                break;
            }
        }
        if (img->root_sector != 0 || buf_off + have >= img->file_size) {
            break;
        }
        if (have <= MAGIC_LEN - 1) {
            break;
        }
        memmove(buf, buf + have - (MAGIC_LEN - 1), MAGIC_LEN - 1);
        buf_off += have - (MAGIC_LEN - 1);
        have = MAGIC_LEN - 1;
    }
    free(buf);
    if (img->root_sector == 0) {
        uint8_t pvd[40];
        /* an ISO 9660 volume for the PlayStation is the PS2 disc */
        if (read_at(fd, 0x8000, pvd, sizeof pvd) == 0 && memcmp(pvd + 1, "CD001", 5) == 0 &&
            memcmp(pvd + 8, "PLAYSTATION", 11) == 0) {
            sf_set_err(err, err_n, "This is the PlayStation 2 disc. The port needs the Xbox disc.");
        } else {
            sf_set_err(err, err_n, "not an Xbox XDVDFS image");
        }
        sf_xdvdfs_close(img);
        return NULL;
    }
    return img;
}

void sf_xdvdfs_close(SfXdvdfs *img)
{
    if (!img) {
        return;
    }
    if (img->fd >= 0) {
        close(img->fd);
    }
    free(img);
}

static int find_component(const uint8_t *dir, uint32_t dir_n, const char *name, size_t nlen, DirEnt *out, char *err, size_t err_n)
{
    uint8_t *seen;
    uint32_t off = 0;
    uint32_t bits;
    int step;

    bits = dir_n / 4;
    if (bits == 0) {
        sf_set_err(err, err_n, "empty Xbox directory");
        return -1;
    }
    seen = calloc(bits, 1);
    if (!seen) {
        sf_set_err(err, err_n, "out of memory");
        return -1;
    }
    for (step = 0; step < 100000; step++) {
        DirEnt ent;
        int cmp;
        uint16_t next;
        uint32_t idx;
        if ((off & 3u) != 0 || off >= dir_n) {
            free(seen);
            sf_set_err(err, err_n, "Xbox directory link is out of range");
            return -1;
        }
        idx = off / 4;
        if (idx >= bits || seen[idx]) {
            free(seen);
            sf_set_err(err, err_n, "Xbox directory tree loops");
            return -1;
        }
        seen[idx] = 1;
        if (parse_ent(dir, dir_n, off, &ent, err, err_n) != 0) {
            free(seen);
            return -1;
        }
        cmp = name_cmp(name, nlen, ent.name, ent.nlen);
        if (cmp == 0) {
            *out = ent;
            free(seen);
            return 0;
        }
        next = cmp < 0 ? ent.left : ent.right;
        if (no_child(next)) {
            free(seen);
            return 1;
        }
        off = (uint32_t)next * 4u;
    }
    free(seen);
    sf_set_err(err, err_n, "Xbox directory tree is too deep");
    return -1;
}

static void fill_ent(SfXdvdfsEnt *out, const DirEnt *ent)
{
    memset(out, 0, sizeof(*out));
    out->sector = ent->sector;
    out->size = ent->size;
    out->is_dir = (ent->attr & 0x10) != 0;
    memcpy(out->name, ent->name, ent->nlen + 1);
}

int sf_xdvdfs_find(SfXdvdfs *img, const char *path, SfXdvdfsEnt *out, char *err, size_t err_n)
{
    uint32_t sector;
    uint32_t dsize;
    const char *p;

    if (!img || !path || !path[0] || !out) {
        sf_set_err(err, err_n, "missing Xbox path");
        return -1;
    }
    sector = img->root_sector;
    dsize = img->root_size;
    p = path;
    while (*p == '/') {
        p++;
    }
    if (*p == 0) {
        sf_set_err(err, err_n, "missing Xbox path");
        return -1;
    }
    while (*p) {
        const char *start = p;
        size_t nlen;
        uint8_t *dir = NULL;
        DirEnt ent;
        int rc;
        int more;
        while (*p && *p != '/') {
            p++;
        }
        nlen = (size_t)(p - start);
        if (nlen == 0 || nlen > 255) {
            sf_set_err(err, err_n, "bad Xbox path");
            return -1;
        }
        if (load_dir(img, sector, dsize, &dir, err, err_n) != 0) {
            return -1;
        }
        rc = find_component(dir, dsize, start, nlen, &ent, err, err_n);
        free(dir);
        if (rc != 0) {
            return rc;
        }
        while (*p == '/') {
            p++;
        }
        more = *p != 0;
        if (more) {
            if ((ent.attr & 0x10) == 0) {
                return 1;
            }
            sector = ent.sector;
            dsize = ent.size;
            continue;
        }
        fill_ent(out, &ent);
        return 0;
    }
    return 1;
}

int sf_xdvdfs_save(SfXdvdfs *img, const SfXdvdfsEnt *ent, const char *dest, char *err, size_t err_n)
{
    uint64_t off;
    uint32_t left;
    uint8_t buf[65536];
    FILE *out;

    if (!img || !ent || !dest) {
        sf_set_err(err, err_n, "missing Xbox file");
        return -1;
    }
    if (ent->is_dir) {
        sf_set_err(err, err_n, "Xbox path is a directory");
        return -1;
    }
    off = img->base + (uint64_t)ent->sector * SECTOR;
    if ((uint64_t)ent->size > img->file_size || off + ent->size > img->file_size) {
        sf_set_err(err, err_n, "Xbox file runs past the end of the image");
        return -1;
    }
    out = fopen(dest, "wb");
    if (!out) {
        sf_set_err(err, err_n, "cannot write %s", dest);
        return -1;
    }
    left = ent->size;
    while (left > 0) {
        size_t chunk = left > sizeof buf ? sizeof buf : left;
        if (read_at(img->fd, off, buf, chunk) != 0 || fwrite(buf, 1, chunk, out) != chunk) {
            fclose(out);
            sf_set_err(err, err_n, "failed to copy an Xbox file");
            return -1;
        }
        off += chunk;
        left -= (uint32_t)chunk;
    }
    if (fclose(out) != 0) {
        sf_set_err(err, err_n, "failed to finish %s", dest);
        return -1;
    }
    return 0;
}

static int append_path(char *dst, size_t n, const char *parent, const char *name)
{
    int wrote = parent[0] == 0 ? snprintf(dst, n, "%s", name) : snprintf(dst, n, "%s/%s", parent, name);
    if (wrote < 0 || (size_t)wrote >= n) {
        return -1;
    }
    return 0;
}

static int walk_loaded(SfXdvdfs *img, uint8_t *dir, uint32_t dir_n, const char *prefix, uint32_t *stack, int depth, SfXdvdfsVisit visit, void *user, char *err, size_t err_n);

static int walk_node(SfXdvdfs *img, uint8_t *dir, uint32_t dir_n, uint32_t off, uint8_t *seen, int at_root, const char *prefix, uint32_t *stack, int depth, SfXdvdfsVisit visit, void *user, char *err, size_t err_n)
{
    DirEnt ent;
    uint32_t idx;
    char path[1024];
    SfXdvdfsEnt shown;
    int rc;

    if (!at_root && off == 0) {
        return 0;
    }
    if ((off & 3u) != 0 || off >= dir_n) {
        sf_set_err(err, err_n, "Xbox directory link is out of range");
        return -1;
    }
    idx = off / 4;
    if (idx >= dir_n / 4 || seen[idx]) {
        sf_set_err(err, err_n, "Xbox directory tree loops");
        return -1;
    }
    seen[idx] = 1;
    if (parse_ent(dir, dir_n, off, &ent, err, err_n) != 0) {
        return -1;
    }
    if (!no_child(ent.left)) {
        rc = walk_node(img, dir, dir_n, (uint32_t)ent.left * 4u, seen, 0, prefix, stack, depth, visit, user, err, err_n);
        if (rc != 0) {
            return rc;
        }
    }
    if (append_path(path, sizeof path, prefix, ent.name) != 0) {
        sf_set_err(err, err_n, "Xbox path is too long");
        return -1;
    }
    fill_ent(&shown, &ent);
    if (visit && visit(path, &shown, user) != 0) {
        return 2;
    }
    if (shown.is_dir) {
        uint8_t *child = NULL;
        int i;
        if (depth + 1 >= 16) {
            sf_set_err(err, err_n, "Xbox directories are nested too deeply");
            return -1;
        }
        for (i = 0; i < depth; i++) {
            if (stack[i] == ent.sector) {
                sf_set_err(err, err_n, "Xbox directory sector loops");
                return -1;
            }
        }
        stack[depth] = ent.sector;
        if (load_dir(img, ent.sector, ent.size, &child, err, err_n) != 0) {
            return -1;
        }
        rc = walk_loaded(img, child, ent.size, path, stack, depth + 1, visit, user, err, err_n);
        free(child);
        if (rc != 0) {
            return rc;
        }
    }
    if (!no_child(ent.right)) {
        return walk_node(img, dir, dir_n, (uint32_t)ent.right * 4u, seen, 0, prefix, stack, depth, visit, user, err, err_n);
    }
    return 0;
}

static int walk_loaded(SfXdvdfs *img, uint8_t *dir, uint32_t dir_n, const char *prefix, uint32_t *stack, int depth, SfXdvdfsVisit visit, void *user, char *err, size_t err_n)
{
    uint8_t *seen;
    int rc;
    if (dir_n < 4) {
        sf_set_err(err, err_n, "empty Xbox directory");
        return -1;
    }
    seen = calloc(dir_n / 4, 1);
    if (!seen) {
        sf_set_err(err, err_n, "out of memory");
        return -1;
    }
    rc = walk_node(img, dir, dir_n, 0, seen, 1, prefix, stack, depth, visit, user, err, err_n);
    free(seen);
    return rc;
}

int sf_xdvdfs_walk(SfXdvdfs *img, const char *dirpath, SfXdvdfsVisit visit, void *user, char *err, size_t err_n)
{
    uint32_t stack[16];
    uint8_t *dir = NULL;
    uint32_t sector;
    uint32_t dsize;
    int rc;

    if (!img) {
        sf_set_err(err, err_n, "missing Xbox image");
        return -1;
    }
    sector = img->root_sector;
    dsize = img->root_size;
    if (dirpath && dirpath[0]) {
        SfXdvdfsEnt ent;
        rc = sf_xdvdfs_find(img, dirpath, &ent, err, err_n);
        if (rc != 0) {
            return rc;
        }
        if (!ent.is_dir) {
            return 1;
        }
        sector = ent.sector;
        dsize = ent.size;
    } else {
        dirpath = "";
    }
    if (load_dir(img, sector, dsize, &dir, err, err_n) != 0) {
        return -1;
    }
    stack[0] = sector;
    rc = walk_loaded(img, dir, dsize, dirpath, stack, 1, visit, user, err, err_n);
    free(dir);
    if (rc == 2) {
        return 0;
    }
    return rc;
}
