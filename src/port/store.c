/* Named, versioned text for the game's saves and the host's settings.
 *
 * The game still keeps its settings in the structs it always used. A file
 * is one "key=value" per line, under [section] headings, with version=1.
 * Numbers are decimal. A list is comma-separated. An opaque block is hex.
 * Replay inputs are the two players' pad words, little-endian, from frame 0
 * through the last frame that is not all zero. game_infor.fname is an
 * address, so it is never written; a load leaves it NULL.
 *
 * The write replaces the file: a temp file beside it, flushed and synced,
 * then renamed over it. */

#include "port/store.h"

#include "host/compat.h"

#include <errno.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define STORE_VERSION 1
#define INPUT_FRAMES 7198

_Static_assert(sizeof(((const _REPLAY_W *)0)->io_unit.key_buff[0]) / sizeof(u16) == INPUT_FRAMES,
               "replay input frames");

enum { F_U8 = 1, F_S8, F_U16, F_S16, F_U32 };

typedef struct Field {
    const char *sec;
    const char *key;
    size_t off;
    int kind;
} Field;

typedef struct SfIniEnt {
    char *sec;
    char *key;
    char *val;
} SfIniEnt;

struct SfIni {
    char *note;
    SfIniEnt *e;
    int n;
    int cap;
};

static const Field k_system[] = {
    { "system", "difficulty", offsetof(struct _SAVE_W, Difficulty), F_U8 },
    { "system", "time_limit", offsetof(struct _SAVE_W, Time_Limit), F_S8 },
    { "system", "damage_level", offsetof(struct _SAVE_W, Damage_Level), F_U8 },
    { "system", "handicap", offsetof(struct _SAVE_W, Handicap), F_U8 },
    { "system", "adjust_x", offsetof(struct _SAVE_W, Adjust_X), F_S8 },
    { "system", "adjust_y", offsetof(struct _SAVE_W, Adjust_Y), F_S8 },
    { "system", "screen_size", offsetof(struct _SAVE_W, Screen_Size), F_U8 },
    { "system", "screen_mode", offsetof(struct _SAVE_W, Screen_Mode), F_U8 },
    { "system", "guard_check", offsetof(struct _SAVE_W, GuardCheck), F_U8 },
    { "system", "auto_save", offsetof(struct _SAVE_W, Auto_Save), F_U8 },
    { "system", "analog_stick", offsetof(struct _SAVE_W, AnalogStick), F_U8 },
    { "system", "bgm_type", offsetof(struct _SAVE_W, BgmType), F_U8 },
    { "system", "sound_mode", offsetof(struct _SAVE_W, SoundMode), F_U8 },
    { "system", "bgm_level", offsetof(struct _SAVE_W, BGM_Level), F_U8 },
    { "system", "se_level", offsetof(struct _SAVE_W, SE_Level), F_U8 },
    { "system", "extra_option", offsetof(struct _SAVE_W, Extra_Option), F_U8 },
    { "system", "sum", offsetof(struct _SAVE_W, sum), F_U32 },
    { "pad0", "vibration", offsetof(struct _SAVE_W, Pad_Infor[0].Vibration), F_U8 },
    { "pad1", "vibration", offsetof(struct _SAVE_W, Pad_Infor[1].Vibration), F_U8 },
};

static const Field k_replay[] = {
    { "replay", "control_time", offsetof(_REPLAY_W, Control_Time_Buff), F_S16 },
    { "replay", "difficulty", offsetof(_REPLAY_W, Difficulty), F_U8 },
    { "replay", "monitor_type", offsetof(_REPLAY_W, Monitor_Type), F_U8 },
    { "replay", "champion", offsetof(_REPLAY_W, champion), F_U8 },
    { "replay", "full_data", offsetof(_REPLAY_W, full_data), F_U8 },
    { "replay", "free", offsetof(_REPLAY_W, free), F_U8 },
    { "replay", "extra_free", offsetof(_REPLAY_W, extra_free), F_U8 },
    { "replay", "sum", offsetof(_REPLAY_W, sum), F_U32 },
};

static const Field k_match[] = {
    { "match", "p0_char", offsetof(_REPLAY_W, game_infor.player_infor[0].my_char), F_U8 },
    { "match", "p0_sa", offsetof(_REPLAY_W, game_infor.player_infor[0].sa), F_S8 },
    { "match", "p0_color", offsetof(_REPLAY_W, game_infor.player_infor[0].color), F_S8 },
    { "match", "p0_type", offsetof(_REPLAY_W, game_infor.player_infor[0].player_type), F_S8 },
    { "match", "p1_char", offsetof(_REPLAY_W, game_infor.player_infor[1].my_char), F_U8 },
    { "match", "p1_sa", offsetof(_REPLAY_W, game_infor.player_infor[1].sa), F_S8 },
    { "match", "p1_color", offsetof(_REPLAY_W, game_infor.player_infor[1].color), F_S8 },
    { "match", "p1_type", offsetof(_REPLAY_W, game_infor.player_infor[1].player_type), F_S8 },
    { "match", "stage", offsetof(_REPLAY_W, game_infor.stage), F_S8 },
    { "match", "direction_working", offsetof(_REPLAY_W, game_infor.Direction_Working), F_S8 },
    { "match", "random_ix16", offsetof(_REPLAY_W, game_infor.Random_ix16), F_S16 },
    { "match", "random_ix32", offsetof(_REPLAY_W, game_infor.Random_ix32), F_S16 },
    { "match", "random_ix16_ex", offsetof(_REPLAY_W, game_infor.Random_ix16_ex), F_S16 },
    { "match", "random_ix32_ex", offsetof(_REPLAY_W, game_infor.Random_ix32_ex), F_S16 },
    { "match", "winner", offsetof(_REPLAY_W, game_infor.winner), F_U8 },
    { "match", "play_type", offsetof(_REPLAY_W, game_infor.play_type), F_U8 },
    { "match", "players_timer", offsetof(_REPLAY_W, game_infor.players_timer), F_U16 },
    { "match", "old_mes_no2", offsetof(_REPLAY_W, game_infor.old_mes_no2), F_S16 },
    { "match", "old_mes_no3", offsetof(_REPLAY_W, game_infor.old_mes_no3), F_S16 },
    { "match", "old_mes_no_pl", offsetof(_REPLAY_W, game_infor.old_mes_no_pl), F_S16 },
    { "match", "mes_already", offsetof(_REPLAY_W, game_infor.mes_already), F_S16 },
};

static const Field k_mini[] = {
    { "mini", "time_limit", offsetof(_REPLAY_W, mini_save_w.Time_Limit), F_S8 },
    { "mini", "damage_level", offsetof(_REPLAY_W, mini_save_w.Damage_Level), F_U8 },
    { "mini", "pad0_vibration", offsetof(_REPLAY_W, mini_save_w.Pad_Infor[0].Vibration), F_U8 },
    { "mini", "pad1_vibration", offsetof(_REPLAY_W, mini_save_w.Pad_Infor[1].Vibration), F_U8 },
};

SfIni *sf_ini_new(void)
{
    return calloc(1, sizeof(SfIni));
}

void sf_ini_free(SfIni *ini)
{
    int i;
    if (!ini) {
        return;
    }
    for (i = 0; i < ini->n; i++) {
        free(ini->e[i].sec);
        free(ini->e[i].key);
        free(ini->e[i].val);
    }
    free(ini->e);
    free(ini->note);
    free(ini);
}

void sf_ini_set_note(SfIni *ini, const char *note)
{
    char *s;
    if (!ini || !note || strchr(note, '\n') || strchr(note, '\r')) {
        return;
    }
    s = strdup(note);
    if (!s) {
        return;
    }
    free(ini->note);
    ini->note = s;
}

int sf_ini_find(const SfIni *ini, const char *section, const char *key, const char **value)
{
    int i;
    if (!ini || !section || !key) {
        return 1;
    }
    for (i = 0; i < ini->n; i++) {
        if (strcmp(ini->e[i].sec, section) == 0 && strcmp(ini->e[i].key, key) == 0) {
            if (value) {
                *value = ini->e[i].val;
            }
            return 0;
        }
    }
    return 1;
}

int sf_ini_set(SfIni *ini, const char *section, const char *key, const char *value)
{
    int i;
    char *sec;
    char *k;
    char *v;
    SfIniEnt *grown;
    int cap;
    if (!ini || !section || !key || !key[0] || !value) {
        return -1;
    }
    if (strchr(key, '=') || strchr(key, '\n') || strchr(key, '\r') || strchr(value, '\n') || strchr(value, '\r') ||
        strchr(section, '\n') || strchr(section, '\r') || strchr(section, '[') || strchr(section, ']')) {
        return -1;
    }
    for (i = 0; i < ini->n; i++) {
        if (strcmp(ini->e[i].sec, section) == 0 && strcmp(ini->e[i].key, key) == 0) {
            v = strdup(value);
            if (!v) {
                return -1;
            }
            free(ini->e[i].val);
            ini->e[i].val = v;
            return 0;
        }
    }
    if (ini->n == ini->cap) {
        cap = ini->cap ? ini->cap * 2 : 16;
        grown = realloc(ini->e, (size_t)cap * sizeof *grown);
        if (!grown) {
            return -1;
        }
        ini->e = grown;
        ini->cap = cap;
    }
    sec = strdup(section);
    k = strdup(key);
    v = strdup(value);
    if (!sec || !k || !v) {
        free(sec);
        free(k);
        free(v);
        return -1;
    }
    ini->e[ini->n].sec = sec;
    ini->e[ini->n].key = k;
    ini->e[ini->n].val = v;
    ini->n++;
    return 0;
}

int sf_ini_set_int(SfIni *ini, const char *section, const char *key, long long value)
{
    char buf[32];
    snprintf(buf, sizeof buf, "%lld", value);
    return sf_ini_set(ini, section, key, buf);
}

static char *trim(char *s)
{
    char *end;
    while (*s == ' ' || *s == '\t') {
        s++;
    }
    end = s + strlen(s);
    while (end > s && (end[-1] == ' ' || end[-1] == '\t')) {
        end--;
    }
    *end = 0;
    return s;
}

/* 0 a line, 1 end of file, -1 a read error. */
static int read_line(FILE *f, char **out)
{
    size_t cap = 128;
    size_t n = 0;
    char *buf;
    int c;
    *out = NULL;
    buf = malloc(cap);
    if (!buf) {
        return -1;
    }
    while ((c = fgetc(f)) != EOF) {
        char *grown;
        size_t ncap;
        if (c == '\n') {
            break;
        }
        if (c == '\r') {
            continue;
        }
        if (n + 1 >= cap) {
            if (cap > SIZE_MAX / 2) {
                free(buf);
                return -1;
            }
            ncap = cap * 2;
            grown = realloc(buf, ncap);
            if (!grown) {
                free(buf);
                return -1;
            }
            buf = grown;
            cap = ncap;
        }
        buf[n++] = (char)c;
    }
    if (c == EOF && n == 0) {
        free(buf);
        return ferror(f) ? -1 : 1;
    }
    buf[n] = 0;
    *out = buf;
    return 0;
}

static int set_section(char **section, const char *name)
{
    char *s = strdup(name);
    if (!s) {
        return -1;
    }
    free(*section);
    *section = s;
    return 0;
}

static int take_line(SfIni *ini, char **section, char *line)
{
    char *eq;
    char *key;
    char *val;
    char *end;
    char *name;
    line = trim(line);
    if (line[0] == 0 || line[0] == ';' || line[0] == '#') {
        return 0;
    }
    if (line[0] == '[') {
        end = strchr(line, ']');
        if (!end || end == line + 1 || end[1] != 0) {
            return -1;
        }
        *end = 0;
        name = trim(line + 1);
        if (name[0] == 0) {
            return -1;
        }
        return set_section(section, name);
    }
    eq = strchr(line, '=');
    if (!eq) {
        return -1;
    }
    *eq = 0;
    key = trim(line);
    val = trim(eq + 1);
    if (key[0] == 0) {
        return -1;
    }
    return sf_ini_set(ini, *section, key, val);
}

int sf_ini_load(SfIni *ini, const char *path)
{
    FILE *f;
    char *section;
    int rc = 0;
    if (!ini || !path) {
        return -1;
    }
    f = fopen(path, "rb");
    if (!f) {
        return errno == ENOENT ? 1 : -1;
    }
    section = strdup("");
    if (!section) {
        fclose(f);
        return -1;
    }
    for (;;) {
        char *line = NULL;
        int lr = read_line(f, &line);
        if (lr == 1) {
            break;
        }
        if (lr != 0 || take_line(ini, &section, line) != 0) {
            free(line);
            rc = -1;
            break;
        }
        free(line);
    }
    free(section);
    fclose(f);
    return rc;
}

static int section_seen(const SfIni *ini, int i)
{
    int j;
    for (j = 0; j < i; j++) {
        if (strcmp(ini->e[j].sec, ini->e[i].sec) == 0) {
            return 1;
        }
    }
    return 0;
}

static int write_doc(FILE *f, const SfIni *ini)
{
    int i;
    int j;
    int any = 0;
    if (ini->note && fprintf(f, "; %s\n\n", ini->note) < 0) {
        return -1;
    }
    for (i = 0; i < ini->n; i++) {
        if (section_seen(ini, i)) {
            continue;
        }
        if (ini->e[i].sec[0] != 0) {
            if (any && fputc('\n', f) == EOF) {
                return -1;
            }
            if (fprintf(f, "[%s]\n", ini->e[i].sec) < 0) {
                return -1;
            }
        }
        for (j = i; j < ini->n; j++) {
            if (strcmp(ini->e[j].sec, ini->e[i].sec) != 0) {
                continue;
            }
            if (fprintf(f, "%s=%s\n", ini->e[j].key, ini->e[j].val) < 0) {
                return -1;
            }
        }
        any = 1;
    }
    return 0;
}

static int mkdir_parent(const char *path)
{
    char *dir;
    char *slash;
    char *back;
    size_t n;
    int rc = 0;
    n = strlen(path);
    dir = malloc(n + 1);
    if (!dir) {
        return -1;
    }
    memcpy(dir, path, n + 1);
    slash = strrchr(dir, '/');
    back = strrchr(dir, '\\');
    if (back && (!slash || back > slash)) {
        slash = back;
    }
    if (slash && slash != dir) {
        *slash = 0;
        if (mkdir(dir, 0755) != 0 && errno != EEXIST) {
            rc = -1;
        }
    }
    free(dir);
    return rc;
}

int sf_ini_save(const SfIni *ini, const char *path)
{
    char *tmp;
    FILE *f;
    size_t n;
    int fd;
    if (!ini || !path) {
        return -1;
    }
    if (mkdir_parent(path) != 0) {
        fprintf(stderr, "save: %s: %s\n", path, strerror(errno));
        return -1;
    }
    n = strlen(path);
    tmp = malloc(n + 5);
    if (!tmp) {
        fprintf(stderr, "save: writing %s failed\n", path);
        return -1;
    }
    memcpy(tmp, path, n);
    memcpy(tmp + n, ".tmp", 5);
    f = fopen(tmp, "wb");
    if (!f) {
        fprintf(stderr, "save: writing %s failed\n", tmp);
        free(tmp);
        return -1;
    }
    if (write_doc(f, ini) != 0 || fflush(f) != 0) {
        fprintf(stderr, "save: writing %s failed\n", tmp);
        fclose(f);
        unlink(tmp);
        free(tmp);
        return -1;
    }
    fd = fileno(f);
    if (fd < 0 || sf_file_sync(fd) != 0) {
        fprintf(stderr, "save: writing %s failed\n", tmp);
        fclose(f);
        unlink(tmp);
        free(tmp);
        return -1;
    }
    if (fclose(f) != 0) {
        fprintf(stderr, "save: writing %s failed\n", tmp);
        unlink(tmp);
        free(tmp);
        return -1;
    }
    if (rename(tmp, path) != 0) {
        fprintf(stderr, "save: writing %s failed\n", path);
        unlink(tmp);
        free(tmp);
        return -1;
    }
    free(tmp);
    return 0;
}

int sf_file_retire(const char *path)
{
    size_t n;
    char *bak;
    int rc;
    if (!path) {
        return -1;
    }
    n = strlen(path);
    bak = malloc(n + 5);
    if (!bak) {
        return -1;
    }
    memcpy(bak, path, n);
    memcpy(bak + n, ".bak", 5);
    unlink(bak);
    rc = rename(path, bak);
    if (rc != 0) {
        fprintf(stderr, "save: %s: %s\n", path, strerror(errno));
    }
    free(bak);
    return rc == 0 ? 0 : -1;
}

static int parse_i64(const char *s, long long min, long long max, long long *out)
{
    char *end;
    long long v;
    if (!s || s[0] == 0) {
        return -1;
    }
    errno = 0;
    v = strtoll(s, &end, 10);
    if (end == s || *end != 0 || errno == ERANGE || v < min || v > max) {
        return -1;
    }
    *out = v;
    return 0;
}

static int opt_i(const SfIni *ini, const char *sec, const char *key, long long min, long long max, long long *out)
{
    const char *v;
    *out = 0;
    if (sf_ini_find(ini, sec, key, &v) != 0) {
        return 0;
    }
    return parse_i64(v, min, max, out);
}

static int parse_list(const char *s, int n, long long min, long long max, long long *out)
{
    int i;
    for (i = 0; i < n; i++) {
        char *end;
        long long v;
        while (*s == ' ' || *s == '\t') {
            s++;
        }
        errno = 0;
        v = strtoll(s, &end, 10);
        if (end == s || errno == ERANGE || v < min || v > max) {
            return -1;
        }
        out[i] = v;
        s = end;
        while (*s == ' ' || *s == '\t') {
            s++;
        }
        if (i + 1 < n) {
            if (*s != ',') {
                return -1;
            }
            s++;
        }
    }
    while (*s == ' ' || *s == '\t') {
        s++;
    }
    return *s == 0 ? 0 : -1;
}

static int opt_list(const SfIni *ini, const char *sec, const char *key, int n, long long min, long long max, long long *out)
{
    const char *v;
    if (sf_ini_find(ini, sec, key, &v) != 0) {
        return 0;
    }
    return parse_list(v, n, min, max, out);
}

static int put_nums(SfIni *ini, const char *sec, const char *key, const long long *v, int n)
{
    char buf[512];
    int i;
    int at = 0;
    buf[0] = 0;
    for (i = 0; i < n; i++) {
        int w = snprintf(buf + at, sizeof buf - (size_t)at, "%s%lld", i ? "," : "", v[i]);
        if (w < 0 || (size_t)w >= sizeof buf - (size_t)at) {
            return -1;
        }
        at += w;
    }
    return sf_ini_set(ini, sec, key, buf);
}

static int put_u8s(SfIni *ini, const char *sec, const char *key, const u8 *src, int n)
{
    long long tmp[32];
    int i;
    if (n < 0 || n > 32) {
        return -1;
    }
    for (i = 0; i < n; i++) {
        tmp[i] = src[i];
    }
    return put_nums(ini, sec, key, tmp, n);
}

static int put_s8s(SfIni *ini, const char *sec, const char *key, const s8 *src, int n)
{
    long long tmp[32];
    int i;
    if (n < 0 || n > 32) {
        return -1;
    }
    for (i = 0; i < n; i++) {
        tmp[i] = src[i];
    }
    return put_nums(ini, sec, key, tmp, n);
}

static int take_u8s(const SfIni *ini, const char *sec, const char *key, u8 *dest, int n)
{
    long long tmp[32];
    int i;
    if (n < 0 || n > 32) {
        return -1;
    }
    memset(tmp, 0, sizeof tmp);
    if (opt_list(ini, sec, key, n, 0, 255, tmp) != 0) {
        return -1;
    }
    for (i = 0; i < n; i++) {
        dest[i] = (u8)tmp[i];
    }
    return 0;
}

static int take_s8s(const SfIni *ini, const char *sec, const char *key, s8 *dest, int n)
{
    long long tmp[32];
    int i;
    if (n < 0 || n > 32) {
        return -1;
    }
    memset(tmp, 0, sizeof tmp);
    if (opt_list(ini, sec, key, n, -128, 127, tmp) != 0) {
        return -1;
    }
    for (i = 0; i < n; i++) {
        dest[i] = (s8)tmp[i];
    }
    return 0;
}

static int hex_nibble(int c)
{
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    return -1;
}

static int parse_hex(const char *s, u8 *out, int n)
{
    int i;
    if (n < 0 || strlen(s) != (size_t)n * 2) {
        return -1;
    }
    for (i = 0; i < n; i++) {
        int hi = hex_nibble((unsigned char)s[i * 2]);
        int lo = hex_nibble((unsigned char)s[i * 2 + 1]);
        if (hi < 0 || lo < 0) {
            return -1;
        }
        out[i] = (u8)((hi << 4) | lo);
    }
    return 0;
}

static int put_hex(SfIni *ini, const char *sec, const char *key, const void *src, int n)
{
    static const char hexd[] = "0123456789abcdef";
    const u8 *p = src;
    char *s;
    int i;
    int rc;
    if (n < 0 || (size_t)n > (SIZE_MAX - 1) / 2) {
        return -1;
    }
    s = malloc((size_t)n * 2 + 1);
    if (!s) {
        return -1;
    }
    for (i = 0; i < n; i++) {
        s[i * 2] = hexd[p[i] >> 4];
        s[i * 2 + 1] = hexd[p[i] & 15];
    }
    s[n * 2] = 0;
    rc = sf_ini_set(ini, sec, key, s);
    free(s);
    return rc;
}

static int take_hex(const SfIni *ini, const char *sec, const char *key, void *dest, int n)
{
    const char *v;
    if (sf_ini_find(ini, sec, key, &v) != 0) {
        return 0;
    }
    return parse_hex(v, dest, n);
}

static void limits(int kind, long long *min, long long *max)
{
    switch (kind) {
    case F_S8:
        *min = -128;
        *max = 127;
        break;
    case F_U8:
        *min = 0;
        *max = 255;
        break;
    case F_S16:
        *min = -32768;
        *max = 32767;
        break;
    case F_U16:
        *min = 0;
        *max = 65535;
        break;
    default:
        *min = 0;
        *max = 4294967295LL;
        break;
    }
}

static long long load_num(const void *base, size_t off, int kind)
{
    const char *p = (const char *)base + off;
    s8 s8v;
    u8 u8v;
    s16 s16v;
    u16 u16v;
    u32 u32v;
    switch (kind) {
    case F_S8:
        memcpy(&s8v, p, sizeof s8v);
        return s8v;
    case F_U8:
        memcpy(&u8v, p, sizeof u8v);
        return u8v;
    case F_S16:
        memcpy(&s16v, p, sizeof s16v);
        return s16v;
    case F_U16:
        memcpy(&u16v, p, sizeof u16v);
        return u16v;
    default:
        memcpy(&u32v, p, sizeof u32v);
        return u32v;
    }
}

static void store_num(void *base, size_t off, int kind, long long v)
{
    char *p = (char *)base + off;
    s8 s8v;
    u8 u8v;
    s16 s16v;
    u16 u16v;
    u32 u32v;
    switch (kind) {
    case F_S8:
        s8v = (s8)v;
        memcpy(p, &s8v, sizeof s8v);
        break;
    case F_U8:
        u8v = (u8)v;
        memcpy(p, &u8v, sizeof u8v);
        break;
    case F_S16:
        s16v = (s16)v;
        memcpy(p, &s16v, sizeof s16v);
        break;
    case F_U16:
        u16v = (u16)v;
        memcpy(p, &u16v, sizeof u16v);
        break;
    default:
        u32v = (u32)v;
        memcpy(p, &u32v, sizeof u32v);
        break;
    }
}

static int write_fields(SfIni *ini, const void *base, const Field *f, int n)
{
    int i;
    for (i = 0; i < n; i++) {
        if (sf_ini_set_int(ini, f[i].sec, f[i].key, load_num(base, f[i].off, f[i].kind)) != 0) {
            return -1;
        }
    }
    return 0;
}

static int read_fields(const SfIni *ini, void *base, const Field *f, int n)
{
    int i;
    for (i = 0; i < n; i++) {
        long long min;
        long long max;
        long long v;
        limits(f[i].kind, &min, &max);
        if (opt_i(ini, f[i].sec, f[i].key, min, max, &v) != 0) {
            return -1;
        }
        store_num(base, f[i].off, f[i].kind, v);
    }
    return 0;
}

static int need_version(const SfIni *ini, const char *sec)
{
    const char *v;
    long long n = 0;
    if (sf_ini_find(ini, sec, "version", &v) != 0) {
        return -1;
    }
    return parse_i64(v, STORE_VERSION, STORE_VERSION, &n);
}

static int write_dir(SfIni *ini, const char *sec, const SystemDir *d, int versioned)
{
    int r;
    char key[8];
    if (versioned && sf_ini_set_int(ini, sec, "version", STORE_VERSION) != 0) {
        return -1;
    }
    for (r = 0; r < 10; r++) {
        snprintf(key, sizeof key, "row%d", r);
        if (put_s8s(ini, sec, key, d->contents[r], 7) != 0) {
            return -1;
        }
    }
    return sf_ini_set_int(ini, sec, "sum", d->sum);
}

static int read_dir(const SfIni *ini, const char *sec, SystemDir *d, int versioned)
{
    int r;
    int i;
    char key[8];
    long long row[7];
    long long sum;
    if (versioned && need_version(ini, sec) != 0) {
        return -1;
    }
    for (r = 0; r < 10; r++) {
        snprintf(key, sizeof key, "row%d", r);
        memset(row, 0, sizeof row);
        if (opt_list(ini, sec, key, 7, -128, 127, row) != 0) {
            return -1;
        }
        for (i = 0; i < 7; i++) {
            d->contents[r][i] = (s8)row[i];
        }
    }
    if (opt_i(ini, sec, "sum", 0, 65535, &sum) != 0) {
        return -1;
    }
    d->sum = (u16)sum;
    return 0;
}

static int write_ranks(SfIni *ini, const struct _SAVE_W *sw)
{
    int i;
    char key[16];
    for (i = 0; i < 20; i++) {
        const RANK_DATA *r = &sw->Ranking[i];
        long long name[3];
        name[0] = r->name[0];
        name[1] = r->name[1];
        name[2] = r->name[2];
        snprintf(key, sizeof key, "r%d_name", i);
        if (put_nums(ini, "rank", key, name, 3) != 0) {
            return -1;
        }
        snprintf(key, sizeof key, "r%d_player", i);
        if (sf_ini_set_int(ini, "rank", key, r->player) != 0) {
            return -1;
        }
        snprintf(key, sizeof key, "r%d_score", i);
        if (sf_ini_set_int(ini, "rank", key, r->score) != 0) {
            return -1;
        }
        snprintf(key, sizeof key, "r%d_cpu_grade", i);
        if (sf_ini_set_int(ini, "rank", key, r->cpu_grade) != 0) {
            return -1;
        }
        snprintf(key, sizeof key, "r%d_grade", i);
        if (sf_ini_set_int(ini, "rank", key, r->grade) != 0) {
            return -1;
        }
        snprintf(key, sizeof key, "r%d_wins", i);
        if (sf_ini_set_int(ini, "rank", key, r->wins) != 0) {
            return -1;
        }
        snprintf(key, sizeof key, "r%d_color", i);
        if (sf_ini_set_int(ini, "rank", key, r->player_color) != 0) {
            return -1;
        }
        snprintf(key, sizeof key, "r%d_clear", i);
        if (sf_ini_set_int(ini, "rank", key, r->all_clear) != 0) {
            return -1;
        }
    }
    return 0;
}

static int read_ranks(const SfIni *ini, struct _SAVE_W *sw)
{
    int i;
    int k;
    char key[16];
    long long name[3];
    long long v;
    for (i = 0; i < 20; i++) {
        RANK_DATA *r = &sw->Ranking[i];
        snprintf(key, sizeof key, "r%d_name", i);
        memset(name, 0, sizeof name);
        if (opt_list(ini, "rank", key, 3, 0, 255, name) != 0) {
            return -1;
        }
        for (k = 0; k < 3; k++) {
            r->name[k] = (u8)name[k];
        }
        snprintf(key, sizeof key, "r%d_player", i);
        if (opt_i(ini, "rank", key, 0, 65535, &v) != 0) {
            return -1;
        }
        r->player = (u16)v;
        snprintf(key, sizeof key, "r%d_score", i);
        if (opt_i(ini, "rank", key, 0, 4294967295LL, &v) != 0) {
            return -1;
        }
        r->score = (u32)v;
        snprintf(key, sizeof key, "r%d_cpu_grade", i);
        if (opt_i(ini, "rank", key, -128, 127, &v) != 0) {
            return -1;
        }
        r->cpu_grade = (s8)v;
        snprintf(key, sizeof key, "r%d_grade", i);
        if (opt_i(ini, "rank", key, -128, 127, &v) != 0) {
            return -1;
        }
        r->grade = (s8)v;
        snprintf(key, sizeof key, "r%d_wins", i);
        if (opt_i(ini, "rank", key, 0, 65535, &v) != 0) {
            return -1;
        }
        r->wins = (u16)v;
        snprintf(key, sizeof key, "r%d_color", i);
        if (opt_i(ini, "rank", key, 0, 255, &v) != 0) {
            return -1;
        }
        r->player_color = (u8)v;
        snprintf(key, sizeof key, "r%d_clear", i);
        if (opt_i(ini, "rank", key, 0, 255, &v) != 0) {
            return -1;
        }
        r->all_clear = (u8)v;
    }
    return 0;
}

static int write_system_body(SfIni *ini, const struct _SAVE_W *sw)
{
    int i;
    char key[8];
    if (sf_ini_set_int(ini, "system", "version", STORE_VERSION) != 0) {
        return -1;
    }
    if (write_fields(ini, sw, k_system, (int)(sizeof k_system / sizeof k_system[0])) != 0) {
        return -1;
    }
    if (put_u8s(ini, "system", "battle_number", sw->Battle_Number, 2) != 0 ||
        put_u8s(ini, "system", "partner_type", sw->Partner_Type, 2) != 0 ||
        put_u8s(ini, "pad0", "shot", sw->Pad_Infor[0].Shot, 8) != 0 ||
        put_u8s(ini, "pad0", "free", sw->Pad_Infor[0].free, 3) != 0 ||
        put_u8s(ini, "pad1", "shot", sw->Pad_Infor[1].Shot, 8) != 0 ||
        put_u8s(ini, "pad1", "free", sw->Pad_Infor[1].free, 3) != 0 ||
        put_u8s(ini, "color", "p0", sw->PL_Color[0], 20) != 0 || put_u8s(ini, "color", "p1", sw->PL_Color[1], 20) != 0) {
        return -1;
    }
    for (i = 0; i < 4; i++) {
        snprintf(key, sizeof key, "row%d", i);
        if (put_s8s(ini, "extra", key, sw->extra_option.contents[i], 8) != 0) {
            return -1;
        }
    }
    return write_ranks(ini, sw);
}

static int read_system_body(const SfIni *ini, struct _SAVE_W *sw)
{
    int i;
    char key[8];
    if (need_version(ini, "system") != 0) {
        return -1;
    }
    if (read_fields(ini, sw, k_system, (int)(sizeof k_system / sizeof k_system[0])) != 0) {
        return -1;
    }
    if (take_u8s(ini, "system", "battle_number", sw->Battle_Number, 2) != 0 ||
        take_u8s(ini, "system", "partner_type", sw->Partner_Type, 2) != 0 ||
        take_u8s(ini, "pad0", "shot", sw->Pad_Infor[0].Shot, 8) != 0 ||
        take_u8s(ini, "pad0", "free", sw->Pad_Infor[0].free, 3) != 0 ||
        take_u8s(ini, "pad1", "shot", sw->Pad_Infor[1].Shot, 8) != 0 ||
        take_u8s(ini, "pad1", "free", sw->Pad_Infor[1].free, 3) != 0 ||
        take_u8s(ini, "color", "p0", sw->PL_Color[0], 20) != 0 || take_u8s(ini, "color", "p1", sw->PL_Color[1], 20) != 0) {
        return -1;
    }
    for (i = 0; i < 4; i++) {
        snprintf(key, sizeof key, "row%d", i);
        if (take_s8s(ini, "extra", key, sw->extra_option.contents[i], 8) != 0) {
            return -1;
        }
    }
    return read_ranks(ini, sw);
}

static int last_input(const _REPLAY_W *w)
{
    int i;
    for (i = INPUT_FRAMES - 1; i >= 0; i--) {
        if (w->io_unit.key_buff[0][i] != 0 || w->io_unit.key_buff[1][i] != 0) {
            return i;
        }
    }
    return -1;
}

static int write_inputs(SfIni *ini, const _REPLAY_W *w)
{
    int last = last_input(w);
    int frames = last + 1;
    u8 *buf;
    int i;
    int rc;
    if (sf_ini_set_int(ini, "inputs", "frames", frames) != 0) {
        return -1;
    }
    if (frames == 0) {
        return 0;
    }
    buf = malloc((size_t)frames * 4);
    if (!buf) {
        return -1;
    }
    for (i = 0; i < frames; i++) {
        u16 a = w->io_unit.key_buff[0][i];
        u16 b = w->io_unit.key_buff[1][i];
        buf[i * 4] = (u8)a;
        buf[i * 4 + 1] = (u8)(a >> 8);
        buf[i * 4 + 2] = (u8)b;
        buf[i * 4 + 3] = (u8)(b >> 8);
    }
    rc = put_hex(ini, "inputs", "data", buf, frames * 4);
    free(buf);
    return rc;
}

static int read_inputs(const SfIni *ini, _REPLAY_W *w)
{
    const char *frames_s;
    const char *data;
    long long frames = 0;
    int has_frames;
    int has_data;
    u8 *buf;
    int i;
    has_frames = sf_ini_find(ini, "inputs", "frames", &frames_s) == 0;
    has_data = sf_ini_find(ini, "inputs", "data", &data) == 0;
    if (!has_frames && !has_data) {
        return 0;
    }
    if (!has_frames || parse_i64(frames_s, 0, INPUT_FRAMES, &frames) != 0) {
        return -1;
    }
    if (frames == 0) {
        return has_data && data[0] != 0 ? -1 : 0;
    }
    if (!has_data) {
        return -1;
    }
    buf = malloc((size_t)frames * 4);
    if (!buf) {
        return -1;
    }
    if (parse_hex(data, buf, (int)frames * 4) != 0) {
        free(buf);
        return -1;
    }
    for (i = 0; i < (int)frames; i++) {
        w->io_unit.key_buff[0][i] = (u16)(buf[i * 4] | (buf[i * 4 + 1] << 8));
        w->io_unit.key_buff[1][i] = (u16)(buf[i * 4 + 2] | (buf[i * 4 + 3] << 8));
    }
    free(buf);
    return 0;
}

static int write_replay_body(SfIni *ini, const _REPLAY_W *w)
{
    int i;
    char key[16];
    if (sf_ini_set_int(ini, "replay", "version", STORE_VERSION) != 0) {
        return -1;
    }
    if (write_fields(ini, w, k_replay, (int)(sizeof k_replay / sizeof k_replay[0])) != 0) {
        return -1;
    }
    if (put_hex(ini, "replay", "free_free", w->free_free, 4) != 0 || put_hex(ini, "header", "data", w->header, 640) != 0 ||
        put_hex(ini, "reserve", "data", w->sega_reserve, 64) != 0) {
        return -1;
    }
    if (write_fields(ini, w, k_match, (int)(sizeof k_match / sizeof k_match[0])) != 0) {
        return -1;
    }
    if (put_s8s(ini, "match", "vital", w->game_infor.Vital_Handicap, 2) != 0) {
        return -1;
    }
    if (write_fields(ini, w, k_mini, (int)(sizeof k_mini / sizeof k_mini[0])) != 0) {
        return -1;
    }
    if (put_u8s(ini, "mini", "battle_number", w->mini_save_w.Battle_Number, 2) != 0 ||
        put_u8s(ini, "mini", "pad0_shot", w->mini_save_w.Pad_Infor[0].Shot, 8) != 0 ||
        put_u8s(ini, "mini", "pad0_free", w->mini_save_w.Pad_Infor[0].free, 3) != 0 ||
        put_u8s(ini, "mini", "pad1_shot", w->mini_save_w.Pad_Infor[1].Shot, 8) != 0 ||
        put_u8s(ini, "mini", "pad1_free", w->mini_save_w.Pad_Infor[1].free, 3) != 0) {
        return -1;
    }
    for (i = 0; i < 4; i++) {
        snprintf(key, sizeof key, "extra_%d", i);
        if (put_s8s(ini, "mini", key, w->mini_save_w.extra_option.contents[i], 8) != 0) {
            return -1;
        }
    }
    if (write_dir(ini, "directions", &w->system_dir, 0) != 0) {
        return -1;
    }
    if (put_hex(ini, "net", "handle_1p", w->net_infor.Handle_1P, 18) != 0 ||
        put_hex(ini, "net", "id_1p", w->net_infor.Id_1P, 8) != 0 ||
        put_hex(ini, "net", "handle_2p", w->net_infor.Handle_2P, 18) != 0 ||
        put_hex(ini, "net", "id_2p", w->net_infor.Id_2P, 8) != 0 ||
        put_hex(ini, "net", "serial", w->net_infor.Serial_No, 6) != 0 ||
        put_hex(ini, "net", "battle_code", w->net_infor.Battle_Code, 14) != 0 ||
        put_hex(ini, "lag", "data", w->lag, 14) != 0) {
        return -1;
    }
    return write_inputs(ini, w);
}

static int read_replay_body(const SfIni *ini, _REPLAY_W *w)
{
    int i;
    char key[16];
    if (need_version(ini, "replay") != 0) {
        return -1;
    }
    if (read_fields(ini, w, k_replay, (int)(sizeof k_replay / sizeof k_replay[0])) != 0) {
        return -1;
    }
    if (take_hex(ini, "replay", "free_free", w->free_free, 4) != 0 || take_hex(ini, "header", "data", w->header, 640) != 0 ||
        take_hex(ini, "reserve", "data", w->sega_reserve, 64) != 0) {
        return -1;
    }
    if (read_fields(ini, w, k_match, (int)(sizeof k_match / sizeof k_match[0])) != 0) {
        return -1;
    }
    if (take_s8s(ini, "match", "vital", w->game_infor.Vital_Handicap, 2) != 0) {
        return -1;
    }
    if (read_fields(ini, w, k_mini, (int)(sizeof k_mini / sizeof k_mini[0])) != 0) {
        return -1;
    }
    if (take_u8s(ini, "mini", "battle_number", w->mini_save_w.Battle_Number, 2) != 0 ||
        take_u8s(ini, "mini", "pad0_shot", w->mini_save_w.Pad_Infor[0].Shot, 8) != 0 ||
        take_u8s(ini, "mini", "pad0_free", w->mini_save_w.Pad_Infor[0].free, 3) != 0 ||
        take_u8s(ini, "mini", "pad1_shot", w->mini_save_w.Pad_Infor[1].Shot, 8) != 0 ||
        take_u8s(ini, "mini", "pad1_free", w->mini_save_w.Pad_Infor[1].free, 3) != 0) {
        return -1;
    }
    for (i = 0; i < 4; i++) {
        snprintf(key, sizeof key, "extra_%d", i);
        if (take_s8s(ini, "mini", key, w->mini_save_w.extra_option.contents[i], 8) != 0) {
            return -1;
        }
    }
    if (read_dir(ini, "directions", &w->system_dir, 0) != 0) {
        return -1;
    }
    if (take_hex(ini, "net", "handle_1p", w->net_infor.Handle_1P, 18) != 0 ||
        take_hex(ini, "net", "id_1p", w->net_infor.Id_1P, 8) != 0 ||
        take_hex(ini, "net", "handle_2p", w->net_infor.Handle_2P, 18) != 0 ||
        take_hex(ini, "net", "id_2p", w->net_infor.Id_2P, 8) != 0 ||
        take_hex(ini, "net", "serial", w->net_infor.Serial_No, 6) != 0 ||
        take_hex(ini, "net", "battle_code", w->net_infor.Battle_Code, 14) != 0 ||
        take_hex(ini, "lag", "data", w->lag, 14) != 0) {
        return -1;
    }
    w->game_infor.fname = NULL;
    return read_inputs(ini, w);
}

static int finish_read(SfIni *ini, const char *path, int rc)
{
    sf_ini_free(ini);
    if (rc < 0) {
        fprintf(stderr, "save: %s is damaged; not loaded\n", path);
    }
    return rc;
}

static int finish_write(SfIni *ini, const char *path, int rc)
{
    if (rc == 0) {
        rc = sf_ini_save(ini, path);
    } else {
        fprintf(stderr, "save: writing %s failed\n", path);
    }
    sf_ini_free(ini);
    return rc;
}

int sf_store_read_system(const char *path, struct _SAVE_W *sw)
{
    SfIni *ini;
    int r;
    if (!path || !sw) {
        return -1;
    }
    ini = sf_ini_new();
    if (!ini) {
        return -1;
    }
    r = sf_ini_load(ini, path);
    if (r != 0) {
        return finish_read(ini, path, r);
    }
    memset(sw, 0, sizeof *sw);
    return finish_read(ini, path, read_system_body(ini, sw));
}

int sf_store_write_system(const char *path, const struct _SAVE_W *sw)
{
    SfIni *ini;
    if (!path || !sw) {
        return -1;
    }
    ini = sf_ini_new();
    if (!ini) {
        fprintf(stderr, "save: writing %s failed\n", path);
        return -1;
    }
    sf_ini_set_note(ini, "System settings. version 1.");
    return finish_write(ini, path, write_system_body(ini, sw));
}

int sf_store_read_directions(const char *path, SystemDir *dir)
{
    SfIni *ini;
    int r;
    if (!path || !dir) {
        return -1;
    }
    ini = sf_ini_new();
    if (!ini) {
        return -1;
    }
    r = sf_ini_load(ini, path);
    if (r != 0) {
        return finish_read(ini, path, r);
    }
    memset(dir, 0, sizeof *dir);
    return finish_read(ini, path, read_dir(ini, "directions", dir, 1));
}

int sf_store_write_directions(const char *path, const SystemDir *dir)
{
    SfIni *ini;
    if (!path || !dir) {
        return -1;
    }
    ini = sf_ini_new();
    if (!ini) {
        fprintf(stderr, "save: writing %s failed\n", path);
        return -1;
    }
    sf_ini_set_note(ini, "Button directions. version 1.");
    return finish_write(ini, path, write_dir(ini, "directions", dir, 1));
}

int sf_store_read_replay(const char *path, _REPLAY_W *w)
{
    SfIni *ini;
    int r;
    if (!path || !w) {
        return -1;
    }
    ini = sf_ini_new();
    if (!ini) {
        return -1;
    }
    r = sf_ini_load(ini, path);
    if (r != 0) {
        return finish_read(ini, path, r);
    }
    memset(w, 0, sizeof *w);
    w->game_infor.fname = NULL;
    return finish_read(ini, path, read_replay_body(ini, w));
}

int sf_store_write_replay(const char *path, const _REPLAY_W *w)
{
    SfIni *ini;
    if (!path || !w) {
        return -1;
    }
    ini = sf_ini_new();
    if (!ini) {
        fprintf(stderr, "save: writing %s failed\n", path);
        return -1;
    }
    sf_ini_set_note(ini, "Replay. version 1.");
    return finish_write(ini, path, write_replay_body(ini, w));
}
