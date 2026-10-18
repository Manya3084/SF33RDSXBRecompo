#ifndef SF_HEAP_H
#define SF_HEAP_H

#include <stddef.h>

void *sf_alloc(size_t n);
void *sf_realloc(void *p, size_t n);
void sf_free(void *p);

#endif
