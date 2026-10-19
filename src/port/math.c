/* The matrix helpers the game's njScale, njTranslate and njCalcPoint use
 * (DC_Ghost.c), with D3DX's conventions: row vectors, a 4x4 row-major
 * matrix, out = a * b, a point transformed with w = 1 into four floats.
 * The output may be one of the inputs. */

#include "types.h"
#include "structs.h"

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
