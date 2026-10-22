/* The game's music on OpenAL.
 *
 * The game drives its music through CRI's ADXT (an ADX streamed from the
 * archive, a queue of ADX stems played back to back, a short ADX from
 * memory) and AIXP (the opening theme's AIX). This keeps those calls and
 * plays each on an OpenAL source: the file is decoded to PCM with the
 * runtime's ADX and AIX decoders (src/game/adx.c, aix.c).
 *
 * The game waits on these streams (a stem ending, the queue running
 * down), so they keep the game's time, not the device's: sf_sound_pump
 * runs once a frame and takes a frame's worth of samples, 1/60 s, from
 * each playing stream. That decides when a stream ends, and the device
 * plays those samples a few frames behind. A run with sound, without it,
 * or faster than real time (a headless replay) sees the same statuses on
 * the same frames, so a session recording replays exactly. The status
 * codes are CRI's: ADXT 0 stopped, 3 playing, 5 ended; AIXP 2 playing,
 * 3 ended. Volumes are CRI's tenths of a decibel.
 *
 * A stream from the archive is read and decoded on a worker thread
 * (src/port/jobs.c), a part at a time, while it plays: starting one reads
 * only its header, and a stem the game queues starts decoding when it is
 * queued. The opening theme is decoded once, when the device opens. The
 * game's time never waits on the decoder's: a frame takes its samples,
 * and only if the decoder has not got that far yet does the game thread
 * wait for it (a track's first frame on a slow disc). The header's length
 * and loop decide the same as the decoded ones up to any sample already
 * decoded, so the statuses are those of a whole decode. */

#include "game/adx.h"
#include "game/aix.h"
#include "host/paths.h"
#include "port/jobs.h"
#include "port/port.h"

#include "types.h"

#include <AL/al.h>
#include <AL/alc.h>
#include <AL/alext.h>
#include <SDL3/SDL.h>

#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define VOICE_BUFFERS 24 /* a frame's samples each */
#define PREROLL 3          /* frames queued before the source starts */
#define FRAME_SAMPLES 1600 /* the most a frame takes: 96 kHz / 60 */
#define ENTRY_MAX 16
#define READ_CHUNK 65536 /* the decoder's read, bytes */
#define HEADER_READ 4096 /* the start's read: the header */

enum { ADXT_STOP = 0, ADXT_PLAYING = 3, ADXT_PLAYEND = 5 };
enum { AIXP_STOP = 0, AIXP_PLAYING = 2, AIXP_PLAYEND = 3 };

struct Decode;

/* Decoded PCM, interleaved s16, with its loop. While dec is set it is
 * still being decoded: total and the loop are the header's (an upper
 * bound), and pcm_need gives the samples. shared PCM (the opening theme's)
 * is not freed with the stream. */
typedef struct Pcm {
    int16_t *pcm;
    int channels;
    int rate;
    int32_t total;
    int loop;
    int32_t loop_start;
    int32_t loop_end;
    struct Decode *dec;
    int shared;
    int no_loop; /* a stem: the queue is its loop */
} Pcm;

/* An archive member read and decoded on a worker. */
typedef struct Decode {
    SfJob job;
    u32 offset; /* in the archive */
    u32 size;
    uint8_t *file; /* the member, read so far */
    u32 file_n;
    SfAdxStream adx;
    int16_t *pcm;
    SDL_AtomicInt ready; /* samples decoded */
    SDL_AtomicInt finished;
    SDL_AtomicInt cancel;
} Decode;

/* One OpenAL source fed from a Pcm, and the stems queued after it. */
typedef struct Voice {
    ALuint source;
    ALuint buffers[VOICE_BUFFERS];
    ALuint free_buf[VOICE_BUFFERS]; /* buffers the source does not hold */
    int free_n;
    Pcm now;
    int32_t pos;
    double due; /* samples owed to the frame clock */
    int started; /* the source was started on what it holds */
    int playing;
    int ended;
    int paused;
    int loop_on; /* AIXP's loop switch; ADX loops follow the file */
    float gain;
    int32_t entries[ENTRY_MAX];
    Pcm entry_pcm[ENTRY_MAX]; /* each queued stem, decoding already */
    int entry_n;
    int seamless;
    /* what now holds, so a rollback can decode it again: an archive
     * member (a stem when stem), an ADX in the game's memory, the AIX */
    int src;
    s32 src_fid;
    const void *src_mem;
    int src_stem;
} Voice;

enum { SRC_NONE, SRC_AFS, SRC_MEM, SRC_AIX };

static ALCdevice *s_dev;
static ALCcontext *s_ctx;
static int s_ready;

/* The handles the game keeps. Static, so a u32 holds their address. */
typedef struct AdxtHandle {
    int used;
    Voice v;
} AdxtHandle;
static AdxtHandle s_adxt[4];
static Voice s_aixp;

/* the opening theme's decode (see aix_get) */
static SfJob s_aix_job;
static Pcm s_aix_cache;
static void aix_cache_run(void *unused);
static void entries_clear(Voice *v);

/* SF_SOUND_DEBUG=1 logs each stream call with the frame it came on. */
static void sdebug(const char *fmt, ...)
{
    static int on = -1;
    va_list ap;
    if (on < 0) {
        on = getenv("SF_SOUND_DEBUG") != NULL;
    }
    if (!on) {
        return;
    }
    fprintf(stderr, "sound: frame %d: ", sf_port_frame);
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
}

static float gain_of(s32 vol)
{
    if (vol <= -960) {
        return 0.0f;
    }
    return powf(10.0f, (float)vol / 200.0f);
}

static void pcm_free(Pcm *p)
{
    if (p->dec) {
        SDL_SetAtomicInt(&p->dec->cancel, 1);
        sf_job_wait(&p->dec->job);
        free(p->dec->file);
        free(p->dec);
    }
    if (!p->shared) {
        free(p->pcm);
    }
    memset(p, 0, sizeof *p);
}

static int pcm_from_adx(const uint8_t *data, uint32_t size, Pcm *out)
{
    SfAdx adx;
    char err[256];
    if (sf_adx_decode(data, size, &adx, err, sizeof err) != 0) {
        fprintf(stderr, "sound: ADX: %s\n", err);
        return -1;
    }
    out->pcm = adx.pcm;
    out->channels = adx.channels;
    out->rate = adx.rate;
    out->total = adx.total;
    out->loop = adx.loop;
    out->loop_start = adx.loop_start;
    out->loop_end = adx.loop_end;
    return 0;
}

/* The worker's half: the member in parts, each decoded as it comes. */
static void decode_run(void *arg)
{
    Decode *d = arg;
    while (!SDL_GetAtomicInt(&d->cancel)) {
        int final;
        if (d->file_n < d->size) {
            u32 n = d->size - d->file_n < READ_CHUNK ? d->size - d->file_n : READ_CHUNK;
            long r = sf_afs_pread(d->file + d->file_n, n, d->offset + d->file_n);
            if (r <= 0) {
                fprintf(stderr, "sound: archive read failed at %u of %u bytes\n", d->file_n, d->size);
                d->size = d->file_n; /* what was read is the file */
            } else {
                d->file_n += (u32)r;
            }
        }
        final = d->file_n >= d->size;
        sf_adx_step(&d->adx, d->file, d->file_n, final, d->pcm, UINT32_MAX);
        SDL_SetAtomicInt(&d->ready, (int)d->adx.produced);
        if (d->adx.finished) {
            break;
        }
        sf_job_progress();
    }
    SDL_SetAtomicInt(&d->finished, 1);
    sf_job_progress();
}

/* A member as a stream: the header now, the rest on a worker. */
static int pcm_from_afs(s32 fid, Pcm *out)
{
    Decode *d;
    char err[256];
    u32 head;
    memset(out, 0, sizeof *out);
    d = calloc(1, sizeof *d);
    if (!d || sf_afs_member(fid, &d->offset, &d->size) != 0 || d->size == 0 || !(d->file = malloc(d->size))) {
        fprintf(stderr, "sound: archive member %d unreadable\n", fid);
        free(d);
        return -1;
    }
    head = d->size < HEADER_READ ? d->size : HEADER_READ;
    if (sf_afs_pread(d->file, head, d->offset) != (long)head) {
        fprintf(stderr, "sound: archive member %d unreadable\n", fid);
        free(d->file);
        free(d);
        return -1;
    }
    d->file_n = head;
    if (sf_adx_open(d->file, d->file_n, &d->adx, err, sizeof err) != 0) {
        fprintf(stderr, "sound: archive member %d (%u bytes, starts %02X %02X %02X %02X) is not a playable ADX: %s\n",
                fid, d->size, d->file[0], d->file[1], d->file[2], d->file[3], err);
        free(d->file);
        free(d);
        return -1;
    }
    d->pcm = malloc((size_t)d->adx.total * (size_t)d->adx.channels * sizeof(int16_t));
    if (!d->pcm) {
        free(d->file);
        free(d);
        return -1;
    }
    out->pcm = d->pcm;
    out->channels = d->adx.channels;
    out->rate = d->adx.rate;
    out->total = d->adx.total;
    out->loop = d->adx.loop;
    out->loop_start = d->adx.loop_start;
    out->loop_end = d->adx.loop_end;
    out->dec = d;
    sf_job_start(&d->job, decode_run, d);
    return 0;
}

typedef struct Need {
    Decode *d;
    int32_t upto;
} Need;

static int need_ready(void *arg)
{
    const Need *n = arg;
    return SDL_GetAtomicInt(&n->d->finished) || SDL_GetAtomicInt(&n->d->ready) > n->upto;
}

/* Samples up to upto may be read: the decoder is past it, or done (and
 * the PCM's length and loop are then the decoded ones). */
static void pcm_need(Pcm *p, int32_t upto)
{
    Need n;
    Decode *d = p->dec;
    if (!d) {
        return;
    }
    n.d = d;
    n.upto = upto;
    if (!need_ready(&n)) {
        sf_job_wait_for(&d->job, need_ready, &n);
    }
    if (SDL_GetAtomicInt(&d->finished)) {
        sf_job_wait(&d->job);
        sf_adx_final(&d->adx, &p->total, &p->loop, &p->loop_start, &p->loop_end);
        if (p->no_loop) {
            p->loop = 0;
        }
        free(d->file);
        free(d);
        p->dec = NULL;
    }
}

/* An ADX in memory: its size from the header (data offset, frames per
 * channel, and the end frame). */
static uint32_t adx_size(const uint8_t *p)
{
    uint32_t off;
    uint32_t block;
    uint32_t ch;
    uint32_t total;
    if (p[0] != 0x80 || p[1] != 0x00) {
        return 0;
    }
    off = ((uint32_t)p[2] << 8 | p[3]) + 4;
    block = p[5];
    ch = p[7];
    total = (uint32_t)p[12] << 24 | (uint32_t)p[13] << 16 | (uint32_t)p[14] << 8 | p[15];
    if (block == 0 || ch == 0) {
        return 0;
    }
    return off + ((total + 31) / 32 + 1) * block * ch;
}

static void voice_init(Voice *v)
{
    if (v->source || !s_ready) {
        return;
    }
    alGenSources(1, &v->source);
    alGenBuffers(VOICE_BUFFERS, v->buffers);
    memcpy(v->free_buf, v->buffers, sizeof v->buffers);
    v->free_n = VOICE_BUFFERS;
    v->gain = 1.0f;
}

/* Buffers the source has played go back to the free list. A source that
 * is not started counts everything it holds as processed, so its new
 * buffers stay until it starts. */
static void voice_reclaim(Voice *v)
{
    ALint done = 0;
    if (!v->started) {
        return;
    }
    alGetSourcei(v->source, AL_BUFFERS_PROCESSED, &done);
    while (done-- > 0 && v->free_n < VOICE_BUFFERS) {
        alSourceUnqueueBuffers(v->source, 1, &v->free_buf[v->free_n]);
        v->free_n++;
    }
}

static void voice_stop(Voice *v)
{
    ALint queued = 0;
    v->playing = 0;
    v->ended = 0;
    v->paused = 0;
    v->src = SRC_NONE;
    if (!v->source) {
        pcm_free(&v->now);
        return;
    }
    alSourceStop(v->source);
    alGetSourcei(v->source, AL_BUFFERS_QUEUED, &queued);
    while (queued-- > 0 && v->free_n < VOICE_BUFFERS) {
        alSourceUnqueueBuffers(v->source, 1, &v->free_buf[v->free_n]);
        v->free_n++;
    }
    v->started = 0;
    pcm_free(&v->now);
}

/* The next stem in the queue becomes the voice's PCM; 0 when there is none. */
static int voice_next_entry(Voice *v)
{
    s32 fid;
    if (v->entry_n == 0) {
        return 0;
    }
    fid = v->entries[0];
    pcm_free(&v->now);
    v->now = v->entry_pcm[0];
    memmove(v->entries, v->entries + 1, (size_t)(v->entry_n - 1) * sizeof v->entries[0]);
    memmove(v->entry_pcm, v->entry_pcm + 1, (size_t)(v->entry_n - 1) * sizeof v->entry_pcm[0]);
    v->entry_n--;
    memset(&v->entry_pcm[v->entry_n], 0, sizeof v->entry_pcm[0]);
    v->src = SRC_NONE;
    if (!v->now.pcm) {
        return 0; /* it did not read */
    }
    v->now.loop = 0; /* a stem plays once; the queue is the loop */
    v->now.no_loop = 1;
    v->pos = 0;
    v->src = SRC_AFS;
    v->src_fid = fid;
    v->src_stem = 1;
    return 1;
}

/* Up to n samples per channel from the voice's PCM into out, following
 * the file's loop or the stem queue; the count written. */
static int32_t voice_read(Voice *v, int16_t *out, int32_t n)
{
    int32_t done = 0;
    while (done < n && v->now.pcm) {
        int32_t end;
        int32_t take;
        pcm_need(&v->now, v->pos + (n - done));
        end = v->now.loop && v->loop_on ? v->now.loop_end : v->now.total;
        take = end - v->pos;
        if (take > n - done) {
            take = n - done;
        }
        if (take > 0) {
            memcpy(out + (size_t)done * v->now.channels, v->now.pcm + (size_t)v->pos * v->now.channels,
                   (size_t)take * v->now.channels * sizeof *out);
            v->pos += take;
            done += take;
        }
        if (v->pos >= end) {
            if (v->now.loop && v->loop_on) {
                v->pos = v->now.loop_start;
            } else if (v->seamless && voice_next_entry(v)) {
                /* a stem with another layout cannot share the stream */
            } else {
                break;
            }
        }
    }
    return done;
}

static void voice_start(Voice *v)
{
    voice_init(v);
    if (!v->now.pcm) {
        return;
    }
    if (v->now.channels > 2) {
        fprintf(stderr, "sound: %d channels; only mono and stereo play\n", v->now.channels);
        return;
    }
    v->playing = 1;
    v->ended = 0;
    v->due = 0.0;
    if (v->source) {
        alSourcef(v->source, AL_GAIN, v->gain);
    }
}

/* A frame of the stream: its samples are taken now, by the game's clock,
 * and queued on the source if the device has room for them. */
static void voice_pump(Voice *v)
{
    static int16_t tmp[FRAME_SAMPLES * 2];
    ALint queued = 0;
    ALint state = 0;
    int32_t want;
    int32_t n;
    if (!v->playing || v->paused) {
        return;
    }
    v->due += (double)v->now.rate / 60.0;
    want = (int32_t)v->due;
    if (want > FRAME_SAMPLES) {
        want = FRAME_SAMPLES;
    }
    v->due -= want;
    n = voice_read(v, tmp, want);
    if (n < want) {
        v->playing = 0;
        v->ended = 1;
        sdebug("voice %p ended", (void *)v);
    }
    if (!v->source) {
        return;
    }
    alGetSourcei(v->source, AL_SOURCE_STATE, &state);
    if (v->started && state == AL_STOPPED) {
        /* it ran dry: what it holds has played; start again on new samples */
        voice_reclaim(v);
        v->started = 0;
    }
    voice_reclaim(v);
    if (n > 0 && v->free_n > 0) {
        ALuint b = v->free_buf[--v->free_n];
        alBufferData(b, v->now.channels == 1 ? AL_FORMAT_MONO16 : AL_FORMAT_STEREO16, tmp,
                     (ALsizei)((size_t)n * v->now.channels * sizeof(int16_t)), v->now.rate);
        alSourceQueueBuffers(v->source, 1, &b);
    }
    /* a full queue (a run faster than the device) drops the frame */
    alGetSourcei(v->source, AL_BUFFERS_QUEUED, &queued);
    alGetSourcei(v->source, AL_SOURCE_STATE, &state);
    if (!v->started && (queued >= PREROLL || (!v->playing && queued > 0))) {
        alSourcePlay(v->source);
        v->started = 1;
    }
}

void sf_sound_pump(void)
{
    size_t i;
    for (i = 0; i < sizeof s_adxt / sizeof s_adxt[0]; i++) {
        voice_pump(&s_adxt[i].v);
    }
    voice_pump(&s_aixp);
}

/* ---- the device ---- */

s32 xb_CreateDirectSound(const char *path)
{
    (void)path;
    if (s_ready) {
        return 1;
    }
    if (!s_aix_job.fn) {
        sf_job_start(&s_aix_job, aix_cache_run, NULL);
    }
    s_dev = alcOpenDevice(NULL);
    s_ctx = s_dev ? alcCreateContext(s_dev, NULL) : NULL;
    if (!s_ctx || !alcMakeContextCurrent(s_ctx)) {
        /* the streams still keep time, so the game runs as it would */
        fprintf(stderr, "sound: no OpenAL device; the game runs silent\n");
        return 1;
    }
    s_ready = 1;
    return 1;
}

/* The app in the background (Android): the device stops playing until it
 * is back. The game itself is stopped meanwhile (SDL holds its thread). */
void sf_sound_pause(int paused)
{
    LPALCDEVICEPAUSESOFT pause_fn;
    LPALCDEVICERESUMESOFT resume_fn;
    if (!s_ready || !alcIsExtensionPresent(s_dev, "ALC_SOFT_pause_device")) {
        return;
    }
    pause_fn = (LPALCDEVICEPAUSESOFT)alcGetProcAddress(s_dev, "alcDevicePauseSOFT");
    resume_fn = (LPALCDEVICERESUMESOFT)alcGetProcAddress(s_dev, "alcDeviceResumeSOFT");
    if (paused && pause_fn) {
        pause_fn(s_dev);
    } else if (!paused && resume_fn) {
        resume_fn(s_dev);
    }
}

/* At exit: the sources stop and the device closes. */
void sf_sound_shutdown(void)
{
    size_t i;
    if (!s_ready) {
        return;
    }
    for (i = 0; i < sizeof s_adxt / sizeof s_adxt[0]; i++) {
        voice_stop(&s_adxt[i].v);
        entries_clear(&s_adxt[i].v);
    }
    voice_stop(&s_aixp);
    if (s_aix_job.fn) {
        sf_job_wait(&s_aix_job);
        s_aix_cache.shared = 0;
        pcm_free(&s_aix_cache);
    }
    sf_se_shutdown();
    alcMakeContextCurrent(NULL);
    alcDestroyContext(s_ctx);
    alcCloseDevice(s_dev);
    s_ctx = NULL;
    s_dev = NULL;
    s_ready = 0;
}

/* ---- ADXT ---- */

void ADXT_Init(void)
{
}

void ADXT_Finish(void)
{
}

void *ADXT_Create(s32 maxch, void *work, s32 worksize)
{
    size_t i;
    (void)maxch;
    (void)work;
    (void)worksize;
    for (i = 0; i < sizeof s_adxt / sizeof s_adxt[0]; i++) {
        if (!s_adxt[i].used) {
            s_adxt[i].used = 1;
            memset(&s_adxt[i].v.now, 0, sizeof s_adxt[i].v.now);
            voice_init(&s_adxt[i].v);
            s_adxt[i].v.loop_on = 1;
            s_adxt[i].v.gain = 1.0f;
            s_adxt[i].v.entry_n = 0;
            s_adxt[i].v.seamless = 0;
            return &s_adxt[i];
        }
    }
    fprintf(stderr, "sound: no free ADXT handle\n");
    return NULL;
}

void ADXT_Destroy(AdxtHandle *a)
{
    if (!a) {
        return;
    }
    voice_stop(&a->v);
    entries_clear(&a->v);
    a->used = 0;
}

void ADXT_Stop(AdxtHandle *a)
{
    if (a) {
        sdebug("ADXT_Stop %p", (void *)&a->v);
        voice_stop(&a->v);
        a->v.seamless = 0;
    }
}

void ADXT_StartAfs(AdxtHandle *a, s32 patid, s32 fid)
{
    (void)patid;
    if (!a) {
        return;
    }
    sdebug("ADXT_StartAfs %p file %d", (void *)&a->v, fid);
    voice_stop(&a->v);
    a->v.seamless = 0;
    if (pcm_from_afs(fid, &a->v.now) == 0) {
        a->v.pos = 0;
        a->v.src = SRC_AFS;
        a->v.src_fid = fid;
        a->v.src_stem = 0;
        voice_start(&a->v);
    }
}

void ADXT_StartMem(AdxtHandle *a, void *adx)
{
    uint32_t size;
    if (!a || !adx) {
        return;
    }
    sdebug("ADXT_StartMem %p", (void *)&a->v);
    voice_stop(&a->v);
    a->v.seamless = 0;
    size = adx_size(adx);
    if (!size) {
        const uint8_t *p = adx;
        fprintf(stderr, "sound: in-memory ADX starts %02X %02X %02X %02X\n", p[0], p[1], p[2], p[3]);
    }
    if (size && pcm_from_adx(adx, size, &a->v.now) == 0) {
        a->v.pos = 0;
        a->v.src = SRC_MEM;
        a->v.src_mem = adx;
        voice_start(&a->v);
    }
}

void ADXT_EntryAfs(AdxtHandle *a, s32 patid, s32 fid)
{
    (void)patid;
    sdebug("ADXT_EntryAfs %p file %d (%d queued)", (void *)(a ? &a->v : NULL), fid, a ? a->v.entry_n : -1);
    if (a && a->v.entry_n < ENTRY_MAX) {
        /* it starts decoding now, to be ready when its turn comes */
        if (pcm_from_afs(fid, &a->v.entry_pcm[a->v.entry_n]) != 0) {
            memset(&a->v.entry_pcm[a->v.entry_n], 0, sizeof a->v.entry_pcm[0]);
        }
        a->v.entries[a->v.entry_n++] = fid;
    }
}

/* The queued stems go, with their audio. */
static void entries_clear(Voice *v)
{
    int i;
    for (i = 0; i < v->entry_n; i++) {
        pcm_free(&v->entry_pcm[i]);
    }
    v->entry_n = 0;
}

void ADXT_ResetEntry(AdxtHandle *a)
{
    if (a) {
        entries_clear(&a->v);
    }
}

s32 ADXT_GetNumFiles(AdxtHandle *a)
{
    return a ? a->v.entry_n : 0;
}

void ADXT_StartSeamless(AdxtHandle *a)
{
    if (!a) {
        return;
    }
    sdebug("ADXT_StartSeamless %p (%d queued)", (void *)&a->v, a->v.entry_n);
    voice_stop(&a->v);
    a->v.seamless = 1;
    if (voice_next_entry(&a->v)) {
        voice_start(&a->v);
    }
}

s32 ADXT_GetStat(AdxtHandle *a)
{
    if (!a) {
        return ADXT_STOP;
    }
    if (a->v.playing) {
        return ADXT_PLAYING;
    }
    return a->v.ended ? ADXT_PLAYEND : ADXT_STOP;
}

void ADXT_Pause(AdxtHandle *a, s32 sw)
{
    if (!a) {
        return;
    }
    sdebug("ADXT_Pause %p %d", (void *)&a->v, sw);
    a->v.paused = sw != 0;
    if (a->v.source && a->v.started) {
        if (sw) {
            alSourcePause(a->v.source);
        } else {
            alSourcePlay(a->v.source);
        }
    }
}

s32 ADXT_GetStatPause(AdxtHandle *a)
{
    return a ? a->v.paused : 0;
}

void ADXT_SetOutVol(AdxtHandle *a, s32 vol)
{
    if (!a) {
        return;
    }
    sdebug("ADXT_SetOutVol %p %d", (void *)&a->v, vol);
    a->v.gain = gain_of(vol);
    if (a->v.source) {
        alSourcef(a->v.source, AL_GAIN, a->v.gain);
    }
}

/* The sound option's mono; OpenAL plays what the device is set to. */
void ADXT_SetOutputMono(s32 flag)
{
    (void)flag;
}

void ADXT_ClearErrCode(AdxtHandle *a)
{
    (void)a;
}

/* ---- AIXP ---- */

void AIXP_Init(void)
{
}

void AIXP_Finish(void)
{
}

void *AIXP_Create(s32 maxnch, s32 nstm, void *work, s32 worksize)
{
    (void)maxnch;
    (void)nstm;
    (void)work;
    (void)worksize;
    voice_init(&s_aixp);
    s_aixp.loop_on = 1;
    s_aixp.gain = 1.0f;
    return &s_aixp;
}

/* The opening theme, data/audio/NAME: its front stereo pair (the AIX's
 * other pairs are the surround channels). */
static char s_aix_name[64];

static int aix_load(const char *fname, Pcm *out)
{
    char base[4096];
    char path[4096 + 300];
    FILE *f;
    long n;
    uint8_t *data;
    SfAix aix;
    char err[256];
    if (sf_exe_dir(base, sizeof base) != 0) {
        return -1;
    }
    snprintf(path, sizeof path, "%s/data/audio/%s", base, fname);
    f = fopen(path, "rb");
    if (!f) {
        fprintf(stderr, "sound: %s missing\n", path);
        return -1;
    }
    fseek(f, 0, SEEK_END);
    n = ftell(f);
    fseek(f, 0, SEEK_SET);
    data = malloc((size_t)n);
    if (!data || fread(data, 1, (size_t)n, f) != (size_t)n) {
        fclose(f);
        free(data);
        return -1;
    }
    fclose(f);
    if (sf_aix_decode(data, (uint32_t)n, &aix, err, sizeof err) != 0) {
        fprintf(stderr, "sound: %s: %s\n", fname, err);
        free(data);
        return -1;
    }
    free(data);
    out->pcm = aix.pcm;
    out->channels = aix.channels;
    out->rate = aix.rate;
    out->total = aix.total;
    out->loop = aix.loop;
    out->loop_start = aix.loop_start;
    out->loop_end = aix.loop_end;
    return 0;
}

/* The opening theme, the one AIX the game plays, is decoded once on a
 * worker as soon as the device opens; streams of it share that PCM. */
#define AIX_OPENING "ST3_3_OP.aix"

static int s_aix_cache_ok;

static void aix_cache_run(void *unused)
{
    (void)unused;
    s_aix_cache_ok = aix_load(AIX_OPENING, &s_aix_cache) == 0;
}

static int aix_get(const char *fname, Pcm *out)
{
    if (strcmp(fname, AIX_OPENING) == 0 && s_aix_job.fn) {
        sf_job_wait(&s_aix_job);
        if (!s_aix_cache_ok) {
            return -1;
        }
        *out = s_aix_cache;
        out->shared = 1;
        return 0;
    }
    return aix_load(fname, out);
}

void xb_aixp_StartFnameSurround(void *aixp, const char *fname, void *dir)
{
    (void)aixp;
    (void)dir;
    sdebug("AIXP start %s", fname);
    voice_stop(&s_aixp);
    if (aix_get(fname, &s_aixp.now) != 0) {
        return;
    }
    snprintf(s_aix_name, sizeof s_aix_name, "%s", fname);
    s_aixp.src = SRC_AIX;
    s_aixp.pos = 0;
    voice_start(&s_aixp);
}

void AIXP_Stop(void *aixp)
{
    (void)aixp;
    sdebug("AIXP_Stop");
    voice_stop(&s_aixp);
}

s32 AIXP_GetStat(void *aixp)
{
    (void)aixp;
    if (s_aixp.playing) {
        return AIXP_PLAYING;
    }
    return s_aixp.ended ? AIXP_PLAYEND : AIXP_STOP;
}

void AIXP_SetLpSw(void *aixp, s32 sw)
{
    (void)aixp;
    s_aixp.loop_on = sw != 0;
}

void AIXP_Pause(void *aixp, s32 sw)
{
    (void)aixp;
    s_aixp.paused = sw != 0;
    if (s_aixp.source && s_aixp.started) {
        if (sw) {
            alSourcePause(s_aixp.source);
        } else {
            alSourcePlay(s_aixp.source);
        }
    }
}

/* Channel 0 sets the front pair's volume; the others are surround. */
void AIXP_SetOutVolDolbyDigital(void *aixp, s32 ch, s32 vol)
{
    (void)aixp;
    if (ch != 0) {
        return;
    }
    s_aixp.gain = gain_of(vol);
    if (s_aixp.source) {
        alSourcef(s_aixp.source, AL_GAIN, s_aixp.gain);
    }
}

/* Retail's physical memory, for the AIX player's work area: from the
 * arena, so a rollback snapshot keeps it. */
void *XPhysicalAlloc(u32 size, u32 addr, u32 align, u32 protect)
{
    (void)addr;
    (void)protect;
    return sf_arena_alloc(size, align ? align : 16);
}

/* ---- rollback: the streams' place in the game's time ---- */

/* What the game sees of a stream (its status, its stem queue) moves with
 * the frames, so a rollback puts it back with the game's state. The
 * decoded audio and the device's source are not copied: a stream whose
 * file changed is decoded again, and the source refills from the place. */
typedef struct StreamState {
    int used;
    int src;
    s32 src_fid;
    const void *src_mem;
    int src_stem;
    int32_t pos;
    double due;
    int playing;
    int ended;
    int paused;
    int loop_on;
    float gain;
    int32_t entries[ENTRY_MAX];
    int entry_n;
    int seamless;
} StreamState;

#define STREAMS (sizeof s_adxt / sizeof s_adxt[0] + 1)

typedef struct SoundState {
    StreamState st[STREAMS];
    char aix[64];
} SoundState;

static Voice *stream(size_t i, int **used)
{
    if (i < sizeof s_adxt / sizeof s_adxt[0]) {
        *used = &s_adxt[i].used;
        return &s_adxt[i].v;
    }
    *used = NULL;
    return &s_aixp;
}

size_t sf_sound_state_size(void)
{
    return sizeof(SoundState);
}

void sf_sound_state_save(void *buf)
{
    SoundState *ss = buf;
    size_t i;
    memset(ss, 0, sizeof *ss);
    for (i = 0; i < STREAMS; i++) {
        int *used;
        Voice *v = stream(i, &used);
        StreamState *t = &ss->st[i];
        t->used = used ? *used : 1;
        t->src = v->src;
        t->src_fid = v->src_fid;
        t->src_mem = v->src_mem;
        t->src_stem = v->src_stem;
        t->pos = v->pos;
        t->due = v->due;
        t->playing = v->playing;
        t->ended = v->ended;
        t->paused = v->paused;
        t->loop_on = v->loop_on;
        t->gain = v->gain;
        memcpy(t->entries, v->entries, sizeof t->entries);
        t->entry_n = v->entry_n;
        t->seamless = v->seamless;
    }
    memcpy(ss->aix, s_aix_name, sizeof ss->aix);
}

void sf_sound_state_restore(const void *buf)
{
    const SoundState *ss = buf;
    size_t i;
    for (i = 0; i < STREAMS; i++) {
        int *used;
        Voice *v = stream(i, &used);
        const StreamState *t = &ss->st[i];
        int same = v->src == t->src && v->now.pcm &&
                   (t->src == SRC_AFS ? v->src_fid == t->src_fid && v->src_stem == t->src_stem
                    : t->src == SRC_MEM ? v->src_mem == t->src_mem
                    : t->src == SRC_AIX ? strcmp(s_aix_name, ss->aix) == 0
                                        : 1);
        if (used) {
            *used = t->used;
        }
        if (!same) {
            voice_stop(v); /* frees the audio, empties the source */
            if (t->src == SRC_AFS && pcm_from_afs(t->src_fid, &v->now) == 0) {
                if (t->src_stem) {
                    v->now.loop = 0;
                    v->now.no_loop = 1;
                }
            } else if (t->src == SRC_MEM && t->src_mem) {
                uint32_t size = adx_size(t->src_mem);
                if (!size || pcm_from_adx(t->src_mem, size, &v->now) != 0) {
                    memset(&v->now, 0, sizeof v->now);
                }
            } else if (t->src == SRC_AIX && aix_get(ss->aix, &v->now) == 0) {
                snprintf(s_aix_name, sizeof s_aix_name, "%s", ss->aix);
            }
        } else if (v->source) {
            /* the same audio from another place: the queued samples go */
            ALint queued = 0;
            alSourceStop(v->source);
            alGetSourcei(v->source, AL_BUFFERS_QUEUED, &queued);
            while (queued-- > 0 && v->free_n < VOICE_BUFFERS) {
                alSourceUnqueueBuffers(v->source, 1, &v->free_buf[v->free_n]);
                v->free_n++;
            }
            v->started = 0;
        }
        v->src = v->now.pcm ? t->src : SRC_NONE;
        v->src_fid = t->src_fid;
        v->src_mem = t->src_mem;
        v->src_stem = t->src_stem;
        v->pos = t->pos;
        v->due = t->due;
        v->playing = t->playing && v->now.pcm;
        v->ended = t->ended;
        v->paused = t->paused;
        v->loop_on = t->loop_on;
        v->gain = t->gain;
        if (v->entry_n != t->entry_n || memcmp(v->entries, t->entries, (size_t)t->entry_n * sizeof v->entries[0]) != 0) {
            /* another queue: its stems decode again */
            int k;
            entries_clear(v);
            for (k = 0; k < t->entry_n; k++) {
                if (pcm_from_afs(t->entries[k], &v->entry_pcm[k]) != 0) {
                    memset(&v->entry_pcm[k], 0, sizeof v->entry_pcm[k]);
                }
            }
        }
        memcpy(v->entries, t->entries, sizeof v->entries);
        v->entry_n = t->entry_n;
        v->seamless = t->seamless;
        if (v->source) {
            alSourcef(v->source, AL_GAIN, v->gain);
        }
    }
}
