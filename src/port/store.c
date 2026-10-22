/* SQLite for the game's saves and the host's settings.
 *
 * game.sqlite: one system row, one directions row, replay slots 0 to 9.
 * host.sqlite: one window row, one row per key. Schema version is 1.
 * A write is a transaction with synchronous=FULL. A file that fails
 * integrity_check is left alone.
 *
 * Not stored: the memory-card header, sega_reserve, free_free, the network
 * record, lag, the pad's free bytes, and fname. A load leaves them zero. */

#include "port/store.h"

#include <sqlite3.h>

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

#define STORE_VERSION 1
#define INPUT_FRAMES 7198
#define REPLAY_SLOTS 10

_Static_assert(sizeof(((const _REPLAY_W *)0)->io_unit.key_buff[0]) / sizeof(u16) == INPUT_FRAMES,
               "replay input frames");

static const char k_game_sql[] =
    "CREATE TABLE IF NOT EXISTS system ("
    " id INTEGER PRIMARY KEY CHECK (id = 1),"
    " difficulty INTEGER NOT NULL, time_limit INTEGER NOT NULL,"
    " battle0 INTEGER NOT NULL, battle1 INTEGER NOT NULL,"
    " damage_level INTEGER NOT NULL, handicap INTEGER NOT NULL,"
    " partner0 INTEGER NOT NULL, partner1 INTEGER NOT NULL,"
    " adjust_x INTEGER NOT NULL, adjust_y INTEGER NOT NULL,"
    " screen_size INTEGER NOT NULL, screen_mode INTEGER NOT NULL,"
    " guard_check INTEGER NOT NULL, auto_save INTEGER NOT NULL,"
    " analog_stick INTEGER NOT NULL, bgm_type INTEGER NOT NULL,"
    " sound_mode INTEGER NOT NULL, bgm_level INTEGER NOT NULL,"
    " se_level INTEGER NOT NULL, extra_option INTEGER NOT NULL,"
    " sum INTEGER NOT NULL,"
    " pad0_vibration INTEGER NOT NULL, pad1_vibration INTEGER NOT NULL,"
    " pad0_shot BLOB NOT NULL, pad1_shot BLOB NOT NULL,"
    " color0 BLOB NOT NULL, color1 BLOB NOT NULL,"
    " extra BLOB NOT NULL);"
    "CREATE TABLE IF NOT EXISTS rank ("
    " idx INTEGER PRIMARY KEY CHECK (idx >= 0 AND idx < 20),"
    " name BLOB NOT NULL, player INTEGER NOT NULL, score INTEGER NOT NULL,"
    " cpu_grade INTEGER NOT NULL, grade INTEGER NOT NULL,"
    " wins INTEGER NOT NULL, player_color INTEGER NOT NULL,"
    " all_clear INTEGER NOT NULL);"
    "CREATE TABLE IF NOT EXISTS directions ("
    " id INTEGER PRIMARY KEY CHECK (id = 1),"
    " rows BLOB NOT NULL, sum INTEGER NOT NULL);"
    "CREATE TABLE IF NOT EXISTS replay ("
    " slot INTEGER PRIMARY KEY CHECK (slot >= 0 AND slot < 10),"
    " saved_at INTEGER NOT NULL,"
    " control_time INTEGER NOT NULL, difficulty INTEGER NOT NULL,"
    " monitor_type INTEGER NOT NULL, champion INTEGER NOT NULL,"
    " full_data INTEGER NOT NULL, sum INTEGER NOT NULL,"
    " p0_char INTEGER NOT NULL, p0_sa INTEGER NOT NULL,"
    " p0_color INTEGER NOT NULL, p0_type INTEGER NOT NULL,"
    " p1_char INTEGER NOT NULL, p1_sa INTEGER NOT NULL,"
    " p1_color INTEGER NOT NULL, p1_type INTEGER NOT NULL,"
    " stage INTEGER NOT NULL, direction_working INTEGER NOT NULL,"
    " vital0 INTEGER NOT NULL, vital1 INTEGER NOT NULL,"
    " random_ix16 INTEGER NOT NULL, random_ix32 INTEGER NOT NULL,"
    " random_ix16_ex INTEGER NOT NULL, random_ix32_ex INTEGER NOT NULL,"
    " winner INTEGER NOT NULL, play_type INTEGER NOT NULL,"
    " players_timer INTEGER NOT NULL,"
    " old_mes_no2 INTEGER NOT NULL, old_mes_no3 INTEGER NOT NULL,"
    " old_mes_no_pl INTEGER NOT NULL, mes_already INTEGER NOT NULL,"
    " mini_time INTEGER NOT NULL,"
    " mini_battle0 INTEGER NOT NULL, mini_battle1 INTEGER NOT NULL,"
    " mini_damage INTEGER NOT NULL,"
    " mini_pad0_vib INTEGER NOT NULL, mini_pad1_vib INTEGER NOT NULL,"
    " mini_pad0_shot BLOB NOT NULL, mini_pad1_shot BLOB NOT NULL,"
    " mini_extra BLOB NOT NULL,"
    " directions BLOB NOT NULL, directions_sum INTEGER NOT NULL,"
    " inputs BLOB NOT NULL);";

static const char k_host_sql[] =
    "CREATE TABLE IF NOT EXISTS window ("
    " id INTEGER PRIMARY KEY CHECK (id = 1),"
    " mode TEXT NOT NULL, width INTEGER NOT NULL,"
    " height INTEGER NOT NULL, aspect TEXT NOT NULL);"
    "CREATE TABLE IF NOT EXISTS key ("
    " name TEXT PRIMARY KEY, scancode TEXT NOT NULL);"
    "CREATE TABLE IF NOT EXISTS setting ("
    " name TEXT PRIMARY KEY, value TEXT NOT NULL);";

static int file_exists(const char *path)
{
    struct stat st;
    return stat(path, &st) == 0;
}

static int exec_sql(sqlite3 *db, const char *sql)
{
    char *err = NULL;
    int rc = sqlite3_exec(db, sql, NULL, NULL, &err);
    if (rc != SQLITE_OK) {
        fprintf(stderr, "save: %s\n", err ? err : sqlite3_errmsg(db));
        sqlite3_free(err);
        return -1;
    }
    return 0;
}

static int user_version(sqlite3 *db, int *ver)
{
    sqlite3_stmt *st = NULL;
    int rc;
    *ver = -1;
    if (sqlite3_prepare_v2(db, "PRAGMA user_version", -1, &st, NULL) != SQLITE_OK) {
        return -1;
    }
    rc = sqlite3_step(st);
    if (rc == SQLITE_ROW) {
        *ver = sqlite3_column_int(st, 0);
    }
    sqlite3_finalize(st);
    return rc == SQLITE_ROW ? 0 : -1;
}

static int integrity_ok(sqlite3 *db)
{
    sqlite3_stmt *st = NULL;
    const char *msg;
    int rc;
    int ok = 0;
    if (sqlite3_prepare_v2(db, "PRAGMA integrity_check", -1, &st, NULL) != SQLITE_OK) {
        return 0;
    }
    rc = sqlite3_step(st);
    msg = rc == SQLITE_ROW ? (const char *)sqlite3_column_text(st, 0) : NULL;
    ok = msg && strcmp(msg, "ok") == 0;
    sqlite3_finalize(st);
    return ok;
}

/* 0 open, 1 no save yet, -1 damaged. writing creates a missing file. */
static int open_db(const char *path, sqlite3 **out, int writing)
{
    int exists;
    int flags;
    int ver = 0;
    int rc;
    sqlite3 *db = NULL;
    if (!path || !out) {
        return -1;
    }
    exists = file_exists(path);
    if (!exists && !writing) {
        return 1;
    }
    flags = SQLITE_OPEN_READWRITE;
    if (!exists) {
        flags |= SQLITE_OPEN_CREATE;
    }
    if (sqlite3_open_v2(path, &db, flags, NULL) != SQLITE_OK) {
        fprintf(stderr, "save: %s: %s\n", path, db ? sqlite3_errmsg(db) : "open failed");
        sqlite3_close(db);
        return -1;
    }
    sqlite3_busy_timeout(db, 2000);
    if (exec_sql(db, "PRAGMA journal_mode=DELETE") != 0 || exec_sql(db, "PRAGMA synchronous=FULL") != 0) {
        sqlite3_close(db);
        return -1;
    }
    if (exists && !integrity_ok(db)) {
        fprintf(stderr, "save: %s is damaged; not loaded\n", path);
        sqlite3_close(db);
        return -1;
    }
    if (user_version(db, &ver) != 0) {
        sqlite3_close(db);
        return -1;
    }
    if (ver != 0 && ver != STORE_VERSION) {
        fprintf(stderr, "save: %s is damaged; not loaded\n", path);
        sqlite3_close(db);
        return -1;
    }
    if (!writing && ver == 0) {
        sqlite3_close(db);
        return 1;
    }
    if (writing && ver == 0) {
        rc = exec_sql(db, "PRAGMA user_version=1");
        if (rc != 0) {
            sqlite3_close(db);
            return -1;
        }
    }
    *out = db;
    return 0;
}

static int ensure(sqlite3 *db, const char *sql)
{
    return exec_sql(db, sql);
}

static int begin(sqlite3 *db)
{
    return exec_sql(db, "BEGIN IMMEDIATE");
}

static int commit(sqlite3 *db)
{
    return exec_sql(db, "COMMIT");
}

static void rollback(sqlite3 *db)
{
    sqlite3_exec(db, "ROLLBACK", NULL, NULL, NULL);
}

static int bind_i(sqlite3_stmt *st, int i, long long v)
{
    return sqlite3_bind_int64(st, i, v) == SQLITE_OK ? 0 : -1;
}

static int bind_blob(sqlite3_stmt *st, int i, const void *p, int n)
{
    const void *src = p ? p : "";
    if (n < 0) {
        return -1;
    }
    return sqlite3_bind_blob(st, i, src, n, SQLITE_TRANSIENT) == SQLITE_OK ? 0 : -1;
}

static int col_i(sqlite3_stmt *st, int i, long long min, long long max, long long *out)
{
    long long v;
    if (sqlite3_column_type(st, i) != SQLITE_INTEGER) {
        return -1;
    }
    v = sqlite3_column_int64(st, i);
    if (v < min || v > max) {
        return -1;
    }
    *out = v;
    return 0;
}

static int col_blob(sqlite3_stmt *st, int i, void *dest, int n)
{
    const void *p;
    if (n < 0 || sqlite3_column_bytes(st, i) != n) {
        return -1;
    }
    if (n == 0) {
        return 0;
    }
    if (sqlite3_column_type(st, i) != SQLITE_BLOB) {
        return -1;
    }
    p = sqlite3_column_blob(st, i);
    if (!p) {
        return -1;
    }
    memcpy(dest, p, (size_t)n);
    return 0;
}

static int step_row(sqlite3_stmt *st)
{
    int rc = sqlite3_step(st);
    if (rc == SQLITE_ROW) {
        return 0;
    }
    if (rc == SQLITE_DONE) {
        return 1;
    }
    return -1;
}

static int prepare(sqlite3 *db, const char *sql, sqlite3_stmt **st)
{
    if (sqlite3_prepare_v2(db, sql, -1, st, NULL) != SQLITE_OK) {
        fprintf(stderr, "save: %s\n", sqlite3_errmsg(db));
        return -1;
    }
    return 0;
}

static const char k_system_ins[] =
    "INSERT OR REPLACE INTO system ("
    "id, difficulty, time_limit, battle0, battle1, damage_level, handicap,"
    "partner0, partner1, adjust_x, adjust_y, screen_size, screen_mode,"
    "guard_check, auto_save, analog_stick, bgm_type, sound_mode,"
    "bgm_level, se_level, extra_option, sum,"
    "pad0_vibration, pad1_vibration, pad0_shot, pad1_shot, color0, color1, extra"
    ") VALUES (1,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)";

static const char k_rank_ins[] =
    "INSERT OR REPLACE INTO rank ("
    "idx, name, player, score, cpu_grade, grade, wins, player_color, all_clear"
    ") VALUES (?,?,?,?,?,?,?,?,?)";

static int write_system_tx(sqlite3 *db, const struct _SAVE_W *sw)
{
    sqlite3_stmt *st = NULL;
    int i;
    int rc = -1;
    if (begin(db) != 0) {
        return -1;
    }
    if (prepare(db, k_system_ins, &st) != 0) {
        rollback(db);
        return -1;
    }
    if (bind_i(st, 1, sw->Difficulty) != 0 || bind_i(st, 2, sw->Time_Limit) != 0 ||
        bind_i(st, 3, sw->Battle_Number[0]) != 0 || bind_i(st, 4, sw->Battle_Number[1]) != 0 ||
        bind_i(st, 5, sw->Damage_Level) != 0 || bind_i(st, 6, sw->Handicap) != 0 ||
        bind_i(st, 7, sw->Partner_Type[0]) != 0 || bind_i(st, 8, sw->Partner_Type[1]) != 0 ||
        bind_i(st, 9, sw->Adjust_X) != 0 || bind_i(st, 10, sw->Adjust_Y) != 0 ||
        bind_i(st, 11, sw->Screen_Size) != 0 || bind_i(st, 12, sw->Screen_Mode) != 0 ||
        bind_i(st, 13, sw->GuardCheck) != 0 || bind_i(st, 14, sw->Auto_Save) != 0 ||
        bind_i(st, 15, sw->AnalogStick) != 0 || bind_i(st, 16, sw->BgmType) != 0 ||
        bind_i(st, 17, sw->SoundMode) != 0 || bind_i(st, 18, sw->BGM_Level) != 0 ||
        bind_i(st, 19, sw->SE_Level) != 0 || bind_i(st, 20, sw->Extra_Option) != 0 ||
        bind_i(st, 21, sw->sum) != 0 || bind_i(st, 22, sw->Pad_Infor[0].Vibration) != 0 ||
        bind_i(st, 23, sw->Pad_Infor[1].Vibration) != 0 ||
        bind_blob(st, 24, sw->Pad_Infor[0].Shot, 8) != 0 || bind_blob(st, 25, sw->Pad_Infor[1].Shot, 8) != 0 ||
        bind_blob(st, 26, sw->PL_Color[0], 20) != 0 || bind_blob(st, 27, sw->PL_Color[1], 20) != 0 ||
        bind_blob(st, 28, sw->extra_option.contents, 32) != 0 || sqlite3_step(st) != SQLITE_DONE) {
        sqlite3_finalize(st);
        rollback(db);
        return -1;
    }
    sqlite3_finalize(st);
    st = NULL;
    if (prepare(db, k_rank_ins, &st) != 0) {
        rollback(db);
        return -1;
    }
    for (i = 0; i < 20; i++) {
        const RANK_DATA *r = &sw->Ranking[i];
        sqlite3_reset(st);
        sqlite3_clear_bindings(st);
        if (bind_i(st, 1, i) != 0 || bind_blob(st, 2, r->name, 3) != 0 || bind_i(st, 3, r->player) != 0 ||
            bind_i(st, 4, r->score) != 0 || bind_i(st, 5, r->cpu_grade) != 0 || bind_i(st, 6, r->grade) != 0 ||
            bind_i(st, 7, r->wins) != 0 || bind_i(st, 8, r->player_color) != 0 || bind_i(st, 9, r->all_clear) != 0 ||
            sqlite3_step(st) != SQLITE_DONE) {
            sqlite3_finalize(st);
            rollback(db);
            return -1;
        }
    }
    sqlite3_finalize(st);
    rc = commit(db);
    if (rc != 0) {
        rollback(db);
    }
    return rc;
}

static int read_ranks(sqlite3 *db, struct _SAVE_W *sw)
{
    sqlite3_stmt *st = NULL;
    int n = 0;
    int rc;
    if (prepare(db, "SELECT idx, name, player, score, cpu_grade, grade, wins, player_color, all_clear "
                    "FROM rank ORDER BY idx",
                &st) != 0) {
        return -1;
    }
    for (;;) {
        long long idx;
        long long player;
        long long score;
        long long cpu;
        long long grade;
        long long wins;
        long long color;
        long long clear;
        RANK_DATA *r;
        rc = step_row(st);
        if (rc == 1) {
            break;
        }
        if (rc != 0 || n >= 20) {
            sqlite3_finalize(st);
            return -1;
        }
        if (col_i(st, 0, 0, 19, &idx) != 0 || idx != n || col_blob(st, 1, sw->Ranking[n].name, 3) != 0 ||
            col_i(st, 2, 0, 65535, &player) != 0 || col_i(st, 3, 0, 4294967295LL, &score) != 0 ||
            col_i(st, 4, -128, 127, &cpu) != 0 || col_i(st, 5, -128, 127, &grade) != 0 ||
            col_i(st, 6, 0, 65535, &wins) != 0 || col_i(st, 7, 0, 255, &color) != 0 ||
            col_i(st, 8, 0, 255, &clear) != 0) {
            sqlite3_finalize(st);
            return -1;
        }
        r = &sw->Ranking[n];
        r->player = (u16)player;
        r->score = (u32)score;
        r->cpu_grade = (s8)cpu;
        r->grade = (s8)grade;
        r->wins = (u16)wins;
        r->player_color = (u8)color;
        r->all_clear = (u8)clear;
        n++;
    }
    sqlite3_finalize(st);
    return n == 20 ? 0 : -1;
}

int sf_store_read_system(const char *path, struct _SAVE_W *sw)
{
    sqlite3 *db = NULL;
    sqlite3_stmt *st = NULL;
    int rc;
    long long v[22];
    int i;
    if (!path || !sw) {
        return -1;
    }
    rc = open_db(path, &db, 0);
    if (rc != 0) {
        return rc;
    }
    if (ensure(db, k_game_sql) != 0) {
        sqlite3_close(db);
        return -1;
    }
    if (prepare(db, "SELECT difficulty, time_limit, battle0, battle1, damage_level, handicap,"
                    "partner0, partner1, adjust_x, adjust_y, screen_size, screen_mode,"
                    "guard_check, auto_save, analog_stick, bgm_type, sound_mode,"
                    "bgm_level, se_level, extra_option, sum, pad0_vibration, pad1_vibration,"
                    "pad0_shot, pad1_shot, color0, color1, extra FROM system WHERE id=1",
                &st) != 0) {
        sqlite3_close(db);
        return -1;
    }
    rc = step_row(st);
    if (rc != 0) {
        sqlite3_finalize(st);
        sqlite3_close(db);
        return rc;
    }
    memset(sw, 0, sizeof *sw);
    for (i = 0; i < 22; i++) {
        long long min = 0;
        long long max = 255;
        if (i == 1 || i == 8 || i == 9) {
            min = -128;
            max = 127;
        } else if (i == 20) {
            min = 0;
            max = 4294967295LL;
        }
        if (col_i(st, i, min, max, &v[i]) != 0) {
            sqlite3_finalize(st);
            sqlite3_close(db);
            return -1;
        }
    }
    sw->Difficulty = (u8)v[0];
    sw->Time_Limit = (s8)v[1];
    sw->Battle_Number[0] = (u8)v[2];
    sw->Battle_Number[1] = (u8)v[3];
    sw->Damage_Level = (u8)v[4];
    sw->Handicap = (u8)v[5];
    sw->Partner_Type[0] = (u8)v[6];
    sw->Partner_Type[1] = (u8)v[7];
    sw->Adjust_X = (s8)v[8];
    sw->Adjust_Y = (s8)v[9];
    sw->Screen_Size = (u8)v[10];
    sw->Screen_Mode = (u8)v[11];
    sw->GuardCheck = (u8)v[12];
    sw->Auto_Save = (u8)v[13];
    sw->AnalogStick = (u8)v[14];
    sw->BgmType = (u8)v[15];
    sw->SoundMode = (u8)v[16];
    sw->BGM_Level = (u8)v[17];
    sw->SE_Level = (u8)v[18];
    sw->Extra_Option = (u8)v[19];
    sw->sum = (u32)v[20];
    sw->Pad_Infor[0].Vibration = (u8)v[21];
    if (col_i(st, 22, 0, 255, &v[0]) != 0) {
        sqlite3_finalize(st);
        sqlite3_close(db);
        return -1;
    }
    sw->Pad_Infor[1].Vibration = (u8)v[0];
    if (col_blob(st, 23, sw->Pad_Infor[0].Shot, 8) != 0 || col_blob(st, 24, sw->Pad_Infor[1].Shot, 8) != 0 ||
        col_blob(st, 25, sw->PL_Color[0], 20) != 0 || col_blob(st, 26, sw->PL_Color[1], 20) != 0 ||
        col_blob(st, 27, sw->extra_option.contents, 32) != 0) {
        sqlite3_finalize(st);
        sqlite3_close(db);
        return -1;
    }
    sqlite3_finalize(st);
    rc = read_ranks(db, sw);
    sqlite3_close(db);
    return rc;
}

int sf_store_write_system(const char *path, const struct _SAVE_W *sw)
{
    sqlite3 *db = NULL;
    int rc;
    if (!path || !sw) {
        return -1;
    }
    rc = open_db(path, &db, 1);
    if (rc != 0) {
        return rc;
    }
    if (ensure(db, k_game_sql) != 0) {
        sqlite3_close(db);
        return -1;
    }
    rc = write_system_tx(db, sw);
    sqlite3_close(db);
    if (rc != 0) {
        fprintf(stderr, "save: writing %s failed\n", path);
    }
    return rc;
}

int sf_store_read_directions(const char *path, SystemDir *dir)
{
    sqlite3 *db = NULL;
    sqlite3_stmt *st = NULL;
    long long sum;
    int rc;
    if (!path || !dir) {
        return -1;
    }
    rc = open_db(path, &db, 0);
    if (rc != 0) {
        return rc;
    }
    if (ensure(db, k_game_sql) != 0) {
        sqlite3_close(db);
        return -1;
    }
    if (prepare(db, "SELECT rows, sum FROM directions WHERE id=1", &st) != 0) {
        sqlite3_close(db);
        return -1;
    }
    rc = step_row(st);
    if (rc != 0) {
        sqlite3_finalize(st);
        sqlite3_close(db);
        return rc;
    }
    memset(dir, 0, sizeof *dir);
    if (col_blob(st, 0, dir->contents, 70) != 0 || col_i(st, 1, 0, 65535, &sum) != 0) {
        sqlite3_finalize(st);
        sqlite3_close(db);
        return -1;
    }
    dir->sum = (u16)sum;
    sqlite3_finalize(st);
    sqlite3_close(db);
    return 0;
}

int sf_store_write_directions(const char *path, const SystemDir *dir)
{
    sqlite3 *db = NULL;
    sqlite3_stmt *st = NULL;
    int rc;
    if (!path || !dir) {
        return -1;
    }
    rc = open_db(path, &db, 1);
    if (rc != 0) {
        return rc;
    }
    if (ensure(db, k_game_sql) != 0 || begin(db) != 0) {
        sqlite3_close(db);
        return -1;
    }
    if (prepare(db, "INSERT OR REPLACE INTO directions (id, rows, sum) VALUES (1, ?, ?)", &st) != 0 ||
        bind_blob(st, 1, dir->contents, 70) != 0 || bind_i(st, 2, dir->sum) != 0 || sqlite3_step(st) != SQLITE_DONE) {
        sqlite3_finalize(st);
        rollback(db);
        sqlite3_close(db);
        fprintf(stderr, "save: writing %s failed\n", path);
        return -1;
    }
    sqlite3_finalize(st);
    rc = commit(db);
    sqlite3_close(db);
    if (rc != 0) {
        fprintf(stderr, "save: writing %s failed\n", path);
    }
    return rc;
}

static int pack_inputs(const _REPLAY_W *w, u8 **out, int *n_out)
{
    int last;
    int i;
    int frames;
    u8 *buf;
    u16 a;
    u16 b;
    last = -1;
    for (i = INPUT_FRAMES - 1; i >= 0; i--) {
        if (w->io_unit.key_buff[0][i] != 0 || w->io_unit.key_buff[1][i] != 0) {
            last = i;
            break;
        }
    }
    frames = last + 1;
    *n_out = frames * 4;
    *out = NULL;
    if (frames == 0) {
        return 0;
    }
    buf = malloc((size_t)frames * 4);
    if (!buf) {
        return -1;
    }
    for (i = 0; i < frames; i++) {
        a = w->io_unit.key_buff[0][i];
        b = w->io_unit.key_buff[1][i];
        buf[i * 4] = (u8)a;
        buf[i * 4 + 1] = (u8)(a >> 8);
        buf[i * 4 + 2] = (u8)b;
        buf[i * 4 + 3] = (u8)(b >> 8);
    }
    *out = buf;
    return 0;
}

static int take_inputs(sqlite3_stmt *st, int col, _REPLAY_W *w)
{
    int got;
    int frames;
    int i;
    const u8 *p;
    got = sqlite3_column_bytes(st, col);
    if (got < 0 || (got % 4) != 0) {
        return -1;
    }
    frames = got / 4;
    if (frames > INPUT_FRAMES) {
        return -1;
    }
    if (frames == 0) {
        return 0;
    }
    if (sqlite3_column_type(st, col) != SQLITE_BLOB) {
        return -1;
    }
    p = sqlite3_column_blob(st, col);
    if (!p) {
        return -1;
    }
    for (i = 0; i < frames; i++) {
        w->io_unit.key_buff[0][i] = (u16)(p[i * 4] | (p[i * 4 + 1] << 8));
        w->io_unit.key_buff[1][i] = (u16)(p[i * 4 + 2] | (p[i * 4 + 3] << 8));
    }
    return 0;
}

static const char k_replay_ins[] =
    "INSERT OR REPLACE INTO replay ("
    "slot, saved_at, control_time, difficulty, monitor_type, champion, full_data, sum,"
    "p0_char, p0_sa, p0_color, p0_type, p1_char, p1_sa, p1_color, p1_type,"
    "stage, direction_working, vital0, vital1,"
    "random_ix16, random_ix32, random_ix16_ex, random_ix32_ex,"
    "winner, play_type, players_timer, old_mes_no2, old_mes_no3, old_mes_no_pl, mes_already,"
    "mini_time, mini_battle0, mini_battle1, mini_damage, mini_pad0_vib, mini_pad1_vib,"
    "mini_pad0_shot, mini_pad1_shot, mini_extra, directions, directions_sum, inputs"
    ") VALUES (?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)";

int sf_store_write_replay(const char *path, int slot, const _REPLAY_W *w)
{
    sqlite3 *db = NULL;
    sqlite3_stmt *st = NULL;
    u8 *inputs = NULL;
    int n_in = 0;
    int rc;
    const struct _REP_GAME_INFOR *g;
    const struct _MINI_SAVE_W *m;
    if (!path || !w || slot < 0 || slot >= REPLAY_SLOTS) {
        return -1;
    }
    if (pack_inputs(w, &inputs, &n_in) != 0) {
        return -1;
    }
    rc = open_db(path, &db, 1);
    if (rc != 0) {
        free(inputs);
        return rc;
    }
    g = &w->game_infor;
    m = &w->mini_save_w;
    if (ensure(db, k_game_sql) != 0 || begin(db) != 0 || prepare(db, k_replay_ins, &st) != 0) {
        free(inputs);
        sqlite3_close(db);
        return -1;
    }
    if (bind_i(st, 1, slot) != 0 || bind_i(st, 2, (long long)time(NULL)) != 0 ||
        bind_i(st, 3, w->Control_Time_Buff) != 0 || bind_i(st, 4, w->Difficulty) != 0 ||
        bind_i(st, 5, w->Monitor_Type) != 0 || bind_i(st, 6, w->champion) != 0 || bind_i(st, 7, w->full_data) != 0 ||
        bind_i(st, 8, w->sum) != 0 || bind_i(st, 9, g->player_infor[0].my_char) != 0 ||
        bind_i(st, 10, g->player_infor[0].sa) != 0 || bind_i(st, 11, g->player_infor[0].color) != 0 ||
        bind_i(st, 12, g->player_infor[0].player_type) != 0 || bind_i(st, 13, g->player_infor[1].my_char) != 0 ||
        bind_i(st, 14, g->player_infor[1].sa) != 0 || bind_i(st, 15, g->player_infor[1].color) != 0 ||
        bind_i(st, 16, g->player_infor[1].player_type) != 0 || bind_i(st, 17, g->stage) != 0 ||
        bind_i(st, 18, g->Direction_Working) != 0 || bind_i(st, 19, g->Vital_Handicap[0]) != 0 ||
        bind_i(st, 20, g->Vital_Handicap[1]) != 0 || bind_i(st, 21, g->Random_ix16) != 0 ||
        bind_i(st, 22, g->Random_ix32) != 0 || bind_i(st, 23, g->Random_ix16_ex) != 0 ||
        bind_i(st, 24, g->Random_ix32_ex) != 0 || bind_i(st, 25, g->winner) != 0 || bind_i(st, 26, g->play_type) != 0 ||
        bind_i(st, 27, g->players_timer) != 0 || bind_i(st, 28, g->old_mes_no2) != 0 ||
        bind_i(st, 29, g->old_mes_no3) != 0 || bind_i(st, 30, g->old_mes_no_pl) != 0 ||
        bind_i(st, 31, g->mes_already) != 0 || bind_i(st, 32, m->Time_Limit) != 0 ||
        bind_i(st, 33, m->Battle_Number[0]) != 0 || bind_i(st, 34, m->Battle_Number[1]) != 0 ||
        bind_i(st, 35, m->Damage_Level) != 0 || bind_i(st, 36, m->Pad_Infor[0].Vibration) != 0 ||
        bind_i(st, 37, m->Pad_Infor[1].Vibration) != 0 || bind_blob(st, 38, m->Pad_Infor[0].Shot, 8) != 0 ||
        bind_blob(st, 39, m->Pad_Infor[1].Shot, 8) != 0 || bind_blob(st, 40, m->extra_option.contents, 32) != 0 ||
        bind_blob(st, 41, w->system_dir.contents, 70) != 0 || bind_i(st, 42, w->system_dir.sum) != 0 ||
        bind_blob(st, 43, inputs, n_in) != 0 || sqlite3_step(st) != SQLITE_DONE) {
        sqlite3_finalize(st);
        rollback(db);
        sqlite3_close(db);
        free(inputs);
        fprintf(stderr, "save: writing %s failed\n", path);
        return -1;
    }
    sqlite3_finalize(st);
    free(inputs);
    rc = commit(db);
    sqlite3_close(db);
    if (rc != 0) {
        fprintf(stderr, "save: writing %s failed\n", path);
    }
    return rc;
}

int sf_store_read_replay(const char *path, int slot, _REPLAY_W *w, long long *saved_at)
{
    sqlite3 *db = NULL;
    sqlite3_stmt *st = NULL;
    int rc;
    int i;
    long long v;
    struct _REP_GAME_INFOR *g;
    struct _MINI_SAVE_W *m;
    /* Columns after saved_at: control_time through mes_already. */
    static const long long k_min[29] = { -32768, 0, 0, 0, 0, 0, 0, -128, -128, -128, 0, -128, -128, -128, -128, -128,
                                         -128, -128, -32768, -32768, -32768, -32768, 0, 0, 0, -32768, -32768, -32768,
                                         -32768 };
    static const long long k_max[29] = { 32767, 255, 255, 255, 255, 4294967295LL, 255, 127, 127, 127, 255, 127, 127,
                                         127, 127, 127, 127, 127, 32767, 32767, 32767, 32767, 255, 255, 65535, 32767,
                                         32767, 32767, 32767 };
    if (!path || !w || slot < 0 || slot >= REPLAY_SLOTS) {
        return -1;
    }
    rc = open_db(path, &db, 0);
    if (rc != 0) {
        return rc;
    }
    if (ensure(db, k_game_sql) != 0) {
        sqlite3_close(db);
        return -1;
    }
    if (prepare(db, "SELECT saved_at, control_time, difficulty, monitor_type, champion, full_data, sum,"
                    "p0_char, p0_sa, p0_color, p0_type, p1_char, p1_sa, p1_color, p1_type,"
                    "stage, direction_working, vital0, vital1,"
                    "random_ix16, random_ix32, random_ix16_ex, random_ix32_ex,"
                    "winner, play_type, players_timer, old_mes_no2, old_mes_no3, old_mes_no_pl, mes_already,"
                    "mini_time, mini_battle0, mini_battle1, mini_damage, mini_pad0_vib, mini_pad1_vib,"
                    "mini_pad0_shot, mini_pad1_shot, mini_extra, directions, directions_sum, inputs "
                    "FROM replay WHERE slot=?",
                &st) != 0 || bind_i(st, 1, slot) != 0) {
        sqlite3_finalize(st);
        sqlite3_close(db);
        return -1;
    }
    rc = step_row(st);
    if (rc != 0) {
        sqlite3_finalize(st);
        sqlite3_close(db);
        return rc;
    }
    memset(w, 0, sizeof *w);
    w->game_infor.fname = NULL;
    if (col_i(st, 0, 0, 4294967295LL, &v) != 0) {
        sqlite3_finalize(st);
        sqlite3_close(db);
        return -1;
    }
    if (saved_at) {
        *saved_at = v;
    }
    /* columns 1..29 are the integers between saved_at and the blobs */
    g = &w->game_infor;
    m = &w->mini_save_w;
    for (i = 0; i < 29; i++) {
        if (col_i(st, i + 1, k_min[i], k_max[i], &v) != 0) {
            sqlite3_finalize(st);
            sqlite3_close(db);
            return -1;
        }
        switch (i) {
        case 0:
            w->Control_Time_Buff = (s16)v;
            break;
        case 1:
            w->Difficulty = (u8)v;
            break;
        case 2:
            w->Monitor_Type = (u8)v;
            break;
        case 3:
            w->champion = (u8)v;
            break;
        case 4:
            w->full_data = (u8)v;
            break;
        case 5:
            w->sum = (u32)v;
            break;
        case 6:
            g->player_infor[0].my_char = (u8)v;
            break;
        case 7:
            g->player_infor[0].sa = (s8)v;
            break;
        case 8:
            g->player_infor[0].color = (s8)v;
            break;
        case 9:
            g->player_infor[0].player_type = (s8)v;
            break;
        case 10:
            g->player_infor[1].my_char = (u8)v;
            break;
        case 11:
            g->player_infor[1].sa = (s8)v;
            break;
        case 12:
            g->player_infor[1].color = (s8)v;
            break;
        case 13:
            g->player_infor[1].player_type = (s8)v;
            break;
        case 14:
            g->stage = (s8)v;
            break;
        case 15:
            g->Direction_Working = (s8)v;
            break;
        case 16:
            g->Vital_Handicap[0] = (s8)v;
            break;
        case 17:
            g->Vital_Handicap[1] = (s8)v;
            break;
        case 18:
            g->Random_ix16 = (s16)v;
            break;
        case 19:
            g->Random_ix32 = (s16)v;
            break;
        case 20:
            g->Random_ix16_ex = (s16)v;
            break;
        case 21:
            g->Random_ix32_ex = (s16)v;
            break;
        case 22:
            g->winner = (u8)v;
            break;
        case 23:
            g->play_type = (u8)v;
            break;
        case 24:
            g->players_timer = (u16)v;
            break;
        case 25:
            g->old_mes_no2 = (s16)v;
            break;
        case 26:
            g->old_mes_no3 = (s16)v;
            break;
        case 27:
            g->old_mes_no_pl = (s16)v;
            break;
        default:
            g->mes_already = (s16)v;
            break;
        }
    }
    if (col_i(st, 30, -128, 127, &v) != 0) {
        sqlite3_finalize(st);
        sqlite3_close(db);
        return -1;
    }
    m->Time_Limit = (s8)v;
    if (col_i(st, 31, 0, 255, &v) != 0) {
        sqlite3_finalize(st);
        sqlite3_close(db);
        return -1;
    }
    m->Battle_Number[0] = (u8)v;
    if (col_i(st, 32, 0, 255, &v) != 0) {
        sqlite3_finalize(st);
        sqlite3_close(db);
        return -1;
    }
    m->Battle_Number[1] = (u8)v;
    if (col_i(st, 33, 0, 255, &v) != 0) {
        sqlite3_finalize(st);
        sqlite3_close(db);
        return -1;
    }
    m->Damage_Level = (u8)v;
    if (col_i(st, 34, 0, 255, &v) != 0) {
        sqlite3_finalize(st);
        sqlite3_close(db);
        return -1;
    }
    m->Pad_Infor[0].Vibration = (u8)v;
    if (col_i(st, 35, 0, 255, &v) != 0) {
        sqlite3_finalize(st);
        sqlite3_close(db);
        return -1;
    }
    m->Pad_Infor[1].Vibration = (u8)v;
    if (col_blob(st, 36, m->Pad_Infor[0].Shot, 8) != 0 || col_blob(st, 37, m->Pad_Infor[1].Shot, 8) != 0 ||
        col_blob(st, 38, m->extra_option.contents, 32) != 0 || col_blob(st, 39, w->system_dir.contents, 70) != 0 ||
        col_i(st, 40, 0, 65535, &v) != 0 || take_inputs(st, 41, w) != 0) {
        sqlite3_finalize(st);
        sqlite3_close(db);
        return -1;
    }
    w->system_dir.sum = (u16)v;
    sqlite3_finalize(st);
    sqlite3_close(db);
    return 0;
}

int sf_store_read_window(const char *path, char *mode, unsigned mode_n, int *width, int *height, char *aspect,
                         unsigned aspect_n)
{
    sqlite3 *db = NULL;
    sqlite3_stmt *st = NULL;
    const char *ms;
    const char *as;
    long long w;
    long long h;
    int rc;
    unsigned n;
    if (!path || !mode || mode_n < 2 || !width || !height || !aspect || aspect_n < 2) {
        return -1;
    }
    rc = open_db(path, &db, 0);
    if (rc != 0) {
        return rc;
    }
    if (ensure(db, k_host_sql) != 0) {
        sqlite3_close(db);
        return -1;
    }
    if (prepare(db, "SELECT mode, width, height, aspect FROM window WHERE id=1", &st) != 0) {
        sqlite3_close(db);
        return -1;
    }
    rc = step_row(st);
    if (rc != 0) {
        sqlite3_finalize(st);
        sqlite3_close(db);
        return rc;
    }
    ms = (const char *)sqlite3_column_text(st, 0);
    as = (const char *)sqlite3_column_text(st, 3);
    if (!ms || !as || col_i(st, 1, 320, 100000, &w) != 0 || col_i(st, 2, 240, 100000, &h) != 0) {
        sqlite3_finalize(st);
        sqlite3_close(db);
        return -1;
    }
    n = (unsigned)strlen(ms);
    if (n == 0 || n >= mode_n || strlen(as) == 0 || strlen(as) >= aspect_n) {
        sqlite3_finalize(st);
        sqlite3_close(db);
        return -1;
    }
    memcpy(mode, ms, n + 1);
    memcpy(aspect, as, strlen(as) + 1);
    *width = (int)w;
    *height = (int)h;
    sqlite3_finalize(st);
    sqlite3_close(db);
    return 0;
}

int sf_store_write_window(const char *path, const char *mode, int width, int height, const char *aspect)
{
    sqlite3 *db = NULL;
    sqlite3_stmt *st = NULL;
    int rc;
    if (!path || !mode || !mode[0] || !aspect || !aspect[0] || width < 320 || width > 100000 || height < 240 ||
        height > 100000) {
        return -1;
    }
    rc = open_db(path, &db, 1);
    if (rc != 0) {
        return rc;
    }
    if (ensure(db, k_host_sql) != 0 || begin(db) != 0 ||
        prepare(db, "INSERT OR REPLACE INTO window (id, mode, width, height, aspect) VALUES (1, ?, ?, ?, ?)", &st) !=
            0 ||
        sqlite3_bind_text(st, 1, mode, -1, SQLITE_TRANSIENT) != SQLITE_OK || bind_i(st, 2, width) != 0 ||
        bind_i(st, 3, height) != 0 || sqlite3_bind_text(st, 4, aspect, -1, SQLITE_TRANSIENT) != SQLITE_OK ||
        sqlite3_step(st) != SQLITE_DONE) {
        sqlite3_finalize(st);
        rollback(db);
        sqlite3_close(db);
        fprintf(stderr, "save: writing %s failed\n", path);
        return -1;
    }
    sqlite3_finalize(st);
    rc = commit(db);
    sqlite3_close(db);
    return rc;
}

int sf_store_read_keys(const char *path, const char *const *names, char values[][64], int n)
{
    sqlite3 *db = NULL;
    sqlite3_stmt *count = NULL;
    sqlite3_stmt *st = NULL;
    long long rows = 0;
    const char *text;
    int i;
    int rc;
    unsigned nlen;
    if (!path || !names || !values || n < 0) {
        return -1;
    }
    rc = open_db(path, &db, 0);
    if (rc != 0) {
        return rc;
    }
    if (ensure(db, k_host_sql) != 0 || prepare(db, "SELECT COUNT(*) FROM key", &count) != 0) {
        sqlite3_close(db);
        return -1;
    }
    if (step_row(count) != 0 || col_i(count, 0, 0, 100000, &rows) != 0) {
        sqlite3_finalize(count);
        sqlite3_close(db);
        return -1;
    }
    sqlite3_finalize(count);
    if (rows == 0) {
        sqlite3_close(db);
        return 1;
    }
    for (i = 0; i < n; i++) {
        values[i][0] = 0;
    }
    if (prepare(db, "SELECT scancode FROM key WHERE name=?", &st) != 0) {
        sqlite3_close(db);
        return -1;
    }
    for (i = 0; i < n; i++) {
        sqlite3_reset(st);
        sqlite3_clear_bindings(st);
        if (!names[i] || sqlite3_bind_text(st, 1, names[i], -1, SQLITE_TRANSIENT) != SQLITE_OK) {
            sqlite3_finalize(st);
            sqlite3_close(db);
            return -1;
        }
        rc = step_row(st);
        if (rc == 1) {
            continue;
        }
        if (rc != 0) {
            sqlite3_finalize(st);
            sqlite3_close(db);
            return -1;
        }
        text = (const char *)sqlite3_column_text(st, 0);
        if (!text) {
            sqlite3_finalize(st);
            sqlite3_close(db);
            return -1;
        }
        nlen = (unsigned)strlen(text);
        if (nlen >= 64) {
            sqlite3_finalize(st);
            sqlite3_close(db);
            return -1;
        }
        memcpy(values[i], text, nlen + 1);
    }
    sqlite3_finalize(st);
    sqlite3_close(db);
    return 0;
}

int sf_store_write_keys(const char *path, const char *const *names, const char *const *values, int n)
{
    sqlite3 *db = NULL;
    sqlite3_stmt *st = NULL;
    int i;
    int rc;
    if (!path || !names || !values || n < 0) {
        return -1;
    }
    rc = open_db(path, &db, 1);
    if (rc != 0) {
        return rc;
    }
    if (ensure(db, k_host_sql) != 0 || begin(db) != 0 || exec_sql(db, "DELETE FROM key") != 0 ||
        prepare(db, "INSERT INTO key (name, scancode) VALUES (?, ?)", &st) != 0) {
        rollback(db);
        sqlite3_close(db);
        fprintf(stderr, "save: writing %s failed\n", path);
        return -1;
    }
    for (i = 0; i < n; i++) {
        sqlite3_reset(st);
        sqlite3_clear_bindings(st);
        if (!names[i] || !values[i] || sqlite3_bind_text(st, 1, names[i], -1, SQLITE_TRANSIENT) != SQLITE_OK ||
            sqlite3_bind_text(st, 2, values[i], -1, SQLITE_TRANSIENT) != SQLITE_OK || sqlite3_step(st) != SQLITE_DONE) {
            sqlite3_finalize(st);
            rollback(db);
            sqlite3_close(db);
            fprintf(stderr, "save: writing %s failed\n", path);
            return -1;
        }
    }
    sqlite3_finalize(st);
    rc = commit(db);
    sqlite3_close(db);
    if (rc != 0) {
        fprintf(stderr, "save: writing %s failed\n", path);
    }
    return rc;
}

/* A setting of the port's (config/host.sqlite's setting table): 0 with
 * its value, 1 when it has no row. */
int sf_store_read_setting(const char *path, const char *name, char *value, unsigned value_n)
{
    sqlite3 *db = NULL;
    sqlite3_stmt *st = NULL;
    const unsigned char *text;
    int rc;
    if (!path || !name || !value || value_n == 0) {
        return -1;
    }
    rc = open_db(path, &db, 0);
    if (rc != 0) {
        return rc;
    }
    if (ensure(db, k_host_sql) != 0 || prepare(db, "SELECT value FROM setting WHERE name=?", &st) != 0 ||
        sqlite3_bind_text(st, 1, name, -1, SQLITE_TRANSIENT) != SQLITE_OK) {
        sqlite3_finalize(st);
        sqlite3_close(db);
        return -1;
    }
    rc = sqlite3_step(st);
    if (rc != SQLITE_ROW) {
        sqlite3_finalize(st);
        sqlite3_close(db);
        return rc == SQLITE_DONE ? 1 : -1;
    }
    text = sqlite3_column_text(st, 0);
    snprintf(value, value_n, "%s", text ? (const char *)text : "");
    sqlite3_finalize(st);
    sqlite3_close(db);
    return 0;
}

int sf_store_write_setting(const char *path, const char *name, const char *value)
{
    sqlite3 *db = NULL;
    sqlite3_stmt *st = NULL;
    int rc;
    if (!path || !name || !value) {
        return -1;
    }
    rc = open_db(path, &db, 1);
    if (rc != 0) {
        return rc;
    }
    if (ensure(db, k_host_sql) != 0 || begin(db) != 0 ||
        prepare(db, "INSERT OR REPLACE INTO setting (name, value) VALUES (?, ?)", &st) != 0 ||
        sqlite3_bind_text(st, 1, name, -1, SQLITE_TRANSIENT) != SQLITE_OK ||
        sqlite3_bind_text(st, 2, value, -1, SQLITE_TRANSIENT) != SQLITE_OK || sqlite3_step(st) != SQLITE_DONE) {
        sqlite3_finalize(st);
        rollback(db);
        sqlite3_close(db);
        fprintf(stderr, "save: writing %s failed\n", path);
        return -1;
    }
    sqlite3_finalize(st);
    rc = commit(db);
    sqlite3_close(db);
    return rc;
}
