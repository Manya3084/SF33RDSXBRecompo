/* The window's settings: windowed, borderless (the desktop's size) or
 * fullscreen (a mode of the display), its size, and the shape the game's
 * 4:3 screen is shown in. SCREEN ADJUST's WINDOW MODE, RESOLUTION and
 * ASPECT RATIO rows step them (sf_display_step) and show them
 * (sf_display_text); config/display.ini keeps them. An older display.txt
 * is read once and kept as display.txt.bak. */

#include "host/paths.h"
#include "host/compat.h"
#include "port/port.h"
#include "port/store.h"

#include <SDL3/SDL.h>

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { MODE_WINDOWED, MODE_BORDERLESS, MODE_FULLSCREEN, MODES };
enum { ASPECT_4_3, ASPECT_16_9, ASPECT_FULL, ASPECTS };

static const char *const k_mode_name[MODES] = { "windowed", "borderless", "fullscreen" };
static const char *const k_mode_text[MODES] = { "WINDOWED", "BORDERLESS", "FULLSCREEN" };
static const char *const k_aspect_name[ASPECTS] = { "4:3", "16:9", "full" };
static const char *const k_aspect_text[ASPECTS] = { "4:3", "16:9", "FULL" };
static const float k_aspect[ASPECTS] = { 4.f / 3.f, 16.f / 9.f, 0.f };

#define RES_MAX 64

typedef struct Res {
    int w;
    int h;
    int mode; /* one of the display's fullscreen modes */
} Res;

static Res s_res[RES_MAX];
static int s_res_n;
static int s_mode = MODE_WINDOWED;
static int s_aspect = ASPECT_4_3;
static int s_w = 1280;
static int s_h = 960;
static char s_text[3][24];

static SDL_DisplayID display(void)
{
    SDL_Window *w = sf_video_window();
    SDL_DisplayID d = w ? SDL_GetDisplayForWindow(w) : 0;
    return d ? d : SDL_GetPrimaryDisplay();
}

static void res_add(int w, int h, int mode)
{
    int i;
    for (i = 0; i < s_res_n; i++) {
        if (s_res[i].w == w && s_res[i].h == h) {
            s_res[i].mode |= mode;
            return;
        }
    }
    if (s_res_n < RES_MAX) {
        s_res[s_res_n].w = w;
        s_res[s_res_n].h = h;
        s_res[s_res_n].mode = mode;
        s_res_n++;
    }
}

static int res_cmp(const void *a, const void *b)
{
    const Res *x = a;
    const Res *y = b;
    return x->w != y->w ? x->w - y->w : x->h - y->h;
}

/* The display's modes, and the 4:3 sizes that fit it for a window. */
static void res_list(void)
{
    static const int k_43[][2] = { { 640, 480 }, { 960, 720 }, { 1280, 960 }, { 1600, 1200 }, { 1920, 1440 },
                                   { 2560, 1920 } };
    const SDL_DisplayMode *desk = SDL_GetDesktopDisplayMode(display());
    SDL_DisplayMode **modes;
    int n = 0;
    int i;
    s_res_n = 0;
    modes = SDL_GetFullscreenDisplayModes(display(), &n);
    for (i = 0; modes && i < n; i++) {
        if (modes[i]->w >= 640 && modes[i]->h >= 480) {
            res_add(modes[i]->w, modes[i]->h, 1);
        }
    }
    SDL_free(modes);
    for (i = 0; i < (int)(sizeof k_43 / sizeof k_43[0]); i++) {
        if (!desk || (k_43[i][0] <= desk->w && k_43[i][1] <= desk->h)) {
            res_add(k_43[i][0], k_43[i][1], 0);
        }
    }
    if (s_res_n == 0) {
        res_add(1280, 960, 0);
    }
    qsort(s_res, (size_t)s_res_n, sizeof s_res[0], res_cmp);
}

static int res_index(void)
{
    int i;
    for (i = 0; i < s_res_n; i++) {
        if (s_res[i].w == s_w && s_res[i].h == s_h) {
            return i;
        }
    }
    return -1;
}

static int config_path(char *path, size_t n, const char *name)
{
    char base[4096];
    if (sf_exe_dir(base, sizeof base) != 0) {
        return -1;
    }
    return snprintf(path, n, "%s/config/%s", base, name) < (int)n ? 0 : -1;
}

/* A number in range replaces *out. Anything else leaves it, as the older
 * text file did when a line was not a size. */
static void take_dim(const char *text, int min, int *out)
{
    char *end;
    long n;
    errno = 0;
    n = strtol(text, &end, 10);
    if (end != text && *end == 0 && errno != ERANGE && n >= min && n <= 100000) {
        *out = (int)n;
    }
}

/* versioned: the [window] section of display.ini, which must say version=1.
 * Otherwise the bare keys of an older display.txt. */
static int take(const SfIni *ini, const char *sec, int versioned)
{
    const char *v;
    int i;
    if (versioned && (sf_ini_find(ini, sec, "version", &v) != 0 || strcmp(v, "1") != 0)) {
        return -1;
    }
    if (sf_ini_find(ini, sec, "mode", &v) == 0) {
        for (i = 0; i < MODES; i++) {
            if (strcmp(v, k_mode_name[i]) == 0) {
                s_mode = i;
            }
        }
    }
    if (sf_ini_find(ini, sec, "aspect", &v) == 0) {
        for (i = 0; i < ASPECTS; i++) {
            if (strcmp(v, k_aspect_name[i]) == 0) {
                s_aspect = i;
            }
        }
    }
    if (sf_ini_find(ini, sec, "width", &v) == 0) {
        take_dim(v, 320, &s_w);
    }
    if (sf_ini_find(ini, sec, "height", &v) == 0) {
        take_dim(v, 240, &s_h);
    }
    return 0;
}

static int save(void)
{
    char path[4200];
    SfIni *ini;
    int rc;
    if (config_path(path, sizeof path, "display.ini") != 0) {
        return -1;
    }
    ini = sf_ini_new();
    if (!ini) {
        fprintf(stderr, "display: cannot write %s\n", path);
        return -1;
    }
    sf_ini_set_note(ini, "Window settings. version 1.");
    if (sf_ini_set(ini, "window", "version", "1") != 0 || sf_ini_set(ini, "window", "mode", k_mode_name[s_mode]) != 0 ||
        sf_ini_set_int(ini, "window", "width", s_w) != 0 || sf_ini_set_int(ini, "window", "height", s_h) != 0 ||
        sf_ini_set(ini, "window", "aspect", k_aspect_name[s_aspect]) != 0) {
        sf_ini_free(ini);
        fprintf(stderr, "display: cannot write %s\n", path);
        return -1;
    }
    rc = sf_ini_save(ini, path);
    sf_ini_free(ini);
    return rc;
}

static void load(void)
{
    char ini_path[4200];
    char txt_path[4200];
    SfIni *doc;
    int r;
    if (config_path(ini_path, sizeof ini_path, "display.ini") != 0) {
        return;
    }
    doc = sf_ini_new();
    if (!doc) {
        return;
    }
    r = sf_ini_load(doc, ini_path);
    if (r == 0) {
        if (take(doc, "window", 1) != 0) {
            fprintf(stderr, "display: %s is damaged; not loaded\n", ini_path);
        }
        sf_ini_free(doc);
        return;
    }
    sf_ini_free(doc);
    if (r < 0) {
        fprintf(stderr, "display: %s is damaged; not loaded\n", ini_path);
        return;
    }
    if (config_path(txt_path, sizeof txt_path, "display.txt") != 0) {
        return;
    }
    doc = sf_ini_new();
    if (!doc) {
        return;
    }
    r = sf_ini_load(doc, txt_path);
    if (r != 0) {
        sf_ini_free(doc);
        return;
    }
    take(doc, "", 0);
    sf_ini_free(doc);
    if (save() == 0) {
        sf_file_retire(txt_path);
    }
}

/* The settings onto the window. */
static void apply(void)
{
    SDL_Window *win = sf_video_window();
    sf_video_set_aspect(k_aspect[s_aspect]);
    if (!win) {
        return; /* headless */
    }
    switch (s_mode) {
    case MODE_WINDOWED:
        SDL_SetWindowFullscreen(win, false);
        SDL_SetWindowSize(win, s_w, s_h);
        SDL_SetWindowPosition(win, SDL_WINDOWPOS_CENTERED_DISPLAY(display()), SDL_WINDOWPOS_CENTERED_DISPLAY(display()));
        break;
    case MODE_BORDERLESS:
        SDL_SetWindowFullscreenMode(win, NULL);
        SDL_SetWindowFullscreen(win, true);
        break;
    default: {
        SDL_DisplayMode m;
        if (SDL_GetClosestFullscreenDisplayMode(display(), s_w, s_h, 0.f, true, &m)) {
            SDL_SetWindowFullscreenMode(win, &m);
        } else {
            SDL_SetWindowFullscreenMode(win, NULL);
        }
        SDL_SetWindowFullscreen(win, true);
        break;
    }
    }
    SDL_SyncWindow(win);
}

/* At start, once the window is up. */
void sf_display_init(void)
{
    load();
    res_list();
    if (s_mode == MODE_FULLSCREEN && (res_index() < 0 || !s_res[res_index()].mode)) {
        /* a size the display does not have: its own */
        const SDL_DisplayMode *desk = SDL_GetDesktopDisplayMode(display());
        if (desk) {
            s_w = desk->w;
            s_h = desk->h;
        }
    }
    if (!getenv("SF_HEADLESS")) {
        apply();
    } else {
        sf_video_set_aspect(k_aspect[s_aspect]);
    }
}

/* SCREEN ADJUST's rows: 0 the window mode, 1 the resolution, 2 the
 * aspect ratio. The value as the menu shows it. */
const char *sf_display_text(int row)
{
    char *t = s_text[row < 0 ? 0 : row > 2 ? 2 : row];
    switch (row) {
    case 0:
        snprintf(t, sizeof s_text[0], "%s", k_mode_text[s_mode]);
        break;
    case 1:
        if (s_mode == MODE_BORDERLESS) {
            snprintf(t, sizeof s_text[0], "DESKTOP");
        } else {
            snprintf(t, sizeof s_text[0], "%dX%d", s_w, s_h);
        }
        break;
    default:
        snprintf(t, sizeof s_text[0], "%s", k_aspect_text[s_aspect]);
        break;
    }
    return t;
}

/* Left (-1) or right (1) on a row; 1 when the setting changed. */
int sf_display_step(int row, int dir)
{
    int i;
    switch (row) {
    case 0:
        s_mode = (s_mode + MODES + dir) % MODES;
        if (s_mode == MODE_FULLSCREEN && (res_index() < 0 || !s_res[res_index()].mode)) {
            const SDL_DisplayMode *desk = SDL_GetDesktopDisplayMode(display());
            if (desk) {
                s_w = desk->w;
                s_h = desk->h;
            }
        }
        break;
    case 1:
        if (s_mode == MODE_BORDERLESS) {
            return 0;
        }
        i = res_index();
        if (i < 0) {
            i = dir > 0 ? -1 : s_res_n;
        }
        /* fullscreen takes only the display's own modes */
        do {
            i += dir;
        } while (i >= 0 && i < s_res_n && s_mode == MODE_FULLSCREEN && !s_res[i].mode);
        if (i < 0 || i >= s_res_n) {
            return 0;
        }
        s_w = s_res[i].w;
        s_h = s_res[i].h;
        break;
    default:
        s_aspect = (s_aspect + ASPECTS + dir) % ASPECTS;
        break;
    }
    save();
    apply();
    return 1;
}

/* SCREEN ADJUST's DEFAULT SETTING: a 1280x960 window, 4:3. */
void sf_display_default(void)
{
    s_mode = MODE_WINDOWED;
    s_w = 1280;
    s_h = 960;
    s_aspect = ASPECT_4_3;
    save();
    apply();
}
