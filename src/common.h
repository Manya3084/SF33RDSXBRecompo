#ifndef SF_COMMON_H
#define SF_COMMON_H

#include "host/compat.h"

#include <stdarg.h>
#include <stddef.h>
#include <stdio.h>

static inline void sf_set_err(char *err, size_t n, const char *fmt, ...)
{
    va_list ap;
    if (!err || n == 0) {
        return;
    }
    va_start(ap, fmt);
    vsnprintf(err, n, fmt, ap);
    va_end(ap);
}

static inline int sf_join(char *out, size_t n, const char *a, const char *b)
{
    size_t alen;
    int wrote;
    int slash;
    if (!out || n == 0 || !a || !b) {
        return -1;
    }
    alen = 0;
    while (a[alen] != 0) {
        alen++;
    }
    slash = alen > 0 && (a[alen - 1] == '/' || a[alen - 1] == '\\');
    wrote = slash ? snprintf(out, n, "%s%s", a, b) : snprintf(out, n, "%s/%s", a, b);
    if (wrote < 0 || (size_t)wrote >= n) {
        return -1;
    }
    return 0;
}

#endif
