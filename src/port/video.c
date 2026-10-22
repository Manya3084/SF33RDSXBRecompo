/* The window the game draws in: SDL3, the Vulkan renderer, and one
 * presented frame per game frame. */

#include "port/gfx.h"
#include "port/port.h"
#include "port/touch.h"
#include "render/renderer.h"

#include <SDL3/SDL.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* a phone's window is the whole screen, past the status and navigation
 * bars (Android's immersive mode) */
#ifdef __ANDROID__
#define SF_PHONE_FULLSCREEN SDL_WINDOW_FULLSCREEN
#else
#define SF_PHONE_FULLSCREEN 0
#endif

static SDL_Window *s_window;
static SfRenderer *s_renderer;

/* ---- the render thread ----
 *
 * The game thread builds a frame's draw list; the render thread draws and
 * presents it (and waits on the display's refresh) while the game thread
 * runs the next frame. At the hand-off the game thread waits for the
 * render thread to finish the frame before, then copies the new frame
 * into the render thread's own list: the vertices and batches, and the
 * textures and palettes whose version changed. Every other use of the
 * renderer (the aspect, the first start's screen, a test's screenshot) is
 * run on the render thread between frames, so the renderer is only ever
 * used from one thread, as OpenGL's context wants. SF_RENDER_THREAD=0
 * draws on the game thread, as does a machine with one core. */

typedef struct RtCall {
    void (*fn)(void *);
    void *arg;
} RtCall;

static struct {
    SDL_Thread *thread;
    SDL_Mutex *lock;
    SDL_Condition *cv;
    int frame; /* list holds a frame to draw */
    RtCall call;
    int quit;
    SfDrawList list;
    int filter;
    char err[512];
    int failed;
} s_rt;

static int rt_main(void *unused)
{
    (void)unused;
    sf_render_make_current(s_renderer, 1);
    SDL_LockMutex(s_rt.lock);
    for (;;) {
        while (!s_rt.frame && !s_rt.call.fn && !s_rt.quit) {
            SDL_WaitCondition(s_rt.cv, s_rt.lock);
        }
        if (s_rt.call.fn) {
            RtCall c = s_rt.call;
            SDL_UnlockMutex(s_rt.lock);
            c.fn(c.arg);
            SDL_LockMutex(s_rt.lock);
            s_rt.call.fn = NULL;
        } else if (s_rt.frame) {
            SDL_UnlockMutex(s_rt.lock);
            sf_render_set_filter(s_renderer, s_rt.filter);
            if (sf_render_present_list(s_renderer, &s_rt.list, s_rt.err, sizeof s_rt.err) != 0) {
                s_rt.failed = 1;
            }
            SDL_LockMutex(s_rt.lock);
            s_rt.frame = 0;
        } else {
            break;
        }
        SDL_BroadcastCondition(s_rt.cv);
    }
    SDL_UnlockMutex(s_rt.lock);
    sf_render_make_current(s_renderer, 0);
    return 0;
}

/* Until the render thread has nothing to do. */
static void rt_idle(void)
{
    SDL_LockMutex(s_rt.lock);
    while (s_rt.frame || s_rt.call.fn) {
        SDL_WaitCondition(s_rt.cv, s_rt.lock);
    }
    SDL_UnlockMutex(s_rt.lock);
}

/* fn(arg) on the thread that owns the renderer, once it is idle. */
static void rt_call(void (*fn)(void *), void *arg)
{
    if (!s_rt.thread) {
        fn(arg);
        return;
    }
    SDL_LockMutex(s_rt.lock);
    while (s_rt.frame || s_rt.call.fn) {
        SDL_WaitCondition(s_rt.cv, s_rt.lock);
    }
    s_rt.call.fn = fn;
    s_rt.call.arg = arg;
    SDL_BroadcastCondition(s_rt.cv);
    while (s_rt.call.fn) {
        SDL_WaitCondition(s_rt.cv, s_rt.lock);
    }
    SDL_UnlockMutex(s_rt.lock);
}

static void rt_start(void)
{
    const char *env = getenv("SF_RENDER_THREAD");
    if (s_rt.thread || (env && strcmp(env, "0") == 0) || SDL_GetNumLogicalCPUCores() < 2) {
        return;
    }
    s_rt.lock = SDL_CreateMutex();
    s_rt.cv = SDL_CreateCondition();
    sf_render_make_current(s_renderer, 0);
    s_rt.thread = SDL_CreateThread(rt_main, "sf3 render", NULL);
    if (!s_rt.thread) {
        sf_render_make_current(s_renderer, 1);
        fprintf(stderr, "video: no render thread: %s\n", SDL_GetError());
    }
}

static void rt_stop(void)
{
    int i;
    if (!s_rt.thread) {
        return;
    }
    rt_idle();
    SDL_LockMutex(s_rt.lock);
    s_rt.quit = 1;
    SDL_BroadcastCondition(s_rt.cv);
    SDL_UnlockMutex(s_rt.lock);
    SDL_WaitThread(s_rt.thread, NULL);
    s_rt.thread = NULL;
    sf_render_make_current(s_renderer, 1);
    for (i = 0; i < SF_TEX_MAX; i++) {
        free(s_rt.list.tex[i].rgba);
    }
    free(s_rt.list.v);
    free(s_rt.list.b);
    memset(&s_rt.list, 0, sizeof s_rt.list);
}

/* The game's frame into the render thread's list (it is idle). */
static void rt_copy(const SfDrawList *l)
{
    SfDrawList *o = &s_rt.list;
    int i;
    for (i = 0; i < SF_TEX_MAX; i++) {
        const SfTexture *t = &l->tex[i];
        SfTexture *d = &o->tex[i];
        if (d->version == t->version && d->w == t->w && d->h == t->h && d->indexed == t->indexed &&
            !d->rgba == !t->rgba) {
            continue;
        }
        if (!t->rgba) {
            free(d->rgba);
            d->rgba = NULL;
        } else {
            size_t n = (size_t)t->w * (size_t)t->h * 4;
            if (!d->rgba || d->w != t->w || d->h != t->h) {
                free(d->rgba);
                d->rgba = malloc(n);
                if (!d->rgba) {
                    sf_fatal("video: no memory for a %dx%d texture", t->w, t->h);
                }
            }
            memcpy(d->rgba, t->rgba, n);
        }
        d->w = t->w;
        d->h = t->h;
        d->indexed = t->indexed;
        d->version = t->version;
    }
    for (i = 0; i < SF_PAL_MAX; i++) {
        if (o->pal[i].version != l->pal[i].version) {
            o->pal[i] = l->pal[i];
        }
    }
    if (o->v_cap < l->v_n) {
        o->v_cap = l->v_cap;
        o->v = realloc(o->v, (size_t)o->v_cap * sizeof *o->v);
    }
    if (o->b_cap < l->b_n) {
        o->b_cap = l->b_cap;
        o->b = realloc(o->b, (size_t)o->b_cap * sizeof *o->b);
    }
    if ((l->v_n && !o->v) || (l->b_n && !o->b)) {
        sf_fatal("video: no memory for the frame");
    }
    if (l->v_n) {
        memcpy(o->v, l->v, (size_t)l->v_n * sizeof *o->v);
    }
    if (l->b_n) {
        memcpy(o->b, l->b, (size_t)l->b_n * sizeof *o->b);
    }
    o->v_n = l->v_n;
    o->b_n = l->b_n;
    o->clear = l->clear;
    memcpy(o->show, l->show, sizeof o->show);
    memcpy(o->show_at, l->show_at, sizeof o->show_at);
    o->cp3 = l->cp3;
    o->cp3_at = l->cp3_at;
    memcpy(o->cp3_rect, l->cp3_rect, sizeof o->cp3_rect);
    /* the controls' picture: its own copy, when it changed */
    if (!l->overlay) {
        o->overlay = NULL;
    } else {
        static uint8_t *pic;
        static size_t pic_n;
        size_t n = (size_t)l->overlay_w * (size_t)l->overlay_h * 4;
        if (o->overlay_version != l->overlay_version || !o->overlay) {
            if (n > pic_n) {
                free(pic);
                pic = malloc(n);
                pic_n = pic ? n : 0;
            }
            if (pic) {
                memcpy(pic, l->overlay, n);
            }
        }
        o->overlay = pic;
        o->overlay_w = l->overlay_w;
        o->overlay_h = l->overlay_h;
        o->overlay_version = l->overlay_version;
    }
}

/* The renderer's calls, on its thread. */
typedef struct CanvasCall {
    const SfCanvas *canvas;
} CanvasCall;

static void call_canvas(void *arg)
{
    char err[256];
    sf_render_present(s_renderer, ((CanvasCall *)arg)->canvas, err, sizeof err);
}

static void call_aspect(void *arg)
{
    sf_render_set_aspect(s_renderer, *(float *)arg);
}

typedef struct ReadCall {
    const uint8_t **rgba;
    int *w;
    int *h;
    int r;
} ReadCall;

static void call_read(void *arg)
{
    ReadCall *c = arg;
    c->r = sf_render_read_screen(s_renderer, c->rgba, c->w, c->h);
}

/* sf_render_read_screen, on the render thread once the frame is drawn. */
static int read_screen(const uint8_t **rgba, int *w, int *h)
{
    ReadCall c;
    c.rgba = rgba;
    c.w = w;
    c.h = h;
    c.r = -1;
    rt_call(call_read, &c);
    return c.r;
}

/* SF_RENDERER=opengl asks for the OpenGL screen even where Vulkan works. */
static int want_gl(void)
{
    const char *r = getenv("SF_RENDERER");
    return r && strcmp(r, "opengl") == 0;
}

/* The window with OpenGL 3.3, where Vulkan is missing or not wanted. */
static void open_gl(int w)
{
    char err[512];
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 3);
    /* compatibility: the first start's screen draws the old way */
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_COMPATIBILITY);
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
    s_window = SDL_CreateWindow("Street Fighter III: 3rd Strike", w * 2, 480 * 2,
                                SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY | SDL_WINDOW_OPENGL);
    if (!s_window) {
        sf_fatal("video: window: %s", SDL_GetError());
    }
    if (sf_render_create(&s_renderer, s_window, SF_RENDER_OPENGL, err, sizeof err) != 0) {
        sf_fatal("video: OpenGL: %s", err);
    }
    fprintf(stderr, "video: OpenGL\n");
}

void sf_video_init(int w, int h)
{
    char err[512];
    (void)h;
    if (getenv("SF_HEADLESS") && want_gl()) {
        /* tests of the OpenGL screen: SDL's offscreen driver (main.c) */
        open_gl(w);
        sf_gfx_init();
        sf_display_init();
        return;
    }
    if (getenv("SF_HEADLESS")) {
        /* tests: nothing on the desktop, the screen only read back */
        if (sf_render_create_headless(&s_renderer, err, sizeof err) != 0) {
            sf_fatal("video: headless Vulkan: %s", err);
        }
        sf_gfx_init();
        sf_display_init();
        return;
    }
    if (!want_gl()) {
        s_window = SDL_CreateWindow("Street Fighter III: 3rd Strike", w * 2, 480 * 2,
                                    SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY | SDL_WINDOW_VULKAN |
                                        SF_PHONE_FULLSCREEN);
        if (s_window && sf_render_create(&s_renderer, s_window, SF_RENDER_VULKAN, err, sizeof err) == 0) {
            sf_gfx_init();
            sf_display_init();
            return;
        }
        fprintf(stderr, "video: Vulkan: %s; trying OpenGL\n", s_window ? err : SDL_GetError());
        if (s_window) {
            SDL_DestroyWindow(s_window);
            s_window = NULL;
        }
    }
    open_gl(w);
    sf_gfx_init();
    sf_display_init();
}

/* A picture of the whole window (the first start's screen). */
void sf_video_present_canvas(const SfCanvas *canvas)
{
    char err[256];
    CanvasCall c;
    (void)err;
    c.canvas = canvas;
    rt_call(call_canvas, &c);
}

SDL_Window *sf_video_window(void)
{
    return s_window;
}

void sf_video_set_aspect(float aspect)
{
    rt_call(call_aspect, &aspect);
}

/* SF_SHOT=FRAME:PATH writes the screen after that frame as a PPM;
 * SF_SHOT_EVERY=N:DIR writes DIR/NNNNNN.ppm every N frames (from
 * SF_SHOT_FROM to SF_SHOT_TO when they are set). */
static void screenshot(int frame)
{
    const uint8_t *rgba;
    int w;
    int h;
    const char *env = getenv("SF_SHOT");
    const char *every = getenv("SF_SHOT_EVERY");
    char *colon;
    char path[4096];
    long want;
    FILE *f;
    int i;
    if (every) {
        long n = strtol(every, &colon, 10);
        const char *from = getenv("SF_SHOT_FROM");
        const char *to = getenv("SF_SHOT_TO");
        if (*colon != ':' || n <= 0 || (from && frame + 1 < atoi(from)) || (to && frame > atoi(to))) {
            return;
        }
        if ((frame + 1) % n == 0) {
            /* a read asks for the next frame's picture; it comes the frame
             * after. A picture already waiting is an old one: ask again. */
            if (read_screen(&rgba, &w, &h) == 0) {
                read_screen(&rgba, &w, &h);
            }
            return;
        }
        if (frame == 0 || frame % n != 0) {
            return;
        }
        snprintf(path, sizeof path, "%s/%06d.ppm", colon + 1, frame);
    } else {
        if (!env) {
            return;
        }
        want = strtol(env, &colon, 10);
        if (*colon != ':' || (frame != want && frame != want + 1)) {
            return;
        }
        snprintf(path, sizeof path, "%s", colon + 1);
    }
    if (read_screen(&rgba, &w, &h) != 0) {
        return;
    }
    f = fopen(path, "wb");
    if (!f) {
        return;
    }
    fprintf(f, "P6\n%d %d\n255\n", w, h);
    for (i = 0; i < w * h; i++) {
        fwrite(&rgba[i * 4], 1, 3, f);
    }
    fclose(f);
    if (!every) {
        fprintf(stderr, "video: frame %d written to %s\n", frame, path);
    }
}

/* The game steps once per frame and was made for 60 of them a second
 * (NTSC Xbox). The window's vsync follows the monitor, which may be
 * faster, so frames are also paced to 1/60 s here. */
static void pace(void)
{
    static Uint64 next;
    const Uint64 step = SDL_NS_PER_SECOND / 60;
    Uint64 now = SDL_GetTicksNS();
    if (next == 0 || now > next + step * 4) {
        next = now; /* first frame, or far behind (a load): start again */
    }
    if (next > now) {
        Uint64 wait = next - now;
        if (wait > 2000000) {
            SDL_DelayNS(wait - 1000000); /* sleep most of it, then spin */
        }
        while (SDL_GetTicksNS() < next) {
        }
    }
    next += step;
}

void sf_video_shutdown(void)
{
    rt_stop();
    if (s_renderer) {
        sf_render_destroy(s_renderer);
        s_renderer = NULL;
    }
    if (s_window) {
        SDL_DestroyWindow(s_window);
        s_window = NULL;
    }
}

/* A turn of the loop with no frame run (a netplay wait): its time. */
void sf_video_wait_frame(void)
{
    if (!getenv("SF_HEADLESS") || getenv("SF_PACE")) {
        pace();
    }
}

void sf_video_present(void)
{
    static int frame;
    char err[512];
    if (sf_net_resim()) {
        /* a netplay frame run again: not shown */
        sf_gfx_frame_ready();
        sf_gfx_frame_done();
        return;
    }
    if (sf_sync_testing() && frame % 30 != 0) {
        /* a sync test: at full speed, a frame in 30 shown */
        frame++;
        sf_gfx_frame_ready();
        sf_gfx_frame_done();
        return;
    }
    sf_gfx_frame_ready();
    sf_touch_frame();
    rt_start();
    if (s_rt.thread) {
        /* the frame before is drawn; this one goes to the render thread */
        rt_idle();
        if (s_rt.failed) {
            sf_fatal("video: %s", s_rt.err);
        }
        rt_copy(sf_gfx_list());
        s_rt.filter = sf_save_screen_filter();
        SDL_LockMutex(s_rt.lock);
        s_rt.frame = 1;
        SDL_BroadcastCondition(s_rt.cv);
        SDL_UnlockMutex(s_rt.lock);
    } else {
        sf_render_set_filter(s_renderer, sf_save_screen_filter());
        if (sf_render_present_list(s_renderer, sf_gfx_list(), err, sizeof err) != 0) {
            sf_fatal("video: %s", err);
        }
    }
    if (getenv("SF_DUMP_QUADS") && atoi(getenv("SF_DUMP_QUADS")) == frame) {
        /* debugging: each quad of the frame, its batch's texture and the
         * texel rectangle it samples */
        const SfDrawList *l = sf_gfx_list();
        uint32_t b;
        for (b = 0; b < l->b_n; b++) {
            const SfBatch *bt = &l->b[b];
            uint32_t q;
            for (q = 0; q < bt->quads; q++) {
                const SfVertex *v = &l->v[bt->first + q * 4];
                const SfTexture *t = &l->tex[bt->shade.tex];
                fprintf(stderr,
                        "quad b%u tex %u (%dx%d) pos %.3f,%.3f-%.3f,%.3f uv %.3f,%.3f-%.3f,%.3f rect %.3f,%.3f-%.3f,%.3f col "
                        "%08X addr %u%s\n",
                        b, bt->shade.tex, t->w, t->h, v[0].x, v[0].y, v[3].x, v[3].y, v[0].u * t->w, v[0].v * t->h,
                        v[3].u * t->w, v[3].v * t->h, v[0].u0 * t->w, v[0].v0 * t->h, v[0].u1 * t->w, v[0].v1 * t->h,
                        v[0].color, (unsigned)bt->shade.address_u, l->cp3 && b >= l->cp3_at ? " (over)" : "");
            }
        }
    }
    if (getenv("SF_DUMP_TEX") && atoi(getenv("SF_DUMP_TEX")) == frame) {
        /* debugging: every texture as a PPM (an indexed one shows its index) */
        const SfDrawList *l = sf_gfx_list();
        int h;
        for (h = 1; h < SF_TEX_MAX; h++) {
            const SfTexture *t = &l->tex[h];
            char path[64];
            FILE *f;
            int i;
            if (!t->rgba) {
                continue;
            }
            snprintf(path, sizeof path, "tex%03d%s.ppm", h, t->indexed ? "i" : "");
            f = fopen(path, "wb");
            if (!f) {
                continue;
            }
            fprintf(f, "P6\n%d %d\n255\n", t->w, t->h);
            for (i = 0; i < t->w * t->h; i++) {
                fwrite(&t->rgba[i * 4], 1, 3, f);
            }
            fclose(f);
        }
    }
    if ((!getenv("SF_HEADLESS") || getenv("SF_PACE")) && !sf_sync_testing()) {
        pace(); /* headless tests run as fast as they can, unless SF_PACE */
    }
    screenshot(frame++);
    sf_gfx_frame_done();
}
