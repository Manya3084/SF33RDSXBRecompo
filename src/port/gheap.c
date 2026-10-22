#include "port/port.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Memory the game can see that the port allocates and frees as it goes:
 * the textures' pixels (src/port/gfx.c), which the game writes through a
 * locked texture's pointer. It is a block of the arena with its own
 * free list written in it, so the game-state snapshot and the rollback
 * history (src/port/state.c, history.c) take the pixels and the heap's
 * bookkeeping together: a rollback puts back which blocks are in use as
 * well as what they hold. First fit, and the same calls give the same
 * blocks on every machine. */

#define GHEAP_SIZE (32u << 20)
#define ALIGN 64u

typedef struct Block {
    uint32_t size; /* the whole block, this header with it */
    uint32_t used;
    uint32_t prev; /* the block before's size, 0 for the first */
    uint32_t pad[13];
} Block;

static uint8_t *s_heap SF_GAME;
static uint32_t s_high SF_GAME;

static void init(void)
{
    Block *b;
    s_heap = sf_arena_alloc(GHEAP_SIZE, ALIGN);
    b = (Block *)s_heap;
    memset(b, 0, sizeof *b);
    b->size = GHEAP_SIZE;
}

static Block *next_of(Block *b)
{
    uint8_t *n = (uint8_t *)b + b->size;
    return n < s_heap + GHEAP_SIZE ? (Block *)n : NULL;
}

void *sf_gheap_alloc(size_t n)
{
    uint32_t want;
    Block *b;
    if (!s_heap) {
        init();
    }
    if (n > GHEAP_SIZE - sizeof(Block)) {
        return NULL;
    }
    want = (uint32_t)((n + sizeof(Block) + ALIGN - 1) & ~(size_t)(ALIGN - 1));
    for (b = (Block *)s_heap; b; b = next_of(b)) {
        if (b->used || b->size < want) {
            continue;
        }
        if (b->size - want >= sizeof(Block) + ALIGN) {
            Block *rest = (Block *)((uint8_t *)b + want);
            Block *after;
            memset(rest, 0, sizeof *rest);
            rest->size = b->size - want;
            rest->prev = want;
            after = next_of(rest);
            if (after) {
                after->prev = rest->size;
            }
            b->size = want;
        }
        b->used = 1;
        if ((uint32_t)((uint8_t *)b + b->size - s_heap) > s_high) {
            s_high = (uint32_t)((uint8_t *)b + b->size - s_heap);
            if (getenv("SF_ARENA_DEBUG")) {
                fprintf(stderr, "gheap: %u KB high\n", s_high >> 10);
            }
        }
        memset(b + 1, 0, b->size - sizeof *b);
        return b + 1;
    }
    fprintf(stderr, "gheap: no %zu bytes free\n", n);
    return NULL;
}

void sf_gheap_free(void *p)
{
    Block *b;
    Block *n;
    if (!p) {
        return;
    }
    b = (Block *)p - 1;
    b->used = 0;
    n = next_of(b);
    if (n && !n->used) {
        Block *after;
        b->size += n->size;
        after = next_of(b);
        if (after) {
            after->prev = b->size;
        }
    }
    if (b->prev) {
        Block *before = (Block *)((uint8_t *)b - b->prev);
        if (!before->used) {
            Block *after;
            before->size += b->size;
            after = next_of(before);
            if (after) {
                after->prev = before->size;
            }
        }
    }
}
