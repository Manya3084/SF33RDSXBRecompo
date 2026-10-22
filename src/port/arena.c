#include "port/port.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <windows.h>
#else
#include <sys/mman.h>
#endif

/* The game's heaps: one arena, carved front to back. The Xbox game sizes
 * its own heaps at start-up and never returns them, so a bump allocator is
 * enough; sf_arena_free only checks the pointer. One block also makes the
 * heaps one region for the game-state snapshot (src/port/state.c). */

#define SF_ARENA (512u << 20)

static uint8_t *s_base;
/* how much is taken is game state: going back to an earlier frame (a
 * rollback, or a netplay session's start) gives the game the same
 * blocks again */
static size_t s_used SF_GAME;
static size_t s_high; /* as far as the arena was ever taken: zero beyond */

void *sf_arena_alloc(size_t size, size_t align)
{
    size_t at;
    if (!s_base) {
#ifdef _WIN32
        /* reserved and committed: pages are given on use */
        s_base = VirtualAlloc(NULL, SF_ARENA, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
        if (!s_base) {
            fprintf(stderr, "port: arena: no %u MB free\n", SF_ARENA >> 20);
            abort();
        }
#else
        void *p = mmap(NULL, SF_ARENA, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
        if (p == MAP_FAILED) {
            perror("port: arena");
            abort();
        }
        s_base = p;
#endif
    }
    if (align < 16) {
        align = 16;
    }
    at = (s_used + align - 1) & ~(align - 1);
    if (at + size > SF_ARENA) {
        fprintf(stderr, "port: arena full (%zu bytes asked)\n", size);
        abort();
    }
    s_used = at + size;
    if (at < s_high) {
        /* taken before, by a run since put back: as zero as fresh memory */
        memset(s_base + at, 0, (s_used < s_high ? s_used : s_high) - at);
    }
    if (s_used > s_high) {
        s_high = s_used;
    }
    if (getenv("SF_ARENA_DEBUG")) {
        fprintf(stderr, "arena: +0x%zx %zu bytes from %p\n", at, size, __builtin_return_address(0));
    }
    return s_base + at;
}

/* The arena and how much of it is in use, for the game-state snapshot. */
void sf_arena_extent(uint8_t **base, size_t *used)
{
    *base = s_base;
    *used = s_used;
}

void sf_arena_free(void *p)
{
    if (p && ((uint8_t *)p < s_base || (uint8_t *)p >= s_base + SF_ARENA)) {
        fprintf(stderr, "port: freeing %p, not in the arena\n", p);
        abort();
    }
}
