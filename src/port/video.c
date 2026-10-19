/* The window the game draws in: SDL3, the Vulkan renderer, and one
 * presented frame per game frame. */

#include "port/gfx.h"
#include "port/port.h"
#include "render/renderer.h"

#include <SDL3/SDL.h>

#include <stdio.h>
#include <stdlib.h>

static SDL_Window *s_window;
static SfRenderer *s_renderer;

void sf_video_init(int w, int h)
{
    char err[512];
    (void)h;
    s_window = SDL_CreateWindow("Street Fighter III: 3rd Strike", w * 2, 480 * 2,
                                SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY | SDL_WINDOW_VULKAN);
    if (!s_window) {
        fprintf(stderr, "video: window: %s\n", SDL_GetError());
        exit(1);
    }
    if (sf_render_create(&s_renderer, s_window, SF_RENDER_VULKAN, err, sizeof err) != 0) {
        fprintf(stderr, "video: Vulkan: %s\n", err);
        exit(1);
    }
    sf_gfx_init();
}

/* SF_SHOT=FRAME:PATH writes the screen after that frame as a PPM. */
static void screenshot(int frame)
{
    const uint8_t *rgba;
    int w;
    int h;
    const char *env = getenv("SF_SHOT");
    char *colon;
    long want;
    FILE *f;
    int i;
    if (!env) {
        return;
    }
    want = strtol(env, &colon, 10);
    if (*colon != ':' || (frame != want && frame != want + 1)) {
        return;
    }
    if (sf_render_read_screen(s_renderer, &rgba, &w, &h) != 0) {
        return;
    }
    f = fopen(colon + 1, "wb");
    if (!f) {
        return;
    }
    fprintf(f, "P6\n%d %d\n255\n", w, h);
    for (i = 0; i < w * h; i++) {
        fwrite(&rgba[i * 4], 1, 3, f);
    }
    fclose(f);
    fprintf(stderr, "video: frame %d written to %s\n", frame, colon + 1);
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

void sf_video_present(void)
{
    static int frame;
    char err[512];
    sf_gfx_frame_ready();
    if (sf_render_present_list(s_renderer, sf_gfx_list(), err, sizeof err) != 0) {
        fprintf(stderr, "video: %s\n", err);
        exit(1);
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
                fprintf(stderr, "quad b%u tex %u (%dx%d) pos %.3f,%.3f-%.3f,%.3f uv %.3f,%.3f-%.3f,%.3f col %08X%s\n", b,
                        bt->shade.tex, t->w, t->h, v[0].x, v[0].y, v[3].x, v[3].y, v[0].u * t->w, v[0].v * t->h,
                        v[3].u * t->w, v[3].v * t->h, v[0].color, l->cp3 && b >= l->cp3_at ? " (over)" : "");
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
    pace();
    screenshot(frame++);
    sf_gfx_frame_done();
}
