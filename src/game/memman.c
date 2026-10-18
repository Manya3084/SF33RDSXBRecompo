#include "game/memman.h"

#include "mem/heap.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>

typedef struct MmCell {
    uint32_t prev;
    uint32_t next;
    int32_t size;
} MmCell;

_Static_assert(sizeof(MmCell) == 12, "MmCell");

static uint32_t mm_initial_number;

static uintptr_t mm_round_up(uint32_t unit, uintptr_t num)
{
    return ~(uintptr_t)(unit - 1) & (num + unit - 1);
}

static uintptr_t mm_round_off(uint32_t unit, uintptr_t num)
{
    return num & ~(uintptr_t)(unit - 1);
}

static int mm_unit_ok(int32_t unit)
{
    return unit >= (int32_t)sizeof(MmCell) && (unit & (unit - 1)) == 0;
}

static MmCell *mm_cell(const MemMan *mmobj, uint32_t off)
{
    return (MmCell *)(mmobj->memHead + off);
}

static uint32_t mm_off(const MemMan *mmobj, const MmCell *cell)
{
    return (uint32_t)((const uint8_t *)cell - mmobj->memHead);
}

void mmSystemInitialize(void)
{
    mm_initial_number = 0;
}

static MmCell *mm_alloc_sub(MemMan *mmobj, int32_t size, int32_t flag)
{
    int32_t size_true;
    int32_t gap_min;
    MmCell *cell;
    size_true = mmobj->ownUnit + (int32_t)mm_round_up((uint32_t)mmobj->ownUnit, (uint32_t)size);
    gap_min = 0x7FFFFFFF;
    cell = NULL;
    if (!mmobj->memHead || size_true < mmobj->ownUnit) {
        return NULL;
    }
    if (flag != 1) {
        MmCell *myself = mm_cell(mmobj, mmobj->cell_1st);
        do {
            MmCell *next = mm_cell(mmobj, myself->next);
            int32_t gap = (int32_t)((uint8_t *)next - (uint8_t *)myself) - myself->size;
            if (gap >= size_true) {
                if (gap == size_true) {
                    cell = myself;
                    break;
                }
                if (gap - size_true < gap_min) {
                    gap_min = gap - size_true;
                    cell = myself;
                }
            }
            myself = next;
        } while (myself->next != MM_END);
        if (!cell) {
            return NULL;
        }
        {
            MmCell *born = (MmCell *)((uint8_t *)cell + cell->size);
            born->prev = mm_off(mmobj, cell);
            born->next = cell->next;
            born->size = size_true;
            mm_cell(mmobj, cell->next)->prev = mm_off(mmobj, born);
            cell->next = mm_off(mmobj, born);
            return born;
        }
    }
    {
        MmCell *myself = mm_cell(mmobj, mmobj->cell_fin);
        do {
            MmCell *prev = mm_cell(mmobj, myself->prev);
            int32_t gap = (int32_t)((uint8_t *)myself - (uint8_t *)prev) - prev->size;
            if (gap >= size_true) {
                if (gap == size_true) {
                    cell = myself;
                    break;
                }
                if (gap - size_true < gap_min) {
                    gap_min = gap - size_true;
                    cell = myself;
                }
            }
            myself = prev;
        } while (myself->prev != MM_END);
        if (!cell) {
            return NULL;
        }
        {
            MmCell *born = (MmCell *)((uint8_t *)cell - size_true);
            born->prev = cell->prev;
            born->next = mm_off(mmobj, cell);
            born->size = size_true;
            mm_cell(mmobj, cell->prev)->next = mm_off(mmobj, born);
            cell->prev = mm_off(mmobj, born);
            return born;
        }
    }
}

void *mmAlloc(MemMan *mmobj, int32_t size, int32_t flag)
{
    MmCell *cell = mm_alloc_sub(mmobj, size, flag);
    if (!cell) {
        return NULL;
    }
    mmobj->remainder -= cell->size;
    if (mmobj->remainderMin > mmobj->remainder) {
        mmobj->remainderMin = mmobj->remainder;
    }
    return (uint8_t *)cell + mmobj->ownUnit;
}

void mmFree(MemMan *mmobj, void *adrs)
{
    MmCell *cell;
    if (!adrs || !mmobj || !mmobj->memHead) {
        return;
    }
    cell = (MmCell *)((uint8_t *)adrs - mmobj->ownUnit);
    mmobj->remainder += cell->size;
    mm_cell(mmobj, cell->prev)->next = cell->next;
    mm_cell(mmobj, cell->next)->prev = cell->prev;
}

int mmHeapInitialize(MemMan *mmobj, void *adrs, int32_t size, int32_t unit, const char *format)
{
    uintptr_t addr;
    uintptr_t head;
    uintptr_t tail;
    MmCell *first;
    MmCell *fin;
    (void)format;
    if (!mmobj || !adrs || size <= 0 || !mm_unit_ok(unit)) {
        return -1;
    }
    memset(mmobj, 0, sizeof *mmobj);
    addr = (uintptr_t)adrs;
    head = mm_round_up((uint32_t)unit, addr);
    tail = mm_round_off((uint32_t)unit, addr + (uintptr_t)size);
    if (tail < head || tail - head > 0x7FFFFFFF || (int32_t)(tail - head) < unit * 2) {
        return -1;
    }
    mmobj->oriHead = adrs;
    mmobj->oriSize = size;
    mmobj->ownUnit = unit;
    mmobj->ownNumber = mm_initial_number++;
    mmobj->memHead = (uint8_t *)head;
    mmobj->memSize = (int32_t)(tail - head);
    mmobj->cell_1st = 0;
    mmobj->cell_fin = (uint32_t)(mmobj->memSize - unit);
    mmobj->remainder = mmobj->memSize - unit * 2;
    mmobj->remainderMin = mmobj->remainder;
    first = mm_cell(mmobj, mmobj->cell_1st);
    fin = mm_cell(mmobj, mmobj->cell_fin);
    first->prev = MM_END;
    first->next = mmobj->cell_fin;
    first->size = unit;
    fin->prev = 0;
    fin->next = MM_END;
    fin->size = unit;
    return 0;
}

int mmHeapCreate(MemMan *mmobj, int32_t size, int32_t unit)
{
    void *block;
    if (!mmobj || size <= 0) {
        return -1;
    }
    block = sf_alloc((size_t)size);
    if (!block) {
        return -1;
    }
    if (mmHeapInitialize(mmobj, block, size, unit, NULL) != 0) {
        sf_free(block);
        return -1;
    }
    mmobj->owned = 1;
    return 0;
}

void mmHeapDestroy(MemMan *mmobj)
{
    if (!mmobj) {
        return;
    }
    if (mmobj->owned) {
        sf_free(mmobj->oriHead);
    }
    memset(mmobj, 0, sizeof *mmobj);
}
