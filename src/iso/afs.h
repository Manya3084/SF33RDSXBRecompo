#ifndef SF_AFS_H
#define SF_AFS_H

#include <stddef.h>

/* 0 when the members were written. 1 when the file is not an AFS.
   -1 on a broken archive or a write error. */
int sf_afs_unpack(const char *afs_path, const char *dest_dir, const char *index_path,
                  void (*progress)(const char *name, void *user), void *user,
                  int *written, char *err, size_t err_n);

#endif
