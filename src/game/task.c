#include "game/task.h"

#include <string.h>

SfTask task[SF_TASK_COUNT];
uint8_t Turbo;
uint8_t Turbo_Timer;
uint8_t Game_pause;
uint8_t Process_Counter;
int32_t current_task_num;

static int slot_ok(uint16_t num)
{
    return num < SF_TASK_COUNT;
}

void xb_ClearTasks(void)
{
    memset(task, 0, sizeof task);
}

void cpReadyTask(uint16_t num, void (*func_adrs)(SfTask *task))
{
    SfTask *task_ptr;
    if (!slot_ok(num)) {
        return;
    }
    task_ptr = &task[num];
    memset(task_ptr, 0, sizeof *task_ptr);
    task_ptr->func_adrs = func_adrs;
    task_ptr->condition = 2;
}

void cpExitTask(uint16_t num)
{
    SfTask *task_ptr;
    void (*callback)(void);
    if (!slot_ok(num)) {
        return;
    }
    task_ptr = &task[num];
    task_ptr->condition = 0;
    callback = task_ptr->callback_adrs;
    if (callback) {
        callback();
    }
}

void xb_TurboFrameStep(void)
{
    int i;
    Process_Counter = 1;
    if (Turbo != 0 && Game_pause != 0x81) {
        Turbo_Timer--;
        if (Turbo_Timer == 0) {
            Turbo_Timer = Turbo;
            Process_Counter = 2;
        }
    }
    current_task_num = 0;
    for (i = 0; i < SF_TASK_COUNT; i++) {
        SfTask *task_ptr = &task[i];
        uint16_t condition = task_ptr->condition;
        if (condition == 1) {
            if (task_ptr->func_adrs) {
                task_ptr->func_adrs(task_ptr);
            }
        } else if (condition == 2) {
            task_ptr->condition = 1;
        }
        current_task_num++;
    }
}

void xb_ReleaseTasks(void)
{
    int i;
    for (i = 0; i < SF_TASK_COUNT; i++) {
        SfTask *task_ptr = &task[i];
        if ((task_ptr->condition == 1 || task_ptr->condition == 4) && task_ptr->callback_adrs) {
            task_ptr->callback_adrs();
        }
    }
    xb_ClearTasks();
}
