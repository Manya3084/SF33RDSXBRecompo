/* The game's state as one number, the same on every machine that runs the
 * same game: two netplay peers send it to each other to see they agree,
 * and a PC and a phone run over one recording print it frame by frame to
 * see they play the same game (SF_SYNC_LOG).
 *
 * It is made of the state that decides what happens next and holds no
 * address (which differs from one build, or one run, to the next): the
 * random tables' places, the clock, and each player's facing, state,
 * position, speed, health, animation and super-art gauge. A game that
 * goes another way shows in these within a frame or two. */

#include "port/port.h"

#include "types.h"
#include "structs.h"

#include <SDL3/SDL.h>

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

extern PLW plw[2];
extern u16 Game_timer;
extern s16 Random_ix16;
extern s16 Random_ix32;
extern s16 Random_ix16_ex;
extern s16 Random_ix32_ex;
extern s16 Random_ix16_com;
extern s16 Random_ix32_com;
extern s16 Random_ix16_ex_com;
extern s16 Random_ix32_ex_com;
extern s16 Random_ix16_bg;

static uint32_t mix(uint32_t h, const void *p, size_t n)
{
    const uint8_t *b = p;
    size_t i;
    for (i = 0; i < n; i++) {
        h = (h ^ b[i]) * 16777619u; /* FNV-1a */
    }
    return h;
}

#define MIX(h, v) ((h) = mix((h), &(v), sizeof(v)))

uint32_t sf_sync_hash(void)
{
    uint32_t h = 2166136261u;
    int i;
    MIX(h, Random_ix16);
    MIX(h, Random_ix32);
    MIX(h, Random_ix16_ex);
    MIX(h, Random_ix32_ex);
    MIX(h, Random_ix16_com);
    MIX(h, Random_ix32_com);
    MIX(h, Random_ix16_ex_com);
    MIX(h, Random_ix32_ex_com);
    MIX(h, Random_ix16_bg);
    MIX(h, Game_timer);
    for (i = 0; i < 2; i++) {
        const WORK *w = &plw[i].wu;
        MIX(h, w->rl_flag);
        MIX(h, w->routine_no);
        MIX(h, w->xyz);
        MIX(h, w->mvxy);
        MIX(h, w->vitality);
        MIX(h, w->vital_new);
        MIX(h, w->dm_vital);
        MIX(h, w->cg_ix);
        MIX(h, w->now_koc);
        MIX(h, w->char_index);
        MIX(h, w->cg_ctr);
        MIX(h, w->cg_number);
        if (plw[i].sa) {
            MIX(h, plw[i].sa->gauge_len);
            MIX(h, plw[i].sa->total_gauge);
        }
    }
    return h;
}

/* SF_SYNC_TEST=PATH: another machine's hashes (sf_sync_pack: "SF3S", the
 * first frame and the count, then a hash a frame, little-endian), checked
 * frame by frame, and the result said in a dialog. With SF_REPLAY of the
 * recording that machine ran, it shows whether this one plays the same
 * game. Only a frame in 30 is drawn meanwhile, and none is waited for. */
static struct {
    int state; /* 0 not read, 1 running, 2 done, -1 off */
    uint32_t first;
    uint32_t count;
    uint32_t *hash;
    uint32_t checked;
} s_test;

static void test_start(void)
{
    const char *path = getenv("SF_SYNC_TEST");
    size_t n = 0;
    uint8_t *d;
    s_test.state = -1;
    if (!path) {
        return;
    }
    d = SDL_LoadFile(path, &n);
    if (!d || n < 12 || SDL_memcmp(d, "SF3S", 4) != 0) {
        fprintf(stderr, "sync: %s is not a hash list\n", path);
        SDL_free(d);
        return;
    }
    SDL_memcpy(&s_test.first, d + 4, 4);
    SDL_memcpy(&s_test.count, d + 8, 4);
    if ((size_t)s_test.count * 4 + 12 > n) {
        s_test.count = (uint32_t)((n - 12) / 4);
    }
    s_test.hash = malloc((size_t)s_test.count * 4);
    if (!s_test.hash) {
        SDL_free(d);
        return;
    }
    SDL_memcpy(s_test.hash, d + 12, (size_t)s_test.count * 4);
    SDL_free(d);
    s_test.state = 1;
    fprintf(stderr, "sync: checking %u frames from frame %u\n", s_test.count, s_test.first);
}

static void test_end(const char *msg)
{
    fprintf(stderr, "sync: %s\n", msg);
    SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_INFORMATION, "Sync test", msg, NULL);
    s_test.state = 2;
}

static void test_frame(uint32_t h)
{
    uint32_t f = (uint32_t)sf_port_frame;
    char msg[256];
    if (s_test.state == 0) {
        test_start();
    }
    if (s_test.state != 1 || f < s_test.first) {
        return;
    }
    if (f - s_test.first >= s_test.count) {
        snprintf(msg, sizeof msg, "The same game as the other machine: all %u frames from frame %u match.", s_test.checked,
                 s_test.first);
        test_end(msg);
        return;
    }
    if (s_test.hash[f - s_test.first] != h) {
        snprintf(msg, sizeof msg,
                 "This machine plays otherwise from frame %u (%u frames matched before it). Hash %08x, the other's %08x.",
                 f, s_test.checked, h, s_test.hash[f - s_test.first]);
        test_end(msg);
        return;
    }
    s_test.checked++;
}

/* A sync test is running: the frames are not drawn (but one in 30) nor
 * paced (src/port/video.c). */
int sf_sync_testing(void)
{
    if (s_test.state == 0) {
        test_start();
    }
    return s_test.state == 1;
}

/* SF_SYNC_LOG=PATH: the frame and its hash, a line a frame. */
void sf_sync_frame(void)
{
    static FILE *f;
    static int tried;
    if (!tried) {
        const char *path = getenv("SF_SYNC_LOG");
        tried = 1;
        f = path ? fopen(path, "w") : NULL;
    }
    uint32_t h = sf_sync_hash();
    if (f) {
        fprintf(f, "%d %08x\n", sf_port_frame, h);
    }
    test_frame(h);
}
