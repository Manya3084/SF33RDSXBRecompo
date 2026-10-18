#include "ui/ui.h"

#include "mem/heap.h"

#include <stdio.h>
#include <string.h>

static const uint8_t k_glyph[][8] = {
    {0x3C, 0x42, 0x46, 0x4A, 0x52, 0x62, 0x42, 0x3C},
    {0x08, 0x18, 0x28, 0x08, 0x08, 0x08, 0x08, 0x3E},
    {0x3C, 0x42, 0x02, 0x0C, 0x30, 0x40, 0x40, 0x7E},
    {0x3C, 0x42, 0x02, 0x1C, 0x02, 0x02, 0x42, 0x3C},
    {0x04, 0x0C, 0x14, 0x24, 0x44, 0x7E, 0x04, 0x04},
    {0x7E, 0x40, 0x40, 0x7C, 0x02, 0x02, 0x42, 0x3C},
    {0x1C, 0x20, 0x40, 0x7C, 0x42, 0x42, 0x42, 0x3C},
    {0x7E, 0x02, 0x04, 0x08, 0x10, 0x10, 0x10, 0x10},
    {0x3C, 0x42, 0x42, 0x3C, 0x42, 0x42, 0x42, 0x3C},
    {0x3C, 0x42, 0x42, 0x42, 0x3E, 0x02, 0x04, 0x38},
    {0x18, 0x24, 0x42, 0x42, 0x7E, 0x42, 0x42, 0x42},
    {0x7C, 0x42, 0x42, 0x7C, 0x42, 0x42, 0x42, 0x7C},
    {0x3C, 0x42, 0x40, 0x40, 0x40, 0x40, 0x42, 0x3C},
    {0x78, 0x44, 0x42, 0x42, 0x42, 0x42, 0x44, 0x78},
    {0x7E, 0x40, 0x40, 0x7C, 0x40, 0x40, 0x40, 0x7E},
    {0x7E, 0x40, 0x40, 0x7C, 0x40, 0x40, 0x40, 0x40},
    {0x3C, 0x42, 0x40, 0x40, 0x4E, 0x42, 0x42, 0x3C},
    {0x42, 0x42, 0x42, 0x7E, 0x42, 0x42, 0x42, 0x42},
    {0x3E, 0x08, 0x08, 0x08, 0x08, 0x08, 0x08, 0x3E},
    {0x1E, 0x04, 0x04, 0x04, 0x04, 0x44, 0x44, 0x38},
    {0x42, 0x44, 0x48, 0x70, 0x48, 0x44, 0x42, 0x42},
    {0x40, 0x40, 0x40, 0x40, 0x40, 0x40, 0x40, 0x7E},
    {0x42, 0x66, 0x5A, 0x5A, 0x42, 0x42, 0x42, 0x42},
    {0x42, 0x62, 0x52, 0x4A, 0x46, 0x42, 0x42, 0x42},
    {0x3C, 0x42, 0x42, 0x42, 0x42, 0x42, 0x42, 0x3C},
    {0x7C, 0x42, 0x42, 0x7C, 0x40, 0x40, 0x40, 0x40},
    {0x3C, 0x42, 0x42, 0x42, 0x4A, 0x44, 0x42, 0x3C},
    {0x7C, 0x42, 0x42, 0x7C, 0x48, 0x44, 0x42, 0x42},
    {0x3C, 0x42, 0x40, 0x3C, 0x02, 0x02, 0x42, 0x3C},
    {0x7F, 0x08, 0x08, 0x08, 0x08, 0x08, 0x08, 0x08},
    {0x42, 0x42, 0x42, 0x42, 0x42, 0x42, 0x42, 0x3C},
    {0x42, 0x42, 0x42, 0x42, 0x42, 0x24, 0x24, 0x18},
    {0x42, 0x42, 0x42, 0x42, 0x5A, 0x5A, 0x66, 0x42},
    {0x42, 0x42, 0x24, 0x18, 0x18, 0x24, 0x42, 0x42},
    {0x41, 0x22, 0x14, 0x08, 0x08, 0x08, 0x08, 0x08},
    {0x7E, 0x02, 0x04, 0x08, 0x10, 0x20, 0x40, 0x7E},
    {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},
    {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x18, 0x18},
    {0x00, 0x18, 0x18, 0x00, 0x00, 0x18, 0x18, 0x00},
    {0x00, 0x00, 0x00, 0x7E, 0x00, 0x00, 0x00, 0x00},
    {0x02, 0x04, 0x08, 0x10, 0x20, 0x40, 0x00, 0x00},
    {0x18, 0x20, 0x40, 0x40, 0x40, 0x20, 0x18, 0x00},
    {0x18, 0x04, 0x02, 0x02, 0x02, 0x04, 0x18, 0x00},
};

static int glyph_index(char c)
{
    if (c >= 'a' && c <= 'z') {
        c = (char)(c - 'a' + 'A');
    }
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'A' && c <= 'Z') {
        return 10 + (c - 'A');
    }
    switch (c) {
    case ' ': return 36;
    case '.': return 37;
    case ':': return 38;
    case '-': return 39;
    case '/': return 40;
    case '(': return 41;
    case ')': return 42;
    default: return 36;
    }
}

int sf_canvas_open(SfCanvas *canvas, int width, int height)
{
    canvas->width = width;
    canvas->height = height;
    canvas->rgba = sf_alloc((size_t)width * (size_t)height * 4u);
    return canvas->rgba ? 0 : -1;
}

void sf_canvas_close(SfCanvas *canvas)
{
    if (!canvas) {
        return;
    }
    sf_free(canvas->rgba);
    canvas->rgba = NULL;
}

static void put(SfCanvas *canvas, int x, int y, uint8_t r, uint8_t g, uint8_t b)
{
    uint8_t *p;
    if (x < 0 || y < 0 || x >= canvas->width || y >= canvas->height) {
        return;
    }
    p = canvas->rgba + ((size_t)y * (size_t)canvas->width + (size_t)x) * 4u;
    p[0] = r;
    p[1] = g;
    p[2] = b;
    p[3] = 255;
}

static void fill(SfCanvas *canvas, uint8_t r, uint8_t g, uint8_t b)
{
    int y;
    int x;
    for (y = 0; y < canvas->height; y++) {
        for (x = 0; x < canvas->width; x++) {
            put(canvas, x, y, r, g, b);
        }
    }
}

static void text(SfCanvas *canvas, int x, int y, const char *s, uint8_t r, uint8_t g, uint8_t b)
{
    int cx = x;
    for (; s && *s; s++) {
        int row;
        int index;
        if (*s == '\n') {
            cx = x;
            y += 20;
            continue;
        }
        index = glyph_index(*s);
        for (row = 0; row < 8; row++) {
            int col;
            uint8_t bits = k_glyph[index][row];
            for (col = 0; col < 8; col++) {
                if (bits & (uint8_t)(0x80 >> col)) {
                    put(canvas, cx + col * 2, y + row * 2, r, g, b);
                    put(canvas, cx + col * 2 + 1, y + row * 2, r, g, b);
                    put(canvas, cx + col * 2, y + row * 2 + 1, r, g, b);
                    put(canvas, cx + col * 2 + 1, y + row * 2 + 1, r, g, b);
                }
            }
        }
        cx += 16;
    }
}

static void blit(SfCanvas *canvas, const uint8_t *rgba, int w, int h, int dx, int dy, int scale)
{
    int y;
    int x;
    if (!rgba || w <= 0 || h <= 0 || scale < 1) {
        return;
    }
    for (y = 0; y < h; y++) {
        for (x = 0; x < w; x++) {
            const uint8_t *p = rgba + ((size_t)y * (size_t)w + (size_t)x) * 4u;
            int sy;
            int sx;
            if (p[3] == 0) {
                continue;
            }
            for (sy = 0; sy < scale; sy++) {
                for (sx = 0; sx < scale; sx++) {
                    put(canvas, dx + x * scale + sx, dy + y * scale + sy, p[0], p[1], p[2]);
                }
            }
        }
    }
}

static const char *kind_name(SfRendererKind kind, int fallback)
{
    if (fallback && kind == SF_RENDER_OPENGL) {
        return "OPENGL (VULKAN FAILED)";
    }
    return kind == SF_RENDER_OPENGL ? "OPENGL" : "VULKAN";
}

void sf_ui_draw(SfCanvas *canvas, const SfUi *ui)
{
    int i;
    fill(canvas, 12, 16, 28);
    text(canvas, 32, 28, "STREET FIGHTER III", 240, 210, 140);
    text(canvas, 32, 52, "3RD STRIKE", 240, 210, 140);
    if (ui->screen == SF_UI_SETUP) {
        text(canvas, 32, 100, "XBOX DATA WAS NOT FOUND.", 220, 220, 220);
        text(canvas, 32, 140, "ENTER: XBOX ISO, ZIP, OR 7Z", 180, 190, 210);
        text(canvas, 32, 164, "F: EXTRACTED XBOX FOLDER", 180, 190, 210);
        text(canvas, 32, 188, "OR DROP THE XBOX DATA HERE", 180, 190, 210);
        text(canvas, 32, 228, "XBOX DISC ONLY.", 180, 190, 210);
    } else if (ui->screen == SF_UI_TITLE) {
        if (ui->opening && ui->opening_w > 0 && ui->opening_h > 0) {
            blit(canvas, ui->opening, ui->opening_w, ui->opening_h, 32, 272, 1);
        }
        if (ui->picture && ui->picture_w > 0 && ui->picture_h > 0) {
            int scale = 2;
            int x;
            if (ui->picture_w * scale > canvas->width - 64 || ui->picture_h * scale > canvas->height - 128) {
                scale = 1;
            }
            x = canvas->width - ui->picture_w * scale - 32;
            blit(canvas, ui->picture, ui->picture_w, ui->picture_h, x < 32 ? 32 : x, 96, scale);
        }
        text(canvas, 32, 110, "DATA READY", 180, 220, 180);
        text(canvas, 32, 150, kind_name(ui->active, ui->gl_fallback), 220, 220, 220);
        text(canvas, 32, 200, "START: DISPLAY", 180, 190, 210);
        text(canvas, 32, 224, "BACK: XBOX LIVE", 180, 190, 210);
        text(canvas, 32, 248, "ESC: QUIT", 180, 190, 210);
    } else if (ui->screen == SF_UI_DISPLAY) {
        text(canvas, 32, 100, "DISPLAY", 220, 220, 220);
        text(canvas, 32, 140, ui->menu_row == 0 ? "> RESOLUTION" : "  RESOLUTION", 220, 220, 220);
        for (i = 0; i < SF_RES_COUNT; i++) {
            char line[64];
            snprintf(line, sizeof line, "%s %s", i == ui->menu_res ? " >" : "  ", sf_res_presets[i].label);
            text(canvas, 48, 168 + i * 20, line, i == ui->menu_res ? 240 : 160, 210, 160);
        }
        text(canvas, 32, 280, ui->menu_row == 1 ? "> RENDERER" : "  RENDERER", 220, 220, 220);
        text(canvas, 48, 304, ui->menu_renderer == 0 ? " > VULKAN" : "   VULKAN", 220, 220, 220);
        text(canvas, 48, 324, ui->menu_renderer == 1 ? " > OPENGL" : "   OPENGL", 220, 220, 220);
        text(canvas, 32, 370, "A APPLY   B CLOSE", 180, 190, 210);
    } else {
        text(canvas, 32, 120, "XBOX LIVE", 220, 220, 220);
        text(canvas, 32, 160, "NOT IN THIS PORT", 220, 180, 140);
        text(canvas, 32, 210, "B CLOSE", 180, 190, 210);
    }
    if (ui->status && ui->status[0]) {
        text(canvas, 32, 430, ui->status, 220, 120, 120);
    }
    if (ui->note && ui->note[0]) {
        text(canvas, 32, 470, ui->note, 160, 170, 190);
    }
}
