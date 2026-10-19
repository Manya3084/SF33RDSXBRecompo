#include "game/pieces.h"

#include "common.h"
#include "game/fs.h"
#include "mem/heap.h"

#include <string.h>

/* texgrpdat group 61: first CG 35904, AFS 79 bg20.bin, texture table at 54492.
   title.bin (AFS 82) fills ColorRAM from row 300. The slogan uses row 8.
   The sketch uses row 10. Chips are Capcom P6, stored in the Dreamcast
   twiddle that ppgRenewDotDataSeqs undoes. The view is the 384x224 game
   screen. The slogan is baked at the hold camera (layer 2, h 319) and
   then slid. The sketch uses layer 1, which has not walked (h 320).
   A scaled chip is a filled quad. Each texel covers every screen pixel
   from its corner up to the next texel's corner. */

enum {
    SF_BG20 = 79,
    SF_TITLE_BIN = 82,
    SF_TEX_TABLE = 54492,
    SF_CG_BASE = 35904
};

struct piece {
    int16_t x;
    int16_t y;
    int16_t local;
};

/* eff48_data_tbl00. cg is 35976 + pattern, so local is cg - 35904. */
static const struct piece k_letters[21] = {
    {336, 112, 92}, {368, 112, 80}, {400, 112, 77}, {336, 80, 77},  {368, 80, 88},  {392, 80, 81},  {416, 80, 75},
    {464, 80, 92},  {496, 80, 90},  {520, 80, 81},  {544, 80, 84},  {576, 80, 87},  {608, 80, 79},  {640, 80, 97},
    {336, 48, 93},  {368, 48, 86},  {400, 48, 78},  {432, 48, 87},  {464, 48, 84},  {496, 48, 76},  {528, 48, 91},
};

static int16_t g_twiddle[1024];
static int g_twiddle_ready;

static uint32_t rd32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static uint16_t rd16(const uint8_t *p)
{
    return (uint16_t)(p[0] | (p[1] << 8));
}

static int16_t rd16s(const uint8_t *p)
{
    return (int16_t)rd16(p);
}

static void twiddle_init(void)
{
    static const int16_t seed[32] = {
        0x0000, 0x0002, 0x0008, 0x000A, 0x0020, 0x0022, 0x0028, 0x002A, 0x0080, 0x0082, 0x0088,
        0x008A, 0x00A0, 0x00A2, 0x00A8, 0x00AA, 0x0200, 0x0202, 0x0208, 0x020A, 0x0220, 0x0222,
        0x0228, 0x022A, 0x0280, 0x0282, 0x0288, 0x028A, 0x02A0, 0x02A2, 0x02A8, 0x02AA,
    };
    static const int16_t add[16] = {
        0x0000, 0x0004, 0x0010, 0x0014, 0x0040, 0x0044, 0x0050, 0x0054,
        0x0100, 0x0104, 0x0110, 0x0114, 0x0140, 0x0144, 0x0150, 0x0154,
    };
    int i;
    int j;
    if (g_twiddle_ready) {
        return;
    }
    for (i = 0; i < 16; i++) {
        for (j = 0; j < 32; j++) {
            g_twiddle[j + i * 64] = (int16_t)(seed[j] + add[i]);
        }
        for (j = 0; j < 32; j++) {
            g_twiddle[j + i * 64 + 32] = (int16_t)(g_twiddle[j + i * 64] + 1);
        }
    }
    g_twiddle_ready = 1;
}

static void untwiddle(const uint8_t *src, uint8_t *dst, int side)
{
    int row;
    int col;
    int n;
    if (side == 32) {
        n = 0;
        for (row = 0; row < 32; row++) {
            for (col = 0; col < 32; col++) {
                dst[row * 32 + col] = src[g_twiddle[n]];
                n++;
            }
        }
        return;
    }
    for (row = 0; row < side; row++) {
        for (col = 0; col < side; col++) {
            dst[row * side + col] = src[g_twiddle[col + (row << 5)]];
        }
    }
}

/* lz_ext_p6_fx. A copy that would pass the output length stops. */
static int lz_ext_p6(const uint8_t *src, const uint8_t *end, uint8_t *dst, uint32_t len)
{
    uint8_t *out = dst;
    uint8_t *stop = dst + len;
    while (out < stop) {
        uint32_t tmp;
        uint32_t flg;
        uint32_t count;
        uint8_t *from;
        if (src >= end) {
            return -1;
        }
        tmp = *src++;
        switch (tmp & 0xC0u) {
        case 0:
            *out++ = (uint8_t)tmp;
            break;
        case 0x40:
            tmp &= 0x3Fu;
            from = out - (tmp >> 2) - 1;
            count = (tmp & 3u) + 2u;
            if (from < dst) {
                return -1;
            }
            while (count > 0 && out < stop) {
                *out++ = *from++;
                count--;
            }
            break;
        case 0x80:
            if (src >= end) {
                return -1;
            }
            tmp = ((tmp & 0x3Fu) << 8) | *src++;
            from = out - (tmp >> 6) - 1;
            count = (tmp & 0x3Fu) + 2u;
            if (from < dst) {
                return -1;
            }
            while (count > 0 && out < stop) {
                *out++ = *from++;
                count--;
            }
            break;
        default:
            flg = tmp & 0x30u;
            count = (tmp & 0xFu) + 2u;
            while (count > 0 && out < stop) {
                uint8_t b;
                if (src >= end) {
                    return -1;
                }
                b = *src++;
                *out++ = (uint8_t)(flg | (b >> 4));
                if (out >= stop) {
                    break;
                }
                *out++ = (uint8_t)(flg | (b & 0xFu));
                count--;
            }
            break;
        }
    }
    return 0;
}

static int chip_linear(const uint8_t *bg, uint32_t bg_n, int code, uint8_t *raw, uint8_t *linear, int *side_out, int *dw, int *dh)
{
    uint32_t off;
    uint32_t at;
    unsigned wh;
    unsigned klass;
    unsigned side;
    unsigned size;
    if (code < 0 || (uint32_t)SF_TEX_TABLE + (uint32_t)code * 4u + 4u > bg_n) {
        return -1;
    }
    off = rd32(bg + SF_TEX_TABLE + (uint32_t)code * 4u);
    at = (uint32_t)SF_TEX_TABLE + off;
    if (at >= bg_n) {
        return -1;
    }
    wh = bg[at];
    klass = (wh & 3u) + 1u;
    if (klass != 1u && klass != 2u && klass != 4u) {
        return -1;
    }
    side = klass * 8u;
    size = (klass * klass) << 6;
    twiddle_init();
    if (lz_ext_p6(bg + at + 1, bg + bg_n, raw, size) != 0) {
        return -1;
    }
    untwiddle(raw, linear, (int)side);
    *side_out = (int)side;
    *dw = (int)((wh & 0xE0u) >> 2);
    *dh = (int)((wh & 0x1Cu) * 2u);
    if (*dw > (int)side) {
        *dw = (int)side;
    }
    if (*dh > (int)side) {
        *dh = (int)side;
    }
    return 0;
}

int sf_pieces_linear(const uint8_t *bg, uint32_t bg_size, int code, uint8_t *dst, int dst_n, int *side)
{
    uint8_t raw[1024];
    uint8_t linear[1024];
    int got;
    int dw;
    int dh;
    if (!bg || !dst || !side || dst_n <= 0) {
        return -1;
    }
    if (chip_linear(bg, bg_size, code, raw, linear, &got, &dw, &dh) != 0) {
        return -1;
    }
    if (dst_n < got * got) {
        return -1;
    }
    memcpy(dst, linear, (size_t)got * (size_t)got);
    *side = got;
    return 0;
}

static uint8_t expand5(unsigned channel)
{
    channel &= 31u;
    return (uint8_t)((channel << 3) | (channel >> 2));
}

static void put_px(uint8_t *rgba, int sx, int sy, const uint16_t *pal, unsigned idx)
{
    uint16_t v;
    uint8_t *p;
    if ((unsigned)sx >= (unsigned)SF_PIECE_W || (unsigned)sy >= (unsigned)SF_PIECE_H || idx >= 64u) {
        return;
    }
    v = pal[idx];
    if ((v & 0x8000u) == 0) {
        return;
    }
    p = rgba + ((size_t)sy * (size_t)SF_PIECE_W + (size_t)sx) * 4u;
    p[0] = expand5(v >> 10);
    p[1] = expand5(v >> 5);
    p[2] = expand5(v);
    p[3] = 255;
}

static void draw_cel(uint8_t *rgba, const uint8_t *bg, uint32_t bg_n, int local, int world_x, int world_y, int shift_h, int shift_v, int scale_num, const uint16_t *pal)
{
    const uint8_t *ents;
    uint32_t off;
    int count;
    int i;
    int x;
    int y;
    double scale;
    if (local < 0 || (uint32_t)local * 4u + 4u > bg_n) {
        return;
    }
    off = rd32(bg + (uint32_t)local * 4u);
    if (off + 2u > bg_n) {
        return;
    }
    count = (int)rd16(bg + off);
    if (count < 0 || off + 2u + (uint32_t)count * 8u > bg_n) {
        return;
    }
    ents = bg + off + 2u;
    x = 0;
    y = 0;
    scale = (double)scale_num / 64.0;
    for (i = 0; i < count; i++) {
        const uint8_t *e = ents + (size_t)i * 8u;
        int16_t tx = rd16s(e);
        int16_t ty = rd16s(e + 2);
        unsigned attr = rd16(e + 4);
        int code = (int)rd16(e + 6);
        uint8_t raw[1024];
        uint8_t linear[1024];
        int side;
        int dw;
        int dh;
        int row;
        int col;
        x -= tx;
        y += ty;
        if (chip_linear(bg, bg_n, code, raw, linear, &side, &dw, &dh) != 0 || dw <= 0 || dh <= 0) {
            continue;
        }
        for (row = 0; row < dh; row++) {
            for (col = 0; col < dw; col++) {
                int u = (attr & 0x8000u) ? dw - 1 - col : col;
                int v = (attr & 0x4000u) ? dh - 1 - row : row;
                int sx0 = (int)((double)(x + col) * scale + (double)world_x - (double)shift_h);
                int sx1 = (int)((double)(x + col + 1) * scale + (double)world_x - (double)shift_h);
                int sy0 = (int)(224.0 - ((double)(y - row) * scale + (double)world_y - (double)shift_v));
                int sy1 = (int)(224.0 - ((double)(y - row - 1) * scale + (double)world_y - (double)shift_v));
                int sx;
                int sy;
                unsigned idx = linear[(size_t)v * (size_t)side + (size_t)u];
                /* The corner lands on the same pixel as before. The span out to
                   the next corner is the rest of this texel's scaled quad, so a
                   scale such as 80/64 fills the pixels truncation used to skip. */
                for (sy = sy0; sy < sy1; sy++) {
                    for (sx = sx0; sx < sx1; sx++) {
                        put_px(rgba, sx, sy, pal, idx);
                    }
                }
            }
        }
    }
}

static int pal_row(const uint8_t *title, uint32_t title_n, int row, uint16_t *out)
{
    const uint8_t *p;
    int i;
    if ((uint32_t)(row + 1) * 128u > title_n) {
        return -1;
    }
    p = title + (size_t)row * 128u;
    for (i = 0; i < 64; i++) {
        out[i] = rd16(p + (size_t)i * 2u);
    }
    return 0;
}

static size_t piece_bytes(void)
{
    return (size_t)SF_PIECE_W * (size_t)SF_PIECE_H * 4u;
}

int sf_pieces_layers(const uint8_t *bg, uint32_t bg_size, const uint8_t *title, uint32_t title_size, uint8_t *sketch, uint8_t *slogan, char *err, size_t err_n)
{
    uint16_t row8[64];
    uint16_t row10[64];
    int i;
    if (!bg || !title || !sketch || !slogan) {
        sf_set_err(err, err_n, "opening pieces need bg20.bin and title.bin");
        return -1;
    }
    if (bg_size < (uint32_t)SF_TEX_TABLE + 4u || pal_row(title, title_size, 8, row8) != 0 || pal_row(title, title_size, 10, row10) != 0) {
        sf_set_err(err, err_n, "bg20.bin or title.bin is short");
        return -1;
    }
    memset(sketch, 0, piece_bytes());
    memset(slogan, 0, piece_bytes());
    draw_cel(sketch, bg, bg_size, 0, 512, 16, 320, 16, 80, row10);
    for (i = 0; i < 21; i++) {
        draw_cel(slogan, bg, bg_size, k_letters[i].local, k_letters[i].x, k_letters[i].y, 319, 16, 64, row8);
    }
    return 0;
}

static void blit_layer(const uint8_t *src, int dx, int dy, uint8_t *dst)
{
    int y;
    int x;
    if (!src || dx >= SF_PIECE_W || dx <= -SF_PIECE_W || dy >= SF_PIECE_H || dy <= -SF_PIECE_H) {
        return;
    }
    for (y = 0; y < SF_PIECE_H; y++) {
        int ty = y + dy;
        int x0;
        int copy;
        const uint8_t *row;
        if ((unsigned)ty >= (unsigned)SF_PIECE_H) {
            continue;
        }
        x0 = dx < 0 ? -dx : 0;
        copy = SF_PIECE_W - (dx < 0 ? -dx : dx);
        row = src + ((size_t)y * (size_t)SF_PIECE_W + (size_t)x0) * 4u;
        for (x = 0; x < copy; x++) {
            const uint8_t *s = row + (size_t)x * 4u;
            uint8_t *d;
            if (s[3] == 0) {
                continue;
            }
            d = dst + ((size_t)ty * (size_t)SF_PIECE_W + (size_t)(x0 + x + dx)) * 4u;
            d[0] = s[0];
            d[1] = s[1];
            d[2] = s[2];
            d[3] = s[3];
        }
    }
}

void sf_pieces_compose(const uint8_t *sketch, const uint8_t *slogan, int shift_x, int shift_y, uint8_t *rgba)
{
    size_t n = piece_bytes();
    if (!rgba) {
        return;
    }
    memset(rgba, 0, n);
    blit_layer(sketch, 0, shift_y, rgba);
    blit_layer(slogan, shift_x, 0, rgba);
}

int sf_pieces_next(const uint8_t *bg, uint32_t bg_size, const uint8_t *title, uint32_t title_size, uint8_t *rgba, char *err, size_t err_n)
{
    uint16_t row24[64];
    if (!bg || !title || !rgba) {
        sf_set_err(err, err_n, "the second sketch needs bg20.bin and title.bin");
        return -1;
    }
    if (bg_size < (uint32_t)SF_TEX_TABLE + 4u || pal_row(title, title_size, 24, row24) != 0) {
        sf_set_err(err, err_n, "bg20.bin or title.bin is short");
        return -1;
    }
    memset(rgba, 0, piece_bytes());
    draw_cel(rgba, bg, bg_size, 0, 512, -224, 320, 16, 128, row24);
    return 0;
}

/* op_bg2_0000 stores layer 2 as 16.16. The hold disp is 511 (h 319).
   Each later step drops the disp by 1, which moves the slogan right by 1. */
int sf_slogan_shift(int32_t layer_x)
{
    int disp = (int16_t)((uint32_t)layer_x >> 16);
    return 511 - disp;
}

int sf_pieces_view(const uint8_t *bg, uint32_t bg_size, const uint8_t *title, uint32_t title_size, uint8_t *rgba, char *err, size_t err_n)
{
    uint8_t *sketch;
    uint8_t *slogan;
    if (!rgba) {
        sf_set_err(err, err_n, "opening pieces need a buffer");
        return -1;
    }
    sketch = sf_alloc(piece_bytes());
    slogan = sf_alloc(piece_bytes());
    if (!sketch || !slogan) {
        sf_free(sketch);
        sf_free(slogan);
        sf_set_err(err, err_n, "the opening pieces did not allocate");
        return -1;
    }
    if (sf_pieces_layers(bg, bg_size, title, title_size, sketch, slogan, err, err_n) != 0) {
        sf_free(sketch);
        sf_free(slogan);
        return -1;
    }
    sf_pieces_compose(sketch, slogan, 0, 0, rgba);
    sf_free(sketch);
    sf_free(slogan);
    return 0;
}

int sf_pieces_load(const char *install, uint8_t **sketch, uint8_t **slogan, uint8_t **next, char *err, size_t err_n)
{
    SfFsFile bg;
    SfFsFile title;
    uint8_t *sketch_px;
    uint8_t *slogan_px;
    uint8_t *next_px;
    if (sketch) {
        *sketch = NULL;
    }
    if (slogan) {
        *slogan = NULL;
    }
    if (next) {
        *next = NULL;
    }
    if (!install || !sketch || !slogan || !next) {
        sf_set_err(err, err_n, "opening pieces need a buffer");
        return -1;
    }
    memset(&bg, 0, sizeof bg);
    memset(&title, 0, sizeof title);
    if (fsOpen(install, SF_BG20, &bg, err, err_n) != 0 || !bg.data || strcmp(bg.name, "bg20.bin") != 0) {
        fsClose(&bg);
        sf_set_err(err, err_n, "bg20.bin did not open");
        return -1;
    }
    if (fsOpen(install, SF_TITLE_BIN, &title, err, err_n) != 0 || !title.data || strcmp(title.name, "title.bin") != 0) {
        fsClose(&bg);
        fsClose(&title);
        sf_set_err(err, err_n, "title.bin did not open");
        return -1;
    }
    sketch_px = sf_alloc(piece_bytes());
    slogan_px = sf_alloc(piece_bytes());
    next_px = sf_alloc(piece_bytes());
    if (!sketch_px || !slogan_px || !next_px) {
        sf_free(sketch_px);
        sf_free(slogan_px);
        sf_free(next_px);
        fsClose(&bg);
        fsClose(&title);
        sf_set_err(err, err_n, "the opening pieces did not allocate");
        return -1;
    }
    if (sf_pieces_layers(bg.data, bg.size, title.data, title.size, sketch_px, slogan_px, err, err_n) != 0 || sf_pieces_next(bg.data, bg.size, title.data, title.size, next_px, err, err_n) != 0) {
        sf_free(sketch_px);
        sf_free(slogan_px);
        sf_free(next_px);
        fsClose(&bg);
        fsClose(&title);
        return -1;
    }
    fsClose(&bg);
    fsClose(&title);
    *sketch = sketch_px;
    *slogan = slogan_px;
    *next = next_px;
    return 0;
}
