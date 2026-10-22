/* The matrix helpers the game's njScale, njTranslate and njCalcPoint use
 * (DC_Ghost.c), with D3DX's conventions: row vectors, a 4x4 row-major
 * matrix, out = a * b, a point transformed with w = 1 into four floats.
 * The output may be one of the inputs. */

#include "types.h"
#include "structs.h"

#include <math.h>
#include <string.h>

f32 *D3DXVec3Transform(f32 *out, const Vec3 *v, const MTX *m)
{
    f32 r[4];
    int c;
    for (c = 0; c < 4; c++) {
        r[c] = v->x * m->a[0][c] + v->y * m->a[1][c] + v->z * m->a[2][c] + m->a[3][c];
    }
    memcpy(out, r, sizeof r);
    return out;
}

MTX *D3DXMatrixMultiply(MTX *out, const MTX *a, const MTX *b)
{
    MTX r;
    int i;
    int j;
    for (i = 0; i < 4; i++) {
        for (j = 0; j < 4; j++) {
            r.a[i][j] = a->a[i][0] * b->a[0][j] + a->a[i][1] * b->a[1][j] + a->a[i][2] * b->a[2][j] +
                        a->a[i][3] * b->a[3][j];
        }
    }
    *out = r;
    return out;
}

MTX *D3DXMatrixScaling(MTX *out, f32 x, f32 y, f32 z)
{
    memset(out, 0, sizeof *out);
    out->a[0][0] = x;
    out->a[1][1] = y;
    out->a[2][2] = z;
    out->a[3][3] = 1.0f;
    return out;
}

MTX *D3DXMatrixTranslation(MTX *out, f32 x, f32 y, f32 z)
{
    memset(out, 0, sizeof *out);
    out->a[0][0] = 1.0f;
    out->a[1][1] = 1.0f;
    out->a[2][2] = 1.0f;
    out->a[3][0] = x;
    out->a[3][1] = y;
    out->a[3][2] = z;
    out->a[3][3] = 1.0f;
    return out;
}

/* ---- atan2, the same on every machine ----
 *
 * The analog stick's direction (mlPAD.c, src/port/input.c) is a game
 * input: two machines playing each other must turn the same stick into
 * the same direction, and the C libraries' atan2 (glibc, bionic, msvcrt)
 * may differ in the last bit. This is fdlibm's (s_atan.c, e_atan2.c),
 * plain double arithmetic, which every IEEE machine computes alike (the
 * build fuses no multiply-add: -ffp-contract=off).
 *
 * ====================================================
 * Copyright (C) 1993 by Sun Microsystems, Inc. All rights reserved.
 *
 * Developed at SunPro, a Sun Microsystems, Inc. business.
 * Permission to use, copy, modify, and distribute this
 * software is freely granted, provided that this notice
 * is preserved.
 * ==================================================== */

#include <stdint.h>

static uint32_t hi_word(double x)
{
    uint64_t u;
    memcpy(&u, &x, sizeof u);
    return (uint32_t)(u >> 32);
}

static uint32_t lo_word(double x)
{
    uint64_t u;
    memcpy(&u, &x, sizeof u);
    return (uint32_t)u;
}

static const double k_atanhi[] = { 4.63647609000806093515e-01, 7.85398163397448278999e-01, 9.82793723247329054082e-01,
                                   1.57079632679489655800e+00 };
static const double k_atanlo[] = { 2.26987774529616870924e-17, 3.06161699786838301793e-17, 1.39033110312309984516e-17,
                                   6.12323399573676603587e-17 };
static const double k_at[] = { 3.33333333333329318027e-01,  -1.99999999998764832476e-01, 1.42857142725034663711e-01,
                               -1.11111104054623557880e-01, 9.09088713343650656196e-02,  -7.69187620504482999495e-02,
                               6.66107313738753120669e-02,  -5.83357013379057348645e-02, 4.97687799461593236017e-02,
                               -3.65315727442169155270e-02, 1.62858201153657823623e-02 };

static double sf_atan(double x)
{
    double w;
    double s1;
    double s2;
    double z;
    int32_t ix;
    int32_t hx;
    int id;
    hx = (int32_t)hi_word(x);
    ix = hx & 0x7fffffff;
    if (ix >= 0x44100000) { /* |x| >= 2^66 */
        if (ix > 0x7ff00000 || (ix == 0x7ff00000 && lo_word(x) != 0)) {
            return x + x; /* NaN */
        }
        return hx > 0 ? k_atanhi[3] + k_atanlo[3] : -k_atanhi[3] - k_atanlo[3];
    }
    if (ix < 0x3fdc0000) { /* |x| < 0.4375 */
        if (ix < 0x3e400000) {
            return x; /* |x| < 2^-27 */
        }
        id = -1;
    } else {
        x = x < 0 ? -x : x;
        if (ix < 0x3ff30000) { /* |x| < 1.1875 */
            if (ix < 0x3fe60000) {
                id = 0;
                x = (2.0 * x - 1.0) / (2.0 + x);
            } else {
                id = 1;
                x = (x - 1.0) / (x + 1.0);
            }
        } else if (ix < 0x40038000) { /* |x| < 2.4375 */
            id = 2;
            x = (x - 1.5) / (1.0 + 1.5 * x);
        } else {
            id = 3;
            x = -1.0 / x;
        }
    }
    z = x * x;
    w = z * z;
    s1 = z * (k_at[0] + w * (k_at[2] + w * (k_at[4] + w * (k_at[6] + w * (k_at[8] + w * k_at[10])))));
    s2 = w * (k_at[1] + w * (k_at[3] + w * (k_at[5] + w * (k_at[7] + w * k_at[9]))));
    if (id < 0) {
        return x - x * (s1 + s2);
    }
    z = k_atanhi[id] - ((x * (s1 + s2) - k_atanlo[id]) - x);
    return hx < 0 ? -z : z;
}

double sf_atan2(double y, double x)
{
    static const double pi_o_4 = 7.8539816339744827900e-01;
    static const double pi_o_2 = 1.5707963267948965580e+00;
    static const double pi = 3.1415926535897931160e+00;
    static const double pi_lo = 1.2246467991473531772e-16;
    double z;
    int32_t k;
    int32_t m;
    int32_t hx = (int32_t)hi_word(x);
    int32_t ix = hx & 0x7fffffff;
    uint32_t lx = lo_word(x);
    int32_t hy = (int32_t)hi_word(y);
    int32_t iy = hy & 0x7fffffff;
    uint32_t ly = lo_word(y);
    if ((uint32_t)ix > 0x7ff00000 || (ix == 0x7ff00000 && lx != 0) || (uint32_t)iy > 0x7ff00000 ||
        (iy == 0x7ff00000 && ly != 0)) {
        return x + y; /* NaN */
    }
    if (hx == 0x3ff00000 && lx == 0) {
        return sf_atan(y); /* x = 1.0 */
    }
    m = ((hy >> 31) & 1) | ((hx >> 30) & 2); /* 2 * sign(x) + sign(y) */
    if ((iy | (int32_t)ly) == 0) {
        switch (m) {
        case 0:
        case 1:
            return y; /* atan(+-0, +anything) = +-0 */
        case 2:
            return pi;
        default:
            return -pi;
        }
    }
    if ((ix | (int32_t)lx) == 0) {
        return hy < 0 ? -pi_o_2 : pi_o_2;
    }
    if (ix == 0x7ff00000) {
        if (iy == 0x7ff00000) {
            switch (m) {
            case 0:
                return pi_o_4;
            case 1:
                return -pi_o_4;
            case 2:
                return 3.0 * pi_o_4;
            default:
                return -3.0 * pi_o_4;
            }
        }
        switch (m) {
        case 0:
            return 0.0;
        case 1:
            return -0.0;
        case 2:
            return pi;
        default:
            return -pi;
        }
    }
    if (iy == 0x7ff00000) {
        return hy < 0 ? -pi_o_2 : pi_o_2;
    }
    k = (iy - ix) >> 20;
    if (k > 60) { /* |y / x| > 2^60 */
        z = pi_o_2 + 0.5 * pi_lo;
        m &= 1;
    } else if (hx < 0 && k < -60) {
        z = 0.0; /* 0 > |y| / x > -2^-60 */
    } else {
        z = sf_atan(fabs(y / x));
    }
    switch (m) {
    case 0:
        return z;
    case 1:
        return -z;
    case 2:
        return pi - (z - pi_lo);
    default:
        return (z - pi_lo) - pi;
    }
}
