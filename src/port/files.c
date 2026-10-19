/* The game's file calls (CRI's ADXF API) over the imported archive.
 *
 * The game reads SF33RDXB.AFS by member number, in 2048-byte sectors from
 * a running position (GD3rd.c's fsOpen and fsRequestFileRead). Retail's
 * CRI layer reads the disc asynchronously; here a read is done when it is
 * asked for, so the next status poll already sees it finished. A read runs
 * on through the sectors after the member, as a sector read from the disc
 * does. */

#include "host/compat.h"
#include "host/paths.h"
#include "port/port.h"

#include "types.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define ADXF_STAT_STOP 1
#define ADXF_STAT_READING 2
#define ADXF_STAT_READEND 3
#define ADXF_STAT_ERROR 4
#define SECTOR 2048

typedef struct AdxfFile {
    int used;
    u32 offset;
    u32 size;
    u32 pos; /* in sectors */
    s32 stat;
} AdxfFile;

static int s_fd = -1;
static u32 s_count;
static u32 *s_toc; /* offset, size pairs */
static off_t s_afs_size;
static s32 s_pt_stat = ADXF_STAT_STOP;
static AdxfFile s_files[16];

s32 ADXF_LoadPartitionNw(s32 ptid, const char *fname, void *dir, void *ptinfo)
{
    char base[4096];
    char path[4096 + 32];
    u32 head[2];
    (void)ptid;
    (void)fname;
    (void)dir;
    (void)ptinfo;
    if (s_fd >= 0) {
        s_pt_stat = ADXF_STAT_READEND;
        return 0;
    }
    if (sf_exe_dir(base, sizeof base) != 0) {
        s_pt_stat = ADXF_STAT_ERROR;
        return -1;
    }
    snprintf(path, sizeof path, "%s/data/afs/SF33RDXB.AFS", base);
    s_fd = open(path, O_RDONLY | O_BINARY);
    if (s_fd < 0 || pread(s_fd, head, sizeof head, 0) != (ssize_t)sizeof head || memcmp(head, "AFS", 4) != 0) {
        fprintf(stderr, "files: %s is not the imported archive\n", path);
        s_pt_stat = ADXF_STAT_ERROR;
        return -1;
    }
    s_count = head[1];
    s_toc = malloc((size_t)s_count * 8);
    if (!s_toc || pread(s_fd, s_toc, (size_t)s_count * 8, 8) != (ssize_t)((size_t)s_count * 8)) {
        s_pt_stat = ADXF_STAT_ERROR;
        return -1;
    }
    s_afs_size = lseek(s_fd, 0, SEEK_END);
    s_pt_stat = ADXF_STAT_READEND;
    return 0;
}

s32 ADXF_GetPtStat(s32 ptid)
{
    (void)ptid;
    return s_pt_stat;
}

void *ADXF_OpenAfs(s32 ptid, s32 flid)
{
    size_t i;
    (void)ptid;
    if (s_fd < 0 || flid < 0 || (u32)flid >= s_count) {
        return NULL;
    }
    for (i = 0; i < sizeof s_files / sizeof s_files[0]; i++) {
        if (!s_files[i].used) {
            s_files[i].used = 1;
            s_files[i].offset = s_toc[flid * 2];
            s_files[i].size = s_toc[flid * 2 + 1];
            s_files[i].pos = 0;
            s_files[i].stat = ADXF_STAT_STOP;
            return &s_files[i];
        }
    }
    fprintf(stderr, "files: no free ADXF handle\n");
    return NULL;
}

void ADXF_Close(void *adxf)
{
    if (adxf) {
        ((AdxfFile *)adxf)->used = 0;
    }
}

s32 ADXF_GetStat(void *adxf)
{
    return adxf ? ((AdxfFile *)adxf)->stat : ADXF_STAT_ERROR;
}

s32 ADXF_StopNw(void *adxf)
{
    if (adxf) {
        ((AdxfFile *)adxf)->stat = ADXF_STAT_STOP;
    }
    return 0;
}

s32 ADXF_ReadNw(void *adxf, s32 nsct, void *buf)
{
    AdxfFile *f = adxf;
    off_t at;
    size_t want;
    ssize_t got;
    if (!f || nsct <= 0) {
        return 0;
    }
    at = (off_t)f->offset + (off_t)f->pos * SECTOR;
    want = (size_t)nsct * SECTOR;
    if (at >= s_afs_size) {
        f->stat = ADXF_STAT_ERROR;
        return 0;
    }
    if (at + (off_t)want > s_afs_size) {
        want = (size_t)(s_afs_size - at);
    }
    got = pread(s_fd, buf, want, at);
    if (got < 0) {
        fprintf(stderr, "files: reading %zu bytes at %lld into %p: %s\n", want, (long long)at, buf, strerror(errno));
        f->stat = ADXF_STAT_ERROR;
        return 0;
    }
    f->pos += (u32)nsct;
    f->stat = ADXF_STAT_READEND;
    return nsct;
}

/* A whole archive member, malloc'd (the sound code's reads). */
int sf_afs_read(s32 fid, u8 **data, u32 *size)
{
    u8 *buf;
    if (s_fd < 0 || fid < 0 || (u32)fid >= s_count) {
        return -1;
    }
    *size = s_toc[fid * 2 + 1];
    buf = malloc(*size ? *size : 1);
    if (!buf || pread(s_fd, buf, *size, (off_t)s_toc[fid * 2]) != (ssize_t)*size) {
        free(buf);
        return -1;
    }
    *data = buf;
    return 0;
}

/* the CRI server tick: nothing to run, reads finish when asked for */
void ADXM_ExecMain(void)
{
}
