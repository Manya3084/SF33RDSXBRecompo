/* The game's graphics calls, on the native renderer.
 *
 * The game logic draws through the library calls retail's platform layer
 * provided: texture and palette handles it locks and writes, a render
 * state call, and screen-space quads. This file keeps the same contract
 * and records what the game draws into an SfDrawList (render/draw.h) that
 * a backend renders.
 *
 * The render state call is a state machine over two words of flags (the
 * render flags and the blend word) plus device states. The device states
 * are kept here by their D3D numbers, because the game's state words map
 * onto them; at each draw they are turned into the renderer's own state.
 * The numbering, the bit layout and every mapping follow retail's
 * platform layer (3s-decomp-ogxb, Source/XBOX/xb_012146.c and
 * xb_0F4537.c); the enum values are the XDK 5849 D3D8Types.h ones. */

#include "port/gfx.h"
#include "port/port.h"

#include "types.h"
#include "structs.h"
#include "sf33rd/AcrSDK/common/plcommon.h"

#include <math.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

s32 flLogOut(const char *fmt, ...);

/* ---- device states, by D3D number ---- */

enum {
    RS_ZFUNC = 0x39,
    RS_ALPHAFUNC = 0x3A,
    RS_ALPHABLENDENABLE = 0x3B,
    RS_ALPHATESTENABLE = 0x3C,
    RS_ALPHAREF = 0x3D,
    RS_SRCBLEND = 0x3E,
    RS_DESTBLEND = 0x3F,
    RS_ZWRITEENABLE = 0x40,
    RS_BLENDOP = 0x4A,
    RS_FOGENABLE = 0x5C,
    RS_LIGHTING = 0x66,
    RS_ZENABLE = 0x8F,
    RS_COUNT = 0x100,
};

enum {
    TSS_ADDRESSU = 0,
    TSS_ADDRESSV = 1,
    TSS_MAGFILTER = 3,
    TSS_MINFILTER = 4,
    TSS_COLOROP = 0xC,
    TSS_ALPHAOP = 0x10,
    TSS_COUNT = 0x20,
};

typedef struct Gfx {
    SfDrawList list;
    u32 rs[RS_COUNT];
    u32 tss[4][TSS_COUNT];
    u16 stage_tex[4];
    u16 pal;
    /* the library's state words */
    u32 flags;        /* render flags as requested */
    u32 flags_dev;    /* render flags as applied */
    u32 blend;        /* blend word as applied */
    u32 blend_next;   /* blend word being built */
    u32 matrix_set;
    s32 stage_mode[4];
    s32 stage_tex_value[4];
    /* the quad states xb_SaveQuadStates keeps */
    s32 saved_light, saved_fog, saved_ops, saved_light_on, saved_fog_on, saved_ops_on;
    u32 saved_colorop, saved_alphaop;
    int clip[4];
    int ready;
} Gfx;

static Gfx g;

SfDrawList *sf_gfx_list(void)
{
    return &g.list;
}

static void rs_set(u32 state, u32 value)
{
    if (state < RS_COUNT) {
        g.rs[state] = value;
    }
}

static void tss_set(u32 stage, u32 state, u32 value)
{
    if (stage < 4 && state < TSS_COUNT) {
        g.tss[stage][state] = value;
    }
}

/* ---- textures ---- */

/* A texture handle: the texture's own image description and pixels (kept
 * linear), the description it was created from, and the caller's copy
 * while it is locked in another layout. */
typedef struct TexEntry {
    int used;
    int linear;   /* created linear: texel texture coordinates */
    plContext info;
    plContext src;
    u8 *pixels;
    u8 *conv;
    size_t conv_size;
    int locked_rect;
    s16 rect[4];
    s32 mode;
    uint32_t content; /* which pixels: a number the state gives each publish */
} TexEntry;

typedef struct PalEntry {
    int used;
    plContext info;
    u32 colors[SF_PAL_ENTRIES];
} PalEntry;

/* The handles, their pixels (in the game's heap: src/port/gheap.c) and
 * the palettes are game state: the game holds the handles and writes the
 * pixels and colours, and a rollback puts them back with the rest. The
 * renderer's copies are the port's, brought up to date after a rollback
 * (sf_gfx_state_restored). */
static TexEntry s_tex[SF_TEX_MAX] SF_GAME;
static PalEntry s_pal[SF_PAL_MAX] SF_GAME;
static uint32_t s_tex_content SF_GAME;
static uint32_t s_tex_shown[SF_TEX_MAX]; /* the content the renderer has */
static u32 s_pal_seen[SF_PAL_MAX][SF_PAL_ENTRIES];

/* A channel mask as the description's length, shift and maximum. */
static void mask_fields(u32 m, s32 *len, s32 *shift, s32 *max)
{
    s32 s = 0;
    s32 n = 0;
    if (m != 0) {
        while (s < 32 && !(m & 1)) {
            m >>= 1;
            s++;
        }
        while (n < 32 && (m & 1)) {
            m >>= 1;
            n++;
        }
    }
    *len = n;
    *shift = s;
    *max = (1 << n) - 1;
}

/* The XDK formats the game's textures take, as their bytes per pixel and
 * channel masks (retail's xb_FormatInfo, including its R5G6B5 red mask
 * 0xF100). */
enum { FMT_A1R5G5B5 = 0x2, FMT_A4R4G4B4 = 0x4, FMT_R5G6B5 = 0x5, FMT_A8R8G8B8 = 0x6, FMT_X8R8G8B8 = 0x7,
       FMT_P8 = 0xB };

static s32 format_info(s32 fmt, u32 *a, u32 *r, u32 *gm, u32 *b)
{
    switch (fmt) {
    case FMT_A8R8G8B8:
        *a = 0xFF000000; *r = 0xFF0000; *gm = 0xFF00; *b = 0xFF;
        return 4;
    case FMT_X8R8G8B8:
        *a = 0; *r = 0xFF0000; *gm = 0xFF00; *b = 0xFF;
        return 4;
    case FMT_P8:
        *a = 0; *r = 0; *gm = 0; *b = 0;
        return 1;
    case FMT_A4R4G4B4:
        *a = 0xF000; *r = 0xF00; *gm = 0xF0; *b = 0xF;
        return 2;
    case FMT_R5G6B5:
        *a = 0; *r = 0xF100; *gm = 0x7E0; *b = 0x1F;
        return 2;
    case FMT_A1R5G5B5:
        *a = 0x8000; *r = 0x7C00; *gm = 0x3E0; *b = 0x1F;
        return 2;
    }
    *a = *r = *gm = *b = 0;
    return 0;
}

static void fill_desc(plContext *d, s32 fmt, s32 w, s32 h)
{
    u32 a;
    u32 r;
    u32 gm;
    u32 b;
    memset(d, 0, sizeof *d);
    d->width = w;
    d->height = h;
    d->bitdepth = format_info(fmt, &a, &r, &gm, &b);
    d->pitch = w * d->bitdepth;
    mask_fields(r, &d->pixelformat.rl, &d->pixelformat.rs, &d->pixelformat.rm);
    mask_fields(gm, &d->pixelformat.gl, &d->pixelformat.gs, &d->pixelformat.gm);
    mask_fields(b, &d->pixelformat.bl, &d->pixelformat.bs, &d->pixelformat.bm);
    mask_fields(a, &d->pixelformat.al, &d->pixelformat.as, &d->pixelformat.am);
}

/* retail's xb_FormatForImage */
static s32 format_for_image(const plContext *img)
{
    switch (img->bitdepth) {
    case 0:
    case 1:
        return FMT_P8;
    case 2:
        if (img->pixelformat.al != 0) {
            return img->pixelformat.al == 1 ? FMT_A1R5G5B5 : FMT_A4R4G4B4;
        }
        return FMT_R5G6B5;
    case 3:
    case 4:
        return img->pixelformat.al ? FMT_A8R8G8B8 : FMT_X8R8G8B8;
    }
    return FMT_A8R8G8B8;
}

/* Pixel access by image description (retail's xb_ImagePixelAddress,
 * xb_GetImagePixel and xb_StoreImagePixel). desc bit 4 marks an indexed
 * image, bit 0x40 one with 8-bit indices, bit 0x10 the nibble order. */
static u8 *pixel_at(const plContext *img, s32 x, s32 y)
{
    if (x < 0 || y < 0 || x >= img->width || y >= img->height || !img->ptr) {
        return NULL;
    }
    if (img->bitdepth == 0) {
        if (img->desc & 0x40) {
            return (u8 *)img->ptr + img->pitch * y + x;
        }
        return (u8 *)img->ptr + img->pitch * y + x / 2;
    }
    return (u8 *)img->ptr + img->pitch * y + img->bitdepth * x;
}

static u32 pixel_get(const plContext *img, s32 x, s32 y)
{
    const PixelFormat *f = &img->pixelformat;
    u8 *p = pixel_at(img, x, y);
    u32 v = 0;
    u32 a;
    u32 r;
    u32 gc;
    u32 b;
    if (!p) {
        return 0;
    }
    if (img->desc & 4) {
        switch (img->bitdepth) {
        case 0:
            if (img->desc & 0x40) {
                return *p;
            }
            if (((img->desc >> 4) ^ (u32)x) & 1) {
                return *p & 0xF;
            }
            return (*p >> 4) & 0xF;
        case 1:
            return *p;
        case 2:
            return *(u16 *)p;
        case 4:
            return *(u32 *)p;
        }
        return 0;
    }
    switch (img->bitdepth) {
    case 1:
        v = *p;
        break;
    case 2:
        v = *(u16 *)p;
        break;
    case 3:
        v = ((u32)p[2] << 16) | ((u32)p[1] << 8) | p[0];
        break;
    case 4:
        v = *(u32 *)p;
        break;
    }
    a = f->al ? ((v >> f->as) & f->am) * 0xFF / f->am : 0xFF;
    r = f->rl ? ((v >> f->rs) & f->rm) * 0xFF / f->rm : 0;
    gc = f->gl ? ((v >> f->gs) & f->gm) * 0xFF / f->gm : 0;
    b = f->bl ? ((v >> f->bs) & f->bm) * 0xFF / f->bm : 0;
    return (a << 24) | (r << 16) | (gc << 8) | b;
}

static void pixel_put(const plContext *img, s32 x, s32 y, u32 color)
{
    const PixelFormat *f = &img->pixelformat;
    u8 *p = pixel_at(img, x, y);
    u32 v;
    if (!p) {
        return;
    }
    if (img->desc & 4) {
        switch (img->bitdepth) {
        case 0:
            if (img->desc & 0x40) {
                *p = (u8)color;
            } else {
                u32 d = *p;
                v = color & 0xF;
                if (((img->desc >> 4) ^ (u32)x) & 1) {
                    d &= 0xF0;
                } else {
                    v <<= 4;
                    d &= 0xF;
                }
                *p = (u8)(d | v);
            }
            break;
        case 1:
            *p = (u8)color;
            break;
        case 2:
            *(u16 *)p = (u16)color;
            break;
        case 4:
            *(u32 *)p = color;
            break;
        }
        return;
    }
    v = (((color >> 16) & 0xFF) * (u32)f->rm / 0xFF) << f->rs | (((color >> 8) & 0xFF) * (u32)f->gm / 0xFF) << f->gs |
        ((color >> 24) * (u32)f->am / 0xFF) << f->as | ((color & 0xFF) * (u32)f->bm / 0xFF) << f->bs;
    switch (img->bitdepth) {
    case 1:
        *p = (u8)v;
        break;
    case 2:
        *(u16 *)p = (u16)v;
        break;
    case 3:
        p[0] = (u8)v;
        p[1] = (u8)(v >> 8);
        p[2] = (u8)(v >> 16);
        break;
    case 4:
        *(u32 *)p = v;
        break;
    }
}

/* dst takes src's pixels, over dst's size (retail's xb_ConvertImagePixels) */
static void convert(const plContext *dst, const plContext *src)
{
    s32 x;
    s32 y;
    for (y = 0; y < dst->height; y++) {
        for (x = 0; x < dst->width; x++) {
            pixel_put(dst, x, y, pixel_get(src, x, y));
        }
    }
}

/* The descriptions differ in anything but flags, pitch and pixels. */
static int desc_differs(const plContext *a, const plContext *b)
{
    return a->width != b->width || a->height != b->height || a->bitdepth != b->bitdepth ||
           memcmp(&a->pixelformat, &b->pixelformat, sizeof a->pixelformat) != 0;
}

static uint32_t s_tex_serial;

/* The texture's pixels into its renderer copy. */
static void tex_upload(u32 h)
{
    TexEntry *t = &s_tex[h];
    SfTexture *o = &g.list.tex[h];
    size_t n = (size_t)t->info.width * t->info.height * 4;
    s32 x;
    s32 y;
    if (!o->rgba || o->w != t->info.width || o->h != t->info.height) {
        free(o->rgba);
        o->rgba = malloc(n);
        o->w = t->info.width;
        o->h = t->info.height;
    }
    o->indexed = t->info.bitdepth == 1 && t->info.pixelformat.rl == 0;
    for (y = 0; y < o->h; y++) {
        for (x = 0; x < o->w; x++) {
            u8 *d = o->rgba + ((size_t)y * o->w + x) * 4;
            if (o->indexed) {
                d[0] = t->pixels[(size_t)y * t->info.pitch + x];
                d[1] = d[2] = 0;
                d[3] = 0xFF;
            } else {
                u32 c = pixel_get(&t->info, x, y);
                d[0] = (u8)(c >> 16);
                d[1] = (u8)(c >> 8);
                d[2] = (u8)c;
                d[3] = (u8)(c >> 24);
            }
        }
    }
    /* Never a number the handle had before: a texture released and a new
     * one made on its handle within a frame looks, by size, like the old
     * one, and the renderer would keep showing the old pixels. */
    o->version = ++s_tex_serial;
    s_tex_shown[h] = t->content;
}

/* The texture's pixels changed: the renderer's copy, and a new number. */
static void tex_publish(u32 h)
{
    s_tex[h].content = ++s_tex_content;
    tex_upload(h);
}

/* After a rollback: the renderer's textures as the state has them. */
void sf_gfx_state_restored(void)
{
    u32 h;
    for (h = 1; h < SF_TEX_MAX; h++) {
        if (s_tex[h].used) {
            if (s_tex_shown[h] != s_tex[h].content) {
                tex_upload(h);
            }
        } else if (g.list.tex[h].rgba) {
            free(g.list.tex[h].rgba);
            memset(&g.list.tex[h], 0, sizeof g.list.tex[h]);
            s_tex_shown[h] = 0;
        }
    }
}

s32 flCreateTextureHandle(void *bits, u32 attribute)
{
    plContext *img = bits;
    u32 h;
    s32 fmt;
    TexEntry *t;
    for (h = 1; h < SF_TEX_MAX && s_tex[h].used; h++) {
    }
    if (h >= SF_TEX_MAX) {
        fprintf(stderr, "gfx: texture handles are full\n");
        return 0;
    }
    if (img->width > 640 || img->height > 640) {
        flLogOut("Creating texture failed @flCreateTextureHandle\n");
        return 0;
    }
    if (attribute == 6) {
        /* a depth texture (LIN_D24S8 or LIN_D16); the game never samples one */
        fmt = FMT_A8R8G8B8;
        fprintf(stderr, "gfx: depth texture %u taken as A8R8G8B8\n", h);
    } else {
        fmt = format_for_image(img);
    }
    t = &s_tex[h];
    memset(t, 0, sizeof *t);
    t->used = 1;
    t->linear = attribute == 7;
    fill_desc(&t->info, fmt, img->width, img->height);
    t->info.desc = img->desc;
    /* the fields, not the padding after them (stack bytes of the caller's,
       which would make the state differ from one run to the next) */
    memcpy(&t->src, img, offsetof(plContext, pixelformat) + sizeof img->pixelformat);
    t->pixels = sf_gheap_alloc((size_t)t->info.pitch * t->info.height);
    t->info.ptr = t->pixels;
    if (img->ptr) {
        convert(&t->info, img);
    }
    t->src.ptr = NULL;
    tex_publish(h);
    return (s32)h;
}

s32 flReleaseTextureHandle(u32 h)
{
    if (h >= SF_TEX_MAX) {
        return 0;
    }
    if (s_tex[h].used) {
        sf_gheap_free(s_tex[h].pixels);
        sf_gheap_free(s_tex[h].conv);
        memset(&s_tex[h], 0, sizeof s_tex[h]);
        s_tex_shown[h] = 0;
        free(g.list.tex[h].rgba);
        memset(&g.list.tex[h], 0, sizeof g.list.tex[h]);
    }
    return 1;
}

s32 xb_GetTextureHandleInfo(void *out, s32 h)
{
    if (h <= 0 || h >= SF_TEX_MAX || !s_tex[h].used) {
        return 0;
    }
    memcpy(out, &s_tex[h].info, sizeof(plContext));
    return 1;
}

s32 flLockTexture(s16 *r, u32 h, plContext *out, s32 mode)
{
    TexEntry *t;
    if (h >= SF_TEX_MAX || !s_tex[h].used) {
        return 0;
    }
    t = &s_tex[h];
    t->locked_rect = r != NULL;
    if (r) {
        memcpy(t->rect, r, sizeof t->rect);
    }
    t->mode = mode;
    if (mode == 1 || t->linear) {
        /* locked in place; a rectangle's pointer starts at its corner */
        *out = t->info;
        if (r) {
            out->ptr = t->pixels + (size_t)t->info.pitch * r[1] + (size_t)t->info.bitdepth * r[0];
        }
    } else if (!desc_differs(&t->info, &t->src)) {
        *out = t->info;
    } else {
        /* handed out in the layout it was created from */
        size_t n = (size_t)t->src.pitch * t->src.height;
        if (t->conv_size < n) {
            sf_gheap_free(t->conv);
            t->conv = sf_gheap_alloc(n);
            t->conv_size = n;
        }
        *out = t->src;
        out->ptr = t->conv;
        convert(out, &t->info);
    }
    out->desc |= 2;
    return 1;
}

s32 flUnlockTexture(u32 h)
{
    TexEntry *t;
    if (h >= SF_TEX_MAX || !s_tex[h].used) {
        return 0;
    }
    t = &s_tex[h];
    if (t->mode != 1 && !t->linear && desc_differs(&t->info, &t->src)) {
        plContext from = t->src;
        from.ptr = t->conv;
        if (t->locked_rect) {
            s32 x;
            s32 y;
            for (y = t->rect[1]; y < t->rect[3]; y++) {
                for (x = t->rect[0]; x < t->rect[2]; x++) {
                    pixel_put(&t->info, x, y, pixel_get(&from, x, y));
                }
            }
        } else {
            convert(&t->info, &from);
        }
    }
    tex_publish(h);
    return 1;
}

/* ---- palettes ---- */

static void pal_desc(plContext *d, s32 w, s32 h, u32 desc)
{
    memset(d, 0, sizeof *d);
    d->desc = desc;
    d->width = w;
    d->height = h;
    d->pitch = w * 4;
    d->bitdepth = 4;
    d->pixelformat.rl = 8;
    d->pixelformat.rs = 16;
    d->pixelformat.rm = 0xFF;
    d->pixelformat.gl = 8;
    d->pixelformat.gs = 8;
    d->pixelformat.gm = 0xFF;
    d->pixelformat.bl = 8;
    d->pixelformat.bs = 0;
    d->pixelformat.bm = 0xFF;
    d->pixelformat.al = 8;
    d->pixelformat.as = 24;
    d->pixelformat.am = 0xFF;
}

s32 flCreatePaletteHandle(void *bits, s32 flag)
{
    plContext *img = bits;
    u32 h;
    (void)flag;
    for (h = 1; h < SF_PAL_MAX && s_pal[h].used; h++) {
    }
    if (h >= SF_PAL_MAX) {
        fprintf(stderr, "gfx: palette handles are full\n");
        return 0;
    }
    if (img->width != 0x100 && img->width != 0x80 && img->width != 0x40 && img->width > 0x20) {
        return 0;
    }
    memset(&s_pal[h], 0, sizeof s_pal[h]);
    s_pal[h].used = 1;
    pal_desc(&s_pal[h].info, img->width, img->height, img->desc);
    s_pal[h].info.ptr = s_pal[h].colors;
    if (img->ptr) {
        convert(&s_pal[h].info, img);
    }
    return (s32)h;
}

s32 flReleasePaletteHandle(s32 h)
{
    if (h > 0 && h < SF_PAL_MAX) {
        s_pal[h].used = 0;
    }
    return 1;
}

/* The game writes a locked palette and never unlocks it; sf_gfx_frame
 * compares each palette with what the renderer has. */
s32 flLockPalette(void *rect, u32 h, plContext *out, s32 flag)
{
    (void)rect;
    (void)flag;
    if (h >= SF_PAL_MAX || !s_pal[h].used) {
        return 0;
    }
    *out = s_pal[h].info;
    return 1;
}

static void pal_publish(void)
{
    u32 h;
    for (h = 1; h < SF_PAL_MAX; h++) {
        SfPalette *o = &g.list.pal[h];
        u32 i;
        if (!s_pal[h].used || memcmp(s_pal_seen[h], s_pal[h].colors, sizeof s_pal_seen[h]) == 0) {
            continue;
        }
        memcpy(s_pal_seen[h], s_pal[h].colors, sizeof s_pal_seen[h]);
        for (i = 0; i < SF_PAL_ENTRIES; i++) {
            u32 c = s_pal[h].colors[i];
            o->rgba[i * 4 + 0] = (u8)(c >> 16);
            o->rgba[i * 4 + 1] = (u8)(c >> 8);
            o->rgba[i * 4 + 2] = (u8)c;
            o->rgba[i * 4 + 3] = (u8)(c >> 24);
        }
        o->version++;
    }
}

/* ---- the render state machine (retail's flSetRenderState) ---- */

static const u32 k_blend_factor[10] = { 0x1, 0x0, 0x302, 0x303, 0x304, 0x305, 0x300, 0x301, 0x306, 0x307 };

/* the low nibble of a blend factor field: 1 ONE, 0 ZERO, 2.. the rest */
static u32 blend_factor(u32 n)
{
    switch (n) {
    case 0x0:
        return 0x0;
    case 0x1:
        return 0x1;
    }
    return n < 10 ? k_blend_factor[n] : 0x1;
}

static void set_blend(u32 cur, u32 next)
{
    u32 changed = cur ^ next;
    g.blend = next;
    if (changed & 0xF) {
        if ((next & 0xF) < 10) {
            rs_set(RS_SRCBLEND, blend_factor(next & 0xF));
        }
    }
    if (changed & 0xF0) {
        if (((next >> 4) & 0xF) < 10) {
            rs_set(RS_DESTBLEND, blend_factor((next >> 4) & 0xF));
        }
    }
    if (changed & 0xC00) {
        switch (next & 0xC00) {
        case 0:
            rs_set(RS_BLENDOP, 0x8006);
            break;
        case 0x400:
            rs_set(RS_BLENDOP, 0x800A);
            break;
        case 0x800:
            rs_set(RS_BLENDOP, 0x800B);
            break;
        }
    }
    if (changed & 0x10000) {
        u32 f = (next & 0x10000) ? 1 : 2;
        tss_set(0, TSS_MINFILTER, f);
        tss_set(0, TSS_MAGFILTER, f);
    }
    if (changed & 0x60000) {
        switch (next & 0x60000) {
        case 0:
            tss_set(0, TSS_ADDRESSU, 1);
            tss_set(0, TSS_ADDRESSV, 1);
            break;
        case 0x20000:
            tss_set(0, TSS_ADDRESSU, 3);
            tss_set(0, TSS_ADDRESSV, 3);
            break;
        case 0x40000:
            tss_set(0, TSS_ADDRESSU, 2);
            tss_set(0, TSS_ADDRESSV, 2);
            break;
        }
    }
    if (changed & 0x380000) {
        u32 f = (next >> 19) & 7;
        rs_set(RS_ALPHATESTENABLE, f != 7);
        rs_set(RS_ALPHAFUNC, 0x200 + f);
    }
    if (changed & 0x7000) {
        rs_set(RS_ZFUNC, 0x200 + ((next >> 12) & 7));
    }
    if (changed & 0x8000) {
        rs_set(RS_ZWRITEENABLE, (next & 0x8000) != 0);
    }
    if (changed & 0x1000000) {
        rs_set(RS_ALPHABLENDENABLE, (next & 0x1000000) != 0);
    }
}

/* The render flags: lighting, fog and the rest of the fixed-function
 * transform groups. Only lighting and fog have a device state the quads
 * look at (xb_SaveQuadStates saves and clears them); the rest are kept. */
static void apply_flags(u32 changed, u32 flags)
{
    if (changed & 0xF00) {
        rs_set(RS_LIGHTING, (flags & 0xF00) != 0);
    }
    if (changed & 0x18000) {
        if ((flags & 0x18000) == 0) {
            rs_set(RS_FOGENABLE, 0);
        } else if ((flags & 0x18000) == 0x8000) {
            rs_set(RS_FOGENABLE, 1);
        }
    }
}

static void update_flags(u32 flags)
{
    apply_flags(g.flags_dev ^ flags, flags);
    g.flags_dev = flags;
}

static void bind_texture(int stage, u32 value)
{
    u32 h = value & 0xFFFF;
    g.stage_tex_value[stage] = (s32)value;
    if (h >= 1 && h < SF_TEX_MAX && s_tex[h].used) {
        g.stage_tex[stage] = (u16)h;
    }
    if (value & 0xFFFF0000) {
        g.pal = (u16)(value >> 16);
    }
}

static void set_mapped(s32 state, u32 value)
{
    union {
        u32 u;
        f32 f;
    } fv;
    switch (state) {
    case 4:
    case 5:
    case 6:
    case 7:
        bind_texture(state - 4, value);
        break;
    case 8:
    case 9:
    case 0xA:
    case 0xB:
        g.stage_mode[state - 8] = (s32)value;
        tss_set((u32)(state - 8), TSS_COLOROP, value == 1 ? 4 : value == 2 ? 7 : 1);
        break;
    case 0xD:
        switch (value & 0xC00) {
        case 0:
            rs_set(RS_BLENDOP, 0x8006);
            break;
        case 0x400:
            rs_set(RS_BLENDOP, 0x800A);
            break;
        case 0x800:
            rs_set(RS_BLENDOP, 0x800B);
            break;
        }
        break;
    case 0x10:
    case 0x11:
        fv.u = value;
        (void)fv.f; /* fog start and end */
        break;
    case 0x12:
        if (value == 0) {
            rs_set(RS_FOGENABLE, 0);
        } else if (value == 0x8000) {
            rs_set(RS_FOGENABLE, 1);
        }
        break;
    case 0x14:
        g.list.clear = value;
        break;
    case 0x5E:
        if ((value & 0xF) < 10) {
            rs_set(RS_SRCBLEND, blend_factor(value & 0xF));
        }
        if (((value >> 4) & 0xF) < 10) {
            rs_set(RS_DESTBLEND, blend_factor((value >> 4) & 0xF));
        }
        break;
    case 0x60:
        rs_set(RS_ALPHAREF, value);
        break;
    case 0x65:
        if (value != (u32)-1) {
            sf_port_missing("flSetRenderState(0x65): drawing into a texture");
        }
        break;
    default:
        /* 0xE, 0xF, 0x15, 0x16-0x19, 0x67, 0x68: the transform, the
         * viewport and colour states the 3D path uses; quads ignore them */
        break;
    }
}

s32 flSetRenderState(s32 func, u32 value)
{
    g.blend_next = g.blend;

    if ((u32)(func - 0x1A) <= 0x1F) {
        u32 bit = 1u << (func - 0x1A);
        if (value != 0) {
            g.matrix_set |= bit;
        } else {
            g.matrix_set &= ~bit;
        }
        return 1;
    }
    if ((u32)(func - 0x3A) <= 0x1F || (u32)(func - 0x5A) <= 2) {
        return 1; /* material records and lights: the 3D path */
    }

    switch (func) {
    case 0:
        g.flags = (g.flags & ~0x60u) | value;
        break;
    case 1:
        g.flags &= ~0xF00u;
        if (value >= 0x100 && value <= 0x800 && (value & 0xFF) == 0) {
            g.flags |= value;
        }
        break;
    case 2:
        g.flags &= ~0x1Cu;
        if (value >= 4 && value <= 0x18 && (value & 3) == 0) {
            g.flags |= value;
        }
        break;
    case 4: case 5: case 6: case 7: case 8: case 9: case 0xA: case 0xB:
    case 0xE: case 0xF: case 0x10: case 0x11: case 0x14: case 0x16: case 0x17: case 0x18: case 0x19:
    case 0x60: case 0x67: case 0x68:
        set_mapped(func, value);
        break;
    case 0xD:
        g.blend_next = (g.blend & ~0xC00u) | value;
        break;
    case 0x12:
        g.flags = (g.flags & ~0x18000u) | value;
        break;
    case 0x13:
        if (value == 0x100 || value == 0x200) {
            g.blend_next = g.blend | value;
        }
        break;
    case 0x15:
        g.flags &= ~3u;
        if (value == 2) {
            g.flags |= 2;
        }
        break;
    case 0x5D:
        g.flags &= 0xFFFFFF;
        g.flags |= value;
        break;
    case 0x5E:
        g.blend_next &= ~0xFFu;
        g.blend_next |= value;
        break;
    case 0x5F:
        g.blend_next = (g.blend & ~0x380000u) | (value << 19);
        break;
    case 0x61:
        g.blend_next = g.blend & ~0x1000000u;
        if (value != 0) {
            g.blend_next |= 0x1000000;
        }
        break;
    case 0x62:
        g.flags &= ~0x4000u;
        if (value != 0) {
            g.flags |= 0x4000;
        }
        break;
    case 0x63:
        g.blend_next = (g.blend & ~0x10000u) | value;
        break;
    case 0x64:
        g.blend_next = (g.blend & ~0x60000u) | value;
        break;
    case 0x66:
        g.flags &= ~0x20000u;
        if (value != 0) {
            g.flags |= 0x20000;
        }
        break;
    case 0x6B:
        g.blend_next = (g.blend & ~0xC00000u) | value;
        break;
    case 0x6C:
        g.blend_next = g.blend & ~0x8000u;
        if (value != 0) {
            g.blend_next |= 0x8000;
        }
        break;
    case 0x6D:
        g.blend_next = (g.blend & ~0x7000u) | (value << 12);
        break;
    }

    if (g.flags_dev != g.flags) {
        update_flags(g.flags);
    }
    if (g.blend != g.blend_next) {
        set_blend(g.blend, g.blend_next);
    }
    return 1;
}

/* ---- quads ---- */

static u8 blend_of(u32 d3d)
{
    switch (d3d) {
    case 0x0: return SF_BF_ZERO;
    case 0x1: return SF_BF_ONE;
    case 0x300: return SF_BF_SRC_COLOR;
    case 0x301: return SF_BF_INV_SRC_COLOR;
    case 0x302: return SF_BF_SRC_ALPHA;
    case 0x303: return SF_BF_INV_SRC_ALPHA;
    case 0x304: return SF_BF_DST_ALPHA;
    case 0x305: return SF_BF_INV_DST_ALPHA;
    case 0x306: return SF_BF_DST_COLOR;
    case 0x307: return SF_BF_INV_DST_COLOR;
    }
    return SF_BF_ONE;
}

static SfBlendKey blend_key(void)
{
    SfBlendKey k;
    memset(&k, 0, sizeof k);
    k.blend = g.rs[RS_ALPHABLENDENABLE] != 0;
    k.src = blend_of(g.rs[RS_SRCBLEND]);
    k.dst = blend_of(g.rs[RS_DESTBLEND]);
    k.op = g.rs[RS_BLENDOP] == 0x800A ? SF_BOP_SUB : g.rs[RS_BLENDOP] == 0x800B ? SF_BOP_REV_SUB : SF_BOP_ADD;
    k.zfunc = g.rs[RS_ZENABLE] ? (u8)(g.rs[RS_ZFUNC] & 7) : SF_CMP_ALWAYS;
    k.zwrite = g.rs[RS_ZENABLE] && g.rs[RS_ZWRITEENABLE];
    return k;
}

static SfShadeState shade_state(int textured)
{
    SfShadeState s;
    memset(&s, 0, sizeof s);
    s.tex = textured ? g.stage_tex[0] : 0;
    s.pal = g.pal;
    s.color_op = (u8)g.tss[0][TSS_COLOROP];
    s.alpha_op = (u8)g.tss[0][TSS_ALPHAOP];
    if (!s.tex) {
        s.color_op = SF_TOP_DISABLE;
        s.alpha_op = SF_TOP_DISABLE;
    }
    s.point = g.tss[0][TSS_MAGFILTER] == 1;
    s.address_u = (u8)g.tss[0][TSS_ADDRESSU];
    s.address_v = (u8)g.tss[0][TSS_ADDRESSV];
    s.texel_uv = s.tex && s_tex[s.tex].linear;
    s.alpha_test = g.rs[RS_ALPHATESTENABLE] != 0;
    s.alpha_func = (u8)(g.rs[RS_ALPHAFUNC] & 7);
    s.alpha_ref = (u8)g.rs[RS_ALPHAREF];
    return s;
}

static int s_quad_textured;

/* A 640x480 pixel is a whole square of the screen at any scale. D3D covers
 * pixel k when k (pixel centres are on integers) lies inside the quad, so
 * an edge at e covers from pixel ceil(e) (after NV2A's 1/16 snap); the
 * edge moves to that pixel's boundary, ceil(e) - 0.5. The texture
 * coordinates are then taken from the quad's own position-to-texture
 * mapping at the moved corners, so what is sampled where does not move:
 * a larger screen covers and samples exactly what 640x480 does. */
static float snap_edge(float e)
{
    return ceilf(floorf(e * 16.0f + 0.5f) / 16.0f) - 0.5f;
}

static void snap_quad(SfVertex *q)
{
    /* uv = a + (x - x0) * dx + (y - y0) * dy, from vertices 0, 1, 2 */
    float ex = q[1].x - q[0].x;
    float ey = q[1].y - q[0].y;
    float fx = q[2].x - q[0].x;
    float fy = q[2].y - q[0].y;
    float det = ex * fy - ey * fx;
    float dux;
    float duy;
    float dvx;
    float dvy;
    int i;
    if (fabsf(det) < 1e-6f) {
        return;
    }
    dux = ((q[1].u - q[0].u) * fy - (q[2].u - q[0].u) * ey) / det;
    duy = ((q[2].u - q[0].u) * ex - (q[1].u - q[0].u) * fx) / det;
    dvx = ((q[1].v - q[0].v) * fy - (q[2].v - q[0].v) * ey) / det;
    dvy = ((q[2].v - q[0].v) * ex - (q[1].v - q[0].v) * fx) / det;
    {
        float x0 = q[0].x;
        float y0 = q[0].y;
        float ua = q[0].u;
        float va = q[0].v;
        for (i = 0; i < 4; i++) {
            float nx = snap_edge(q[i].x);
            float ny = snap_edge(q[i].y);
            q[i].u = ua + (nx - x0) * dux + (ny - y0) * duy;
            q[i].v = va + (nx - x0) * dvx + (ny - y0) * dvy;
            q[i].x = nx;
            q[i].y = ny;
        }
    }
}

/* A mirrored sprite's texture rectangle back on its cell. The game biases
 * a mirrored chip's coordinates by -0.68 texel (-0.34 past 1.5 times its
 * size; MTRANS.c's chip setup), so its rectangle starts 0.68 into the cell
 * before it. Retail samples the bias, and drawn 1:1 no sample leaves the
 * cell; zoomed in, the edge pixels sample that texel of the next cell, and
 * a column or row of it shows through the sprite. Only that bias moves:
 * other fractions (a scroll) are texels the quad really covers. */
static void cell_rect(float *lo, float *hi, float size, int mirrored)
{
    float a = *lo * size;
    float b = *hi * size;
    float f = a - floorf(a);
    float w = floorf(b - a + 0.5f);
    float start;
    if (!mirrored || w < 1.0f || (fabsf(f - 0.32f) > 0.002f && fabsf(f - 0.66f) > 0.002f)) {
        return;
    }
    start = ceilf(a);
    *lo = start / size;
    *hi = (start + w) / size;
}

static void push_quad(const SfVertex v[4])
{
    SfDrawList *l = &g.list;
    SfBlendKey bk = blend_key();
    SfShadeState ss = shade_state(s_quad_textured);
    SfBatch *b = l->b_n ? &l->b[l->b_n - 1] : NULL;
    if (l->v_n + 4 > l->v_cap) {
        l->v_cap = l->v_cap ? l->v_cap * 2 : 4096;
        l->v = realloc(l->v, l->v_cap * sizeof *l->v);
    }
    memcpy(&l->v[l->v_n], v, 4 * sizeof *v);
    {
        SfVertex *q = &l->v[l->v_n];
        float u0 = q[0].u;
        float u1 = q[0].u;
        float v0 = q[0].v;
        float v1 = q[0].v;
        int i;
        for (i = 1; i < 4; i++) {
            u0 = q[i].u < u0 ? q[i].u : u0;
            u1 = q[i].u > u1 ? q[i].u : u1;
            v0 = q[i].v < v0 ? q[i].v : v0;
            v1 = q[i].v > v1 ? q[i].v : v1;
        }
        if (ss.tex) {
            /* mirrored: the coordinate falls as the screen position rises */
            float su = ss.texel_uv ? 1.0f : (float)l->tex[ss.tex].w;
            float sv = ss.texel_uv ? 1.0f : (float)l->tex[ss.tex].h;
            float du = fabsf(q[1].x - q[0].x) > fabsf(q[2].x - q[0].x) ? (q[1].u - q[0].u) / (q[1].x - q[0].x)
                                                                      : (q[2].u - q[0].u) / (q[2].x - q[0].x);
            float dv = fabsf(q[1].y - q[0].y) > fabsf(q[2].y - q[0].y) ? (q[1].v - q[0].v) / (q[1].y - q[0].y)
                                                                      : (q[2].v - q[0].v) / (q[2].y - q[0].y);
            if (su > 0.0f && sv > 0.0f) {
                cell_rect(&u0, &u1, su, du < 0.0f);
                cell_rect(&v0, &v1, sv, dv < 0.0f);
            }
        }
        for (i = 0; i < 4; i++) {
            q[i].u0 = u0;
            q[i].v0 = v0;
            q[i].u1 = u1;
            q[i].v1 = v1;
        }
        snap_quad(q);
    }
    if (!b || memcmp(&b->blend, &bk, sizeof bk) != 0 || memcmp(&b->shade, &ss, sizeof ss) != 0 ||
        b->clip[0] != g.clip[0] || b->clip[1] != g.clip[1] || b->clip[2] != g.clip[2] || b->clip[3] != g.clip[3]) {
        if (l->b_n == l->b_cap) {
            l->b_cap = l->b_cap ? l->b_cap * 2 : 256;
            l->b = realloc(l->b, l->b_cap * sizeof *l->b);
        }
        b = &l->b[l->b_n++];
        b->blend = bk;
        b->shade = ss;
        b->first = l->v_n;
        b->quads = 0;
        b->clip[0] = (int16_t)g.clip[0];
        b->clip[1] = (int16_t)g.clip[1];
        b->clip[2] = (int16_t)g.clip[2];
        b->clip[3] = (int16_t)g.clip[3];
    }
    b->quads++;
    l->v_n += 4;
}

static void vtx(SfVertex *v, f32 x, f32 y, f32 z, u32 c, f32 u, f32 t)
{
    v->x = x;
    v->y = y;
    v->z = z;
    v->color = c;
    v->u = u;
    v->v = t;
}

/* retail's xb_SaveQuadStates and xb_RestoreQuadStates */
static void save_quad_states(int textured)
{
    g.saved_light = (s32)g.rs[RS_LIGHTING];
    g.saved_light_on = g.saved_light == 1;
    if (g.saved_light_on) {
        rs_set(RS_LIGHTING, 0);
    }
    g.saved_fog = (s32)g.rs[RS_FOGENABLE];
    g.saved_fog_on = g.saved_fog == 1;
    if (g.saved_fog_on) {
        rs_set(RS_FOGENABLE, 0);
    }
    if (!textured) {
        g.saved_colorop = g.tss[0][TSS_COLOROP];
        g.saved_alphaop = g.tss[0][TSS_ALPHAOP];
        tss_set(0, TSS_COLOROP, 1);
        tss_set(0, TSS_ALPHAOP, 1);
        g.saved_ops_on = 1;
    } else {
        g.saved_ops_on = 0;
    }
    s_quad_textured = textured;
}

static void restore_quad_states(void)
{
    if (g.saved_light_on) {
        rs_set(RS_LIGHTING, (u32)g.saved_light);
    }
    if (g.saved_fog_on) {
        rs_set(RS_FOGENABLE, (u32)g.saved_fog);
    }
    if (g.saved_ops_on) {
        tss_set(0, TSS_COLOROP, g.saved_colorop);
        tss_set(0, TSS_ALPHAOP, g.saved_alphaop);
    }
}

s32 ps2SeqsRenderQuadInit_A(void)
{
    save_quad_states(1);
    return 1;
}

s32 ps2SeqsRenderQuadInit_B(void)
{
    save_quad_states(0);
    return 1;
}

void ps2SeqsRenderQuadEnd(void)
{
    restore_quad_states();
}

/* The quads map the first vertex's z from -1..1 to 0..1, in place, as
 * retail does, and use it for all four vertices. */

void ps2SeqsRenderQuad_Ax(f32 *q)
{
    SfVertex v[4];
    u32 c;
    q[2] = (q[2] + 1.0f) / 2.0f;
    memcpy(&c, &q[10], sizeof c);
    vtx(&v[0], q[0], q[1], q[2], c, q[6], q[7]);
    vtx(&v[1], q[3], q[1], q[2], c, q[8], q[7]);
    vtx(&v[2], q[0], q[4], q[2], c, q[6], q[9]);
    vtx(&v[3], q[3], q[4], q[2], c, q[8], q[9]);
    push_quad(v);
}

void ps2SeqsRenderQuad_A2(Sprite *spr, u32 col)
{
    SfVertex v[4];
    f32 z;
    spr->v[0].z = (spr->v[0].z + 1.0f) / 2.0f;
    z = spr->v[0].z;
    vtx(&v[0], spr->v[0].x, spr->v[0].y, z, col, spr->t[0].s, spr->t[0].t);
    vtx(&v[1], spr->v[3].x, spr->v[0].y, z, col, spr->t[3].s, spr->t[0].t);
    vtx(&v[2], spr->v[0].x, spr->v[3].y, z, col, spr->t[0].s, spr->t[3].t);
    vtx(&v[3], spr->v[3].x, spr->v[3].y, z, col, spr->t[3].s, spr->t[3].t);
    push_quad(v);
}

void ps2SeqsRenderQuad_A(Sprite *spr, u32 col)
{
    SfVertex v[4];
    f32 z;
    int i;
    spr->v[0].z = (spr->v[0].z + 1.0f) / 2.0f;
    z = spr->v[0].z;
    for (i = 0; i < 4; i++) {
        vtx(&v[i], spr->v[i].x, spr->v[i].y, z, col, spr->t[i].s, spr->t[i].t);
    }
    push_quad(v);
}

void ps2SeqsRenderQuad_B(Quad *q, u32 col)
{
    SfVertex v[4];
    f32 z;
    int i;
    q->v[0].z = (q->v[0].z + 1.0f) / 2.0f;
    z = q->v[0].z;
    for (i = 0; i < 4; i++) {
        vtx(&v[i], q->v[i].x, q->v[i].y, z, col, 0.0f, 0.0f);
    }
    push_quad(v);
}

/* A flat rectangle: x0, y0, z, x1, y1, then the colour at +0x18. */
void xb_DrawColoredQuad(f32 *q)
{
    SfVertex v[4];
    u32 c;
    q[2] = (q[2] + 1.0f) / 2.0f;
    memcpy(&c, &q[6], sizeof c);
    vtx(&v[0], q[0], q[1], q[2], c, 0.0f, 0.0f);
    vtx(&v[1], q[3], q[1], q[2], c, 0.0f, 0.0f);
    vtx(&v[2], q[0], q[4], q[2], c, 0.0f, 0.0f);
    vtx(&v[3], q[3], q[4], q[2], c, 0.0f, 0.0f);
    push_quad(v);
}

/* ---- the screen ---- */

/* The part of the screen the window shows: retail's xb_SetViewportRect
 * sets the corners of the screen copy, xb_SetDisplayArgs where it lands. */
void xb_SetViewportRect(s32 x, s32 y, s32 w, s32 h)
{
    g.list.show[0] = x;
    g.list.show[1] = y;
    g.list.show[2] = w;
    g.list.show[3] = h;
}

void sf_video_set_offset(int x, int y)
{
    g.list.show_at[0] = x;
    g.list.show_at[1] = y;
}

s32 flLogOut(const char *fmt, ...)
{
    va_list ap;
    fputs("game: ", stderr);
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    if (fmt[0] && fmt[strlen(fmt) - 1] != '\n') {
        fputc('\n', stderr);
    }
    return 1;
}

/* The viewport matrix: scale by w/2 and -h/2, offset to the centre, depth
 * zmin..zmax (retail's xb_SetViewportRect4; the centre is w / 2 in
 * integers, as retail has it). */
void flmatMakeViewport(f32 *m, s32 x, s32 y, s32 w, s32 h, f32 zmin, f32 zmax)
{
    memset(m, 0, 16 * sizeof *m);
    m[0] = (f32)w / 2.0f;
    m[5] = (f32)-h / 2.0f;
    m[10] = zmax - zmin;
    m[12] = (f32)(w / 2) + (f32)x;
    m[13] = (f32)(h / 2) + (f32)y;
    m[14] = zmin;
    m[15] = 1.0f;
}

void xb_MatrixIdentity(f32 *m)
{
    int i;
    for (i = 0; i < 16; i++) {
        m[i] = (i % 5) == 0 ? 1.0f : 0.0f;
    }
}

/* retail's xb_InitVideoMemory, for the states it sets */
void sf_gfx_init(void)
{
    memset(&g, 0, sizeof g);
    g.list.clear = 0x80808080;
    /* D3D's defaults for what follows */
    rs_set(RS_SRCBLEND, 0x1);
    rs_set(RS_DESTBLEND, 0x0);
    rs_set(RS_BLENDOP, 0x8006);
    rs_set(RS_ZFUNC, 0x203);
    rs_set(RS_ZWRITEENABLE, 1);
    tss_set(0, TSS_MAGFILTER, 1);
    tss_set(0, TSS_MINFILTER, 1);
    tss_set(0, TSS_ADDRESSU, 1);
    tss_set(0, TSS_ADDRESSV, 1);
    /* retail's start-up states */
    rs_set(0x67, 0);
    rs_set(0x93, 0);
    rs_set(RS_ZENABLE, 1);
    rs_set(RS_ZWRITEENABLE, 1);
    rs_set(RS_ALPHATESTENABLE, 1);
    rs_set(RS_ALPHAFUNC, 0x205);
    rs_set(RS_ALPHAREF, 0);
    tss_set(0, TSS_COLOROP, 4);
    tss_set(0, 0xE, 2);
    tss_set(0, 0xF, 1);
    tss_set(0, TSS_ALPHAOP, 4);
    tss_set(0, 0x12, 2);
    tss_set(0, 0x13, 1);
    g.flags = 0x202;
    apply_flags(0xFFFFFFFFu, 0x202);
    g.flags_dev = 0x202;
    g.blend = 0;
    set_blend(0, 0x138B032);
    g.clip[0] = 0;
    g.clip[1] = 0;
    g.clip[2] = SF_SCREEN_W;
    g.clip[3] = SF_SCREEN_H;
    g.list.show[0] = 0;
    g.list.show[1] = 0;
    g.list.show[2] = 640;
    g.list.show[3] = 448;
    g.ready = 1;
}

/* After the backend drew the frame: the next one starts empty. */
void sf_gfx_frame_done(void)
{
    g.list.v_n = 0;
    g.list.b_n = 0;
    g.list.cp3 = 0;
}

/* The PS2-sized picture: what is drawn so far is copied from the screen's
 * rect onto the output (src/port/system.c decides when, as retail's
 * CP3toPS2Draw). */
void sf_gfx_copy_screen(int x0, int y0, int x1, int y1)
{
    g.list.cp3 = 1;
    g.list.cp3_at = g.list.b_n;
    g.list.cp3_rect[0] = x0;
    g.list.cp3_rect[1] = y0;
    g.list.cp3_rect[2] = x1;
    g.list.cp3_rect[3] = y1;
}

void sf_gfx_frame_ready(void)
{
    pal_publish();
}
