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

#include <SDL3/SDL.h>

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifndef _WIN32
#include <dlfcn.h>
#endif

#ifdef _WIN32
/* PE has no __start_/__stop_ section bounds, but the linker keeps the
 * port's sections apart from .data and .bss: the state is .data, .bss
 * and sfgame, found in the program's section table (data_ranges). */
#include <windows.h>
#elif defined(__ANDROID__)
/* Android's libc has no __data_start: the game library's writable
 * segments are read from its program headers instead (data_ranges). */
#include <link.h>
extern char __start_sfport_data[];
extern char __stop_sfport_data[];
extern char __start_sfport_bss[];
extern char __stop_sfport_bss[];
extern char __start_sfkeep[];
extern char __stop_sfkeep[];
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

/* The program's writable data, before the port's sections are cut out:
 * on Linux .data to the end of .bss; on Android each writable segment of
 * the game library, less what the loader makes read-only (RELRO). */
#define BASES 4
static uint8_t *s_base[BASES][2] SF_KEEP;
static int s_base_n SF_KEEP;

#ifdef __ANDROID__
static int phdr_cb(struct dl_phdr_info *info, size_t size, void *data)
{
    uintptr_t me = (uintptr_t)data;
    uintptr_t rlo = 0;
    uintptr_t rhi = 0;
    int mine = 0;
    int i;
    (void)size;
    for (i = 0; i < info->dlpi_phnum; i++) {
        const ElfW(Phdr) *ph = &info->dlpi_phdr[i];
        uintptr_t a = info->dlpi_addr + ph->p_vaddr;
        if (ph->p_type == PT_LOAD && me >= a && me < a + ph->p_memsz) {
            mine = 1;
        }
        if (ph->p_type == PT_GNU_RELRO) {
            rlo = a;
            rhi = a + ph->p_memsz;
        }
    }
    if (!mine) {
        return 0;
    }
    s_base_n = 0;
    for (i = 0; i < info->dlpi_phnum && s_base_n < BASES; i++) {
        const ElfW(Phdr) *ph = &info->dlpi_phdr[i];
        uintptr_t a = info->dlpi_addr + ph->p_vaddr;
        uintptr_t e = a + ph->p_memsz;
        if (ph->p_type != PT_LOAD || !(ph->p_flags & PF_W)) {
            continue;
        }
        if (rhi > a && rlo <= a) {
            a = rhi < e ? rhi : e; /* the read-only start of the segment goes */
        }
        if (e > a) {
            s_base[s_base_n][0] = (uint8_t *)a;
            s_base[s_base_n][1] = (uint8_t *)e;
            s_base_n++;
        }
    }
    return 1;
}
#endif

static void data_ranges(void)
{
    if (s_base_n) {
        return;
    }
#if defined(_WIN32)
    {
        uint8_t *image = (uint8_t *)GetModuleHandleW(NULL);
        const IMAGE_NT_HEADERS *nt = (const IMAGE_NT_HEADERS *)(image + ((const IMAGE_DOS_HEADER *)image)->e_lfanew);
        const IMAGE_SECTION_HEADER *sec = IMAGE_FIRST_SECTION(nt);
        int i;
        s_base_n = 0;
        for (i = 0; i < nt->FileHeader.NumberOfSections && s_base_n < BASES; i++) {
            char name[9] = { 0 };
            memcpy(name, sec[i].Name, 8);
            if (strcmp(name, ".data") == 0 || strcmp(name, ".bss") == 0 || strcmp(name, "sfgame") == 0) {
                s_base[s_base_n][0] = image + sec[i].VirtualAddress;
                s_base[s_base_n][1] = image + sec[i].VirtualAddress + sec[i].Misc.VirtualSize;
                s_base_n++;
            }
        }
    }
#elif defined(__ANDROID__)
    dl_iterate_phdr(phdr_cb, (void *)&s_reg);
#else
    s_base[0][0] = (uint8_t *)__data_start;
    s_base[0][1] = (uint8_t *)_end;
    s_base_n = 1;
#endif
}

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
#ifdef _WIN32
    uint8_t *cut[4][2] = { { NULL, NULL } }; /* the port's sections are apart already */
    int ncut = 0;
#else
    uint8_t *cut[4][2] = {
        { (uint8_t *)__start_sfport_data, (uint8_t *)__stop_sfport_data },
        { (uint8_t *)__start_sfport_bss, (uint8_t *)__stop_sfport_bss },
        { (uint8_t *)__start_sfkeep, (uint8_t *)__stop_sfkeep },
    };
    int ncut = 3;
#endif
    uint8_t *arena;
    size_t used;
    int k;
    int bi;
    s_reg_n = 0;
    data_ranges();
    if (all < 0) {
        all = getenv("SF_ROLLBACK_ALL") != NULL;
    }
    if (all && ncut) {
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
    for (bi = 0; bi < s_base_n; bi++) {
        uint8_t *at = s_base[bi][0];
        uint8_t *end = s_base[bi][1];
        for (k = 0; k < ncut; k++) {
            if (cut[k][0] >= at && cut[k][1] <= end) {
                add_region(at, cut[k][0]);
                at = cut[k][1];
            }
        }
        add_region(at, end);
    }
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

/* After the state is put back: the port's copies of what the game sees
 * (the renderer's textures, the sound effects' waves) brought into line. */
void sf_state_restored(void)
{
    sf_gfx_state_restored();
    sf_se_state_restored();
}

int sf_state_regions(SfStateRegion *out, int max)
{
    int i;
    for (i = 0; i < s_reg_n && i < max; i++) {
        out[i].p = s_reg[i].p;
        out[i].n = s_reg[i].n;
    }
    return i;
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
    sf_files_sync(); /* no read still landing in the memory copied */
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
    sf_files_sync(); /* no read still landing in the memory copied */
    regions();
    for (i = 0; i < s_reg_n; i++) {
        memcpy(s_reg[i].p, buf, s_reg[i].n);
        buf += s_reg[i].n;
    }
    sf_sound_state_restore(buf);
    sf_state_restored();
}

/* ---- the start-up state ----
 *
 * The state as start-up left it (before the first frame), for a netplay
 * session: both machines go back to it, so they start from the same
 * game. Its regions are kept with it (the arena may be taken further
 * since), and a page of zeros is not copied. */

static struct {
    SfStateRegion reg[32];
    int n;
    uint8_t **page; /* by page of the regions in order; NULL for zeros */
    size_t pages;
    uint8_t *sound;
} s_boot SF_KEEP;

static int all_zero(const uint8_t *p, size_t n)
{
    static const uint8_t zero[PAGE];
    return memcmp(p, zero, n) == 0;
}

void sf_state_boot_save(void)
{
    size_t k = 0;
    size_t kept = 0;
    int i;
    sf_files_sync();
    sf_state_size();
    s_boot.n = sf_state_regions(s_boot.reg, 32);
    s_boot.pages = 0;
    for (i = 0; i < s_boot.n; i++) {
        s_boot.pages += (s_boot.reg[i].n + PAGE - 1) / PAGE;
    }
    s_boot.page = calloc(s_boot.pages, sizeof *s_boot.page);
    s_boot.sound = malloc(sf_sound_state_size());
    if (!s_boot.page || !s_boot.sound) {
        return;
    }
    for (i = 0; i < s_boot.n; i++) {
        size_t off;
        for (off = 0; off < s_boot.reg[i].n; off += PAGE, k++) {
            size_t n = s_boot.reg[i].n - off < PAGE ? s_boot.reg[i].n - off : PAGE;
            if (!all_zero(s_boot.reg[i].p + off, n)) {
                s_boot.page[k] = malloc(n);
                if (s_boot.page[k]) {
                    memcpy(s_boot.page[k], s_boot.reg[i].p + off, n);
                    kept++;
                }
            }
        }
    }
    sf_sound_state_save(s_boot.sound);
    fprintf(stderr, "state: start-up kept, %zu KB of %zu KB\n", kept * 4, s_boot.pages * 4);
}

int sf_state_boot_ok(void)
{
    return s_boot.page != NULL;
}

/* Back to start-up (the history stopped first: the caller's). */
int sf_state_boot_restore(void)
{
    size_t k = 0;
    int i;
    if (!s_boot.page) {
        return -1;
    }
    sf_files_sync();
    for (i = 0; i < s_boot.n; i++) {
        size_t off;
        for (off = 0; off < s_boot.reg[i].n; off += PAGE, k++) {
            size_t n = s_boot.reg[i].n - off < PAGE ? s_boot.reg[i].n - off : PAGE;
            if (s_boot.page[k]) {
                memcpy(s_boot.reg[i].p + off, s_boot.page[k], n);
            } else {
                memset(s_boot.reg[i].p + off, 0, n);
            }
        }
    }
    sf_sound_state_restore(s_boot.sound);
    sf_state_restored();
    sf_save_restart();
    return 0;
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

/* SF_TRACE=NAME:SIZE,... prints those globals (found by their symbol
 * names) after each frame, between SF_TRACE_FROM and SF_TRACE_TO: two
 * builds (another compiler, another processor) run over the same
 * recording print the same lines until they part. */
static void trace_frame(void)
{
#ifndef _WIN32
    static const char *spec;
    static int from = -1;
    static int to;
    char name[128];
    const char *p;
    if (from < 0) {
        spec = getenv("SF_TRACE");
        from = getenv("SF_TRACE_FROM") ? atoi(getenv("SF_TRACE_FROM")) : 0;
        to = getenv("SF_TRACE_TO") ? atoi(getenv("SF_TRACE_TO")) : 0x7FFFFFFF;
    }
    if (!spec || sf_port_frame < from || sf_port_frame > to) {
        return;
    }
    fprintf(stderr, "trace %d:", sf_port_frame);
    for (p = spec; *p;) {
        size_t n = strcspn(p, ":,");
        int size = 4;
        const uint8_t *at;
        if (n >= sizeof name) {
            break;
        }
        memcpy(name, p, n);
        name[n] = 0;
        p += n;
        if (*p == ':') {
            size = atoi(p + 1);
            p += strcspn(p, ",");
        }
        if (*p == ',') {
            p++;
        }
        at = dlsym(RTLD_DEFAULT, name);
        fprintf(stderr, " %s=", name);
        if (!at) {
            fprintf(stderr, "?");
            continue;
        }
        while (size-- > 0) {
            fprintf(stderr, "%02x", *at++);
        }
    }
    fputc('\n', stderr);
#endif
}

/* The global at an address, for the tests' reports ("?" when unknown). */
static const char *sym_name(const void *a)
{
#ifdef _WIN32
    (void)a;
    return "?";
#else
    Dl_info di;
    return dladdr(a, &di) && di.dli_sname ? di.dli_sname : "?";
#endif
}

/* SF_STATE_PROFILE=FROM:FRAMES: which pages of the state change, and how
 * often, over FRAMES frames from FROM (what a rollback has to copy). */
static void profile_frame(void)
{
#ifndef _WIN32
    static int from = -2;
    static int frames;
    static size_t pages;
    static uint64_t *prev;
    static uint64_t *now;
    static uint32_t *count;
    static int at;
    size_t i;
    if (from == -2) {
        const char *e = getenv("SF_STATE_PROFILE");
        from = e ? atoi(e) : -1;
        frames = e && strchr(e, ':') ? atoi(strchr(e, ':') + 1) : 600;
    }
    if (from < 0 || sf_port_frame < from) {
        return;
    }
    if (!prev) {
        regions();
        pages = 0;
        for (i = 0; i < (size_t)s_reg_n; i++) {
            pages += (s_reg[i].n + PAGE - 1) / PAGE;
            fprintf(stderr, "profile: region %zu at %p, %zu KB\n", i, (void *)s_reg[i].p, s_reg[i].n / 1024);
        }
        prev = malloc(pages * 8);
        now = malloc(pages * 8);
        count = calloc(pages, 4);
        page_hashes(prev);
        return;
    }
    page_hashes(now);
    {
        size_t changed = 0;
        for (i = 0; i < pages; i++) {
            if (now[i] != prev[i]) {
                count[i]++;
                changed++;
            }
        }
        if (at % 60 == 0) {
            fprintf(stderr, "profile: frame %d: %zu pages changed\n", sf_port_frame, changed);
        }
    }
    memcpy(prev, now, pages * 8);
    if (++at < frames) {
        return;
    }
    {
        size_t ever = 0;
        uint64_t sum = 0;
        for (i = 0; i < pages; i++) {
            ever += count[i] != 0;
            sum += count[i];
        }
        fprintf(stderr, "profile: %zu pages (%zu KB); %zu changed at all (%zu KB); %.1f a frame\n", pages,
                pages * 4, ever, ever * 4, (double)sum / frames);
        for (i = 0; i < pages; i++) {
            if (count[i]) {
                uint8_t *a = page_addr(i);
                fprintf(stderr, "profile: page %p x%u %s\n", (void *)a, count[i], sym_name(a));
            }
        }
    }
    exit(0);
#endif
}

/* SF_HIST_TEST=FROM[:EVERY[:BACK]] tests the rollback history
 * (src/port/history.c) as netplay uses it: from FROM each frame is kept,
 * and every EVERY frames the game goes BACK frames back and runs them
 * again on the same pads; each frame run again must hash as it did the
 * first time. */
#define HIST_TEST_FRAMES 900
static void hist_test_frame(void)
{
    static int from = -2;
    static int every;
    static int back;
    static int base;
    static int known;
    static int again;
    static int bad;
    static int rollbacks;
    static uint64_t *hash;
    static uint64_t *pages;
    static double save_ms;
    static double load_ms;
    static int saves;
    int idx;
    int fresh;
    uint64_t h;
    uint64_t t0;
    if (from == -2) {
        const char *e = getenv("SF_HIST_TEST");
        const char *c;
        from = e ? atoi(e) : -1;
        c = e ? strchr(e, ':') : NULL;
        every = c ? atoi(c + 1) : 7;
        c = c ? strchr(c + 1, ':') : NULL;
        back = c ? atoi(c + 1) : 6;
    }
    if (from < 0 || sf_port_frame < from) {
        return;
    }
    if (!hash) {
        regions();
        s_t.pages = 0;
        for (idx = 0; idx < s_reg_n; idx++) {
            s_t.pages += (s_reg[idx].n + PAGE - 1) / PAGE;
        }
        pages = malloc(s_t.pages * sizeof *pages * (HIST_TEST_FRAMES + 1));
        hash = calloc(HIST_TEST_FRAMES + 1, sizeof *hash);
        if (sf_hist_start(sf_port_frame) != 0) {
            exit(1);
        }
        sf_input_tape_mark();
        base = sf_port_frame;
        known = 1;
        page_hashes(pages);
        hash[0] = fold(pages);
        return;
    }
    idx = sf_port_frame - base;
    {
        uint64_t *now = pages + (size_t)HIST_TEST_FRAMES * s_t.pages;
        page_hashes(now);
        h = fold(now);
        if (idx < known && h != hash[idx] && bad < 4) {
            size_t i;
            int shown = 0;
            for (i = 0; i < s_t.pages && shown < 8; i++) {
                if (now[i] != pages[(size_t)idx * s_t.pages + i]) {
                    uint8_t *a = page_addr(i);
                    fprintf(stderr, "hist test: frame %d page %p (%s) otherwise\n", sf_port_frame, (void *)a,
                            sym_name(a));
                    shown++;
                }
            }
        }
        if (idx >= known) {
            memcpy(pages + (size_t)idx * s_t.pages, now, s_t.pages * sizeof *now);
        }
    }
    fresh = idx >= known;
    if (!fresh) {
        again++;
        if (h != hash[idx]) {
            bad++;
            if (bad <= 10) {
                fprintf(stderr, "hist test: frame %d came out otherwise the second time\n", sf_port_frame);
            }
        }
    } else {
        hash[idx] = h;
        known = idx + 1;
    }
    t0 = SDL_GetPerformanceCounter();
    sf_hist_save(sf_port_frame);
    save_ms += (double)(SDL_GetPerformanceCounter() - t0) * 1000.0 / (double)SDL_GetPerformanceFrequency();
    saves++;
    if (idx == HIST_TEST_FRAMES) {
        fprintf(stderr,
                "hist test: %s: %d rollbacks of %d frames, %d frames run again, %d otherwise; save %.3f ms, load %.3f ms\n",
                bad ? "FAILED" : "the same", rollbacks, back, again, bad, save_ms / saves, rollbacks ? load_ms / rollbacks : 0.0);
        exit(bad ? 2 : 0);
    }
    if (fresh && idx % every == 0 && idx >= back) {
        int target = sf_port_frame - back;
        t0 = SDL_GetPerformanceCounter();
        if (sf_hist_load(target) != 0) {
            fprintf(stderr, "hist test: frame %d is not kept (oldest %d)\n", target, sf_hist_oldest());
            exit(2);
        }
        load_ms += (double)(SDL_GetPerformanceCounter() - t0) * 1000.0 / (double)SDL_GetPerformanceFrequency();
        sf_port_frame = target;
        sf_input_tape_seek(target - base);
        rollbacks++;
    }
}

/* SF_BOOT_TEST=FRAME: after FRAME, back to start-up with the recording
 * from its start: with SF_SYNC_LOG, the frames after it must log as the
 * first ones did (tools: compare the two runs of frame numbers). */
static void boot_test_frame(void)
{
    static int at = -2;
    if (at == -2) {
        at = getenv("SF_BOOT_TEST") ? atoi(getenv("SF_BOOT_TEST")) : -1;
    }
    if (at < 0 || sf_port_frame != at) {
        return;
    }
    at = -1;
    fprintf(stderr, "boot test: back to start-up after frame %d\n", sf_port_frame);
    sf_state_boot_restore();
    sf_input_replay_rewind();
    sf_port_frame = 0;
}

/* After each game frame. */
void sf_state_test_frame(void)
{
    trace_frame();
    profile_frame();
    hist_test_frame();
    boot_test_frame();
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
