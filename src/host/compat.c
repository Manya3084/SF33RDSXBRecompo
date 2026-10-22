/* The halves of src/host/compat.h that are code. */

#include "host/compat.h"

#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define FD_PATH "/proc/self/fd/"

/* "/proc/self/fd/N" is a descriptor the program holds (Android's file
 * picker gives one, not a path, and the file may not be openable by its
 * path): a copy of it, rewound. Anything else is opened. */
int sf_open_source(const char *path)
{
    if (strncmp(path, FD_PATH, sizeof FD_PATH - 1) == 0) {
        int fd = dup(atoi(path + sizeof FD_PATH - 1));
        if (fd >= 0 && lseek(fd, 0, SEEK_SET) < 0) {
            close(fd);
            return -1;
        }
        return fd;
    }
    return open(path, O_RDONLY | O_BINARY);
}

int sf_stat_source(const char *path, struct stat *st)
{
    if (strncmp(path, FD_PATH, sizeof FD_PATH - 1) == 0) {
        return fstat(atoi(path + sizeof FD_PATH - 1), st);
    }
    return stat(path, st);
}

#ifdef _WIN32
#include <windows.h>

long sf_pread(int fd, void *buf, size_t n, long long off)
{
    HANDLE h = (HANDLE)_get_osfhandle(fd);
    OVERLAPPED at;
    DWORD got = 0;
    if (h == INVALID_HANDLE_VALUE) {
        return -1;
    }
    memset(&at, 0, sizeof at);
    at.Offset = (DWORD)(unsigned long long)off;
    at.OffsetHigh = (DWORD)((unsigned long long)off >> 32);
    if (!ReadFile(h, buf, (DWORD)n, &got, &at)) {
        return GetLastError() == ERROR_HANDLE_EOF ? 0 : -1;
    }
    return (long)got;
}
#endif
