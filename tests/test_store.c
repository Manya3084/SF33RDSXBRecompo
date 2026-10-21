/* The save file is SQLite. A struct written out and read back matches,
 * apart from the memory-card fields the file does not keep. A missing
 * file is not created by a read. A file that is not a database, a wrong
 * schema version, a value outside a field's range, or a system row
 * without its twenty rankings is rejected and left as it is. */

#include "port/store.h"

#include <sqlite3.h>

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

static int file_is(const char *path, const char *text)
{
    char *s = slurp(path);
    int ok = s && strcmp(s, text) == 0;
    free(s);
    return ok;
}

static int sql_exec(const char *path, const char *sql)
{
    sqlite3 *db = NULL;
    char *err = NULL;
    int rc;
    if (sqlite3_open(path, &db) != SQLITE_OK) {
        sqlite3_close(db);
        return -1;
    }
    rc = sqlite3_exec(db, sql, NULL, NULL, &err);
    sqlite3_free(err);
    sqlite3_close(db);
    return rc == SQLITE_OK ? 0 : -1;
}

static int sql_int(const char *path, const char *sql, int *out)
{
    sqlite3 *db = NULL;
    sqlite3_stmt *st = NULL;
    if (sqlite3_open(path, &db) != SQLITE_OK) {
        sqlite3_close(db);
        return -1;
    }
    if (sqlite3_prepare_v2(db, sql, -1, &st, NULL) != SQLITE_OK || sqlite3_step(st) != SQLITE_ROW) {
        sqlite3_finalize(st);
        sqlite3_close(db);
        return -1;
    }
    *out = sqlite3_column_int(st, 0);
    sqlite3_finalize(st);
    sqlite3_close(db);
    return 0;
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

static void clear_system_unstored(struct _SAVE_W *a)
{
    memset(a->Pad_Infor[0].free, 0, sizeof a->Pad_Infor[0].free);
    memset(a->Pad_Infor[1].free, 0, sizeof a->Pad_Infor[1].free);
}

static s8 s_fname;

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
    w->game_infor.fname = &s_fname;
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

static void clear_replay_unstored(_REPLAY_W *w)
{
    memset(w->header, 0, sizeof w->header);
    memset(w->sega_reserve, 0, sizeof w->sega_reserve);
    memset(w->free_free, 0, sizeof w->free_free);
    w->game_infor.fname = NULL;
    memset(w->mini_save_w.Pad_Infor[0].free, 0, sizeof w->mini_save_w.Pad_Infor[0].free);
    memset(w->mini_save_w.Pad_Infor[1].free, 0, sizeof w->mini_save_w.Pad_Infor[1].free);
    memset(&w->net_infor, 0, sizeof w->net_infor);
    memset(w->lag, 0, sizeof w->lag);
    w->free = 0;
    w->extra_free = 0;
}

static void test_game(const char *dir)
{
    char path[512];
    char missing[512];
    struct _SAVE_W sys_a;
    struct _SAVE_W sys_b;
    struct _SAVE_W blank;
    SystemDir dir_a;
    SystemDir dir_b;
    _REPLAY_W *rep_a;
    _REPLAY_W *rep_b;
    long long saved = 0;
    int r;
    int c;
    int got = 0;
    snprintf(missing, sizeof missing, "%s/missing.sqlite", dir);
    expect(sf_store_read_system(missing, &sys_b) == 1, "missing system");
    expect(access(missing, F_OK) != 0, "read does not create");

    fill_system(&sys_a);
    snprintf(path, sizeof path, "%s/game.sqlite", dir);
    expect(sf_store_write_system(path, &sys_a) == 0, "write system");
    clear_system_unstored(&sys_a);
    memset(&sys_b, 0x5a, sizeof sys_b);
    expect(sf_store_read_system(path, &sys_b) == 0, "read system");
    expect(sys_b.Screen_Mode == 3, "screen mode 3");
    expect(sys_b.Time_Limit == -9, "time limit");
    expect_mem(&sys_a, &sys_b, sizeof sys_a, "system");

    sys_a.Screen_Mode = 255;
    expect(sf_store_write_system(path, &sys_a) == 0, "write mode 255");
    expect(sf_store_read_system(path, &sys_b) == 0, "read mode 255");
    expect(sys_b.Screen_Mode == 255, "mode not clamped");
    sys_a.Screen_Mode = 3;
    expect(sf_store_write_system(path, &sys_a) == 0, "restore mode 3");

    memset(&dir_a, 0, sizeof dir_a);
    for (r = 0; r < 10; r++) {
        for (c = 0; c < 7; c++) {
            dir_a.contents[r][c] = (s8)(r * 7 + c - 30);
        }
    }
    dir_a.sum = 0xBEEF;
    expect(sf_store_write_directions(path, &dir_a) == 0, "write directions");
    memset(&dir_b, 0x5a, sizeof dir_b);
    expect(sf_store_read_directions(path, &dir_b) == 0, "read directions");
    expect_mem(&dir_a, &dir_b, sizeof dir_a, "directions");
    expect(sf_store_read_system(path, &sys_b) == 0, "system kept");
    expect_mem(&sys_a, &sys_b, sizeof sys_a, "system after directions");

    rep_a = calloc(1, sizeof *rep_a);
    rep_b = calloc(1, sizeof *rep_b);
    expect(rep_a && rep_b, "replay alloc");
    if (rep_a && rep_b) {
        fill_replay(rep_a);
        expect(sf_store_write_replay(path, 3, rep_a) == 0, "write replay 3");
        clear_replay_unstored(rep_a);
        memset(rep_b, 0x5a, sizeof *rep_b);
        saved = 0;
        expect(sf_store_read_replay(path, 3, rep_b, &saved) == 0, "read replay 3");
        expect(rep_b->game_infor.fname == NULL, "fname");
        expect(saved > 0, "saved at");
        expect(rep_b->io_unit.key_buff[0][50] == 0, "gap frame");
        expect(rep_b->io_unit.key_buff[0][7197] == 2, "last frame");
        expect(rep_b->Control_Time_Buff == -2, "control time");
        expect(rep_b->header[0] == 0 && rep_b->lag[0] == 0 && rep_b->net_infor.Handle_1P[0] == 0, "dropped fields");
        expect_mem(rep_a, rep_b, sizeof *rep_a, "replay 3");

        memset(rep_a, 0, sizeof *rep_a);
        rep_a->game_infor.player_infor[0].my_char = 9;
        rep_a->header[0] = 1;
        rep_a->io_unit.key_buff[0][10] = 0x1111;
        expect(sf_store_write_replay(path, 7, rep_a) == 0, "write replay 7");
        clear_replay_unstored(rep_a);
        memset(rep_b, 0x5a, sizeof *rep_b);
        expect(sf_store_read_replay(path, 7, rep_b, NULL) == 0, "read replay 7");
        expect(rep_b->io_unit.key_buff[0][10] == 0x1111, "short frame");
        expect(rep_b->io_unit.key_buff[0][11] == 0, "after last");
        expect(rep_b->io_unit.key_buff[0][7197] == 0, "short tail");
        expect_mem(rep_a, rep_b, sizeof *rep_a, "replay 7");
        expect(sf_store_read_replay(path, 3, rep_b, NULL) == 0, "replay 3 kept");
        expect(rep_b->io_unit.key_buff[0][7197] == 2, "slot 3 last");
        expect(rep_b->game_infor.player_infor[0].my_char == 5, "slot 3 char");
        expect(sf_store_read_replay(path, 0, rep_b, NULL) == 1, "empty slot");
        expect(sf_store_read_replay(path, -1, rep_b, NULL) == -1, "slot low");
        expect(sf_store_read_replay(path, 10, rep_b, NULL) == -1, "slot high");
    }
    free(rep_a);
    free(rep_b);

    expect(sql_exec(path, "UPDATE system SET time_limit=999 WHERE id=1") == 0, "set 999");
    expect(sf_store_read_system(path, &sys_b) == -1, "reject range");
    fill_system(&sys_a);
    clear_system_unstored(&sys_a);
    expect(sf_store_write_system(path, &sys_a) == 0, "rewrite system");
    expect(sql_exec(path, "DELETE FROM rank WHERE idx=19") == 0, "drop a rank");
    expect(sf_store_read_system(path, &sys_b) == -1, "reject ranks");
    expect(sf_store_write_system(path, &sys_a) == 0, "restore ranks");
    expect(sql_exec(path, "PRAGMA user_version=2") == 0, "set version 2");
    memset(&blank, 0, sizeof blank);
    blank.Difficulty = 9;
    expect(sf_store_read_system(path, &sys_b) == -1, "reject version");
    expect(sf_store_write_system(path, &blank) == -1, "version not overwritten");
    expect(sql_int(path, "PRAGMA user_version", &got) == 0 && got == 2, "version stays");
    expect(sql_int(path, "SELECT difficulty FROM system WHERE id=1", &got) == 0 && got == 4, "row stays");
}

static void test_host(const char *dir)
{
    char path[512];
    char mode[32];
    char aspect[32];
    char values[3][64];
    static const char *const names[] = { "up", "down", "left" };
    static const char *const vals[] = { "Up", "Down", "" };
    int width = 0;
    int height = 0;
    snprintf(path, sizeof path, "%s/host.sqlite", dir);
    expect(sf_store_read_window(path, mode, sizeof mode, &width, &height, aspect, sizeof aspect) == 1, "missing window");
    expect(sf_store_read_keys(path, names, values, 3) == 1, "missing keys");
    expect(sf_store_write_window(path, "windowed", 100, 960, "4:3") == -1, "reject width");
    expect(access(path, F_OK) != 0, "width not created");

    expect(sf_store_write_keys(path, names, vals, 3) == 0, "write keys");
    expect(sf_store_read_window(path, mode, sizeof mode, &width, &height, aspect, sizeof aspect) == 1, "no window yet");
    expect(sf_store_read_keys(path, names, values, 3) == 0, "read keys");
    expect(strcmp(values[0], "Up") == 0 && strcmp(values[1], "Down") == 0 && values[2][0] == 0, "key values");

    expect(sf_store_write_window(path, "windowed", 1280, 960, "4:3") == 0, "write window");
    width = 0;
    height = 0;
    mode[0] = 0;
    aspect[0] = 0;
    expect(sf_store_read_window(path, mode, sizeof mode, &width, &height, aspect, sizeof aspect) == 0, "read window");
    expect(strcmp(mode, "windowed") == 0 && width == 1280 && height == 960 && strcmp(aspect, "4:3") == 0, "window");
    expect(sf_store_read_keys(path, names, values, 3) == 0, "keys kept");
    expect(strcmp(values[0], "Up") == 0, "up kept");

    expect(sql_exec(path, "DELETE FROM key WHERE name='up'") == 0, "drop up");
    memset(values, 0x5a, sizeof values);
    expect(sf_store_read_keys(path, names, values, 3) == 0, "keys remain");
    expect(values[0][0] == 0 && strcmp(values[1], "Down") == 0, "missing name");
    expect(sf_store_read_window(path, mode, sizeof mode, &width, &height, aspect, sizeof aspect) == 0, "window kept");
}

static void test_garbage(const char *dir)
{
    char path[512];
    struct _SAVE_W sys;
    static const char text[] = "this is not a database\n";
    snprintf(path, sizeof path, "%s/garbage", dir);
    memset(&sys, 0, sizeof sys);
    expect(write_text(path, text) == 0, "write garbage");
    expect(sf_store_read_system(path, &sys) == -1, "read garbage");
    expect(sf_store_write_system(path, &sys) == -1, "write garbage");
    expect(file_is(path, text), "garbage kept");
}

static void rm(const char *dir, const char *name)
{
    char path[512];
    snprintf(path, sizeof path, "%s/%s", dir, name);
    unlink(path);
}

int main(void)
{
    char dir[] = "/tmp/sf33rd-store-XXXXXX";
    if (!mkdtemp(dir)) {
        perror("mkdtemp");
        return 1;
    }
    test_game(dir);
    test_host(dir);
    test_garbage(dir);
    rm(dir, "missing.sqlite");
    rm(dir, "game.sqlite");
    rm(dir, "game.sqlite-journal");
    rm(dir, "host.sqlite");
    rm(dir, "host.sqlite-journal");
    rm(dir, "garbage");
    rm(dir, "garbage-journal");
    if (rmdir(dir) != 0) {
        perror(dir);
        s_fail = 1;
    }
    return s_fail ? 1 : 0;
}
