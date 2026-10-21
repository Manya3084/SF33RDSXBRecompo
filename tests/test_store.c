/* The save file is text. A struct written out and read back matches,
 * including a replay's last input frame and the bytes the file keeps as
 * hex. A value outside the field's range, or a version other than 1,
 * rejects the file. */

#include "port/store.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int s_fail;

static void expect(int cond, const char *msg)
{
    if (!cond) {
        fprintf(stderr, "fail: %s\n", msg);
        s_fail = 1;
    }
}

static void expect_mem(const void *a, const void *b, size_t n, const char *msg)
{
    const unsigned char *x = a;
    const unsigned char *y = b;
    size_t i;
    if (memcmp(a, b, n) == 0) {
        return;
    }
    for (i = 0; i < n; i++) {
        if (x[i] != y[i]) {
            fprintf(stderr, "fail: %s at %zu: %02x != %02x\n", msg, i, x[i], y[i]);
            s_fail = 1;
            return;
        }
    }
}

static char *slurp(const char *path)
{
    FILE *f;
    long n;
    char *s;
    f = fopen(path, "rb");
    if (!f) {
        return NULL;
    }
    if (fseek(f, 0, SEEK_END) != 0 || (n = ftell(f)) < 0 || fseek(f, 0, SEEK_SET) != 0) {
        fclose(f);
        return NULL;
    }
    s = malloc((size_t)n + 1);
    if (!s) {
        fclose(f);
        return NULL;
    }
    if (fread(s, 1, (size_t)n, f) != (size_t)n) {
        free(s);
        fclose(f);
        return NULL;
    }
    s[n] = 0;
    fclose(f);
    return s;
}

static void fill_system(struct _SAVE_W *a)
{
    int i;
    int r;
    int c;
    memset(a, 0, sizeof *a);
    a->Difficulty = 4;
    a->Time_Limit = -9;
    a->Battle_Number[0] = 2;
    a->Battle_Number[1] = 3;
    a->Damage_Level = 2;
    a->Handicap = 1;
    a->Partner_Type[0] = 5;
    a->Partner_Type[1] = 6;
    a->Adjust_X = -3;
    a->Adjust_Y = 8;
    a->Screen_Size = 1;
    a->Screen_Mode = 3;
    a->GuardCheck = 1;
    a->Auto_Save = 1;
    a->AnalogStick = 1;
    a->BgmType = 2;
    a->SoundMode = 1;
    a->BGM_Level = 12;
    a->SE_Level = 9;
    a->Extra_Option = 1;
    a->sum = 0xA1B2C3D4u;
    for (i = 0; i < 8; i++) {
        a->Pad_Infor[0].Shot[i] = (u8)(10 + i);
        a->Pad_Infor[1].Shot[i] = (u8)(20 + i);
    }
    a->Pad_Infor[0].Vibration = 1;
    a->Pad_Infor[1].Vibration = 2;
    a->Pad_Infor[0].free[0] = 3;
    a->Pad_Infor[0].free[1] = 4;
    a->Pad_Infor[0].free[2] = 5;
    a->Pad_Infor[1].free[2] = 6;
    for (i = 0; i < 20; i++) {
        a->PL_Color[0][i] = (u8)(i + 1);
        a->PL_Color[1][i] = (u8)(30 + i);
        a->Ranking[i].name[0] = (u8)(1 + i);
        a->Ranking[i].name[1] = 2;
        a->Ranking[i].name[2] = 3;
        a->Ranking[i].player = (u16)(100 + i);
        a->Ranking[i].score = 1000u + (u32)i;
        a->Ranking[i].cpu_grade = (s8)(i - 10);
        a->Ranking[i].grade = (s8)(i - 5);
        a->Ranking[i].wins = (u16)(3 + i);
        a->Ranking[i].player_color = (u8)i;
        a->Ranking[i].all_clear = (u8)(i & 1);
    }
    for (r = 0; r < 4; r++) {
        for (c = 0; c < 8; c++) {
            a->extra_option.contents[r][c] = (s8)(r * 8 + c - 5);
        }
    }
}

static void fill_replay(_REPLAY_W *w)
{
    int r;
    int c;
    memset(w, 0, sizeof *w);
    w->header[0] = 0xAB;
    w->header[639] = 0xCD;
    w->sega_reserve[0] = 0x11;
    w->sega_reserve[63] = 0x33;
    w->Control_Time_Buff = -2;
    w->Difficulty = 3;
    w->Monitor_Type = 1;
    w->free_free[3] = 0x22;
    w->champion = 1;
    w->full_data = 2;
    w->free = 3;
    w->extra_free = 4;
    w->sum = 0x12345678u;
    w->game_infor.player_infor[0].my_char = 5;
    w->game_infor.player_infor[0].sa = -1;
    w->game_infor.player_infor[0].color = 2;
    w->game_infor.player_infor[0].player_type = -2;
    w->game_infor.player_infor[1].my_char = 8;
    w->game_infor.player_infor[1].sa = 1;
    w->game_infor.player_infor[1].color = 3;
    w->game_infor.player_infor[1].player_type = 4;
    w->game_infor.stage = -1;
    w->game_infor.Direction_Working = 1;
    w->game_infor.Vital_Handicap[0] = -8;
    w->game_infor.Vital_Handicap[1] = 9;
    w->game_infor.Random_ix16 = -100;
    w->game_infor.Random_ix32 = 200;
    w->game_infor.Random_ix16_ex = -300;
    w->game_infor.Random_ix32_ex = 400;
    w->game_infor.fname = (s8 *)(uintptr_t)1; /* an address must not come back */
    w->game_infor.winner = 1;
    w->game_infor.play_type = 2;
    w->game_infor.players_timer = 1000;
    w->game_infor.old_mes_no2 = -2;
    w->game_infor.old_mes_no3 = -3;
    w->game_infor.old_mes_no_pl = -4;
    w->game_infor.mes_already = -5;
    w->mini_save_w.Time_Limit = -4;
    w->mini_save_w.Battle_Number[0] = 6;
    w->mini_save_w.Battle_Number[1] = 7;
    w->mini_save_w.Damage_Level = 2;
    w->mini_save_w.Pad_Infor[0].Shot[7] = 9;
    w->mini_save_w.Pad_Infor[0].Vibration = 1;
    w->mini_save_w.Pad_Infor[1].free[2] = 4;
    for (r = 0; r < 4; r++) {
        for (c = 0; c < 8; c++) {
            w->mini_save_w.extra_option.contents[r][c] = (s8)(c - r - 5);
        }
    }
    for (r = 0; r < 10; r++) {
        for (c = 0; c < 7; c++) {
            w->system_dir.contents[r][c] = (s8)(r + c - 9);
        }
    }
    w->system_dir.contents[9][6] = -9;
    w->system_dir.sum = 50;
    w->net_infor.Handle_1P[0] = 'K';
    w->net_infor.Handle_1P[17] = 0x7F;
    w->net_infor.Id_1P[7] = 0x42;
    w->net_infor.Handle_2P[0] = 'R';
    w->net_infor.Id_2P[0] = 0x10;
    w->net_infor.Serial_No[5] = 0x11;
    w->net_infor.Battle_Code[13] = 0x12;
    w->lag[0] = 1;
    w->lag[13] = 0xEE;
    w->io_unit.key_buff[0][0] = 0x0001;
    w->io_unit.key_buff[1][0] = 0x8000;
    w->io_unit.key_buff[0][100] = 0x00FF;
    w->io_unit.key_buff[1][100] = 0x0100;
    w->io_unit.key_buff[0][7197] = 0x0002;
    w->io_unit.key_buff[1][7197] = 0x0003;
}

static int write_text(const char *path, const char *text)
{
    FILE *f = fopen(path, "wb");
    if (!f) {
        return -1;
    }
    if (fputs(text, f) < 0) {
        fclose(f);
        return -1;
    }
    return fclose(f) == 0 ? 0 : -1;
}

int main(void)
{
    char dir[] = "/tmp/sf33rd-store-XXXXXX";
    char path[512];
    struct _SAVE_W sys_a;
    struct _SAVE_W sys_b;
    SystemDir dir_a;
    SystemDir dir_b;
    _REPLAY_W *rep_a;
    _REPLAY_W *rep_b;
    SfIni *ini;
    const char *v = NULL;
    char *text;
    int r;
    int c;
    if (!mkdtemp(dir)) {
        perror("mkdtemp");
        return 1;
    }

    fill_system(&sys_a);
    snprintf(path, sizeof path, "%s/system.ini", dir);
    expect(sf_store_write_system(path, &sys_a) == 0, "write system");
    text = slurp(path);
    expect(text && strstr(text, "[system]") && strstr(text, "version=1") && strstr(text, "screen_mode=3") &&
               strstr(text, "time_limit=-9"),
           "system text");
    free(text);
    memset(&sys_b, 0x5a, sizeof sys_b);
    expect(sf_store_read_system(path, &sys_b) == 0, "read system");
    expect_mem(&sys_a, &sys_b, sizeof sys_a, "system");

    snprintf(path, sizeof path, "%s/partial.ini", dir);
    expect(write_text(path, "[system]\nversion=1\nbattle_number=4, 5\ntime_limit=-9\n") == 0, "write partial");
    memset(&sys_a, 0, sizeof sys_a);
    sys_a.Battle_Number[0] = 4;
    sys_a.Battle_Number[1] = 5;
    sys_a.Time_Limit = -9;
    memset(&sys_b, 0x5a, sizeof sys_b);
    expect(sf_store_read_system(path, &sys_b) == 0, "read partial");
    expect_mem(&sys_a, &sys_b, sizeof sys_a, "partial");

    snprintf(path, sizeof path, "%s/bad-ver.ini", dir);
    expect(write_text(path, "[system]\nversion=2\ntime_limit=-9\n") == 0, "write bad version");
    expect(sf_store_read_system(path, &sys_b) == -1, "reject version");
    snprintf(path, sizeof path, "%s/bad-range.ini", dir);
    expect(write_text(path, "[system]\nversion=1\ntime_limit=999\n") == 0, "write bad range");
    expect(sf_store_read_system(path, &sys_b) == -1, "reject range");
    snprintf(path, sizeof path, "%s/bad-list.ini", dir);
    expect(write_text(path, "[system]\nversion=1\nbattle_number=1\n") == 0, "write bad list");
    expect(sf_store_read_system(path, &sys_b) == -1, "reject list");
    expect(sf_store_read_system("/tmp/sf33rd-store-missing.ini", &sys_b) == 1, "missing system");

    memset(&dir_a, 0, sizeof dir_a);
    for (r = 0; r < 10; r++) {
        for (c = 0; c < 7; c++) {
            dir_a.contents[r][c] = (s8)(r * 7 + c - 30);
        }
    }
    dir_a.sum = 0xBEEF;
    snprintf(path, sizeof path, "%s/directions.ini", dir);
    expect(sf_store_write_directions(path, &dir_a) == 0, "write directions");
    memset(&dir_b, 0x5a, sizeof dir_b);
    expect(sf_store_read_directions(path, &dir_b) == 0, "read directions");
    expect_mem(&dir_a, &dir_b, sizeof dir_a, "directions");

    rep_a = calloc(1, sizeof *rep_a);
    rep_b = calloc(1, sizeof *rep_b);
    expect(rep_a && rep_b, "replay alloc");
    if (rep_a && rep_b) {
        fill_replay(rep_a);
        snprintf(path, sizeof path, "%s/replay01.ini", dir);
        expect(sf_store_write_replay(path, rep_a) == 0, "write replay");
        text = slurp(path);
        expect(text && strstr(text, "frames=7198") && strstr(text, "data=01000080"), "replay inputs");
        free(text);
        rep_a->game_infor.fname = NULL;
        memset(rep_b, 0x5a, sizeof *rep_b);
        expect(sf_store_read_replay(path, rep_b) == 0, "read replay");
        expect(rep_b->game_infor.fname == NULL, "fname");
        expect(rep_b->io_unit.key_buff[0][50] == 0, "gap frame");
        expect(rep_b->io_unit.key_buff[0][7197] == 2, "last frame");
        expect(rep_b->Control_Time_Buff == -2, "control time");
        expect_mem(rep_a, rep_b, sizeof *rep_a, "replay");
    }
    free(rep_a);
    free(rep_b);

    snprintf(path, sizeof path, "%s/bare.ini", dir);
    expect(write_text(path, "mode=windowed\nup=Left Shift\n") == 0, "write bare");
    ini = sf_ini_new();
    expect(ini && sf_ini_load(ini, path) == 0, "load bare");
    expect(ini && sf_ini_find(ini, "", "mode", &v) == 0 && strcmp(v, "windowed") == 0, "bare mode");
    expect(ini && sf_ini_find(ini, "", "up", &v) == 0 && strcmp(v, "Left Shift") == 0, "bare key");
    sf_ini_free(ini);
    snprintf(path, sizeof path, "%s/garbage.ini", dir);
    expect(write_text(path, "this is not a key\n") == 0, "write garbage");
    ini = sf_ini_new();
    expect(ini && sf_ini_load(ini, path) == -1, "reject garbage");
    sf_ini_free(ini);

    snprintf(path, sizeof path, "%s/system.ini", dir);
    unlink(path);
    snprintf(path, sizeof path, "%s/partial.ini", dir);
    unlink(path);
    snprintf(path, sizeof path, "%s/bad-ver.ini", dir);
    unlink(path);
    snprintf(path, sizeof path, "%s/bad-range.ini", dir);
    unlink(path);
    snprintf(path, sizeof path, "%s/bad-list.ini", dir);
    unlink(path);
    snprintf(path, sizeof path, "%s/directions.ini", dir);
    unlink(path);
    snprintf(path, sizeof path, "%s/replay01.ini", dir);
    unlink(path);
    snprintf(path, sizeof path, "%s/bare.ini", dir);
    unlink(path);
    snprintf(path, sizeof path, "%s/garbage.ini", dir);
    unlink(path);
    rmdir(dir);
    return s_fail ? 1 : 0;
}
