/* The game's saves (its SaveInit and SaveMove), as files in data/save.
 *
 * Retail's save layer is an Xbox menu over memory-unit files: a slot list,
 * yes/no prompts, a scrambled file per save. What the game needs from it is
 * the data: kind 0 is the system data (the current mode's save_w), kind 1
 * the system directions (its system_dir), kind 2 a replay; mode 0 loads,
 * mode 1 saves, mode 2 is the load at boot, mode 3 the menu's exit to the
 * dashboard. Here each kind is a file next to the game's data. The system
 * data and directions are done in the call that runs them. Replays have
 * ten slots, as retail's memory unit did, picked on a page drawn by
 * src/port/menu_ext.c; SaveMove returns 1 while it is open, 0 when the
 * replay is saved or loaded, -1 when the player backs out. Loading applies the data the
 * way retail does (3s-decomp-ogxb, Source/XBOX/xb_121642.c:
 * xb_SaveApplySystem, xb_SaveApplyDirection, xb_SavePrepareSystem).
 *
 * A file is "SF3S", a version, the kind, the payload size, the payload,
 * then the payload's CRC-32. The payload is the game's struct as this
 * build lays it out. */

#include "host/paths.h"
#include "port/port.h"

#include "types.h"
#include "structs.h"

#include <zlib.h>

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#define SAVE_VERSION 1u

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

static const char *const k_name[2] = { "system.sav", "directions.sav" };

/* The player who picked the replay to load (retail's, read by Load_Replay). */
u8 D_007AA5EF SF_GAME;

static int s_kind;
static int s_mode;
static int s_pending;
static u8 s_screen_filter;
static s32 s_boot_loaded;

/* Whether the load at boot has run: the title takes Start only after it
 * (retail's flag at 0x7A1808, read by the entry task). */
s32 xb_Get7A1808(void)
{
    return s_boot_loaded;
}

/* Retail's screen-filter option (save_w's Screen_Mode): smooth when set. */
int sf_save_screen_filter(void)
{
    return s_screen_filter;
}

/* data/save/NAME; kind 0 and 1 have their names, a replay is replayNN.sav */
static int save_path(int kind, char *out, size_t n)
{
    char base[4096];
    if (sf_exe_dir(base, sizeof base) != 0) {
        return -1;
    }
    snprintf(out, n, "%s/data/save", base);
    if (mkdir(out, 0755) != 0 && errno != EEXIST) {
        fprintf(stderr, "save: %s: %s\n", out, strerror(errno));
        return -1;
    }
    if (kind >= 2) {
        snprintf(out, n, "%s/data/save/replay%02d.sav", base, kind - 1);
    } else {
        snprintf(out, n, "%s/data/save/%s", base, k_name[kind]);
    }
    return 0;
}

static int write_file(int kind, const void *data, u32 size)
{
    char path[4096 + 64];
    char tmp[4096 + 80];
    u32 head[4];
    u32 crc;
    FILE *f;
    if (save_path(kind, path, sizeof path) != 0) {
        return -1;
    }
    snprintf(tmp, sizeof tmp, "%s.tmp", path);
    memcpy(&head[0], "SF3S", 4);
    head[1] = SAVE_VERSION;
    head[2] = (u32)kind;
    head[3] = size;
    crc = (u32)crc32(0, data, size);
    f = fopen(tmp, "wb");
    if (!f || fwrite(head, sizeof head, 1, f) != 1 || fwrite(data, size, 1, f) != 1 || fwrite(&crc, 4, 1, f) != 1) {
        fprintf(stderr, "save: writing %s failed\n", tmp);
        if (f) {
            fclose(f);
        }
        return -1;
    }
    if (fclose(f) != 0 || rename(tmp, path) != 0) {
        fprintf(stderr, "save: writing %s failed\n", path);
        return -1;
    }
    return 0;
}

/* 0 when the file is there and whole, 1 when there is none, -1 when it is
 * damaged or from another layout. */
static int read_file(int kind, void *data, u32 size)
{
    char path[4096 + 64];
    u32 head[4];
    u32 crc;
    FILE *f;
    if (save_path(kind, path, sizeof path) != 0) {
        return -1;
    }
    f = fopen(path, "rb");
    if (!f) {
        return 1;
    }
    if (fread(head, sizeof head, 1, f) != 1 || memcmp(&head[0], "SF3S", 4) != 0 || head[1] != SAVE_VERSION ||
        head[2] != (u32)kind || head[3] != size || fread(data, size, 1, f) != 1 || fread(&crc, 4, 1, f) != 1 ||
        crc != (u32)crc32(0, data, size)) {
        fclose(f);
        fprintf(stderr, "save: %s is damaged; not loaded\n", path);
        return -1;
    }
    fclose(f);
    return 0;
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
    s_screen_filter = sw->Screen_Mode;
    fill_directions();
}

static void run(void)
{
    struct _SAVE_W sw;
    SystemDir dir;

    switch (s_kind) {
    case 0:
        if (s_mode == 1) {
            Save_Game_Data();
            write_file(0, &save_w[Present_Mode], sizeof save_w[0]);
        } else if (read_file(0, &sw, sizeof sw) == 0) {
            apply_system(&sw);
        }
        break;
    case 1:
        if (s_mode == 1) {
            write_file(1, &system_dir[Present_Mode], sizeof system_dir[0]);
        } else if (read_file(1, &dir, sizeof dir) == 0) {
            system_dir[Present_Mode] = dir;
            fill_directions();
        }
        break;
    default:
        break;
    }
}

/* ---- replays: kind 2 + slot in the file's header, files replay01..10 ---- */

#define REPLAY_KIND(slot) (2 + (slot))

/* What the slot holds, for the page: 1 a replay (its two characters and
 * when it was saved), 0 nothing, -1 a damaged file. */
int sf_replay_slot(int slot, int chars[2], long long *when)
{
    static _REPLAY_W w;
    char path[4096 + 64];
    struct stat st;
    int r;
    if (slot < 0 || slot >= SF_REPLAY_SLOTS) {
        return 0;
    }
    r = read_file(REPLAY_KIND(slot), &w, sizeof w);
    if (r != 0) {
        return r > 0 ? 0 : -1;
    }
    chars[0] = w.game_infor.player_infor[0].my_char;
    chars[1] = w.game_infor.player_infor[1].my_char;
    *when = 0;
    if (save_path(REPLAY_KIND(slot), path, sizeof path) == 0 && stat(path, &st) == 0) {
        *when = (long long)st.st_mtime;
    }
    return 1;
}

/* retail's xb_SavePrepareReplay: the match's characters and stage, the
 * rules and directions it was played with, then the recorded inputs. */
int sf_replay_save(int slot)
{
    struct _SAVE_W *sw = &save_w[Present_Mode];
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
    return write_file(REPLAY_KIND(slot), &Replay_w, sizeof Replay_w);
}

/* retail's xb_SaveApplyReplay */
int sf_replay_load(int slot)
{
    static _REPLAY_W w;
    if (read_file(REPLAY_KIND(slot), &w, sizeof w) != 0) {
        return -1;
    }
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
