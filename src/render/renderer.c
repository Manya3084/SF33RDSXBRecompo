#include "render/renderer_internal.h"

#include "common.h"

#include <stdlib.h>

int sf_render_create(SfRenderer **out, SDL_Window *window, SfRendererKind kind, char *err, size_t err_n)
{
    if (kind == SF_RENDER_OPENGL) {
        return sf_gl_create(out, window, err, err_n);
    }
    return sf_vk_create(out, window, err, err_n);
}

void sf_render_destroy(SfRenderer *renderer)
{
    if (!renderer) {
        return;
    }
    if (renderer->kind == SF_RENDER_OPENGL) {
        sf_gl_destroy(renderer);
    } else {
        sf_vk_destroy(renderer);
    }
}

SfRendererKind sf_render_kind(const SfRenderer *renderer)
{
    return renderer ? renderer->kind : SF_RENDER_VULKAN;
}

int sf_render_present(SfRenderer *renderer, const SfCanvas *canvas, char *err, size_t err_n)
{
    if (!renderer) {
        return -1;
    }
    if (renderer->kind == SF_RENDER_OPENGL) {
        return sf_gl_present(renderer, canvas, err, err_n);
    }
    return sf_vk_present(renderer, canvas, err, err_n);
}

int sf_render_present_list(SfRenderer *renderer, const SfDrawList *list, char *err, size_t err_n)
{
    if (!renderer) {
        return -1;
    }
    if (renderer->kind == SF_RENDER_OPENGL) {
        sf_set_err(err, err_n, "the OpenGL renderer does not draw the game's screen yet");
        return -1;
    }
    return sf_vk_present_list(renderer, list, err, err_n);
}

int sf_render_read_screen(SfRenderer *renderer, uint8_t *rgba)
{
    if (!renderer || renderer->kind == SF_RENDER_OPENGL) {
        return -1;
    }
    return sf_vk_read_screen(renderer, rgba);
}
