/* The game's state at each of the last frames, for rollback: a netplay
 * game runs a frame on a guess at the other player's pad, and when the
 * real pad comes and differs, puts the state back to that frame and runs
 * the frames since again.
 *
 * Copying the whole state (src/port/state.c: some 43 MB) each frame is
 * too slow for that, but a frame writes some 50 of its 11,000 pages. So
 * the state's pages are kept read-only; the first write to a page after
 * a frame's save stops in a fault handler, which copies the page as it
 * was into that frame's log and lets the write go on. Going back to a
 * frame copies its log, and every later frame's, back, newest first:
 * what each page was when that frame was saved.
 *
 * A page the state only partly covers (shared with the port's own
 * variables, which the system may write and a fault would break) is not
 * kept read-only but copied at every save.
 *
 * Writes from the system (a file read into the game's memory) do not
 * fault but fail on a read-only page: sf_hist_touch makes such pages
 * writable first (src/port/files.c). */

#include "port/port.h"

#include <errno.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
#else
#include <signal.h>
#include <sys/mman.h>
#include <unistd.h>
#endif

#define PAGE 4096
#define SPANS 16
#define FRAMES 24        /* frames kept; a rollback goes back fewer */
#define POOL_PAGES 8192  /* 32 MB of logged pages, shared by the frames */

enum { CLEAN, LOGGING, DIRTY };

typedef struct Span {
    uint8_t *p; /* page-aligned */
    size_t pages;
    size_t first; /* its first page's index */
} Span;

typedef struct Frame {
    int frame;
    uint64_t slot0;   /* its first page in the pool (a count, not mod POOL_PAGES) */
    atomic_uint n;    /* pages logged */
    uint32_t *page;   /* their indices */
    uint64_t *slot;   /* their places in the pool, or ~0 when the pool was full */
    uint8_t *sound;   /* sf_sound_state_save */
    uint8_t *always;  /* the always-copied pages */
} Frame;

/* All that the fault handler writes is here, in memory of its own (never
 * in a page it might find read-only). */
typedef struct Hist {
    int on;
    Span span[SPANS];
    int span_n;
    size_t pages;
    _Atomic uint8_t *state; /* by page */
    uint32_t *always;       /* indices of the pages not kept read-only */
    size_t always_n;
    uint8_t *pool;
    atomic_uint_fast64_t pool_head;
    uint64_t pool_tail; /* the oldest kept frame's first slot */
    atomic_int pool_full;
    Frame f[FRAMES];
    int newest;   /* the frame now logging (the last saved) */
    int count;    /* frames kept, the newest among them */
    uint8_t *arena;
    size_t arena_used;
} Hist;

static Hist *s_h;

/* ---- pages ---- */

static int page_of(const uint8_t *a, size_t *idx)
{
    int i;
    for (i = 0; i < s_h->span_n; i++) {
        const Span *s = &s_h->span[i];
        if (a >= s->p && a < s->p + s->pages * PAGE) {
            *idx = s->first + (size_t)(a - s->p) / PAGE;
            return 1;
        }
    }
    return 0;
}

static uint8_t *page_addr(size_t idx)
{
    int i;
    for (i = 0; i < s_h->span_n; i++) {
        const Span *s = &s_h->span[i];
        if (idx < s->first + s->pages) {
            return s->p + (idx - s->first) * PAGE;
        }
    }
    return NULL;
}

static void protect(uint8_t *p, size_t n, int writable)
{
#ifdef _WIN32
    DWORD old;
    VirtualProtect(p, n, writable ? PAGE_READWRITE : PAGE_READONLY, &old);
#else
    mprotect(p, n, writable ? PROT_READ | PROT_WRITE : PROT_READ);
#endif
}

/* The page's state bytes from src (a whole page's copy) back into it. */
static void put_back(uint8_t *page, const uint8_t *src)
{
    SfStateRegion reg[32];
    int n = sf_state_regions(reg, 32);
    int i;
    for (i = 0; i < n; i++) {
        uint8_t *lo = reg[i].p > page ? reg[i].p : page;
        uint8_t *hi = reg[i].p + reg[i].n < page + PAGE ? reg[i].p + reg[i].n : page + PAGE;
        if (hi > lo) {
            memcpy(lo, src + (lo - page), (size_t)(hi - lo));
        }
    }
}

/* ---- the fault ---- */

/* A write to a page of the state: 1 when it was a kept page, now logged
 * and writable. Any thread may come here. */
static int on_write(void *addr)
{
    Hist *h = s_h;
    size_t idx;
    if (!h || !h->on || !page_of(addr, &idx)) {
        return 0;
    }
    for (;;) {
        uint8_t st = atomic_load(&h->state[idx]);
        if (st == CLEAN) {
            if (atomic_compare_exchange_strong(&h->state[idx], &st, LOGGING)) {
                break;
            }
            continue;
        }
        if (st == DIRTY) {
            /* writable already: another thread's fault opened it while
             * this one was on its way, or the page is not ours to keep */
            uint8_t *p = page_addr(idx);
            protect(p, PAGE, 1);
            return 1;
        }
        /* LOGGING: another thread is copying it */
    }
    {
        Frame *f = &h->f[h->newest];
        uint8_t *p = page_addr(idx);
        unsigned k = atomic_fetch_add(&f->n, 1);
        uint64_t slot = atomic_fetch_add(&h->pool_head, 1);
        if (slot - h->pool_tail >= POOL_PAGES) {
            atomic_store(&h->pool_full, 1);
            slot = ~(uint64_t)0;
        } else {
            memcpy(h->pool + (slot % POOL_PAGES) * PAGE, p, PAGE);
        }
        f->page[k] = (uint32_t)idx;
        f->slot[k] = slot;
        protect(p, PAGE, 1);
        atomic_store(&h->state[idx], DIRTY);
    }
    return 1;
}

#ifdef _WIN32
static LONG CALLBACK on_fault(EXCEPTION_POINTERS *e)
{
    const EXCEPTION_RECORD *r = e->ExceptionRecord;
    if (r->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && r->NumberParameters >= 2 && r->ExceptionInformation[0] == 1 &&
        on_write((void *)r->ExceptionInformation[1])) {
        return EXCEPTION_CONTINUE_EXECUTION;
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

static void install(void)
{
    static int done;
    if (!done) {
        AddVectoredExceptionHandler(1, on_fault);
        done = 1;
    }
}
#else
static struct sigaction s_old;

static void on_fault(int sig, siginfo_t *si, void *uc)
{
    int e = errno;
    if (on_write(si->si_addr)) {
        errno = e;
        return;
    }
    errno = e;
    /* not ours: whoever had the signal before (the crash report) */
    if (s_old.sa_flags & SA_SIGINFO) {
        s_old.sa_sigaction(sig, si, uc);
    } else if (s_old.sa_handler != SIG_DFL && s_old.sa_handler != SIG_IGN) {
        s_old.sa_handler(sig);
    } else {
        signal(sig, SIG_DFL); /* the write again, and the default end */
    }
}

static void install(void)
{
    static int done;
    struct sigaction sa;
    if (done) {
        return;
    }
    memset(&sa, 0, sizeof sa);
    sa.sa_sigaction = on_fault;
    sa.sa_flags = SA_SIGINFO | SA_RESTART;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGSEGV, &sa, &s_old);
    done = 1;
}
#endif

/* ---- the frames ---- */

static void protect_all(int writable)
{
    int i;
    size_t a;
    for (i = 0; i < s_h->span_n; i++) {
        protect(s_h->span[i].p, s_h->span[i].pages * PAGE, writable);
    }
    /* the always-copied pages stay writable */
    for (a = 0; a < s_h->always_n; a++) {
        protect(page_addr(s_h->always[a]), PAGE, 1);
    }
}

static void frame_open(Frame *f, int frame)
{
    size_t a;
    f->frame = frame;
    f->slot0 = atomic_load(&s_h->pool_head);
    atomic_store(&f->n, 0);
    sf_sound_state_save(f->sound);
    for (a = 0; a < s_h->always_n; a++) {
        memcpy(f->always + a * PAGE, page_addr(s_h->always[a]), PAGE);
    }
}

/* The pages the frame logged are kept read-only again. */
static void frame_close(Frame *f)
{
    unsigned i;
    unsigned n = atomic_load(&f->n);
    for (i = 0; i < n; i++) {
        uint8_t dirty = DIRTY;
        protect(page_addr(f->page[i]), PAGE, 0);
        atomic_compare_exchange_strong(&s_h->state[f->page[i]], &dirty, CLEAN);
    }
}

static void build(void)
{
    SfStateRegion reg[32];
    int n;
    int i;
    size_t k;
    sf_state_size(); /* the regions as they are now */
    n = sf_state_regions(reg, 32);
    s_h->span_n = 0;
    s_h->pages = 0;
    for (i = 0; i < n && s_h->span_n < SPANS; i++) {
        uint8_t *lo = (uint8_t *)((uintptr_t)reg[i].p & ~(uintptr_t)(PAGE - 1));
        uint8_t *hi = (uint8_t *)(((uintptr_t)(reg[i].p + reg[i].n) + PAGE - 1) & ~(uintptr_t)(PAGE - 1));
        Span *last = s_h->span_n ? &s_h->span[s_h->span_n - 1] : NULL;
        if (!reg[i].n) {
            continue;
        }
        if (last && lo < last->p + last->pages * PAGE) {
            /* the regions share a page (a small cut between them) */
            lo = last->p + last->pages * PAGE;
            if (hi <= lo) {
                continue;
            }
        }
        if (last && lo == last->p + last->pages * PAGE) {
            last->pages += (size_t)(hi - lo) / PAGE;
        } else {
            Span *s = &s_h->span[s_h->span_n++];
            s->p = lo;
            s->pages = (size_t)(hi - lo) / PAGE;
        }
    }
    for (i = 0; i < s_h->span_n; i++) {
        s_h->span[i].first = s_h->pages;
        s_h->pages += s_h->span[i].pages;
    }
    /* the pages the state only partly covers */
    s_h->always = malloc(s_h->pages * sizeof *s_h->always);
    s_h->always_n = 0;
    for (k = 0; k < s_h->pages; k++) {
        uint8_t *p = page_addr(k);
        size_t covered = 0;
        for (i = 0; i < n; i++) {
            uint8_t *lo = reg[i].p > p ? reg[i].p : p;
            uint8_t *hi = reg[i].p + reg[i].n < p + PAGE ? reg[i].p + reg[i].n : p + PAGE;
            if (hi > lo) {
                covered += (size_t)(hi - lo);
            }
        }
        if (covered < PAGE) {
            s_h->always[s_h->always_n++] = (uint32_t)k;
        }
    }
    sf_arena_extent(&s_h->arena, &s_h->arena_used);
}

static void release(void)
{
    int i;
    if (!s_h) {
        return;
    }
    for (i = 0; i < FRAMES; i++) {
        free(s_h->f[i].page);
        free(s_h->f[i].slot);
        free(s_h->f[i].sound);
        free(s_h->f[i].always);
    }
    free((void *)s_h->state);
    free(s_h->always);
    free(s_h->pool);
    free(s_h);
    s_h = NULL;
}

void sf_hist_stop(void)
{
    if (!s_h) {
        return;
    }
    s_h->on = 0;
    protect_all(1);
    release();
}

/* Keeping frames, the state now being frame's. */
int sf_hist_start(int frame)
{
    int i;
    int ok;
    sf_hist_stop();
    sf_files_sync();
    install();
    s_h = calloc(1, sizeof *s_h);
    if (!s_h) {
        return -1;
    }
    build();
    ok = s_h->pages > 0;
    s_h->state = calloc(s_h->pages, 1);
    s_h->pool = malloc((size_t)POOL_PAGES * PAGE);
    ok = ok && s_h->state && s_h->pool && s_h->always;
    for (i = 0; ok && i < FRAMES; i++) {
        Frame *f = &s_h->f[i];
        f->page = malloc(s_h->pages * sizeof *f->page);
        f->slot = malloc(s_h->pages * sizeof *f->slot);
        f->sound = malloc(sf_sound_state_size());
        f->always = malloc(s_h->always_n * PAGE + 1);
        ok = f->page && f->slot && f->sound && f->always;
    }
    if (!ok) {
        fprintf(stderr, "history: no memory\n");
        release();
        return -1;
    }
    s_h->newest = 0;
    s_h->count = 1;
    frame_open(&s_h->f[0], frame);
    s_h->on = 1;
    protect_all(0);
    fprintf(stderr, "history: %zu pages kept, %zu copied each frame\n", s_h->pages - s_h->always_n, s_h->always_n);
    return 0;
}

int sf_hist_on(void)
{
    return s_h != NULL;
}

/* The state now is frame's (after it ran): kept, and the oldest kept
 * frame dropped when there is no room. */
void sf_hist_save(int frame)
{
    Frame *f;
    uint8_t *arena;
    size_t used;
    if (!s_h) {
        return;
    }
    sf_files_sync();
    sf_arena_extent(&arena, &used);
    if (used != s_h->arena_used || atomic_load(&s_h->pool_full)) {
        /* the game took more of the arena, or a frame wrote more than the
         * pool holds: keeping starts again from here */
        fprintf(stderr, "history: %s at frame %d; frames before it are gone\n",
                used != s_h->arena_used ? "the arena grew" : "the pool filled", frame);
        sf_hist_start(frame);
        return;
    }
    frame_close(&s_h->f[s_h->newest]);
    if (s_h->count == FRAMES) {
        /* the oldest goes; its pool pages with it */
        int oldest = (s_h->newest + 1) % FRAMES;
        s_h->pool_tail = s_h->f[(oldest + 1) % FRAMES].slot0;
        s_h->count--;
    }
    s_h->newest = (s_h->newest + 1) % FRAMES;
    s_h->count++;
    f = &s_h->f[s_h->newest];
    frame_open(f, frame);
}

/* The oldest frame kept. */
int sf_hist_oldest(void)
{
    if (!s_h) {
        return -1;
    }
    return s_h->f[(s_h->newest + FRAMES - s_h->count + 1) % FRAMES].frame;
}

/* Back to frame's state: 0, or -1 when it is not kept. The frames after
 * it are dropped; frame stays kept (and newest). */
int sf_hist_load(int frame)
{
    int back;
    int i;
    size_t a;
    Frame *target = NULL;
    if (!s_h) {
        return -1;
    }
    for (back = 0; back < s_h->count; back++) {
        Frame *f = &s_h->f[(s_h->newest + FRAMES - back) % FRAMES];
        if (f->frame == frame) {
            target = f;
            break;
        }
    }
    if (!target) {
        return -1;
    }
    sf_files_sync();
    s_h->on = 0;
    protect_all(1);
    for (i = 0; i <= back; i++) {
        Frame *f = &s_h->f[(s_h->newest + FRAMES - i) % FRAMES];
        unsigned n = atomic_load(&f->n);
        unsigned k;
        for (k = 0; k < n; k++) {
            if (f->slot[k] != ~(uint64_t)0) {
                put_back(page_addr(f->page[k]), s_h->pool + (f->slot[k] % POOL_PAGES) * PAGE);
            }
        }
    }
    for (a = 0; a < s_h->always_n; a++) {
        put_back(page_addr(s_h->always[a]), target->always + a * PAGE);
    }
    sf_sound_state_restore(target->sound);
    sf_gfx_state_restored();
    /* the frame logs anew from here */
    s_h->newest = (s_h->newest + FRAMES - back) % FRAMES;
    s_h->count -= back;
    atomic_store(&s_h->pool_head, target->slot0);
    atomic_store(&target->n, 0);
    memset((void *)s_h->state, CLEAN, s_h->pages);
    protect_all(0);
    s_h->on = 1;
    return 0;
}

/* A system write (a file read) is coming to [p, p + n): its pages logged
 * and writable first. */
void sf_hist_touch(void *p, size_t n)
{
    uint8_t *a = (uint8_t *)((uintptr_t)p & ~(uintptr_t)(PAGE - 1));
    uint8_t *end = (uint8_t *)p + n;
    if (!s_h || !s_h->on) {
        return;
    }
    for (; a < end; a += PAGE) {
        size_t idx;
        if (page_of(a, &idx) && atomic_load(&s_h->state[idx]) != DIRTY) {
            on_write(a);
        }
    }
}
