#include "mem/heap.h"

#include <stdlib.h>

#if defined(SF_USE_MIMALLOC)
#include <mimalloc.h>
#endif

void *sf_alloc(size_t n)
{
    if (n == 0) {
        n = 1;
    }
#if defined(SF_USE_MIMALLOC)
    return mi_malloc(n);
#else
    return malloc(n);
#endif
}

void *sf_realloc(void *p, size_t n)
{
    if (n == 0) {
        n = 1;
    }
#if defined(SF_USE_MIMALLOC)
    return mi_realloc(p, n);
#else
    return realloc(p, n);
#endif
}

void sf_free(void *p)
{
#if defined(SF_USE_MIMALLOC)
    mi_free(p);
#else
    free(p);
#endif
}
