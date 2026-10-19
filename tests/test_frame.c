#include "game/memman.h"
#include "game/opening.h"
#include "game/task.h"

#include <stdint.h>
#include <stdio.h>

static int g_fail;

static void expect(int cond, const char *msg)
{
    if (!cond) {
        fprintf(stderr, "FAIL %s\n", msg);
        g_fail++;
    } else {
        printf("ok %s\n", msg);
    }
}

static int g_calls;
static int g_callbacks;
static int g_seen_index;
static int g_seen_condition;

static void on_task(SfTask *task_ptr)
{
    g_calls++;
    g_seen_index = current_task_num;
    g_seen_condition = task_ptr->condition;
}

static void on_callback(void)
{
    g_callbacks++;
}

static void test_tasks(void)
{
    int i;
    xb_ClearTasks();
    Turbo = 0;
    Turbo_Timer = 0;
    Game_pause = 0;
    g_calls = 0;
    g_callbacks = 0;
    cpReadyTask(3, on_task);
    expect(task[3].condition == 2 && task[3].func_adrs == on_task, "ready arms condition 2");
    expect(task[3].r_no[0] == 0 && task[3].callback_adrs == NULL, "ready clears the slot");
    xb_TurboFrameStep();
    expect(g_calls == 0 && task[3].condition == 1, "first step waits");
    expect(current_task_num == SF_TASK_COUNT && Process_Counter == 1, "step visits 12 slots");
    xb_TurboFrameStep();
    expect(g_calls == 1 && g_seen_index == 3 && g_seen_condition == 1, "second step runs the slot");
    task[3].callback_adrs = on_callback;
    cpExitTask(3);
    expect(task[3].condition == 0 && g_callbacks == 1, "exit clears and calls back");
    cpReadyTask(99, on_task);
    expect(task[0].func_adrs == NULL, "a slot past the table is ignored");
    xb_ClearTasks();
    g_calls = 0;
    for (i = 0; i < SF_TASK_COUNT; i++) {
        cpReadyTask((uint16_t)i, on_task);
    }
    xb_TurboFrameStep();
    xb_TurboFrameStep();
    expect(g_calls == SF_TASK_COUNT, "all 12 slots run");
    Turbo = 3;
    Turbo_Timer = 1;
    Game_pause = 0;
    xb_TurboFrameStep();
    expect(Process_Counter == 2 && Turbo_Timer == 3, "turbo reloads the timer");
    Turbo_Timer = 2;
    xb_TurboFrameStep();
    expect(Process_Counter == 1 && Turbo_Timer == 1, "turbo counts down");
    Game_pause = 0x81;
    Turbo_Timer = 1;
    xb_TurboFrameStep();
    expect(Process_Counter == 1 && Turbo_Timer == 1, "pause 0x81 skips turbo");
    xb_ClearTasks();
    g_callbacks = 0;
    cpReadyTask(1, on_task);
    xb_TurboFrameStep();
    task[1].callback_adrs = on_callback;
    task[1].condition = 1;
    cpReadyTask(2, on_task);
    task[2].callback_adrs = on_callback;
    task[2].condition = 4;
    cpReadyTask(4, on_task);
    task[4].callback_adrs = on_callback;
    task[4].condition = 2;
    xb_ReleaseTasks();
    expect(g_callbacks == 2, "release calls condition 1 and 4");
    expect(task[1].func_adrs == NULL && task[2].condition == 0, "release clears the table");
}

static void test_heap(void)
{
    MemMan heap;
    MemMan other;
    void *a;
    void *b;
    void *c;
    int32_t after_one;
    mmSystemInitialize();
    expect(mmHeapCreate(&heap, 4096, 16) == 0, "heap create");
    expect(heap.owned == 1 && heap.ownUnit == 16 && heap.remainder == 4096 - 32, "heap remainder");
    expect(heap.ownNumber == 0, "first heap number");
    a = mmAlloc(&heap, 16, 0);
    after_one = heap.remainder;
    b = mmAlloc(&heap, 16, 0);
    expect(a && b && a != b, "two blocks");
    expect(heap.remainder == after_one - 32, "remainder tracks the cell");
    mmFree(&heap, a);
    expect(heap.remainder == after_one, "free restores the remainder");
    c = mmAlloc(&heap, 16, 0);
    expect(c == a, "exact hole is reused");
    after_one = heap.remainder;
    mmFree(&heap, NULL);
    expect(heap.remainder == after_one, "free of null is ignored");
    mmHeapDestroy(&heap);
    expect(mmHeapCreate(&heap, 4096, 16) == 0, "second heap");
    expect(heap.ownNumber == 1, "heap numbers increase");
    a = mmAlloc(&heap, 32, 0);
    b = mmAlloc(&heap, 32, 1);
    expect(a && b && (uint8_t *)b > (uint8_t *)a, "end allocation sits higher");
    {
        int n = 0;
        while (n < 200 && mmAlloc(&heap, 64, 0)) {
            n++;
        }
        expect(n < 200 && mmAlloc(&heap, 64, 0) == NULL, "a full heap returns null");
    }
    mmHeapDestroy(&heap);
    expect(mmHeapCreate(&other, 16, 16) != 0, "a block smaller than two units fails");
    expect(mmHeapInitialize(&heap, (void *)(uintptr_t)8, 16, 3, NULL) != 0, "unit must be a power of two");
}

static void test_opening(void)
{
    SfOpening op;
    int i;
    sf_opening_init(&op);
    expect(op.timer == 0 && op.status == 0 && op.r_no_0 == 0 && op.screen == 0, "the opening clock boots clear");
    for (i = 0; i < 11; i++) {
        sf_opening_step(&op);
    }
    expect(op.r_no_0 == 1 && op.r_no_1 == 0 && op.timer == 2 && op.status == 0 && op.pos_offset == 192, "the clock starts at 2");
    expect(op.layer_r[0] == 0 && op.screen == 0, "background 0 is not selected during the countdown");
    sf_opening_step(&op);
    expect(op.timer == 3 && op.status == 0 && op.held == 0 && op.index == 0, "time 3 has not posted 101");
    expect(op.layer_r[0] == 1 && op.layer_r[1] == 1 && op.layer_r[2] == 1 && op.layer_x[2] == 0x02000000, "the first background move arms layer 2");
    sf_opening_step(&op);
    expect(op.timer == 4 && op.status == 101 && op.scene == 1 && op.held == 1 && op.index == 0, "time 4 posts 101 and holds background 0");
    expect(op.screen == 0 && op.layer_x[2] == 0x01FF0000 && op.layer_y[2] == 0, "background 0 leaves the screens off and walks layer 2");
    expect(op.f6_y[0] == 15 && op.f6_y[1] == -224 && op.show_next == 0, "the first sketch reads 15 at time 4");
    for (i = 0; i < 7; i++) {
        sf_opening_step(&op);
    }
    expect(op.timer == 11 && op.status == 2 && op.scene == 2 && op.index == 0 && op.obj_disp == 0 && op.layer_x[2] == 0x01F80000 && op.f6_y[0] == 11, "time 11 posts status 2 and background 0 keeps walking");
    while (op.timer < 79) {
        sf_opening_step(&op);
    }
    expect(op.timer == 79 && op.status == 11 && op.scene == 11 && op.index == 1 && op.obj_disp == 1 && op.r_no_2 == 2 && op.show_next == 0 && op.screen == 0 && op.layer_x[2] == 0x01B50000 && op.layer_y[0] == 0 && op.f6_y[0] == -23, "time 79 posts status 11 and leaves the old picture up for this frame");
    sf_opening_step(&op);
    expect(op.timer == 80 && op.show_next == 1 && op.f6_y[1] == -222 && op.layer_x[2] == 0x02000000 && (op.screen & 1u) == 1u && op.layer_y[0] == -8 && op.status == 11 && op.r_no_1 == 1, "the next frame shows the second sketch and the ground quake starts at -8");
    while (op.timer < 123) {
        sf_opening_step(&op);
    }
    expect(op.timer == 123 && op.f6_y[1] == -136 && op.f6_stop[1] == 1 && op.status == 16 && op.r_no_1 == 1 && op.layer_x[2] == 0x02000000 && op.layer_y[0] == 3, "the second sketch stops at -136 and scene 102 stays unposted");
    sf_opening_step(&op);
    expect(op.timer == 124 && op.status == 102 && op.r_no_1 == 2 && op.r_no_2 == 1 && op.index == 2 && op.show_line == 1 && op.show_next == 0 && op.screen == 0 && op.line_x == 449 && op.layer_x[2] == 0x02000000 && op.layer_y[2] == 0 && op.layer_y[0] == 0, "time 124 posts 102, drops the ground, and steps the next sketch to 449");
    sf_opening_step(&op);
    expect(op.timer == 125 && op.line_x == 451 && op.layer_y[2] == 0x00010000 && op.show_line == 1 && op.index == 2, "the next frame steps the sketch to 451 and walks layer 2 down");
    while (op.timer < 183) {
        sf_opening_step(&op);
    }
    expect(op.timer == 183 && op.line_x == 538 && op.line_stop == 1 && op.status == 8 && op.index == 2 && op.r_no_1 == 2, "the sketch stops at 538 on timer 183");
    sf_opening_step(&op);
    expect(op.timer == 184 && op.status == 8 && op.index == 2 && op.line_x == 538 && op.r_no_1 == 2, "status 9 stays unposted");
}

int main(void)
{
    test_tasks();
    test_heap();
    test_opening();
    if (g_fail) {
        fprintf(stderr, "%d failed\n", g_fail);
        return 1;
    }
    return 0;
}
