#ifndef SF_MEMMAN_H
#define SF_MEMMAN_H

#include <stdint.h>

/* Retail cell heap. A cell header is 12 bytes and the unit passed by the game is 16.
   Links are byte offsets from memHead so a 16-byte unit still holds the header.
   mmHeapCreate takes the block from sf_alloc. mmHeapInitialize uses a block the caller owns. */

#define MM_END 0xFFFFFFFFu

typedef struct MemMan {
    uint8_t *memHead;
    int32_t memSize;
    uint32_t ownNumber;
    int32_t ownUnit;
    int32_t remainder;
    int32_t remainderMin;
    uint32_t cell_1st;
    uint32_t cell_fin;
    uint8_t *oriHead;
    int32_t oriSize;
    int32_t debIndex;
    int owned;
} MemMan;

void mmSystemInitialize(void);

/* 0 when the heap is usable. The format string is ignored, as in the retail body. */
int mmHeapInitialize(MemMan *mmobj, void *adrs, int32_t size, int32_t unit, const char *format);

/* Allocates the block with sf_alloc, then initializes it. 0 on success. */
int mmHeapCreate(MemMan *mmobj, int32_t size, int32_t unit);

void mmHeapDestroy(MemMan *mmobj);

/* flag 1 searches from the end of the heap. Any other flag searches from the start. */
void *mmAlloc(MemMan *mmobj, int32_t size, int32_t flag);
void mmFree(MemMan *mmobj, void *adrs);

#endif
