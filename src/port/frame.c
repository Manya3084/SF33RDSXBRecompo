/* One game frame, in the order retail's frame function (0xC86E7) runs it.
 * The Xbox Live polling, the Live menu and the pad-removed screen are not
 * part of the port; everything else is the game's own call, or the native
 * platform function of the same name in src/port. */

#include "port/port.h"

#include "types.h"
#include "structs.h"

/* Types as the defining files have them. */
extern MPP mpp_w;
extern s32 system_init_level;
extern u16 p1sw_0, p1sw_1, p1sw_buff, p2sw_0, p2sw_1, p2sw_buff;
extern u16 p3sw_0, p3sw_1, p3sw_buff, p4sw_0, p4sw_1, p4sw_buff;
extern u8 Play_Mode, Game_pause, Mode_Type;
extern u8 Interrupt_Flag;
extern u32 Interrupt_Timer;
extern s32 X_Adjust, Y_Adjust;
extern s16 Correct_X[4], Correct_Y[4];
extern struct _TASK task[11];

void ppgWorkInitializeApprication(void);
void njUserInit(void);
void palCreateGhost(void);
void ppgMakeConvTableTexDC(void);
void njdp2d_init(void);
void initRenderState(s32 flag);
void appViewSetItems(void *prm);
void appViewMatrix(void);
void setBackGroundColor(u32 color);
void appSetupTempPriority(void);
void flPADGetALL(void);
void keyConvert(void);
void render_start(void);
void render_end(void);
void njUserMain(void);
void seqsBeforeProcess(void);
void seqsAfterProcess(void);
void njdp2d_draw(s32 pri);
void CP3toPS2Draw(void);
void Scrn_Renew(void);
void Irl_Family(void);
void Irl_Scrn(void);
void BGM_Server(void);
s32 flSetRenderState(s32 state, u32 value);

void sf_game_init(void)
{
    sf_video_init(640, 448);
    flSetRenderState(0x14, 0);
    system_init_level = 0;
    ppgWorkInitializeApprication();
    njUserInit();
    palCreateGhost();
    ppgMakeConvTableTexDC();
}

void sf_game_frame(void)
{
    u16 sw;

    njdp2d_init();
    initRenderState(0);
    mpp_w.vprm.x0 = 0.0f;
    mpp_w.vprm.y0 = 0.0f;
    mpp_w.vprm.ne = -1.0f;
    mpp_w.ds_h[0] = mpp_w.ds_h[1];
    mpp_w.ds_v[0] = mpp_w.ds_v[1];
    mpp_w.ds_h[1] = 100;
    mpp_w.ds_v[1] = 100;
    mpp_w.vprm.fa = 1.0f;
    mpp_w.vprm.x1 = (mpp_w.ds_h[0] * 384) / 100.0f;
    mpp_w.vprm.y1 = (mpp_w.ds_v[0] * 224) / 100.0f;
    appViewSetItems(&mpp_w.vprm);
    appViewMatrix();
    sf_video_set_offset(X_Adjust + Correct_X[0], Y_Adjust + Correct_Y[0]);
    setBackGroundColor(0xFF000000);
    appSetupTempPriority();
    flPADGetALL();
    keyConvert();
    Interrupt_Flag = 0;

    if ((Play_Mode != 3 && Play_Mode != 1) || Game_pause != 0x81) {
        p1sw_1 = p1sw_0;
        p2sw_1 = p2sw_0;
        p3sw_1 = p3sw_0;
        p4sw_1 = p4sw_0;
        p1sw_0 = p1sw_buff;
        p2sw_0 = p2sw_buff;
        p3sw_0 = p3sw_buff;
        p4sw_0 = p4sw_buff;
        if (task[3].condition == 1 && Mode_Type == 4 && Play_Mode == 1) {
            sw = p2sw_0;
            p2sw_0 = p1sw_0;
            p1sw_0 = sw;
        }
    }

    render_start();
    mpp_w.inGame = 0;
    njUserMain();
    seqsBeforeProcess();
    njdp2d_draw(7);
    seqsAfterProcess();
    CP3toPS2Draw();
    njdp2d_draw(0x18);
    render_end();
    sf_video_present();
    Interrupt_Timer++;
    Interrupt_Flag = 1;
    Scrn_Renew();
    Irl_Family();
    Irl_Scrn();
    BGM_Server();
}
