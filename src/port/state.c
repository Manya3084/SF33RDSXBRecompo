/* The game's state, as one block that can be saved and put back: what
 * rollback netcode needs to run frames again from an earlier one.
 *
 * The state is the program's writable data (.data and .bss: the decomp's
 * globals, and the generated stubs' blocks) less the port's own sections
 * (sfport_data and sfport_bss: the window, renderer, textures, sound and
 * input devices, renamed at build in cmake/decomp.cmake), and the used
 * part of the arena (the game's heaps). Saving copies them, restoring
 * copies them back.
 *
 * SF_ROLLBACK_TEST=FRAME:K tests that the game runs frames again exactly:
 * at FRAME it saves, runs K frames hashing the state after each (and each
 * 4 KB page of it), puts the state back, runs the same K frames on the
 * same pads (the input tape) and compares. A difference names the frame
 * and the pages, by address, that came out otherwise. */

#include "port/port.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
/* PE has no __start_/__stop_ section bounds: the snapshot is Linux-only
 * for now, and on Windows it is empty. */
static char __data_start[1], _end[1];
static char __start_sfport_data[1], __stop_sfport_data[1], __start_sfport_bss[1], __stop_sfport_bss[1];
static char __start_sfkeep[1], __stop_sfkeep[1];
#else
extern char __data_start[];
extern char _end[];
extern char __start_sfport_data[];
extern char __stop_sfport_data[];
extern char __start_sfport_bss[];
extern char __stop_sfport_bss[];
extern char __start_sfkeep[];
extern char __stop_sfkeep[];
#endif

void sf_arena_extent(uint8_t **base, size_t *used);

#define REGIONS 32
#define PAGE 4096

typedef struct Region {
    uint8_t *p;
    size_t n;
} Region;

static Region s_reg[REGIONS] SF_KEEP;
static int s_reg_n SF_KEEP;

static void add_region(uint8_t *a, uint8_t *b)
{
    if (b > a && s_reg_n < REGIONS) {
        s_reg[s_reg_n].p = a;
        s_reg[s_reg_n].n = (size_t)(b - a);
        s_reg_n++;
    }
}

/* [lo, hi) less the port's sections, wherever the linker put them (with
 * SF_ROLLBACK_ALL, a test, less only sfkeep: the snapshot's own state) */
static void regions(void)
{
    static int all = -1;
    uint8_t *cut[4][2] = {
        { (uint8_t *)__start_sfport_data, (uint8_t *)__stop_sfport_data },
        { (uint8_t *)__start_sfport_bss, (uint8_t *)__stop_sfport_bss },
        { (uint8_t *)__start_sfkeep, (uint8_t *)__stop_sfkeep },
    };
    int ncut = 3;
    uint8_t *at = (uint8_t *)__data_start;
    uint8_t *end = (uint8_t *)_end;
    uint8_t *arena;
    size_t used;
    int k;
    s_reg_n = 0;
#ifdef _WIN32
    (void)cut;
    (void)ncut;
    (void)at;
    (void)end;
    (void)arena;
    (void)used;
    (void)k;
    (void)all;
    return;
#endif
    if (all < 0) {
        all = getenv("SF_ROLLBACK_ALL") != NULL;
    }
    if (all) {
        cut[0][0] = cut[2][0];
        cut[0][1] = cut[2][1];
        ncut = 1;
    }
    /* the cuts in address order */
    for (k = 0; k < ncut; k++) {
        int j;
        for (j = k + 1; j < ncut; j++) {
            if (cut[j][0] < cut[k][0]) {
                uint8_t *t0 = cut[k][0];
                uint8_t *t1 = cut[k][1];
                cut[k][0] = cut[j][0];
                cut[k][1] = cut[j][1];
                cut[j][0] = t0;
                cut[j][1] = t1;
            }
        }
    }
    for (k = 0; k < ncut; k++) {
        if (cut[k][0] >= at && cut[k][1] <= end) {
            add_region(at, cut[k][0]);
            at = cut[k][1];
        }
    }
    add_region(at, end);
    {
        /* SF_ROLLBACK_EXTRA=ADDR:LEN,...: port ranges a test takes in too */
        const char *e = getenv("SF_ROLLBACK_EXTRA");
        while (e && *e) {
            char *c;
            unsigned long long p0 = strtoull(e, &c, 0);
            unsigned long long n0 = *c == ':' ? strtoull(c + 1, &c, 0) : 0;
            add_region((uint8_t *)(uintptr_t)p0, (uint8_t *)(uintptr_t)(p0 + n0));
            e = *c == ',' ? c + 1 : c;
            if (c == e && *e != ',') {
                break;
            }
        }
    }
    sf_arena_extent(&arena, &used);
    add_region(arena, arena + used);
}

/* The memory regions, then the port subsystems the game reads back (the
 * music streams' place: sound.c). */
size_t sf_state_size(void)
{
    size_t n = sf_sound_state_size();
    int i;
    regions();
    for (i = 0; i < s_reg_n; i++) {
        n += s_reg[i].n;
    }
    return n;
}

void sf_state_save(uint8_t *buf)
{
    int i;
    regions();
    for (i = 0; i < s_reg_n; i++) {
        memcpy(buf, s_reg[i].p, s_reg[i].n);
        buf += s_reg[i].n;
    }
    sf_sound_state_save(buf);
}

void sf_state_restore(const uint8_t *buf)
{
    int i;
    regions();
    for (i = 0; i < s_reg_n; i++) {
        memcpy(s_reg[i].p, buf, s_reg[i].n);
        buf += s_reg[i].n;
    }
    sf_sound_state_restore(buf);
}

static uint64_t hash_bytes(const uint8_t *p, size_t n, uint64_t h)
{
    size_t i;
    for (i = 0; i + 8 <= n; i += 8) {
        uint64_t w;
        memcpy(&w, p + i, 8);
        h = (h ^ w) * 0x100000001B3ull;
        h ^= h >> 29;
    }
    for (; i < n; i++) {
        h = (h ^ p[i]) * 0x100000001B3ull;
    }
    return h;
}

/* ---- the test ---- */

static struct {
    int on;
    int from;
    int frames;
    int step; /* 0 before, 1 first run, 2 second run, 3 done */
    int at;
    uint8_t *saved;
    size_t size;
    size_t pages;
    uint64_t *page_hash; /* the first run's, frames by pages */
    uint64_t *hash;      /* the first run's, by frame */
    uint8_t *first;      /* the first run's state after its first frame */
    int bad;
} s_t SF_KEEP;

static void page_hashes(uint64_t *out)
{
    size_t k = 0;
    int i;
    for (i = 0; i < s_reg_n; i++) {
        size_t off;
        for (off = 0; off < s_reg[i].n; off += PAGE) {
            size_t n = s_reg[i].n - off < PAGE ? s_reg[i].n - off : PAGE;
            out[k++] = hash_bytes(s_reg[i].p + off, n, 0xCBF29CE484222325ull);
        }
    }
}

static uint64_t fold(const uint64_t *pages)
{
    return hash_bytes((const uint8_t *)pages, s_t.pages * sizeof *pages, 0xCBF29CE484222325ull);
}

/* the page k's address */
static uint8_t *page_addr(size_t k)
{
    int i;
    for (i = 0; i < s_reg_n; i++) {
        size_t n = (s_reg[i].n + PAGE - 1) / PAGE;
        if (k < n) {
            return s_reg[i].p + k * PAGE;
        }
        k -= n;
    }
    return NULL;
}

/* After each game frame. */
void sf_state_test_frame(void)
{
    const char *env;
    uint64_t *now;
    size_t i;
    if (s_t.step == 0) {
        env = getenv("SF_ROLLBACK_TEST");
        if (!env) {
            s_t.step = 3;
            return;
        }
        s_t.from = atoi(env);
        s_t.frames = strchr(env, ':') ? atoi(strchr(env, ':') + 1) : 60;
        if (s_t.frames < 1 || s_t.frames > 600) {
            s_t.frames = 60;
        }
        if (sf_port_frame < s_t.from) {
            return;
        }
        s_t.size = sf_state_size();
        s_t.saved = malloc(s_t.size);
        s_t.pages = 0;
        for (i = 0; i < (size_t)s_reg_n; i++) {
            s_t.pages += (s_reg[i].n + PAGE - 1) / PAGE;
        }
        s_t.page_hash = malloc(s_t.pages * sizeof *s_t.page_hash * (size_t)s_t.frames);
        s_t.hash = malloc(sizeof *s_t.hash * (size_t)s_t.frames);
        s_t.first = malloc(s_t.size);
        if (!s_t.saved || !s_t.page_hash || !s_t.hash || !s_t.first) {
            fprintf(stderr, "rollback test: no memory\n");
            exit(1);
        }
        sf_state_save(s_t.saved);
        sf_input_tape_mark();
        fprintf(stderr, "rollback test: saved %zu bytes (%zu pages) at frame %d\n", s_t.size, s_t.pages,
                sf_port_frame);
        s_t.step = 1;
        s_t.at = 0;
        return;
    }
    if (s_t.step == 3) {
        return;
    }
    now = s_t.page_hash + (size_t)s_t.at * s_t.pages;
    if (s_t.step == 1) {
        page_hashes(now);
        s_t.hash[s_t.at] = fold(now);
        if (s_t.at == 0) {
            sf_state_save(s_t.first);
        }
        if (s_t.at > 0) {
            size_t changed = 0;
            for (i = 0; i < s_t.pages; i++) {
                changed += now[i] != now[i - s_t.pages];
            }
            if (s_t.at == 1 || s_t.at == s_t.frames - 1) {
                fprintf(stderr, "rollback test: frame %d changed %zu pages (%zu KB)\n", s_t.at, changed,
                        changed * PAGE / 1024);
            }
        }
        if (++s_t.at == s_t.frames) {
            sf_state_restore(s_t.saved);
            sf_input_tape_rewind();
            s_t.step = 2;
            s_t.at = 0;
            fprintf(stderr, "rollback test: put back; running the %d frames again\n", s_t.frames);
        }
        return;
    }
    if (s_t.at == 0) {
        /* the bytes that came out otherwise, as address ranges */
        uint8_t *now_state = malloc(s_t.size);
        size_t off = 0;
        int ri;
        int shown = 0;
        sf_state_save(now_state);
        for (ri = 0; ri < s_reg_n && shown < 60; ri++) {
            size_t j = 0;
            while (j < s_reg[ri].n && shown < 60) {
                size_t k;
                if (now_state[off + j] == s_t.first[off + j]) {
                    j++;
                    continue;
                }
                for (k = j; k < s_reg[ri].n && k < j + 64 && now_state[off + k] != s_t.first[off + k]; k++) {
                }
                fprintf(stderr, "rollback test: differs at %p (+%zu bytes)\n", (void *)(s_reg[ri].p + j), k - j);
                shown++;
                j = k + 1;
            }
            off += s_reg[ri].n;
        }
        free(now_state);
    }
    {
        uint64_t *again = malloc(s_t.pages * sizeof *again);
        page_hashes(again);
        if (fold(again) != s_t.hash[s_t.at]) {
            int shown = 0;
            fprintf(stderr, "rollback test: frame %d differs:", s_t.at);
            for (i = 0; i < s_t.pages && shown < 12; i++) {
                if (again[i] != now[i]) {
                    fprintf(stderr, " %p", (void *)page_addr(i));
                    shown++;
                }
            }
            fprintf(stderr, "\n");
            s_t.bad++;
        }
        free(again);
    }
    if (++s_t.at == s_t.frames) {
        fprintf(stderr, "rollback test: %s (%d of %d frames differ)\n", s_t.bad ? "FAILED" : "the same", s_t.bad,
                s_t.frames);
        sf_input_tape_off();
        s_t.step = 3;
        exit(s_t.bad ? 2 : 0);
    }
}
