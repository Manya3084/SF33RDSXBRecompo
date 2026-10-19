#include "port/port.h"

#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>

/* One arena in the low 2 GB (MAP_32BIT), carved front to back. The Xbox
 * game sizes its own heaps at start-up and never returns them, so a bump
 * allocator is enough; sf_low_free only checks the pointer. */

#define SF_LOW_ARENA (512u << 20)

static uint8_t *s_base;
static size_t s_used;

void *sf_low_alloc(size_t size, size_t align)
{
    size_t at;
    if (!s_base) {
        void *p = mmap(NULL, SF_LOW_ARENA, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_32BIT | MAP_NORESERVE, -1, 0);
        if (p == MAP_FAILED) {
            perror("port: low arena");
            abort();
        }
        s_base = p;
    }
    if (align < 16) {
        align = 16;
    }
    at = (s_used + align - 1) & ~(align - 1);
    if (at + size > SF_LOW_ARENA) {
        fprintf(stderr, "port: low arena full (%zu bytes asked)\n", size);
        abort();
    }
    s_used = at + size;
    return s_base + at;
}

/* The arena and how much of it is in use, for the game-state snapshot. */
void sf_low_extent(uint8_t **base, size_t *used)
{
    *base = s_base;
    *used = s_used;
}

void sf_low_free(void *p)
{
    if (p && ((uint8_t *)p < s_base || (uint8_t *)p >= s_base + SF_LOW_ARENA)) {
        fprintf(stderr, "port: freeing %p, not in the low arena\n", p);
        abort();
    }
}
