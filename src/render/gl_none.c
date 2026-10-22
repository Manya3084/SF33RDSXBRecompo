/* No OpenGL (the Android build draws with Vulkan only): the OpenGL
 * calls say so. */

#include "render/renderer_internal.h"

#include "common.h"

int sf_gl_create(SfRenderer **out, SDL_Window *window, char *err, size_t err_n)
{
    (void)window;
    *out = NULL;
    sf_set_err(err, err_n, "this build has no OpenGL");
    return -1;
}

void sf_gl_destroy(SfRenderer *renderer)
{
    (void)renderer;
}

int sf_gl_present(SfRenderer *renderer, const SfCanvas *canvas, char *err, size_t err_n)
{
    (void)renderer;
    (void)canvas;
    sf_set_err(err, err_n, "this build has no OpenGL");
    return -1;
}

int sf_gl_present_list(SfRenderer *renderer, const SfDrawList *list, char *err, size_t err_n)
{
    (void)renderer;
    (void)list;
    sf_set_err(err, err_n, "this build has no OpenGL");
    return -1;
}

int sf_gl_read_screen(SfRenderer *renderer, const uint8_t **rgba, int *w, int *h)
{
    (void)renderer;
    (void)rgba;
    (void)w;
    (void)h;
    return -1;
}

void sf_gl_scene_destroy(SfRenderer *renderer)
{
    (void)renderer;
}
