#ifndef SF_HOST_COMPAT_H
#define SF_HOST_COMPAT_H

/* The few POSIX calls the runtime uses that Windows (MinGW-w64) spells
 * differently. On Linux this is the fsync wrapper. */

#include <fcntl.h>
#include <stdio.h>
#include <sys/stat.h>
#include <sys/types.h>

#ifndef O_BINARY
#define O_BINARY 0 /* POSIX opens are binary already */
#endif

#ifdef _WIN32
#include <direct.h>
#include <io.h>
#include <stdlib.h>

#define mkdir(path, mode) _mkdir(path)
#define realpath(path, resolved) _fullpath((resolved), (path), 4096)
#define lstat stat
#define S_ISLNK(m) 0

/* pread: a read at an offset that leaves the file's position alone, so
 * threads can share one descriptor (src/host/compat.c) */
long sf_pread(int fd, void *buf, size_t n, long long off);
#define pread(fd, buf, n, off) sf_pread((fd), (buf), (n), (long long)(off))

#include <time.h>
/* the signal's number: Windows has no strsignal */
#define strsignal(sig) "signal"

static inline struct tm *sf_localtime_r(const time_t *t, struct tm *out)
{
    return localtime_s(out, t) == 0 ? out : NULL;
}
#define localtime_r(t, tm) sf_localtime_r((t), (tm))

/* Flush a file to disk. Windows names the call differently. */
static inline int sf_file_sync(int fd)
{
    return _commit(fd);
}
#else
#include <unistd.h>

static inline int sf_file_sync(int fd)
{
    return fsync(fd);
}
#endif

#endif
