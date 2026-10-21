/* The game's saves (its SaveInit and SaveMove), in data/save/game.sqlite.
 *
 * Retail's save layer is an Xbox menu over memory-unit files. What the game
 * needs from it is the data: kind 0 is the system data (the current mode's
 * save_w), kind 1 the system directions (its system_dir), kind 2 a replay;
 * mode 0 loads, mode 1 saves, mode 2 is the load at boot, mode 3 the menu's
 * exit to the dashboard. Replays have ten slots, picked on a page drawn by
 * src/port/menu_ext.c; SaveMove returns 1 while it is open, 0 when the
 * replay is saved or loaded, -1 when the player backs out. Loading applies
 * the data the way retail does (3s-decomp-ogxb, Source/XBOX/xb_121642.c:
 * xb_SaveApplySystem, xb_SaveApplyDirection, xb_SavePrepareSystem).
 *
 * The file is SQLite (src/port/store.c). The whole system row and the
 * directions are loaded when the program starts, once the init task has
 * laid down the factory defaults, so a saved game is already in effect
 * before the logos. A missing database is created from the original
 * defaults: Game_Default_Data and Dir_Default_Data. A damaged database is
 * left as it is. Replay slots stay in the file until a replay screen
 * asks for one. */

#include "host/paths.h"
#include "host/compat.h"
#include "port/port.h"
#include "port/store.h"

#include "types.h"
#include "structs.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

/* Storage the game declares and retail's platform files defined. */
u8 sound_mode SF_GAME;
s16 bgm_level SF_GAME;
s16 se_level SF_GAME;

extern struct _SAVE_W save_w[6];
extern _REPLAY_W Replay_w;
extern struct _REP_GAME_INFOR Rep_Game_Infor[11];
extern SystemDir system_dir[6];
extern u8 Present_Mode;
extern u8 Forbid_Reset;
extern u8 bgm_type;
extern MPP mpp_w;
extern s32 X_Adjust;
extern s32 Y_Adjust;
extern s16 Correct_X[4];
extern s16 Correct_Y[4];
extern u8 Disp_Size_H;
extern u8 Disp_Size_V;

void setupSoundMode(void);
void SsBgmHalfVolume(s16 flag);
void setSeVolume(void);
void Copy_Save_w(void);
void Copy_Check_w(void);
void Save_Game_Data(void);
void dspwhUnpack(u8 src, u8 *xdsp, u8 *ydsp);
void Setup_Disp_Size(s16 flag);
s16 Check_SysDir_Page(void);

extern const struct _SAVE_W Game_Default_Data;
extern const SystemDir Dir_Default_Data;

/* The player who picked the replay to load (retail's, read by Load_Replay). */
u8 D_007AA5EF SF_GAME;

static int s_kind;
static int s_mode;
static int s_pending;
static s32 s_boot_loaded;

/* Whether the load at boot has run: the title takes Start only after it
 * (retail's flag at 0x7A1808, read by the entry task). */
s32 xb_Get7A1808(void)
{
    return s_boot_loaded;
}

/* SCREEN ADJUST's FILTER, as the menu left it (sys_w.screen_mode):
 * 0 OFF, 1 SMOOTH, 2 CRT CURVED, 3 CRT FLAT. Older saves still read 0 or 1. */
extern struct _SYSTEM_W sys_w;

int sf_save_screen_filter(void)
{
    return sys_w.screen_mode > 3 ? 0 : sys_w.screen_mode;
}

/* What the system file last had for the screen. A damaged file is not
 * rewritten. Opening SCREEN ADJUST does not write; a change does. */
static int s_system_ok;
static int s_screen_known;
static s8 s_screen_x;
static s8 s_screen_y;
static u8 s_screen_size;
static u8 s_screen_mode;

static int game_db(char *out, size_t n);

static void remember_screen(const struct _SAVE_W *sw)
{
    s_screen_x = sw->Adjust_X;
    s_screen_y = sw->Adjust_Y;
    s_screen_size = sw->Screen_Size;
    s_screen_mode = sw->Screen_Mode;
    s_screen_known = 1;
}

void sf_system_commit_screen(void)
{
    struct _SAVE_W *sw = &save_w[1];
    char path[4096 + 64];
    if (!s_system_ok) {
        return;
    }
    if (s_screen_known && sw->Adjust_X == s_screen_x && sw->Adjust_Y == s_screen_y &&
        sw->Screen_Size == s_screen_size && sw->Screen_Mode == s_screen_mode) {
        return;
    }
    if (game_db(path, sizeof path) != 0 || sf_store_write_system(path, sw) != 0) {
        return;
    }
    remember_screen(sw);
}

/* data/save/game.sqlite. Created on the first save. */
static int game_db(char *out, size_t n)
{
    char base[4096];
    int wrote;
    if (sf_exe_dir(base, sizeof base) != 0) {
        return -1;
    }
    wrote = snprintf(out, n, "%s/data/save", base);
    if (wrote < 0 || (size_t)wrote >= n) {
        return -1;
    }
    if (mkdir(out, 0755) != 0 && errno != EEXIST) {
        fprintf(stderr, "save: %s: %s\n", out, strerror(errno));
        return -1;
    }
    wrote = snprintf(out, n, "%s/data/save/game.sqlite", base);
    return wrote < 0 || (size_t)wrote >= n ? -1 : 0;
}

/* No row yet: write the original default and read that back as the save. */
static int open_system(struct _SAVE_W *sw)
{
    char path[4096 + 64];
    int r;
    if (game_db(path, sizeof path) != 0) {
        return -1;
    }
    r = sf_store_read_system(path, sw);
    if (r != 1) {
        return r;
    }
    *sw = Game_Default_Data;
    if (sf_store_write_system(path, sw) != 0) {
        return -1;
    }
    return 0;
}

static int open_directions(SystemDir *dir)
{
    char path[4096 + 64];
    int r;
    if (game_db(path, sizeof path) != 0) {
        return -1;
    }
    r = sf_store_read_directions(path, dir);
    if (r != 1) {
        return r;
    }
    *dir = Dir_Default_Data;
    if (sf_store_write_directions(path, dir) != 0) {
        return -1;
    }
    return 0;
}

static int open_replay(int slot, _REPLAY_W *w)
{
    char path[4096 + 64];
    int r;
    if (game_db(path, sizeof path) != 0) {
        return -1;
    }
    r = sf_store_read_replay(path, slot, w, NULL);
    if (r == 0) {
        w->game_infor.fname = NULL;
    }
    return r;
}

/* The pages of system directions past the first one in use come from mode 0. */
static void fill_directions(void)
{
    s16 page = Check_SysDir_Page();
    int r;
    int i;
    if (page < 9) {
        for (r = page + 1; r < 10; r++) {
            for (i = 0; i < 7; i++) {
                system_dir[Present_Mode].contents[r][i] = system_dir[0].contents[r][i];
            }
        }
    }
}

/* retail's xb_SaveApplySystem */
static void apply_system(const struct _SAVE_W *in)
{
    struct _SAVE_W *sw = &save_w[Present_Mode];
    *sw = *in;
    save_w[4] = *in;
    save_w[5] = *in;
    bgm_type = sw->BgmType;
    sound_mode = sw->SoundMode;
    bgm_level = sw->BGM_Level;
    se_level = sw->SE_Level;
    if (mpp_w.cutAnalogStickData != 0) {
        sw->AnalogStick = 0;
        mpp_w.cutAnalogStickData = 0;
        save_w[4].AnalogStick = 0;
        save_w[5].AnalogStick = 0;
    }
    setupSoundMode();
    SsBgmHalfVolume(0);
    setSeVolume();
    Copy_Save_w();
    Copy_Check_w();
    Y_Adjust = sw->Adjust_Y * 4;
    X_Adjust = sw->Adjust_X * 4;
    sf_video_set_offset(Correct_X[0] + X_Adjust, Correct_Y[0] + Y_Adjust);
    dspwhUnpack(sw->Screen_Size, &Disp_Size_H, &Disp_Size_V);
    Setup_Disp_Size(0);
    sys_w.screen_mode = sw->Screen_Mode > 3 ? 0 : sw->Screen_Mode;
    fill_directions();
    remember_screen(sw);
}

/* The init task writes the factory defaults into save_w and system_dir.
 * This replaces them with the database. Called once that write has
 * finished. A damaged file is left alone and the defaults stay. */
void sf_save_startup(void)
{
    static int done;
    struct _SAVE_W sw;
    SystemDir dir;
    if (done) {
        return;
    }
    done = 1;
    if (open_system(&sw) == 0) {
        s_system_ok = 1;
        apply_system(&sw);
    }
    if (open_directions(&dir) == 0) {
        system_dir[Present_Mode] = dir;
        fill_directions();
    }
}

static void run(void)
{
    struct _SAVE_W sw;
    SystemDir dir;
    char path[4096 + 64];
    int loaded;

    switch (s_kind) {
    case 0:
        if (s_mode == 1) {
            Save_Game_Data();
            if (game_db(path, sizeof path) == 0 &&
                sf_store_write_system(path, &save_w[Present_Mode]) == 0) {
                s_system_ok = 1;
                if (Present_Mode == 1) {
                    remember_screen(&save_w[1]);
                }
            }
        } else {
            loaded = open_system(&sw);
            if (loaded == 0) {
                s_system_ok = 1;
                apply_system(&sw);
            }
        }
        break;
    case 1:
        if (s_mode == 1) {
            if (game_db(path, sizeof path) == 0) {
                sf_store_write_directions(path, &system_dir[Present_Mode]);
            }
        } else if (open_directions(&dir) == 0) {
            system_dir[Present_Mode] = dir;
            fill_directions();
        }
        break;
    default:
        break;
    }
}

/* ---- replays: rows 0 .. 9 of game.sqlite ---- */

/* What the slot holds, for the page: 1 a replay (its two characters and
 * when it was saved), 0 nothing, -1 a damaged file. */
int sf_replay_slot(int slot, int chars[2], long long *when)
{
    static _REPLAY_W w;
    char path[4096 + 64];
    long long saved = 0;
    int r;
    if (slot < 0 || slot >= SF_REPLAY_SLOTS) {
        return 0;
    }
    if (game_db(path, sizeof path) != 0) {
        return -1;
    }
    r = sf_store_read_replay(path, slot, &w, &saved);
    if (r != 0) {
        return r > 0 ? 0 : -1;
    }
    chars[0] = w.game_infor.player_infor[0].my_char;
    chars[1] = w.game_infor.player_infor[1].my_char;
    *when = saved;
    return 1;
}

/* retail's xb_SavePrepareReplay: the match's characters and stage, the
 * rules and directions it was played with, then the recorded inputs. */
int sf_replay_save(int slot)
{
    struct _SAVE_W *sw = &save_w[Present_Mode];
    char path[4096 + 64];
    Replay_w.game_infor = Rep_Game_Infor[10];
    Replay_w.game_infor.fname = NULL; /* an address, meaningless in a file */
    Replay_w.mini_save_w.Pad_Infor[0] = sw->Pad_Infor[0];
    Replay_w.mini_save_w.Pad_Infor[1] = sw->Pad_Infor[1];
    Replay_w.mini_save_w.Time_Limit = sw->Time_Limit;
    Replay_w.mini_save_w.Battle_Number[0] = sw->Battle_Number[0];
    Replay_w.mini_save_w.Battle_Number[1] = sw->Battle_Number[1];
    Replay_w.mini_save_w.Damage_Level = sw->Damage_Level;
    Replay_w.mini_save_w.extra_option = sw->extra_option;
    Replay_w.system_dir = system_dir[Present_Mode];
    if (game_db(path, sizeof path) != 0) {
        return -1;
    }
    return sf_store_write_replay(path, slot, &Replay_w);
}

/* retail's xb_SaveApplyReplay */
int sf_replay_load(int slot)
{
    static _REPLAY_W w;
    if (slot < 0 || slot >= SF_REPLAY_SLOTS || open_replay(slot, &w) != 0) {
        return -1;
    }
    w.game_infor.fname = NULL;
    Replay_w = w;
    return 0;
}

void SaveInit(s32 kind, s32 mode)
{
    s_kind = kind >= 0 && kind < 3 ? kind : 0;
    s_mode = mode;
    s_pending = 1;
    Forbid_Reset = 1;
}

/* 1 while an operation runs, 0 when it is done, -1 when a replay's page
 * was left without one. */
s32 SaveMove(void)
{
    if (s_kind == 2) {
        s32 r = sf_replay_page(s_pending, s_mode == 1);
        s_pending = 0;
        if (r <= 0) {
            Forbid_Reset = 0;
        }
        return r;
    }
    if (s_pending) {
        s_pending = 0;
        if (s_mode == 3) {
            /* the menu's exit to the dashboard: there is none, so quit */
            sf_port_quit();
        }
        if (s_mode == 2) {
            /* the boot load works in mode 1, the console's (retail's
             * boot state sets it before it loads) */
            Present_Mode = 1;
        }
        run();
        if (s_mode == 2) {
            s_boot_loaded = 1;
        }
    }
    Forbid_Reset = 0;
    return 0;
}
