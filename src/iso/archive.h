#ifndef SF_ARCHIVE_H
#define SF_ARCHIVE_H

#include <stddef.h>
#include <stdint.h>

int sf_mkdir_p(const char *path, char *err, size_t err_n);
int sf_rm_rf(const char *path);

typedef int (*SfArchiveVisitFn)(const char *name, int is_dir, uint64_t size, void *user);

int sf_archive_visit(const char *archive, SfArchiveVisitFn fn, void *user, char *err, size_t err_n);

/* map returns 1 and fills dest to write the member, 0 to skip, -1 to abort. */
typedef int (*SfArchiveMapFn)(const char *name, int is_dir, char *dest, size_t dest_n, void *user);

int sf_archive_export(const char *archive, SfArchiveMapFn map, void *user,
                      void (*progress)(const char *name, void *progress_user), void *progress_user,
                      int *written, char *err, size_t err_n);

#endif
