#ifndef SF_XDVDFS_H
#define SF_XDVDFS_H

#include <stddef.h>
#include <stdint.h>

typedef struct SfXdvdfs SfXdvdfs;

typedef struct SfXdvdfsEnt {
    uint32_t sector;
    uint32_t size;
    int is_dir;
    char name[256];
} SfXdvdfsEnt;

SfXdvdfs *sf_xdvdfs_open(const char *path, char *err, size_t err_n);
void sf_xdvdfs_close(SfXdvdfs *img);

/* 0 found, 1 missing, -1 error. Path uses '/' and is case-insensitive. */
int sf_xdvdfs_find(SfXdvdfs *img, const char *path, SfXdvdfsEnt *out, char *err, size_t err_n);

/* Stream file bytes to dest. */
int sf_xdvdfs_save(SfXdvdfs *img, const SfXdvdfsEnt *ent, const char *dest, char *err, size_t err_n);

typedef int (*SfXdvdfsVisit)(const char *path, const SfXdvdfsEnt *ent, void *user);

/* Walk a directory. dirpath "" is the volume root. 0 ok, 1 missing, -1 error. */
int sf_xdvdfs_walk(SfXdvdfs *img, const char *dirpath, SfXdvdfsVisit visit, void *user, char *err, size_t err_n);

#endif
