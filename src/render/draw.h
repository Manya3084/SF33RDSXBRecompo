#ifndef SF_RENDER_DRAW_H
#define SF_RENDER_DRAW_H

/* The game's frame as the renderer sees it: textures, palettes and a list
 * of screen-space quads with the state each was drawn with. src/port/gfx.c
 * fills it from the game's calls; a backend draws it into the 640x480
 * screen and shows the game's part of that screen in the window. */

#include <stdint.h>

#define SF_TEX_MAX 256
#define SF_PAL_MAX 1088
#define SF_PAL_ENTRIES 256
#define SF_SCREEN_W 640
#define SF_SCREEN_H 480

/* A texture, kept as RGBA8. An indexed texture keeps its index in R. */
typedef struct SfTexture {
    int w;
    int h;
    int indexed;
    uint8_t *rgba;
    uint32_t version;
} SfTexture;

/* A palette, 256 entries of RGBA8. */
typedef struct SfPalette {
    uint8_t rgba[SF_PAL_ENTRIES * 4];
    uint32_t version;
} SfPalette;

/* Blend factors and ops, compare functions, texture ops: Vulkan-neutral. */
enum { SF_BF_ZERO, SF_BF_ONE, SF_BF_SRC_COLOR, SF_BF_INV_SRC_COLOR, SF_BF_SRC_ALPHA, SF_BF_INV_SRC_ALPHA,
       SF_BF_DST_ALPHA, SF_BF_INV_DST_ALPHA, SF_BF_DST_COLOR, SF_BF_INV_DST_COLOR, SF_BF_COUNT };
enum { SF_BOP_ADD, SF_BOP_SUB, SF_BOP_REV_SUB };
enum { SF_CMP_NEVER, SF_CMP_LESS, SF_CMP_EQUAL, SF_CMP_LEQUAL, SF_CMP_GREATER, SF_CMP_NOTEQUAL, SF_CMP_GEQUAL,
       SF_CMP_ALWAYS };
/* Stage 0 colour and alpha ops (D3DTOP numbering, as the shader takes it). */
enum { SF_TOP_DISABLE = 1, SF_TOP_SELECTARG1 = 2, SF_TOP_SELECTARG2 = 3, SF_TOP_MODULATE = 4, SF_TOP_ADD = 7 };
enum { SF_ADDR_WRAP = 1, SF_ADDR_MIRROR = 2, SF_ADDR_CLAMP = 3 };

/* The pipeline part of the state: what a backend bakes into a pipeline. */
typedef struct SfBlendKey {
    uint8_t blend;
    uint8_t src;
    uint8_t dst;
    uint8_t op;
    uint8_t zfunc;
    uint8_t zwrite;
    uint8_t pad[2];
} SfBlendKey;

/* The shader part of the state. tex is 0 when nothing is bound. */
typedef struct SfShadeState {
    uint16_t tex;
    uint16_t pal;
    uint8_t color_op;
    uint8_t alpha_op;
    uint8_t point;
    uint8_t address_u;
    uint8_t address_v;
    uint8_t texel_uv;
    uint8_t alpha_test;
    uint8_t alpha_func;
    uint8_t alpha_ref;
    uint8_t pad[3];
} SfShadeState;

typedef struct SfVertex {
    float x;
    float y;
    float z;
    uint32_t color; /* D3DCOLOR: 0xAARRGGBB */
    float u;
    float v;
    /* the quad's texture rectangle (its smallest and largest u and v):
     * filtering stays inside it, as it does when the quad is drawn 1:1 */
    float u0;
    float v0;
    float u1;
    float v1;
} SfVertex;

/* A run of quads (4 vertices each, strip order 0 1 2 3) with one state. */
typedef struct SfBatch {
    SfBlendKey blend;
    SfShadeState shade;
    uint32_t first;
    uint32_t quads;
    int16_t clip[4]; /* x0, y0, x1, y1 */
} SfBatch;

typedef struct SfDrawList {
    SfTexture tex[SF_TEX_MAX];
    SfPalette pal[SF_PAL_MAX];
    SfVertex *v;
    uint32_t v_n;
    uint32_t v_cap;
    SfBatch *b;
    uint32_t b_n;
    uint32_t b_cap;
    uint32_t clear;   /* D3DCOLOR the screen starts each frame with */
    int show[4];      /* the screen's x0, y0, x1, y1 blended onto the output */
    int show_at[2];   /* where that lands on the 640x480 output (y adds 16) */
    /* The game's PS2-sized picture: once the batches before cp3_at are
     * drawn, the screen's cp3_rect is copied, stretched and opaque, onto
     * the output, and the screen starts again from clear (retail's
     * CP3toPS2Draw). */
    int cp3;
    uint32_t cp3_at;
    int cp3_rect[4];
} SfDrawList;

#endif
