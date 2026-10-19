/* The game's pads, from SDL3.
 *
 * Retail's pad layer reads each Xbox gamepad (a button word, eight analog
 * buttons, two sticks) and turns it into the library's TARPAD records
 * through two fixed tables; keyConvert then joins each port as the next
 * player the first time it is pressed and turns the pads into the game's
 * switch words. This keeps that conversion and fills the gamepad record
 * from SDL3: port 0 is the keyboard, ports 1 to 3 the first three
 * gamepads. The reference is 3s-decomp-ogxb, Source/XBOX/xb_012146.c
 * (xb_PADRead, xb_PADReadOne, xb_PADConvertStick) and xb_061267.c
 * (keyConvert).
 *
 * Keyboard: arrows are the d-pad, Z A, X B, A X, S Y, Q White, E Black,
 * W the left trigger, R the right trigger, Enter Start, Backspace Back. */

#include "port/port.h"

#include "types.h"
#include "structs.h"

#include <SDL3/SDL.h>

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define PORTS 4

/* An Xbox gamepad as the XDK's input sample reads it. */
enum { XB_DPAD_UP = 1, XB_DPAD_DOWN = 2, XB_DPAD_LEFT = 4, XB_DPAD_RIGHT = 8, XB_START = 0x10, XB_BACK = 0x20,
       XB_LTHUMB = 0x40, XB_RTHUMB = 0x80 };
enum { XB_A, XB_B, XB_X, XB_Y, XB_BLACK, XB_WHITE, XB_LTRIG, XB_RTRIG };

typedef struct XbGamepad {
    int connected;
    u16 buttons;
    u8 analog[8];
    f32 lx;
    f32 ly; /* up is positive */
    f32 rx;
    f32 ry;
} XbGamepad;

TARPAD tarpad_root[PORTS];

extern MPP mpp_w;
extern FLPAD *flpad_adr[2];
extern IO io_w;
extern u8 Interface_Type[2];
extern struct _SAVE_W save_w[6];
extern u8 Present_Mode;
extern u8 Game_pause;
extern struct _TASK task[11];
extern u16 p1sw_buff, p2sw_buff, p3sw_buff, p4sw_buff;
extern const u32 flpad_io_map[25];
void flPADSetRepeatSw(FLPAD *pad, u32 IOdata, u8 ctr, u8 times);

/* each analog button past 0x1F at half scale sets a bit above the word */
static const u16 k_analog_bit[8][2] = {
    { 0, 0x100 }, { 1, 0x200 }, { 2, 0x400 }, { 3, 0x800 }, { 4, 0x1000 }, { 5, 0x2000 }, { 6, 0x4000 }, { 7, 0x8000 },
};

/* the 16 library buttons: the gamepad bit, and 1 + the analog index that
 * gives its strength (0 for a digital button) */
static const u32 k_library_button[16][2] = {
    { 1, 0 },      { 2, 0 },      { 4, 0 },      { 8, 0 },      { 0x100, 1 }, { 0x200, 2 }, { 0x1000, 5 }, { 0x4000, 7 },
    { 0x400, 3 }, { 0x800, 4 }, { 0x2000, 6 }, { 0x8000, 8 }, { 64, 0 },   { 128, 0 },   { 32, 0 },     { 16, 0 },
};

/* the d-pad nibble with opposite directions cancelled */
static const u8 k_dpad_fix[16] = { 0, 1, 2, 0, 4, 5, 6, 4, 8, 9, 10, 8, 0, 1, 2, 0 };

/* a stick's direction as d-pad bits, by 22.5 degree sector from the right */
static const u8 k_stick_dir[17] = { 8, 9, 9, 1, 1, 5, 5, 4, 4, 6, 6, 2, 2, 10, 10, 8, 1 };

/* the game's thresholds per port: analog button, left and right stick */
static s16 s_threshold[PORTS][3];
static SDL_Gamepad *s_pads[PORTS];

/* the game's switch bits from the library's, Xbox values */
static const u32 k_ioconv[24][2] = {
    { 0x1, 0x1 },     { 0x2, 0x2 },     { 0x4, 0x4 },      { 0x8, 0x8 },      { 0x100, 0x10 },  { 0x200, 0x20 },
    { 0x400, 0x40 },  { 0x400, 0x80 },  { 0x10, 0x100 },   { 0x20, 0x200 },   { 0x40, 0x400 },  { 0x40, 0x800 },
    { 0x0, 0x1000 },  { 0x0, 0x2000 },  { 0x8000, 0x4000 }, { 0x4000, 0x8000 }, { 0x0, 0x10000 }, { 0x0, 0x20000 },
    { 0x0, 0x40000 }, { 0x0, 0x80000 }, { 0x0, 0x100000 }, { 0x0, 0x200000 }, { 0x0, 0x400000 }, { 0x0, 0x800000 },
};

s32 xb_PADInit(void)
{
    SDL_InitSubSystem(SDL_INIT_GAMEPAD);
    memset(s_pads, 0, sizeof s_pads);
    return 1;
}

void xb_PADSetConfig(s32 i, s16 a, s16 b, s16 c)
{
    if (i >= 0 && i < PORTS) {
        s_threshold[i][0] = a;
        s_threshold[i][1] = b;
        s_threshold[i][2] = c;
    }
}

/* level 0..7 for time frames */
s32 xb_PADSetVibration(s32 pad, s32 level, u32 time)
{
    u32 v = ((u32)level << 13) + 0x1FFF;
    if (v > 0xFFFF) {
        v = 0xFFFF;
    }
    if (pad < 0 || pad >= PORTS) {
        return 0;
    }
    if (s_pads[pad]) {
        SDL_RumbleGamepad(s_pads[pad], (Uint16)v, (Uint16)v, time * 1000 / 60);
    }
    return 1;
}

static void refresh_gamepads(void)
{
    SDL_JoystickID *ids;
    int n = 0;
    int i;
    int port = 1;
    if (getenv("SF_NO_GAMEPADS")) {
        return; /* tests: only the keyboard port and scripted presses */
    }
    for (i = 1; i < PORTS; i++) {
        if (s_pads[i] && !SDL_GamepadConnected(s_pads[i])) {
            SDL_CloseGamepad(s_pads[i]);
            s_pads[i] = NULL;
        }
    }
    ids = SDL_GetGamepads(&n);
    for (i = 0; ids && i < n && port < PORTS; i++) {
        SDL_Gamepad *g = SDL_GetGamepadFromID(ids[i]);
        int j;
        int open = 0;
        for (j = 1; j < PORTS; j++) {
            if (s_pads[j] && s_pads[j] == g) {
                open = 1;
            }
        }
        if (open) {
            continue;
        }
        while (port < PORTS && s_pads[port]) {
            port++;
        }
        if (port < PORTS) {
            s_pads[port] = SDL_OpenGamepad(ids[i]);
            if (s_pads[port]) {
                fprintf(stderr, "input: port %d is %s\n", port, SDL_GetGamepadName(s_pads[port]));
            } else {
                fprintf(stderr, "input: gamepad %u: %s\n", (unsigned)ids[i], SDL_GetError());
            }
        }
    }
    SDL_free(ids);
}

static f32 axis(SDL_Gamepad *g, SDL_GamepadAxis a)
{
    return (f32)SDL_GetGamepadAxis(g, a) / 32767.0f;
}

/* The keyboard as a gamepad: one row per binding, so the bindings can be
 * changed (and later saved) without touching the reading. A binding is a
 * button bit (XB_*) or an analog button (index | 0x100). */
typedef struct KeyBinding {
    SDL_Scancode key;
    u16 target;
} KeyBinding;

#define BIND_ANALOG 0x100

static KeyBinding s_keys[] = {
    { SDL_SCANCODE_UP, XB_DPAD_UP },        { SDL_SCANCODE_DOWN, XB_DPAD_DOWN },
    { SDL_SCANCODE_LEFT, XB_DPAD_LEFT },    { SDL_SCANCODE_RIGHT, XB_DPAD_RIGHT },
    { SDL_SCANCODE_RETURN, XB_START },      { SDL_SCANCODE_BACKSPACE, XB_BACK },
    { SDL_SCANCODE_Z, BIND_ANALOG | XB_A }, { SDL_SCANCODE_X, BIND_ANALOG | XB_B },
    { SDL_SCANCODE_A, BIND_ANALOG | XB_X }, { SDL_SCANCODE_S, BIND_ANALOG | XB_Y },
    { SDL_SCANCODE_E, BIND_ANALOG | XB_BLACK }, { SDL_SCANCODE_Q, BIND_ANALOG | XB_WHITE },
    { SDL_SCANCODE_W, BIND_ANALOG | XB_LTRIG }, { SDL_SCANCODE_R, BIND_ANALOG | XB_RTRIG },
};

/* SF_AUTOPRESS=F[:BUTTON],...: that button (Start when none is named;
 * up, down, left, right, a, b, x, y, start, back) held on the keyboard
 * port for six frames from frame F (for driving the game in tests). */
static void auto_press(XbGamepad *x)
{
    static int frame;
    const char *env = getenv("SF_AUTOPRESS");
    int now = frame++;
    while (env && *env) {
        char *end;
        char name[16];
        size_t n = 0;
        long f = strtol(env, &end, 10);
        if (end == env) {
            break;
        }
        env = end;
        if (*env == ':') {
            env++;
            while (*env && *env != ',' && n + 1 < sizeof name) {
                name[n++] = *env++;
            }
        }
        name[n] = 0;
        if (*env == ',') {
            env++;
        }
        if (now < f || now >= f + 6) {
            continue;
        }
        if (n == 0 || !strcmp(name, "start")) {
            x->buttons |= XB_START;
        } else if (!strcmp(name, "back")) {
            x->buttons |= XB_BACK;
        } else if (!strcmp(name, "up")) {
            x->buttons |= XB_DPAD_UP;
        } else if (!strcmp(name, "down")) {
            x->buttons |= XB_DPAD_DOWN;
        } else if (!strcmp(name, "left")) {
            x->buttons |= XB_DPAD_LEFT;
        } else if (!strcmp(name, "right")) {
            x->buttons |= XB_DPAD_RIGHT;
        } else if (!strcmp(name, "a")) {
            x->analog[XB_A] = 0xFF;
        } else if (!strcmp(name, "b")) {
            x->analog[XB_B] = 0xFF;
        } else if (!strcmp(name, "x")) {
            x->analog[XB_X] = 0xFF;
        } else if (!strcmp(name, "y")) {
            x->analog[XB_Y] = 0xFF;
        }
    }
}

static void read_keyboard(XbGamepad *x)
{
    const bool *k = SDL_GetKeyboardState(NULL);
    size_t i;
    x->connected = 1;
    auto_press(x);
    for (i = 0; i < sizeof s_keys / sizeof s_keys[0]; i++) {
        if (!k[s_keys[i].key]) {
            continue;
        }
        if (s_keys[i].target & BIND_ANALOG) {
            x->analog[s_keys[i].target & 7] = 0xFF;
        } else {
            x->buttons |= s_keys[i].target;
        }
    }
}

static void read_gamepad(SDL_Gamepad *g, XbGamepad *x)
{
    f32 lt;
    f32 rt;
    if (!g) {
        return;
    }
    x->connected = 1;
    x->buttons = (u16)((SDL_GetGamepadButton(g, SDL_GAMEPAD_BUTTON_DPAD_UP) ? XB_DPAD_UP : 0) |
                       (SDL_GetGamepadButton(g, SDL_GAMEPAD_BUTTON_DPAD_DOWN) ? XB_DPAD_DOWN : 0) |
                       (SDL_GetGamepadButton(g, SDL_GAMEPAD_BUTTON_DPAD_LEFT) ? XB_DPAD_LEFT : 0) |
                       (SDL_GetGamepadButton(g, SDL_GAMEPAD_BUTTON_DPAD_RIGHT) ? XB_DPAD_RIGHT : 0) |
                       (SDL_GetGamepadButton(g, SDL_GAMEPAD_BUTTON_START) ? XB_START : 0) |
                       (SDL_GetGamepadButton(g, SDL_GAMEPAD_BUTTON_BACK) ? XB_BACK : 0) |
                       (SDL_GetGamepadButton(g, SDL_GAMEPAD_BUTTON_LEFT_STICK) ? XB_LTHUMB : 0) |
                       (SDL_GetGamepadButton(g, SDL_GAMEPAD_BUTTON_RIGHT_STICK) ? XB_RTHUMB : 0));
    x->analog[XB_A] = SDL_GetGamepadButton(g, SDL_GAMEPAD_BUTTON_SOUTH) ? 0xFF : 0;
    x->analog[XB_B] = SDL_GetGamepadButton(g, SDL_GAMEPAD_BUTTON_EAST) ? 0xFF : 0;
    x->analog[XB_X] = SDL_GetGamepadButton(g, SDL_GAMEPAD_BUTTON_WEST) ? 0xFF : 0;
    x->analog[XB_Y] = SDL_GetGamepadButton(g, SDL_GAMEPAD_BUTTON_NORTH) ? 0xFF : 0;
    x->analog[XB_BLACK] = SDL_GetGamepadButton(g, SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER) ? 0xFF : 0;
    x->analog[XB_WHITE] = SDL_GetGamepadButton(g, SDL_GAMEPAD_BUTTON_LEFT_SHOULDER) ? 0xFF : 0;
    lt = axis(g, SDL_GAMEPAD_AXIS_LEFT_TRIGGER);
    rt = axis(g, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER);
    x->analog[XB_LTRIG] = (u8)(lt <= 0.f ? 0 : lt >= 1.f ? 255 : lt * 255.f);
    x->analog[XB_RTRIG] = (u8)(rt <= 0.f ? 0 : rt >= 1.f ? 255 : rt * 255.f);
    x->lx = axis(g, SDL_GAMEPAD_AXIS_LEFTX);
    x->ly = -axis(g, SDL_GAMEPAD_AXIS_LEFTY);
    x->rx = axis(g, SDL_GAMEPAD_AXIS_RIGHTX);
    x->ry = -axis(g, SDL_GAMEPAD_AXIS_RIGHTY);
}

/* retail's xb_PADConvertStick */
static void convert_stick(PAD_STICK *st, s16 depth)
{
    f32 radian = 0.0f;
    if ((st->y | st->x) != 0) {
        radian = (f32)atan2(-st->y, st->x);
        if (radian < 0.0f) {
            radian += 6.2831855f;
        }
    }
    st->rad = radian;
    st->ang = (s16)(360.0f * (st->rad / 6.2831855f));
    st->pow = (s16)sqrt((double)(st->y * st->y) + (double)(st->x * st->x));
    if (st->pow > 0x7F) {
        st->pow = 0x7F;
    }
    if (st->pow < depth) {
        st->x = 0;
        st->y = 0;
        st->rad = 0.0f;
        st->ang = 0;
        st->pow = 0;
    }
}

static u32 stick_button(const PAD_STICK *st)
{
    if (st->pow < 0x10) {
        return 0;
    }
    return k_stick_dir[(int)(st->ang / 22.5f)];
}

/* retail's xb_PADReadOne */
static void read_one(int i, const XbGamepad *x)
{
    TARPAD *t = &tarpad_root[i];
    u8 kan[8];
    u32 io;
    u32 sw = 0;
    int j;
    t->state = 1;
    t->kind = x->connected ? 1 : 0;
    t->anstate = 0x63;
    memset(t->anshot.pow, 0, sizeof t->anshot.pow);
    if (t->kind != 1) {
        t->sw = 0;
        memset(t->stick, 0, sizeof t->stick);
        return;
    }
    io = x->buttons & 0xFF;
    for (j = 0; j < 8; j++) {
        kan[j] = (u8)(x->analog[j] >> 1);
    }
    for (j = 0; j < 8; j++) {
        if (kan[k_analog_bit[j][0]] > 0x1F) {
            io |= k_analog_bit[j][1];
        }
    }
    for (j = 0; j < 16; j++) {
        if (io & k_library_button[j][0]) {
            sw |= flpad_io_map[j];
            if (k_library_button[j][1] != 0) {
                u8 v = kan[k_library_button[j][1] - 1];
                t->anshot.pow[j] = (v < s_threshold[i][0]) ? 0 : v;
            } else {
                t->anshot.pow[j] = 0x7F;
            }
        }
    }
    t->stick[0].x = (s16)(x->lx * 127.5f);
    t->stick[1].x = (s16)(x->rx * 127.5f);
    t->stick[0].y = (s16)(x->ly * -127.5f);
    t->sw = sw;
    t->stick[1].y = (s16)(x->ry * -127.5f);
}

/* retail's xb_PADRead */
void xb_PADRead(void)
{
    XbGamepad x[PORTS];
    int i;
    SDL_PumpEvents();
    refresh_gamepads();
    memset(x, 0, sizeof x);
    read_keyboard(&x[0]);
    for (i = 1; i < PORTS; i++) {
        static u16 last[PORTS];
        read_gamepad(s_pads[i], &x[i]);
        if (getenv("SF_INPUT_DEBUG") && x[i].buttons != last[i]) {
            int b;
            fprintf(stderr, "input: port %d buttons %04X:", i, x[i].buttons);
            for (b = 0; b < SDL_GAMEPAD_BUTTON_COUNT; b++) {
                if (s_pads[i] && SDL_GetGamepadButton(s_pads[i], (SDL_GamepadButton)b)) {
                    fprintf(stderr, " %s", SDL_GetGamepadStringForButton((SDL_GamepadButton)b));
                }
            }
            fputc('\n', stderr);
            last[i] = x[i].buttons;
        }
    }
    for (i = 0; i < PORTS; i++) {
        TARPAD *t = &tarpad_root[i];
        read_one(i, &x[i]);
        t->conn.xbox.handle = x[i].connected ? &tarpad_root[i] : NULL;
        t->sw = (t->sw & 0xFFF0) | k_dpad_fix[t->sw & 0xF];
        convert_stick(&t->stick[0], s_threshold[i][1]);
        convert_stick(&t->stick[1], s_threshold[i][2]);
        t->sw |= ((stick_button(&t->stick[1]) << 4) | stick_button(&t->stick[0])) << 16;
    }
}

/* retail's xb_ResetInputState: no port has joined */
void sf_input_reset(void)
{
    int i;
    for (i = 0; i < 4; i++) {
        mpp_w.xb_player_port[i] = -1;
        mpp_w.xb_port_player[i] = -1;
    }
    mpp_w.xb_pad_count = 0;
}

/* retail's keyConvert */
void keyConvert(void)
{
    IOPad *pad;
    FLPAD *fp;
    u32 currSw;
    s32 i;
    s32 j;
    s32 port;
    s32 repeat_on = 0;

    if (mpp_w.xb_pad_count < 4) {
        for (port = 0; port < 4; port++) {
            if ((u8)mpp_w.xb_port_player[port] == 0xFF && flpad_adr[0][port].sw != 0) {
                mpp_w.xb_port_player[port] = (s8)mpp_w.xb_pad_count;
                mpp_w.xb_player_port[mpp_w.xb_pad_count] = (s8)port;
                mpp_w.xb_pad_count++;
                fprintf(stderr, "input: port %d joins as player %d\n", (int)port, (int)mpp_w.xb_pad_count);
            }
        }
    }

    if (io_w.xb_clear_pads > 0) {
        io_w.xb_clear_pads--;
        flpad_adr[0][0].sw = 0;
        flpad_adr[0][0].sw_new = 0;
        flpad_adr[0][0].sw_off = 0;
        flpad_adr[0][0].sw_old = 0;
        flpad_adr[0][0].sw_repeat = 0;
    }

    if (save_w[Present_Mode].extra_option.contents[0][4] && mpp_w.inGame && Game_pause == 0) {
        repeat_on = 1;
        if (task[3].condition == 1 && task[3].r_no[0] != 10) {
            repeat_on = 0;
        }
    }

    io_w.sw[0] = io_w.sw[1] = io_w.sw[2] = io_w.sw[3] = 0;

    for (port = 0; port < 4; port++) {
        i = mpp_w.xb_port_player[port];
        if (i == -1) {
            continue;
        }
        flPADSetRepeatSw(&flpad_adr[0][port], 0xFF000F, 15, 3);
        if (repeat_on) {
            flPADSetRepeatSw(&flpad_adr[0][port], 0x3FF0, 2, 1);
        } else {
            flPADSetRepeatSw(&flpad_adr[0][port], 0x3FF0, 10, 2);
        }
        fp = &flpad_adr[0][port];
        pad = &io_w.data[i];
        pad->state = fp->state;
        pad->anstate = fp->anstate;
        pad->kind = fp->kind;
        pad->sw = fp->sw;
        pad->sw_old = fp->sw_old;
        pad->sw_new = fp->sw_new;
        pad->sw_off = fp->sw_off;
        pad->sw_chg = fp->sw_chg;
        pad->sw_repeat = fp->sw_repeat;
        pad->stick[0] = fp->stick[0];
        pad->stick[1] = fp->stick[1];

        if (mpp_w.useAnalogStickData) {
            for (j = 16; j <= 20; j += 4) {
                if (!(fp->sw & 0xF)) {
                    pad->sw |= (pad->sw >> j) & 0xF;
                    pad->sw_old |= (pad->sw_old >> j) & 0xF;
                    pad->sw_new |= (pad->sw_new >> j) & 0xF;
                    pad->sw_off |= (pad->sw_off >> j) & 0xF;
                    pad->sw_chg |= (pad->sw_chg >> j) & 0xF;
                    pad->sw_repeat |= (pad->sw_repeat >> j) & 0xF;
                }
            }
        }

        /* retail writes all four players; the game's table holds two */
        if (i < 2) {
            Interface_Type[i] = (pad->kind == 0 || pad->kind == 0x8000) ? 0 : 2;
        }

        currSw = pad->sw;
        for (j = 0; j < 4; j++) {
            if (currSw & k_ioconv[j][1]) {
                io_w.sw[i] |= k_ioconv[j][0];
            }
        }
        for (j = 12; j < 16; j++) {
            if (currSw & k_ioconv[j][1]) {
                io_w.sw[i] |= k_ioconv[j][0];
            }
        }
        if (repeat_on) {
            currSw = pad->sw_repeat;
        }
        for (j = 4; j < 12; j++) {
            if (currSw & k_ioconv[j][1]) {
                io_w.sw[i] |= k_ioconv[j][0];
            }
        }
    }

    p1sw_buff = (u16)io_w.sw[0];
    p2sw_buff = (u16)io_w.sw[1];
    p3sw_buff = (u16)io_w.sw[2];
    p4sw_buff = (u16)io_w.sw[3];
}

/* retail's xb_GetJoiningPlayer: the player who just pressed Start (or the
 * button the game counts with it), player 1 first; -1 for none */
extern u16 p1sw_0, p1sw_1, p2sw_0, p2sw_1;

s32 xb_GetJoiningPlayer(void)
{
    s32 r = -1;
    if ((p2sw_0 & 0x4100) & ~p2sw_1) {
        r = 1;
    }
    if ((p1sw_0 & 0x4100) & ~p1sw_1) {
        r = 0;
    }
    return r;
}

/* retail's xb_GetStartBackPress: 1 when the player's port just pressed
 * Start, 0 for Back, -1 for neither */
s32 xb_GetStartBackPress(s32 pl_id)
{
    s32 port;
    if (pl_id < 0 || pl_id >= 4) {
        return -1;
    }
    port = mpp_w.xb_player_port[pl_id];
    if (port < 0 || port >= 4) {
        return -1;
    }
    switch (flpad_adr[0][port].sw_new & 0x30) {
    case 0x10:
        return 1;
    case 0x20:
        return 0;
    }
    return -1;
}
