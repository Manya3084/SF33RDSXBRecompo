/* The game's start-up and its frame of game logic, on the native platform.
 *
 * Retail starts the game in its platform layer: a work pool it carves the
 * game's buffers from, the subsystems' initialisation in a fixed order,
 * the region, the first task. This file keeps that order and those sizes,
 * so the game's heaps are laid out as retail lays them out. What was the
 * Xbox's own (its print layer's font, the Xbox Live work, the pad-removed
 * screen, the region from the console's settings) is left out or comes
 * from the port. The reference is 3s-decomp-ogxb, Source/XBOX/xb_0C8301.c
 * (njUserInit, njUserMain), xb_0C7E80.c (cpLoopTask, the region) and
 * xb_011000.c (the pool). */

#include "game/memman.h"
#include "port/gfx.h"
#include "port/port.h"

#include "types.h"
#include "structs.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Globals the game logic uses whose storage was in retail's platform
 * files, with the types the game declares them with. */
MPP mpp_w SF_GAME;
PLW plw[2] SF_GAME;
s32 system_init_level SF_GAME;
MemMan rckey_mmobj SF_GAME; /* RAMCNT's heap, on the native memory manager */

/* Types as the defining files have them. */
extern struct _TASK task[11];
extern struct _SYSTEM_W sys_w;
extern u8 CPU_Time_Lag[2];
extern u8 CPU_Rec[2];
extern u8 Replay_Status[2];
extern f32 Frame_Zoom_X;
extern f32 Frame_Zoom_Y;
extern f32 Screen_Zoom_X;
extern f32 Screen_Zoom_Y;
extern f32 SA_Zoom_X;
extern f32 SA_Zoom_Y;
extern u8 Disp_Size_H;
extern u8 Disp_Size_V;
extern u8 Country;
extern u8 Turbo;
extern u8 Turbo_Timer;
extern u8 Screen_PAL;
extern u8 Interface_Type[2];
extern u8 Process_Counter;
extern u8 Game_pause;
extern u8 Mode_Type;
extern u8 Play_Mode;
extern u32 Interrupt_Timer;
extern s16 Correct_X[4];
extern s16 Correct_Y[4];
extern u16 p1sw_0, p1sw_1, p2sw_0, p2sw_1;

void mmSystemInitialize(void);
u32 seqsGetUseMemorySize(void);
void seqsInitialize(void *adrs);
void ppg_Initialize(void *lcmAdrs, s32 lcmSize);
void zlib_Initialize(void *tempAdrs, s32 tempSize);
void Init_ram_control_work(u8 *adrs, s32 size);
void appSetupBasePriority(void);
void appSetupTempPriority(void);
void initRenderState(s32 flag);
void appViewSetItems(VPRM *prm);
void appViewMatrix(void);
void setBackGroundColor(u32 color);
void xb_SetViewportRect(s32 x, s32 y, s32 w, s32 h);
void Setup_Disp_Size(s16 flag);
void Init_sound_system(void);
void Init_bgm_work(void);
s32 Setup_Directory_Record_Data(void);
void sndInitialLoad(void);
void cpReadyTask(u16 num, void (*func_adrs)(void));
void Init_Task(struct _TASK *task_ptr);
void Check_Replay_Status(s16 PL_id, u8 Status);

/* ---- the work pool (retail's xb_PoolInit and xb_PoolAllocAligned) ----
 *
 * 14 MB, aligned to 0x40, taken from the front (or the back for "top"). */

#define SF_POOL_SIZE 0xE00000u
#define SF_POOL_ALIGN 0x40u

/* the work pool's ends move as the game takes and gives back work: game state */
static uint8_t *s_pool_base SF_GAME;
static uintptr_t s_pool_front SF_GAME;
static uintptr_t s_pool_back SF_GAME;

static void pool_init(void)
{
    s_pool_base = sf_arena_alloc(SF_POOL_SIZE, SF_POOL_ALIGN);
    memset(s_pool_base, 0, SF_POOL_SIZE);
    s_pool_front = (uintptr_t)s_pool_base;
    s_pool_back = (uintptr_t)s_pool_base + SF_POOL_SIZE;
}

static void *pool_alloc(u32 size, int top)
{
    uintptr_t at;
    size = (size + SF_POOL_ALIGN - 1) & ~(SF_POOL_ALIGN - 1);
    if (s_pool_front + size > s_pool_back) {
        fprintf(stderr, "port: work pool full (0x%X bytes asked)\n", size);
        return NULL;
    }
    if (top) {
        s_pool_back -= size;
        return (void *)s_pool_back;
    }
    at = s_pool_front;
    s_pool_front += size;
    return (void *)at;
}

/* what is left between the two ends (retail calls it the used size) */
static u32 pool_left(void)
{
    return (u32)(s_pool_back - s_pool_front);
}

/* ---- start-up ---- */

/* The region: retail reads the console's region and language. 1 and 2
 * are the Japanese and Asian builds, 3 Europe, 4 North America (the
 * default); SF_COUNTRY picks another. */
static void setup_region(void)
{
    const char *env = getenv("SF_COUNTRY");
    Turbo = 0;
    Screen_PAL = 0;
    Country = 4;
    if (env && env[0] >= '1' && env[0] <= '4' && env[1] == 0) {
        Country = (u8)(env[0] - '0');
    }
}

/* retail's xb_ResetScreenState, without the three black frames it presents
 * (the window starts black) */
static void reset_screen_state(void)
{
    appSetupBasePriority();
    appSetupTempPriority();
    initRenderState(0);
    mpp_w.vprm.x0 = 0.0f;
    mpp_w.vprm.y0 = 0.0f;
    mpp_w.vprm.x1 = 640.0f;
    mpp_w.vprm.y1 = 448.0f;
    mpp_w.vprm.ne = -1.0f;
    mpp_w.vprm.fa = 1.0f;
    appViewSetItems(&mpp_w.vprm);
    appViewMatrix();
    sf_video_set_offset(0, 0);
    setBackGroundColor(0xFF000000);
    xb_SetViewportRect(0, 0, 640, 448);
}

void sf_input_reset(void);
extern IO io_w;
extern u16 p3sw_0, p3sw_1, p4sw_0, p4sw_1;

/* retail's xb_ResetInputState (xb_0611EA.c): no port has joined and no
 * switch is held. Start-up runs it, and the Capcom logo at the start of
 * each attract loop. */
void xb_ResetInputState(void)
{
    sf_input_reset();
    memset(&io_w, 0, sizeof io_w);
    memset(Interface_Type, 0, sizeof Interface_Type);
    p1sw_0 = 0;
    p1sw_1 = 0;
    p2sw_0 = 0;
    p2sw_1 = 0;
    p3sw_0 = 0;
    p3sw_1 = 0;
    p4sw_0 = 0;
    p4sw_1 = 0;
}

void njUserInit(void)
{
    u32 size;
    int i;

    mpp_w.vprm.x0 = 0.0f;
    mpp_w.vprm.y0 = 0.0f;
    mpp_w.vprm.x1 = 384.0f;
    mpp_w.vprm.y1 = 224.0f;
    mpp_w.vprm.ne = -1.0f;
    mpp_w.sysStop = 0;
    mpp_w.inGame = 0;
    mpp_w.ctrDemo = 0;
    mpp_w.language = 0;
    mpp_w.langload = -1;
    mpp_w.xb_unk35 = 0;
    mpp_w.vprm.fa = 1.0f;
    appViewSetItems(&mpp_w.vprm);
    appViewMatrix();
    mmSystemInitialize();

    pool_init();
    mpp_w.fmsFrame.pFrame = (u8 *)s_pool_front;
    mpp_w.fmsFrame.heapnum = 0;
    seqsInitialize(pool_alloc(seqsGetUseMemorySize(), 0));
    ppg_Initialize(pool_alloc(0x60000, 0), 0x60000);
    zlib_Initialize(pool_alloc(0x10000, 0), 0x10000);
    /* retail's print layer buffer and its Live text buffer, each 0xD400;
     * taken here too so the game's heap is the size it is on retail */
    pool_alloc(0xD400, 0);
    pool_alloc(0xD400, 0);
    size = pool_left();
    mpp_w.ramcntBuff = pool_alloc(size, 0);
    Init_ram_control_work(mpp_w.ramcntBuff, (s32)size);
    reset_screen_state();

    for (i = 0; i < 20; i++) {
        mpp_w.useChar[i] = 0;
    }

    Interrupt_Timer = 0;
    SA_Zoom_X = 0.0f;
    SA_Zoom_Y = 0.0f;
    Disp_Size_H = 100;
    Disp_Size_V = 100;
    mpp_w.ds_h[1] = 100;
    mpp_w.ds_h[0] = 100;
    mpp_w.ds_v[1] = 100;
    mpp_w.ds_v[0] = 100;
    Country = 0;
    setup_region();
    sf_language_init(); /* GAME OPTION's LANGUAGE: the region and the text */
    Turbo_Timer = 1;
    Screen_Zoom_X = 1.0f;
    Screen_Zoom_Y = 1.0f;
    Setup_Disp_Size(0);
    Frame_Zoom_X = Screen_Zoom_X + SA_Zoom_X;
    Correct_X[0] = 0;
    Correct_Y[0] = 0;
    Frame_Zoom_Y = Screen_Zoom_Y + SA_Zoom_Y;
    sys_w.disp.new = 1;
    sys_w.disp.now = 1;
    sys_w.pause = 0;
    sys_w.reset = 0;
    xb_ResetInputState();
    Init_sound_system();
    Init_bgm_work();
    Setup_Directory_Record_Data();
    sndInitialLoad();
    memset(task, 0, sizeof task);
    cpReadyTask(0, (void (*)(void))Init_Task);
}

/* ---- a frame of game logic ---- */

/* The task loop. Retail walks 12 slots of 0x14 bytes, one past the 11 its
 * table holds (the slot after it is never armed); this walks the 11. */
void cpLoopTask(void)
{
    u32 i;

    Process_Counter = 1;
    if (Turbo && Game_pause != 0x81 && --Turbo_Timer == 0) {
        Turbo_Timer = Turbo;
        Process_Counter = 2;
    }
    for (i = 0; i < sizeof task / sizeof task[0]; i++) {
        switch (task[i].condition) {
        case 1:
            ((void (*)(struct _TASK *))task[i].func_adrs)(&task[i]);
            break;
        case 2:
            task[i].condition = 1;
            break;
        }
    }
}

s32 njUserMain(void)
{
    CPU_Time_Lag[0] = 0;
    CPU_Time_Lag[1] = 0;
    CPU_Rec[0] = 0;
    CPU_Rec[1] = 0;

    Check_Replay_Status(0, Replay_Status[0]);
    Check_Replay_Status(1, Replay_Status[1]);

    Frame_Zoom_X = Screen_Zoom_X + SA_Zoom_X;
    Frame_Zoom_Y = Screen_Zoom_Y + SA_Zoom_Y;

    if (sys_w.disp.now == sys_w.disp.new) {
        cpLoopTask();
        if (Game_pause != 0x81 && Mode_Type == 1 && Play_Mode == 1) {
            if (plw[0].wu.operator == 0 && CPU_Rec[0] == 0 && Replay_Status[0] == 1) {
                p1sw_0 = 0;
                Check_Replay_Status(0, 1);
            }
            if (plw[1].wu.operator == 0 && CPU_Rec[1] == 0 && Replay_Status[1] == 1) {
                p2sw_0 = 0;
                Check_Replay_Status(1, 1);
            }
        }
    } else {
        sys_w.disp.now = sys_w.disp.new;
    }
    return sys_w.gd_error;
}

/* ---- the PS2-sized picture (retail's CP3toPS2Draw, xb_0F4537.c) ---- */

static int s_cp3_on SF_GAME;

extern struct _SAVE_W save_w[6];
extern u8 Disp_Size_H;
extern u8 Disp_Size_V;
void dspwhUnpack(u8 src, u8 *xdsp, u8 *ydsp);

void CP3toPS2DrawOn(void)
{
    s_cp3_on = 1;
}

void CP3toPS2DrawOff(void)
{
    s_cp3_on = 0;
}

s32 getCP3toFullScreenDrawFlag(void)
{
    return s_cp3_on;
}

/* With the copy on and the game not stopped: the screen's corner the
 * screen-size option leaves (383x223 at 100%) goes onto the output, and
 * what is drawn after it is blended over from the 638x447 area. */
void CP3toPS2Draw(void)
{
    if (mpp_w.sysStop == 2 || !s_cp3_on) {
        return;
    }
    dspwhUnpack(save_w[1].Screen_Size, &Disp_Size_H, &Disp_Size_V);
    sf_gfx_copy_screen(0, 0, 0x247 - Disp_Size_H * 2, 0x1A7 - Disp_Size_V * 2);
    xb_SetViewportRect(0, 0, 0x27E - (s32)((100.0f - Disp_Size_H) * -3.3333333f), 0x34F - Disp_Size_V * 4);
}


