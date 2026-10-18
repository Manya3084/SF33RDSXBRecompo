#ifndef SF_TASK_H
#define SF_TASK_H

#include <stdint.h>

/* Retail task table is 12 slots of 0x14 bytes at 0x7BE020.
   Pointers are native width here. The condition values match the Xbox walk. */

#define SF_TASK_COUNT 12

typedef struct SfTask SfTask;

struct SfTask {
    void (*func_adrs)(SfTask *task);
    void (*callback_adrs)(void);
    uint8_t r_no[4];
    uint16_t condition;
};

extern SfTask task[SF_TASK_COUNT];
extern uint8_t Turbo;
extern uint8_t Turbo_Timer;
extern uint8_t Game_pause;
extern uint8_t Process_Counter;
extern int32_t current_task_num;

/* 0xC82BC. Zeros the slot, stores the function, and sets condition 2. */
void cpReadyTask(uint16_t num, void (*func_adrs)(SfTask *task));

/* 0xC82E3. Clears condition, then calls callback_adrs when it is set. */
void cpExitTask(uint16_t num);

/* 0xC82AD. Zeros all 12 slots. */
void xb_ClearTasks(void);

/* 0xC8249. Turbo accounting, then one pass of the 12 slots.
   Condition 1 runs the function. Condition 2 becomes 1 and waits a frame. */
void xb_TurboFrameStep(void);

/* 0xC8301. Unnamed in the retail map. Calls callbacks whose condition is 1 or 4,
   then clears the table. */
void xb_ReleaseTasks(void);

#endif
