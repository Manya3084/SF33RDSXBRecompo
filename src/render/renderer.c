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
        return sf_gl_present_list(renderer, list, err, err_n);
    }
    return sf_vk_present_list(renderer, list, err, err_n);
}

void sf_render_set_aspect(SfRenderer *renderer, float aspect)
{
    if (renderer) {
        renderer->aspect = aspect;
    }
}

void sf_render_set_filter(SfRenderer *renderer, int filter)
{
    if (renderer) {
        renderer->filter = filter < 0 ? 0 : (filter > 3 ? 3 : filter);
    }
}

int sf_render_read_screen(SfRenderer *renderer, const uint8_t **rgba, int *w, int *h)
{
    if (!renderer) {
        return -1;
    }
    if (renderer->kind == SF_RENDER_OPENGL) {
        return sf_gl_read_screen(renderer, rgba, w, h);
    }
    return sf_vk_read_screen(renderer, rgba, w, h);
}

void sf_render_make_current(SfRenderer *renderer, int current)
{
    if (renderer && renderer->kind == SF_RENDER_OPENGL) {
        SDL_GL_MakeCurrent(renderer->window, current ? renderer->gl : NULL);
    }
}

int sf_render_create_headless(SfRenderer **out, char *err, size_t err_n)
{
    return sf_vk_create_headless(out, err, err_n);
}
