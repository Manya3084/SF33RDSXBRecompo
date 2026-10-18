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

int sf_render_create(SfRenderer **out, SDL_Window *window, SfRendererKind kind, char *err, size_t err_n);
void sf_render_destroy(SfRenderer *renderer);
SfRendererKind sf_render_kind(const SfRenderer *renderer);
int sf_render_present(SfRenderer *renderer, const SfCanvas *canvas, char *err, size_t err_n);

#endif
