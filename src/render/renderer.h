#ifndef SF_RENDERER_H
#define SF_RENDERER_H

#include "host/settings.h"

#include <SDL3/SDL.h>
#include <stddef.h>

typedef struct SfCanvas {
    int width;
    int height;
    uint8_t *rgba;
} SfCanvas;

typedef struct SfRenderer SfRenderer;
typedef struct SfDrawList SfDrawList;

int sf_render_create(SfRenderer **out, SDL_Window *window, SfRendererKind kind, char *err, size_t err_n);
void sf_render_destroy(SfRenderer *renderer);
SfRendererKind sf_render_kind(const SfRenderer *renderer);
int sf_render_present(SfRenderer *renderer, const SfCanvas *canvas, char *err, size_t err_n);

/* The game's screen (render/draw.h). Vulkan only for now. */
int sf_render_present_list(SfRenderer *renderer, const SfDrawList *list, char *err, size_t err_n);

/* The game's output, RGBA at the render scale. The first call asks for
 * it with the next frame and returns 1; the call after that frame points
 * rgba at it and returns 0. -1 when there is none. */
int sf_render_read_screen(SfRenderer *renderer, const uint8_t **rgba, int *w, int *h);

#endif
