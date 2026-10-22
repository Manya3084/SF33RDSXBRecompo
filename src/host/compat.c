/* The Windows halves of src/host/compat.h. */

#ifdef _WIN32
#include "host/compat.h"

#include <string.h>
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
#else
typedef int sf_compat_unused; /* ISO C wants something in a file */
#endif
