#include "game/aix.h"

#include "common.h"
#include "game/adx.h"
#include "mem/heap.h"

#include <string.h>

#define SF_AIX_BODY_MAX (64u * 1024u * 1024u)

static uint16_t be16(const uint8_t *p)
{
    return (uint16_t)((p[0] << 8) | p[1]);
}

static uint32_t be32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}

/* Copy track 0's payloads into one ADX. AIXP carries one piece. AIXE ends
   the stream. size in the packet is the byte count after the size field. */
static int take_track0(const uint8_t *file, uint32_t size, uint32_t start, uint8_t **body, uint32_t *body_n, char *err, size_t err_n)
{
    uint32_t off;
    uint32_t need;
    uint32_t filled;
    uint8_t *buf;
    off = start;
    need = 0;
    while (off + 16u <= size) {
        uint32_t id = be32(file + off);
        uint32_t psz = be32(file + off + 4);
        uint32_t total;
        unsigned len;
        if (psz > size || psz + 8u < psz) {
            sf_set_err(err, err_n, "aix packet");
            return -1;
        }
        total = psz + 8u;
        if (total < 16u || off > size - total) {
            sf_set_err(err, err_n, "aix packet");
            return -1;
        }
        if (id == 0x41495845u) {
            break;
        }
        if (id != 0x41495850u) {
            sf_set_err(err, err_n, "aix packet");
            return -1;
        }
        len = be16(file + off + 10);
        if (len > total - 16u) {
            sf_set_err(err, err_n, "aix packet");
            return -1;
        }
        if (file[off + 8] == 0) {
            if (need > SF_AIX_BODY_MAX - len) {
                sf_set_err(err, err_n, "aix memory");
                return -1;
            }
            need += len;
        }
        off += total;
    }
    if (need == 0) {
        sf_set_err(err, err_n, "aix track");
        return -1;
    }
    buf = sf_alloc(need);
    if (!buf) {
        sf_set_err(err, err_n, "aix memory");
        return -1;
    }
    off = start;
    filled = 0;
    while (off + 16u <= size && filled < need) {
        uint32_t id = be32(file + off);
        uint32_t total = be32(file + off + 4) + 8u;
        unsigned len = be16(file + off + 10);
        if (id == 0x41495845u) {
            break;
        }
        if (file[off + 8] == 0) {
            memcpy(buf + filled, file + off + 16, len);
            filled += len;
        }
        off += total;
    }
    if (filled != need) {
        sf_free(buf);
        sf_set_err(err, err_n, "aix packet");
        return -1;
    }
    *body = buf;
    *body_n = filled;
    return 0;
}

int sf_aix_decode(const uint8_t *file, uint32_t size, SfAix *out, char *err, size_t err_n)
{
    uint32_t hsize;
    uint8_t *body;
    uint32_t body_n;
    SfAdx adx;
    if (out) {
        memset(out, 0, sizeof *out);
    }
    if (!file || !out || size < 16u || be32(file) != 0x41495846u) {
        sf_set_err(err, err_n, "aix header");
        return -1;
    }
    hsize = be32(file + 4);
    if (hsize < 8u || hsize > size - 8u || memcmp(file + hsize + 2, "(c)CRI", 6) != 0) {
        sf_set_err(err, err_n, "aix header");
        return -1;
    }
    body = NULL;
    body_n = 0;
    if (take_track0(file, size, hsize + 8u, &body, &body_n, err, err_n) != 0) {
        return -1;
    }
    memset(&adx, 0, sizeof adx);
    if (sf_adx_decode(body, body_n, &adx, err, err_n) != 0) {
        sf_free(body);
        sf_adx_free(&adx);
        return -1;
    }
    sf_free(body);
    out->channels = adx.channels;
    out->rate = adx.rate;
    out->total = adx.total;
    out->loop = adx.loop;
    out->loop_start = adx.loop_start;
    out->loop_end = adx.loop_end;
    out->pcm = adx.pcm;
    adx.pcm = NULL;
    sf_adx_free(&adx);
    return 0;
}

void sf_aix_free(SfAix *aix)
{
    if (!aix) {
        return;
    }
    sf_free(aix->pcm);
    memset(aix, 0, sizeof *aix);
}
