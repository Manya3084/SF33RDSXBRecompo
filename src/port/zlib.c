/* The game's zlib calls (its zlibApp.c API) over the system zlib.
 *
 * The game's own wrapper declares a 32-bit z_stream and asks for zlib
 * 1.1.4, so it cannot drive the system library on x86-64. This keeps its
 * three calls and their results: zlib_Initialize gives the inflater a heap
 * from the game's memory manager, zlib_Decompress inflates a whole zlib
 * stream and returns the bytes written, or 0 on any error. */

#include "types.h"

#include <zlib.h>

#include <string.h>

#include "game/memman.h"

static MemMan s_heap;
static int s_ready;

static voidpf heap_alloc(voidpf opaque, uInt items, uInt size)
{
    (void)opaque;
    return mmAlloc(&s_heap, (s32)(items * size), 0);
}

static void heap_free(voidpf opaque, voidpf adrs)
{
    (void)opaque;
    mmFree(&s_heap, adrs);
}

void zlib_Initialize(void *tempAdrs, s32 tempSize)
{
    if (tempAdrs == 0) {
        for (;;) {
        }
    }
    mmHeapInitialize(&s_heap, tempAdrs, tempSize, 16, "- for zlib -");
    s_ready = 1;
}

s32 zlib_Decompress(void *srcBuff, s32 srcSize, void *dstBuff, s32 dstSize)
{
    z_stream z;
    int r;
    memset(&z, 0, sizeof z);
    z.next_in = srcBuff;
    z.avail_in = (uInt)srcSize;
    z.next_out = dstBuff;
    z.avail_out = (uInt)dstSize;
    if (s_ready) {
        z.zalloc = heap_alloc;
        z.zfree = heap_free;
    }
    if (inflateInit(&z) != Z_OK) {
        return 0;
    }
    for (;;) {
        r = inflate(&z, Z_NO_FLUSH);
        if (r == Z_STREAM_END) {
            break;
        }
        if (r != Z_OK) {
            inflateEnd(&z);
            return 0;
        }
    }
    if (inflateEnd(&z) != Z_OK) {
        return 0;
    }
    return (s32)z.total_out;
}

void *zlib_Malloc(void *opaque, u32 items, u32 size)
{
    return heap_alloc(opaque, items, size);
}

void zlib_Free(void *opaque, void *adrs)
{
    heap_free(opaque, adrs);
}
