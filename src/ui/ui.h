#ifndef SF_UI_H
#define SF_UI_H

#include "host/settings.h"
#include "render/renderer.h"

#include <stdint.h>

typedef enum SfUiScreen {
    SF_UI_SETUP = 0,
    SF_UI_TITLE,
    SF_UI_DISPLAY,
    SF_UI_LIVE
} SfUiScreen;

typedef struct SfUi {
    SfUiScreen screen;
    int menu_row;
    int menu_res;
    int menu_renderer;
    int gl_fallback;
    SfRendererKind active;
    const char *status;
    const char *note;
    const char *track;
    const uint8_t *picture;
    int picture_w;
    int picture_h;
    const uint8_t *opening;
    int opening_w;
    int opening_h;
    int opening_hold;
} SfUi;

int sf_canvas_open(SfCanvas *canvas, int width, int height);
void sf_canvas_close(SfCanvas *canvas);
void sf_ui_draw(SfCanvas *canvas, const SfUi *ui);

#endif
