#ifndef SF_UI_H
#define SF_UI_H

#include "host/settings.h"
#include "render/renderer.h"

#include <stdint.h>

/* The first start's screen: a status line (what is being read, or why it
 * failed) and a note. */
typedef struct SfUi {
    const char *status;
    const char *note;
} SfUi;

int sf_canvas_open(SfCanvas *canvas, int width, int height);
void sf_canvas_close(SfCanvas *canvas);
void sf_ui_draw(SfCanvas *canvas, const SfUi *ui);
void sf_ui_glyph(char c, uint8_t rows[8]);

#endif
