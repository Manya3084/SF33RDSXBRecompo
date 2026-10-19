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
    static uint8_t rgba[SF_SCREEN_W * SF_SCREEN_H * 4];
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
    if (sf_render_read_screen(s_renderer, rgba) != 0) {
        return;
    }
    f = fopen(colon + 1, "wb");
    if (!f) {
        return;
    }
    fprintf(f, "P6\n%d %d\n255\n", SF_SCREEN_W, SF_SCREEN_H);
    for (i = 0; i < SF_SCREEN_W * SF_SCREEN_H; i++) {
        fwrite(&rgba[i * 4], 1, 3, f);
    }
    fclose(f);
    fprintf(stderr, "video: frame %d written to %s\n", frame, colon + 1);
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
    screenshot(frame++);
    sf_gfx_frame_done();
}
