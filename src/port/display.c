/* The window's settings: windowed, borderless (the desktop's size) or
 * fullscreen (a mode of the display), its size, and the shape the game's
 * 4:3 screen is shown in. SCREEN ADJUST's WINDOW MODE, RESOLUTION and
 * ASPECT RATIO rows step them (sf_display_step) and show them
 * (sf_display_text). config/host.sqlite keeps them. A missing row is
 * written from the original window: 1280x960, 4:3. */

#include "host/paths.h"
#include "host/compat.h"
#include "port/port.h"
#include "port/store.h"

#include <SDL3/SDL.h>

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

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

#ifdef __ANDROID__
/* A phone has no window to size: the first row is the orientation, held
 * where it is set (not turned by the phone's sensor), kept in the same
 * place as a computer's window mode. */
#include <jni.h>

static int s_portrait;
static const char *const k_orient_name[2] = { "landscape", "portrait" };
static const char *const k_orient_text[2] = { "LANDSCAPE", "PORTRAIT" };

static void set_orientation(void)
{
    JNIEnv *env = (JNIEnv *)SDL_GetAndroidJNIEnv();
    jobject act = (jobject)SDL_GetAndroidActivity();
    jclass cls;
    jmethodID m;
    SDL_SetHint(SDL_HINT_ORIENTATIONS, s_portrait ? "Portrait" : "LandscapeLeft");
    if (!env || !act) {
        return;
    }
    cls = (*env)->GetObjectClass(env, act);
    m = (*env)->GetMethodID(env, cls, "setFixedOrientation", "(Z)V");
    if (m) {
        (*env)->CallVoidMethod(env, act, m, (jboolean)(s_portrait != 0));
    }
    if ((*env)->ExceptionCheck(env)) {
        (*env)->ExceptionClear(env);
    }
    (*env)->DeleteLocalRef(env, cls);
    (*env)->DeleteLocalRef(env, act);
}
#endif

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

/* config/host.sqlite, created on the first save. */
static int host_db(char *out, size_t n)
{
    char base[4096];
    int wrote;
    if (sf_exe_dir(base, sizeof base) != 0) {
        return -1;
    }
    wrote = snprintf(out, n, "%s/config", base);
    if (wrote < 0 || (size_t)wrote >= n) {
        return -1;
    }
    if (mkdir(out, 0755) != 0 && errno != EEXIST) {
        fprintf(stderr, "display: %s: %s\n", out, strerror(errno));
        return -1;
    }
    wrote = snprintf(out, n, "%s/config/host.sqlite", base);
    return wrote < 0 || (size_t)wrote >= n ? -1 : 0;
}

static int named(const char *v, const char *const *names, int count)
{
    int i;
    for (i = 0; i < count; i++) {
        if (strcmp(v, names[i]) == 0) {
            return i;
        }
    }
    return -1;
}

static int save(void)
{
    char path[4096 + 64];
    if (host_db(path, sizeof path) != 0) {
        return -1;
    }
#ifdef __ANDROID__
    if (sf_store_write_window(path, k_orient_name[s_portrait], s_w, s_h, k_aspect_name[s_aspect]) != 0) {
#else
    if (sf_store_write_window(path, k_mode_name[s_mode], s_w, s_h, k_aspect_name[s_aspect]) != 0) {
#endif
        fprintf(stderr, "display: cannot write %s\n", path);
        return -1;
    }
    return 0;
}

/* No window row yet: the built-in 1280x960 4:3 window becomes the file.
 * A damaged database is left as it is, and the built-in window stays. */
static void load(void)
{
    char path[4096 + 64];
    char mode[32];
    char aspect[32];
    int width;
    int height;
    int r;
    int i;
    if (host_db(path, sizeof path) != 0) {
        return;
    }
    r = sf_store_read_window(path, mode, sizeof mode, &width, &height, aspect, sizeof aspect);
    if (r == 1) {
        save();
        return;
    }
    if (r != 0) {
        return;
    }
#ifdef __ANDROID__
    s_portrait = named(mode, k_orient_name, 2) == 1;
#endif
    i = named(mode, k_mode_name, MODES);
    if (i >= 0) {
        s_mode = i;
    }
    i = named(aspect, k_aspect_name, ASPECTS);
    if (i >= 0) {
        s_aspect = i;
    }
    s_w = width;
    s_h = height;
}

/* The settings onto the window. */
static void apply(void)
{
    SDL_Window *win = sf_video_window();
    sf_video_set_aspect(k_aspect[s_aspect]);
    if (!win) {
        return; /* headless */
    }
#ifdef __ANDROID__
    set_orientation();
    SDL_SetWindowFullscreen(win, true); /* past the status and navigation bars */
    return;
#endif
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
#ifdef __ANDROID__
        snprintf(t, sizeof s_text[0], "%s", k_orient_text[s_portrait]);
        break;
#endif
        snprintf(t, sizeof s_text[0], "%s", k_mode_text[s_mode]);
        break;
    case 1:
#ifdef __ANDROID__
        snprintf(t, sizeof s_text[0], "DEVICE");
        break;
#endif
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
#ifdef __ANDROID__
    if (row == 0) {
        s_portrait = !s_portrait;
        save();
        apply();
        return 1;
    }
    if (row == 1) {
        return 0; /* the phone's own screen */
    }
#endif
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
#ifdef __ANDROID__
    s_portrait = 0;
#endif
    save();
    apply();
}
