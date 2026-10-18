#include "render/renderer_internal.h"

#include "common.h"
#include "render/nv2a.h"

#include <stdlib.h>

int sf_gl_create(SfRenderer **out, SDL_Window *window, char *err, size_t err_n)
{
    SfRenderer *renderer = calloc(1, sizeof(*renderer));
    if (!renderer) {
        sf_set_err(err, err_n, "out of memory");
        return -1;
    }
    renderer->kind = SF_RENDER_OPENGL;
    renderer->window = window;
    renderer->gl = SDL_GL_CreateContext(window);
    if (!renderer->gl) {
        sf_set_err(err, err_n, "OpenGL context failed: %s", SDL_GetError());
        free(renderer);
        return -1;
    }
    SDL_GL_MakeCurrent(window, renderer->gl);
    SDL_GL_SetSwapInterval(1);
    glGenTextures(1, &renderer->gl_tex);
    glBindTexture(GL_TEXTURE_2D, renderer->gl_tex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP);
    *out = renderer;
    return 0;
}

void sf_gl_destroy(SfRenderer *renderer)
{
    if (!renderer) {
        return;
    }
    if (renderer->gl) {
        SDL_GL_MakeCurrent(renderer->window, renderer->gl);
        if (renderer->gl_tex) {
            glDeleteTextures(1, &renderer->gl_tex);
        }
        SDL_GL_DestroyContext(renderer->gl);
    }
    free(renderer);
}

int sf_gl_present(SfRenderer *renderer, const SfCanvas *canvas, char *err, size_t err_n)
{
    int w = 0;
    int h = 0;
    (void)err;
    (void)err_n;
    if (!renderer || !canvas || !canvas->rgba || canvas->width <= 0 || canvas->height <= 0) {
        return -1;
    }
    SDL_GL_MakeCurrent(renderer->window, renderer->gl);
    sf_gl_clip_canvas();
    SDL_GetWindowSizeInPixels(renderer->window, &w, &h);
    if (w <= 0 || h <= 0) {
        return -1;
    }
    glViewport(0, 0, w, h);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);
    glEnable(GL_TEXTURE_2D);
    glBindTexture(GL_TEXTURE_2D, renderer->gl_tex);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    if (renderer->tex_w != canvas->width || renderer->tex_h != canvas->height) {
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, canvas->width, canvas->height, 0, GL_RGBA, GL_UNSIGNED_BYTE, canvas->rgba);
        renderer->tex_w = canvas->width;
        renderer->tex_h = canvas->height;
    } else {
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, canvas->width, canvas->height, GL_RGBA, GL_UNSIGNED_BYTE, canvas->rgba);
    }
    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();
    glOrtho(0, w, h, 0, -1, 1);
    glMatrixMode(GL_MODELVIEW);
    glLoadIdentity();
    glClearColor(0.f, 0.f, 0.f, 1.f);
    glClear(GL_COLOR_BUFFER_BIT);
    glColor4f(1.f, 1.f, 1.f, 1.f);
    /* The first canvas row is the top. GL treats the first uploaded row as v = 0. */
    glBegin(GL_QUADS);
    glTexCoord2f(0.f, 0.f);
    glVertex2f(0.f, 0.f);
    glTexCoord2f(1.f, 0.f);
    glVertex2f((float)w, 0.f);
    glTexCoord2f(1.f, 1.f);
    glVertex2f((float)w, (float)h);
    glTexCoord2f(0.f, 1.f);
    glVertex2f(0.f, (float)h);
    glEnd();
    SDL_GL_SwapWindow(renderer->window);
    return 0;
}
