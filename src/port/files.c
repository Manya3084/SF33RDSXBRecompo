/* The game's file calls (CRI's ADXF API) over the imported archive.
 *
 * The game reads SF33RDXB.AFS by member number, in 2048-byte sectors from
 * a running position (GD3rd.c's fsOpen and fsRequestFileRead). Retail's
 * CRI layer reads the disc asynchronously, and so does this: a read runs
 * on a worker thread (src/port/jobs.c) from when it is asked for, and the
 * next status poll waits for it, so that poll always sees it finished, on
 * the frame it would if the read were done at once. The game asks on one
 * frame and polls on the next, so the read has the rest of the frame. A
 * read runs on through the sectors after the member, as a sector read
 * from the disc does. */

#include "host/compat.h"
#include "host/paths.h"
#include "port/jobs.h"
#include "port/port.h"

#include "types.h"

#include <SDL3/SDL.h>

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
    /* the read on the worker (its job is s_jobs[] at the same place) */
    void *buf;
    size_t want;
    off_t at;
} AdxfFile;

static int s_fd = -1;
static u32 s_count;
static u32 *s_toc; /* offset, size pairs */
static off_t s_afs_size;
static s32 s_pt_stat = ADXF_STAT_STOP;
/* The handles are game state: the game keeps them, and a rollback (or a
 * netplay session's start) puts back which are open and where they are.
 * A save or a rollback waits for every read first (sf_files_sync), so a
 * handle put back is never mid-read. The worker jobs are the port's. */
static AdxfFile s_files[16] SF_GAME;
static SfJob s_jobs[16];

static SfJob *job_of(const AdxfFile *f)
{
    return &s_jobs[f - s_files];
}

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

/* The archive as a number: two machines with the same disc imported
 * have the same (a netplay session checks it). 0 before it is open. */
uint64_t sf_afs_identity(void)
{
    uint64_t h = 0xCBF29CE484222325ull;
    u32 i;
    if (!s_toc) {
        return 0;
    }
    for (i = 0; i < s_count * 2; i++) {
        h = (h ^ s_toc[i]) * 0x100000001B3ull;
    }
    return (h ^ (uint64_t)s_afs_size) * 0x100000001B3ull;
}

/* Retail's disc-error screen, which waits for the disc to come back. The
 * archive does not come back: say so and stop. */
void xb_ShowDiskReadError(void)
{
    static const char msg[] = "The game data cannot be read (data/afs/SF33RDXB.AFS next to the program).\n"
                              "Delete the data folder and start the game again to import the Xbox disc.";
    sf_fatal("files: %s", msg);
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

/* The worker's half of a read. */
static void read_run(void *arg)
{
    AdxfFile *f = arg;
    ssize_t got;
    sf_hist_touch(f->buf, f->want); /* a read-only page fails a read */
    got = pread(s_fd, f->buf, f->want, f->at);
    if (got < 0) {
        fprintf(stderr, "files: reading %zu bytes at %lld into %p: %s\n", f->want, (long long)f->at, f->buf,
                strerror(errno));
        f->stat = ADXF_STAT_ERROR;
    }
}

/* Every read finished: before the game's memory is copied or put back
 * (src/port/state.c). */
void sf_files_sync(void)
{
    size_t i;
    for (i = 0; i < sizeof s_files / sizeof s_files[0]; i++) {
        sf_job_wait(&s_jobs[i]);
    }
}

void ADXF_Close(void *adxf)
{
    if (adxf) {
        sf_job_wait(job_of((AdxfFile *)adxf));
        ((AdxfFile *)adxf)->used = 0;
    }
}

s32 ADXF_GetStat(void *adxf)
{
    if (!adxf) {
        return ADXF_STAT_ERROR;
    }
    sf_job_wait(job_of((AdxfFile *)adxf));
    return ((AdxfFile *)adxf)->stat;
}

s32 ADXF_StopNw(void *adxf)
{
    if (adxf) {
        sf_job_wait(job_of((AdxfFile *)adxf));
        ((AdxfFile *)adxf)->stat = ADXF_STAT_STOP;
    }
    return 0;
}

s32 ADXF_ReadNw(void *adxf, s32 nsct, void *buf)
{
    AdxfFile *f = adxf;
    off_t at;
    size_t want;
    if (!f || nsct <= 0) {
        return 0;
    }
    sf_job_wait(job_of(f));
    at = (off_t)f->offset + (off_t)f->pos * SECTOR;
    want = (size_t)nsct * SECTOR;
    if (at >= s_afs_size) {
        f->stat = ADXF_STAT_ERROR;
        return 0;
    }
    if (at + (off_t)want > s_afs_size) {
        want = (size_t)(s_afs_size - at);
    }
    f->pos += (u32)nsct;
    f->stat = ADXF_STAT_READEND;
    f->buf = buf;
    f->want = want;
    f->at = at;
    sf_job_start(job_of(f), read_run, f);
    return nsct;
}

/* An archive member's place, and a read from the archive: for the music
 * decoder's worker, which reads a member in parts (pread leaves the
 * descriptor's position alone, so threads can share it). */
int sf_afs_member(s32 fid, u32 *offset, u32 *size)
{
    if (s_fd < 0 || fid < 0 || (u32)fid >= s_count) {
        return -1;
    }
    *offset = s_toc[fid * 2];
    *size = s_toc[fid * 2 + 1];
    return 0;
}

long sf_afs_pread(void *buf, u32 n, u32 offset)
{
    return s_fd < 0 ? -1 : (long)pread(s_fd, buf, n, (off_t)offset);
}

/* the CRI server tick: nothing to run, reads finish when asked for */
void ADXM_ExecMain(void)
{
}
