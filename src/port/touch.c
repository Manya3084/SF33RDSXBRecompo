/* On-screen controls for a phone: the Xbox pad's d-pad, A, B, X, Y,
 * Black, White, Start and Back, pressed with the fingers.
 *
 * In landscape the 4:3 game leaves a band each side: the d-pad sits in the
 * left one, the six buttons in the right (X Y White over A B Black, as
 * the Xbox pad's face reads), Back and Start at the top of each band. In
 * portrait they sit below the game. The controls press what the keyboard
 * would (src/port/input.c reads them with it, so they are recorded and
 * replayed like any input), and BUTTON CONFIG decides what each button
 * does.
 *
 * They are drawn into a picture at half the window's size, redrawn only
 * when the layout or what is held changes, which the renderer lays over
 * the game (SfDrawList's overlay). SCREEN ADJUST's TOUCH PAD turns them
 * off (src/port/display.c). Android only; on a computer they are never on. */

#include "port/touch.h"

#include "port/gfx.h"
#include "port/port.h"
#include "ui/ui.h"

#include <SDL3/SDL.h>

#include <math.h>
#include <stdlib.h>
#include <string.h>

enum { CTL_DPAD, CTL_BUTTON };

typedef struct Ctl {
    int kind;
    int bit; /* TOUCH_* */
    const char *label;
    uint8_t rgb[3];
    float x, y, r; /* in window pixels */
} Ctl;

#define CTLS 9

static struct {
    int on;
    int w, h; /* the window the layout is for */
    Ctl ctl[CTLS];
    int n;
    unsigned held; /* TOUCH_* held now */
    unsigned drawn_held;
    int drawn_w, drawn_h;
    uint8_t *pic;
    int pic_w, pic_h;
    uint32_t version;
} s_t = {
#ifdef __ANDROID__
    .on = 1,
#else
    .on = 0,
#endif
};

int sf_touch_on(void)
{
    return s_t.on;
}

void sf_touch_set_on(int on)
{
#ifdef __ANDROID__
    s_t.on = on != 0;
#else
    (void)on;
#endif
}

static void add(int kind, int bit, const char *label, uint8_t r, uint8_t g, uint8_t b, float x, float y, float rad)
{
    Ctl *c = &s_t.ctl[s_t.n++];
    c->kind = kind;
    c->bit = bit;
    c->label = label;
    c->rgb[0] = r;
    c->rgb[1] = g;
    c->rgb[2] = b;
    c->x = x;
    c->y = y;
    c->r = rad;
}

/* The controls for a window w x h. */
static void layout(int w, int h)
{
    float pad;
    float br;
    float sr;
    s_t.n = 0;
    s_t.w = w;
    s_t.h = h;
    if (w >= h) {
        /* the band beside the 4:3 game, or a fifth of the width at least */
        float band = (w - h * 4.f / 3.f) / 2.f;
        float bx;
        if (band < w * 0.2f) {
            band = w * 0.2f;
        }
        pad = fminf(band * 0.40f, h * 0.24f);
        br = fminf(band * 0.14f, h * 0.085f);
        sr = br * 0.7f;
        add(CTL_DPAD, 0, NULL, 230, 230, 230, band / 2.f, h * 0.62f, pad);
        bx = w - band;
        add(CTL_BUTTON, TOUCH_X, "X", 60, 120, 230, bx + band * 0.22f, h * 0.50f, br);
        add(CTL_BUTTON, TOUCH_Y, "Y", 230, 200, 40, bx + band * 0.50f, h * 0.44f, br);
        add(CTL_BUTTON, TOUCH_WHITE, "WH", 235, 235, 235, bx + band * 0.78f, h * 0.38f, br);
        add(CTL_BUTTON, TOUCH_A, "A", 60, 190, 80, bx + band * 0.22f, h * 0.76f, br);
        add(CTL_BUTTON, TOUCH_B, "B", 220, 60, 60, bx + band * 0.50f, h * 0.70f, br);
        add(CTL_BUTTON, TOUCH_BLACK, "BK", 70, 70, 70, bx + band * 0.78f, h * 0.64f, br);
        add(CTL_BUTTON, TOUCH_BACK, "BACK", 180, 180, 180, band / 2.f, h * 0.12f, sr);
        add(CTL_BUTTON, TOUCH_START, "START", 180, 180, 180, w - band / 2.f, h * 0.12f, sr);
    } else {
        /* below the game (which sits in the middle: the lower part is free) */
        float top = h / 2.f + (w * 3.f / 4.f) / 2.f;
        float area = h - top;
        float cy = top + area * 0.55f;
        pad = fminf(w * 0.20f, area * 0.38f);
        br = fminf(w * 0.075f, area * 0.13f);
        sr = br * 0.7f;
        add(CTL_DPAD, 0, NULL, 230, 230, 230, w * 0.24f, cy, pad);
        add(CTL_BUTTON, TOUCH_X, "X", 60, 120, 230, w * 0.60f, cy - br * 1.2f, br);
        add(CTL_BUTTON, TOUCH_Y, "Y", 230, 200, 40, w * 0.75f, cy - br * 1.6f, br);
        add(CTL_BUTTON, TOUCH_WHITE, "WH", 235, 235, 235, w * 0.90f, cy - br * 2.0f, br);
        add(CTL_BUTTON, TOUCH_A, "A", 60, 190, 80, w * 0.60f, cy + br * 1.2f, br);
        add(CTL_BUTTON, TOUCH_B, "B", 220, 60, 60, w * 0.75f, cy + br * 0.8f, br);
        add(CTL_BUTTON, TOUCH_BLACK, "BK", 70, 70, 70, w * 0.90f, cy + br * 0.4f, br);
        add(CTL_BUTTON, TOUCH_BACK, "BACK", 180, 180, 180, w * 0.38f, top + area * 0.08f + sr, sr);
        add(CTL_BUTTON, TOUCH_START, "START", 180, 180, 180, w * 0.62f, top + area * 0.08f + sr, sr);
    }
}

/* What a finger at (x, y), in window pixels, holds. */
static unsigned hit(float x, float y)
{
    unsigned held = 0;
    int i;
    for (i = 0; i < s_t.n; i++) {
        const Ctl *c = &s_t.ctl[i];
        float dx = x - c->x;
        float dy = y - c->y;
        float d = sqrtf(dx * dx + dy * dy);
        if (c->kind == CTL_DPAD) {
            /* eight ways: an axis counts once the finger is 0.38 of the
             * way along it, so a diagonal is about 45 degrees wide */
            if (d < c->r * 1.35f && d > c->r * 0.18f) {
                if (dx < -0.38f * d) {
                    held |= TOUCH_LEFT;
                } else if (dx > 0.38f * d) {
                    held |= TOUCH_RIGHT;
                }
                if (dy < -0.38f * d) {
                    held |= TOUCH_UP;
                } else if (dy > 0.38f * d) {
                    held |= TOUCH_DOWN;
                }
            }
        } else if (d < c->r * 1.25f) {
            held |= (unsigned)c->bit;
        }
    }
    return held;
}

unsigned sf_touch_read(void)
{
    SDL_Window *win = sf_video_window();
    SDL_TouchID *devs;
    int nd = 0;
    int w = 0;
    int h = 0;
    int i;
    unsigned held = 0;
    if (!s_t.on || !win) {
        s_t.held = 0;
        return 0;
    }
    SDL_GetWindowSizeInPixels(win, &w, &h);
    if (w <= 0 || h <= 0) {
        return 0;
    }
    if (w != s_t.w || h != s_t.h) {
        layout(w, h);
    }
    devs = SDL_GetTouchDevices(&nd);
    for (i = 0; devs && i < nd; i++) {
        int nf = 0;
        SDL_Finger **f = SDL_GetTouchFingers(devs[i], &nf);
        int k;
        for (k = 0; f && k < nf; k++) {
            held |= hit(f[k]->x * (float)w, f[k]->y * (float)h);
        }
        SDL_free(f);
    }
    SDL_free(devs);
    s_t.held = held;
    return held;
}

/* ---- the picture ---- */

static void blend(int x, int y, const uint8_t rgb[3], int a)
{
    uint8_t *p;
    int keep;
    if (x < 0 || y < 0 || x >= s_t.pic_w || y >= s_t.pic_h || a <= 0) {
        return;
    }
    p = s_t.pic + ((size_t)y * (size_t)s_t.pic_w + (size_t)x) * 4u;
    /* over what is there, with straight alpha */
    keep = 255 - a;
    p[0] = (uint8_t)((rgb[0] * a + p[0] * keep) / 255);
    p[1] = (uint8_t)((rgb[1] * a + p[1] * keep) / 255);
    p[2] = (uint8_t)((rgb[2] * a + p[2] * keep) / 255);
    p[3] = (uint8_t)(a + p[3] * keep / 255);
}

/* A disc, its rim brighter. */
static void disc(float cx, float cy, float r, const uint8_t rgb[3], int fill_a, int rim_a)
{
    int y;
    for (y = (int)(cy - r - 1); y <= (int)(cy + r + 1); y++) {
        int x;
        for (x = (int)(cx - r - 1); x <= (int)(cx + r + 1); x++) {
            float d = sqrtf((x + 0.5f - cx) * (x + 0.5f - cx) + (y + 0.5f - cy) * (y + 0.5f - cy));
            if (d <= r) {
                blend(x, y, rgb, d > r - fmaxf(2.f, r * 0.08f) ? rim_a : fill_a);
            }
        }
    }
}

static void label(float cx, float cy, const char *s, float px, const uint8_t rgb[3], int a)
{
    size_t n = strlen(s);
    float x0 = cx - (float)n * 8.f * px / 2.f;
    float y0 = cy - 8.f * px / 2.f;
    size_t i;
    for (i = 0; i < n; i++) {
        uint8_t rows[8];
        int row;
        sf_ui_glyph(s[i], rows);
        for (row = 0; row < 8; row++) {
            int col;
            for (col = 0; col < 8; col++) {
                if (rows[row] & (0x80 >> col)) {
                    int y;
                    for (y = (int)(y0 + row * px); y < (int)(y0 + (row + 1) * px); y++) {
                        int x;
                        for (x = (int)(x0 + (i * 8 + col) * px); x < (int)(x0 + (i * 8 + col + 1) * px); x++) {
                            blend(x, y, rgb, a);
                        }
                    }
                }
            }
        }
    }
}

/* An arrow head at (cx, cy) pointing (dx, dy). */
static void arrow(float cx, float cy, float dx, float dy, float size, const uint8_t rgb[3], int a)
{
    int y;
    for (y = (int)(cy - size); y <= (int)(cy + size); y++) {
        int x;
        for (x = (int)(cx - size); x <= (int)(cx + size); x++) {
            float u = (x + 0.5f - cx) * dx + (y + 0.5f - cy) * dy;   /* along */
            float v = -(x + 0.5f - cx) * dy + (y + 0.5f - cy) * dx;  /* across */
            if (u <= size * 0.5f && u >= -size * 0.5f && fabsf(v) <= (size * 0.5f - u) * 0.9f) {
                blend(x, y, rgb, a);
            }
        }
    }
}

static void draw(void)
{
    static const uint8_t white[3] = { 255, 255, 255 };
    float k;
    int i;
    s_t.pic_w = s_t.w / 2;
    s_t.pic_h = s_t.h / 2;
    free(s_t.pic);
    s_t.pic = calloc((size_t)s_t.pic_w * (size_t)s_t.pic_h, 4);
    if (!s_t.pic) {
        return;
    }
    k = 0.5f; /* the picture is half the window */
    for (i = 0; i < s_t.n; i++) {
        const Ctl *c = &s_t.ctl[i];
        float x = c->x * k;
        float y = c->y * k;
        float r = c->r * k;
        if (c->kind == CTL_DPAD) {
            static const int dirs[4][3] = { { 0, -1, TOUCH_UP }, { 0, 1, TOUCH_DOWN }, { -1, 0, TOUCH_LEFT },
                                            { 1, 0, TOUCH_RIGHT } };
            int d;
            disc(x, y, r, c->rgb, 40, 110);
            for (d = 0; d < 4; d++) {
                int held = (s_t.held & (unsigned)dirs[d][2]) != 0;
                arrow(x + dirs[d][0] * r * 0.62f, y + dirs[d][1] * r * 0.62f, (float)dirs[d][0], (float)dirs[d][1],
                      r * 0.36f, white, held ? 230 : 120);
            }
        } else {
            int held = (s_t.held & (unsigned)c->bit) != 0;
            /* the label as large as fits: a letter at a fifth of the disc */
            float px = fmaxf(1.f, fminf(r * 0.1f, r * 1.5f / (8.f * (float)strlen(c->label))));
            disc(x, y, r, c->rgb, held ? 170 : 70, held ? 240 : 150);
            label(x, y, c->label, px, white, held ? 255 : 200);
        }
    }
    s_t.drawn_held = s_t.held;
    s_t.drawn_w = s_t.w;
    s_t.drawn_h = s_t.h;
    s_t.version++;
}

void sf_touch_frame(void)
{
    SfDrawList *l = sf_gfx_list();
    if (!s_t.on || s_t.n == 0) {
        l->overlay = NULL;
        return;
    }
    if (!s_t.pic || s_t.drawn_held != s_t.held || s_t.drawn_w != s_t.w || s_t.drawn_h != s_t.h) {
        draw();
    }
    l->overlay = s_t.pic;
    l->overlay_w = s_t.pic_w;
    l->overlay_h = s_t.pic_h;
    l->overlay_version = s_t.version;
}
