/* The game's sound effects on OpenAL.
 *
 * The game asks for an effect through its sound engine (cseTsbRequest,
 * mlTsbRequest in the decomp's CapSndEng), which on the Xbox queues the
 * request for retail's XACT layer: a request names a slot, a cue, a group
 * and a priority. Slot 0 holds the shared effects (sound bank 0, "SE",
 * over the wave banks SE1, SE2 and NR); slots 1 and 2 hold the two
 * players' character banks (sound banks 1 to 20, one wave bank each).
 * This file is that layer: retail's request queue, its 48 voices, its
 * group and priority rules and its pan tables (3s-decomp-ogxb,
 * Source/XBOX/xb_0F50C9.c, xb_0F4537.c, xb_0F44B2.c, xb_0F501C.c), with
 * XACT's cue playback done here.
 *
 * The cue tables are the XBE's XACT sound banks, written to
 * data/audio/sdbk at import. Their layout is the XDK's
 * (_XACT_SOUNDBANK_FILE_HEADER and the rest, from the type records in
 * xactengd.lib): a cue names a sound; a sound is either one wave (flag 8)
 * or tracks of events. The events these banks use are Play (a wave, maybe
 * looping), AmpEG (an envelope), SetVolume (a fade), StopEG (stop with
 * the envelope's release), SetMixBins (speaker levels, folded to stereo)
 * and PitchLFO, a vibrato (_XACT_TRACK_EVENT_PITCHLFO: wDelay, bDelta,
 * bPitchMod, which the XDK's DSLFODESC page gives units: the delay in
 * 32-sample blocks, a rate of 375 * delta / 16384 Hz, a depth of
 * bPitchMod / 128 octave). Every one in these banks is the same, 3.7 Hz
 * and a quarter octave, on about half of each character's sounds; the
 * wave is taken as a triangle. SF_SE_LFO=0 leaves it out.
 *
 * Voices end by the frame clock, not by the audio device, so a run plays
 * the same requests the same way with or without sound.
 *
 * A wave bank's waves are decoded on a worker (src/port/jobs.c) as soon
 * as the bank is registered, in order; the first play of a wave takes it
 * from there, waiting only if the worker has not reached it yet. */

#include "game/xwb.h"
#include "host/paths.h"
#include "port/jobs.h"
#include "port/port.h"

#include "types.h"

#include <AL/al.h>
#include <AL/alc.h>
#include <AL/alext.h>
#include <SDL3/SDL.h>

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SE_VOICES 48
#define SE_QUEUE 0x30
#define SE_SLOTS 3
#define SE_BANKS 21
#define SE_WAVEBANKS 8
#define SE_TRACKS 4
#define SE_FPS 60

/* The pan tables (retail's D_003F3300, D_003F3408): the left and right
 * mix bin volumes in hundredths of a decibel for pan -32 to 32. */
static const s32 k_pan_l[65] = {
    0,    0,    0,    0,    0,    -6,   -13,  -27,  -41,  -63,  -78,  -93,  -108, -123, -139, -147, -155,
    -163, -172, -180, -188, -197, -205, -205, -205, -205, -205, -205, -205, -205, -205, -205, -205, -205,
    -205, -205, -205, -205, -205, -205, -205, -205, -205, -223, -240, -258, -277, -296, -315, -335, -355,
    -376, -397, -419, -441, -464, -487, -511, -536, -562, -588, -615, -643, -672, -702,
};
static const s32 k_pan_r[65] = {
    -702, -672, -643, -615, -588, -562, -536, -511, -487, -464, -441, -419, -397, -376, -355, -335, -315,
    -296, -277, -258, -240, -223, -205, -205, -205, -205, -205, -205, -205, -205, -205, -205, -205, -205,
    -205, -205, -205, -205, -205, -205, -205, -205, -205, -197, -188, -180, -172, -163, -155, -147, -139,
    -123, -108, -93,  -78,  -63,  -41,  -27,  -13,  -6,   0,    0,    0,    0,    0,
};

/* Retail's mono switch (mlSysSetMono writes it): every effect centred. */
s32 D_00797100 SF_GAME;

/* ---- the sound banks (cue tables) ---- */

typedef struct SoundBank {
    uint8_t *data;
    uint32_t size;
    uint16_t sounds;
    uint16_t cues;
    uint16_t wavebanks;
    uint32_t sound_off;
    uint32_t wavebank_off;
} SoundBank;

static SoundBank s_sdbk[SE_BANKS];

static uint16_t rd16(const uint8_t *p)
{
    return (uint16_t)(p[0] | (p[1] << 8));
}

static uint32_t rd32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static int bank_load(int i, SoundBank *b)
{
    char base[4096];
    char path[4200];
    FILE *fp;
    long n;
    if (sf_exe_dir(base, sizeof base) != 0) {
        return -1;
    }
    snprintf(path, sizeof path, "%s/data/audio/sdbk/%02d.sdbk", base, i);
    fp = fopen(path, "rb");
    if (!fp) {
        return -1;
    }
    if (fseek(fp, 0, SEEK_END) != 0 || (n = ftell(fp)) < 0x38 || n > 256 * 1024 || fseek(fp, 0, SEEK_SET) != 0) {
        fclose(fp);
        return -1;
    }
    b->data = malloc((size_t)n);
    if (!b->data || fread(b->data, 1, (size_t)n, fp) != (size_t)n) {
        fclose(fp);
        free(b->data);
        b->data = NULL;
        return -1;
    }
    fclose(fp);
    b->size = (uint32_t)n;
    b->sounds = rd16(b->data + 0x1C);
    b->cues = rd16(b->data + 0x1E);
    b->wavebanks = rd16(b->data + 0x22);
    b->wavebank_off = rd32(b->data + 0x08);
    b->sound_off = 0x38u + 20u * b->cues;
    if (memcmp(b->data, "SDBK", 4) != 0 || rd16(b->data + 4) != 11 || b->sound_off + 20u * b->sounds > b->size ||
        b->wavebank_off + 16u * b->wavebanks > b->size) {
        fprintf(stderr, "se: %s is not a sound bank\n", path);
        free(b->data);
        memset(b, 0, sizeof *b);
        return -1;
    }
    return 0;
}

/* ---- the wave banks (in the game's memory) ---- */

typedef struct WaveBank {
    const uint8_t *data;
    uint32_t size;
    char name[17];
    int count;
    ALuint *buffers; /* made on first use */
    int32_t *frames;
    int *rates;
    /* the worker's decode of every wave */
    SfJob job;
    SfWave *dec;
    int *dec_ok;
    SDL_AtomicInt dec_n; /* waves decoded */
    SDL_AtomicInt cancel;
} WaveBank;

static WaveBank s_wb[SE_WAVEBANKS];

/* ---- voices ---- */

typedef struct Track {
    int on;
    ALuint src;
    WaveBank *wb;
    int wave;
    int loop;
    float gain;     /* the sound's and the track's levels, linear */
    float bin[6];   /* SetMixBins' speaker levels, linear: front L/R, centre, LFE, rear L/R */
    float l, r;     /* those folded to stereo */
    float pitch;
    int start;      /* the frame its Play runs */
    int end;        /* the frame it has played out, or -1 while it loops */
    int stop;       /* the frame of its StopEG, or -1 */
    int release;    /* frames of release after a stop */
    int released;   /* the frame the release began, or -1 */
    /* AmpEG, in frames, and its sustain level */
    int eg;
    int attack, hold, decay;
    float sustain;
    /* PitchLFO: Hz, octaves, and frames before it starts */
    float lfo_rate;
    float lfo_depth;
    int lfo_delay;
    /* SetVolume's fade */
    int fade_at, fade_len;
    float fade_to;
} Track;

typedef struct Voice {
    int cue;   /* nonzero while it holds a cue */
    int state; /* 1: stopped, freed at the next frame */
    u16 slot;
    u16 cue_idx;
    u8 grp;
    u8 prio;
    float pan_l, pan_r; /* the source's mix bin volumes, linear */
    Track tr[SE_TRACKS];
} Voice;

typedef struct Request {
    u16 slot;
    u16 cue_idx;
    u8 grp;
    u8 prio;
    u8 e;
    s32 pan;
    u16 g;
} Request;

typedef struct Slot {
    int loaded;
    int bank; /* sound bank */
    WaveBank *own; /* the wave bank this slot registered */
} Slot;

static Voice s_voice[SE_VOICES];
static Request s_queue[SE_QUEUE];
static u32 s_queued;
static Slot s_slot[SE_SLOTS];
static s32 s_entry[SE_SLOTS] = { -1, -1, -1 };
static int s_ready;
static int s_al;
static int s_loop_points;
static float s_master = 1.0f;
static int s_cue_serial;

static float db100(s32 v)
{
    return v <= -9600 ? 0.0f : powf(10.0f, (float)v / 2000.0f);
}

static int frames_of_ms(uint32_t ms)
{
    return (int)((ms * SE_FPS + 999u) / 1000u);
}

/* XACT envelope times count 512-sample periods at 48 kHz. */
static int frames_of_eg(uint32_t t)
{
    return (int)((t * 512u * SE_FPS + 47999u) / 48000u);
}

static int al_ok(void)
{
    if (!s_al && alcGetCurrentContext()) {
        s_al = 1;
        s_loop_points = alIsExtensionPresent("AL_SOFT_loop_points");
    }
    return s_al;
}

static void wb_drop(WaveBank *wb)
{
    int i;
    int t;
    if (!wb->data) {
        return;
    }
    for (i = 0; i < SE_VOICES; i++) {
        for (t = 0; t < SE_TRACKS; t++) {
            Track *tr = &s_voice[i].tr[t];
            if (tr->on && tr->wb == wb) {
                if (tr->src) {
                    alSourceStop(tr->src);
                    alSourcei(tr->src, AL_BUFFER, 0);
                }
                tr->on = 0;
            }
        }
    }
    if (wb->dec) {
        SDL_SetAtomicInt(&wb->cancel, 1);
        sf_job_wait(&wb->job);
        for (i = 0; i < wb->count; i++) {
            sf_wave_free(&wb->dec[i]);
        }
    }
    free(wb->dec);
    free(wb->dec_ok);
    if (wb->buffers) {
        for (i = 0; i < wb->count; i++) {
            if (wb->buffers[i]) {
                alDeleteBuffers(1, &wb->buffers[i]);
            }
        }
    }
    free(wb->buffers);
    free(wb->frames);
    free(wb->rates);
    memset(wb, 0, sizeof *wb);
}

/* The worker's half: each wave of the bank, in order. */
static void wb_decode_run(void *arg)
{
    WaveBank *wb = arg;
    int i;
    for (i = 0; i < wb->count && !SDL_GetAtomicInt(&wb->cancel); i++) {
        char err[64];
        wb->dec_ok[i] = sf_xwb_entry(wb->data, wb->size, i, &wb->dec[i], err, sizeof err) == 0;
        if (!wb->dec_ok[i]) {
            fprintf(stderr, "se: %s wave %d: %s\n", wb->name, i, err);
        }
        SDL_SetAtomicInt(&wb->dec_n, i + 1);
        sf_job_progress();
    }
}

typedef struct WaveNeed {
    WaveBank *wb;
    int idx;
} WaveNeed;

static int wave_decoded(void *arg)
{
    const WaveNeed *n = arg;
    return SDL_GetAtomicInt(&n->wb->dec_n) > n->idx;
}

static WaveBank *wb_register(const uint8_t *data, uint32_t size)
{
    int i;
    int count;
    WaveBank *wb = NULL;
    for (i = 0; i < SE_WAVEBANKS; i++) {
        if (s_wb[i].data == data) {
            /* the game reloaded this memory: what was decoded from it is gone */
            wb_drop(&s_wb[i]);
        }
    }
    for (i = 0; i < SE_WAVEBANKS && !wb; i++) {
        if (!s_wb[i].data) {
            wb = &s_wb[i];
        }
    }
    if (!wb || !data || sf_xwb_count(data, size, &count, NULL, 0) != 0 || count <= 0) {
        return NULL;
    }
    wb->buffers = calloc((size_t)count, sizeof *wb->buffers);
    wb->frames = calloc((size_t)count, sizeof *wb->frames);
    wb->rates = calloc((size_t)count, sizeof *wb->rates);
    if (!wb->buffers || !wb->frames || !wb->rates) {
        free(wb->buffers);
        free(wb->frames);
        free(wb->rates);
        memset(wb, 0, sizeof *wb);
        return NULL;
    }
    wb->data = data;
    wb->size = size;
    wb->count = count;
    sf_xwb_name(data, size, wb->name);
    wb->dec = calloc((size_t)count, sizeof *wb->dec);
    wb->dec_ok = calloc((size_t)count, sizeof *wb->dec_ok);
    if (wb->dec && wb->dec_ok) {
        SDL_SetAtomicInt(&wb->dec_n, 0);
        SDL_SetAtomicInt(&wb->cancel, 0);
        memset(&wb->job, 0, sizeof wb->job);
        sf_job_start(&wb->job, wb_decode_run, wb);
    } else {
        free(wb->dec);
        free(wb->dec_ok);
        wb->dec = NULL;
        wb->dec_ok = NULL;
    }
    return wb;
}

static WaveBank *wb_find(const char *name)
{
    int i;
    for (i = 0; i < SE_WAVEBANKS; i++) {
        if (s_wb[i].data && strncmp(s_wb[i].name, name, 16) == 0) {
            return &s_wb[i];
        }
    }
    return NULL;
}

/* The wave's length in frames and its rate; its AL buffer when sound is on. */
static int wave_prepare(WaveBank *wb, int idx)
{
    SfWave w;
    char err[64];
    if (idx < 0 || idx >= wb->count) {
        return -1;
    }
    if (wb->rates[idx] != 0) {
        return 0;
    }
    if (wb->dec) {
        /* the worker's, once it has got this far */
        WaveNeed n;
        n.wb = wb;
        n.idx = idx;
        sf_job_wait_for(&wb->job, wave_decoded, &n);
        if (!wave_decoded(&n) || !wb->dec_ok[idx]) {
            wb->rates[idx] = -1;
            return -1;
        }
        w = wb->dec[idx];
        memset(&wb->dec[idx], 0, sizeof wb->dec[idx]);
    } else if (sf_xwb_entry(wb->data, wb->size, idx, &w, err, sizeof err) != 0) {
        fprintf(stderr, "se: %s wave %d: %s\n", wb->name, idx, err);
        wb->rates[idx] = -1;
        return -1;
    }
    wb->frames[idx] = w.samples;
    wb->rates[idx] = w.rate;
    if (al_ok()) {
        alGenBuffers(1, &wb->buffers[idx]);
        alBufferData(wb->buffers[idx], w.channels == 2 ? AL_FORMAT_STEREO16 : AL_FORMAT_MONO16, w.pcm,
                     (ALsizei)((size_t)w.samples * (size_t)w.channels * 2), w.rate);
        if (w.loop_len > 0 && s_loop_points) {
            ALint pts[2] = { w.loop_start, w.loop_start + w.loop_len };
            alBufferiv(wb->buffers[idx], AL_LOOP_POINTS_SOFT, pts);
        }
    }
    sf_wave_free(&w);
    return wb->rates[idx] > 0 ? 0 : -1;
}

/* ---- playing a cue ---- */

static void track_reset(Track *tr)
{
    ALuint src = tr->src;
    memset(tr, 0, sizeof *tr);
    tr->src = src;
    tr->gain = 1.0f;
    tr->bin[0] = tr->bin[1] = 1.0f;
    tr->l = tr->r = 1.0f;
    tr->pitch = 1.0f;
    tr->end = -1;
    tr->stop = -1;
    tr->released = -1;
    tr->fade_at = -1;
    tr->sustain = 1.0f;
}

static void track_wave(Track *tr, WaveBank *wb, int wave, int loop, int start)
{
    float secs;
    if (!wb || wave_prepare(wb, wave) != 0) {
        return;
    }
    tr->on = 1;
    tr->wb = wb;
    tr->wave = wave;
    tr->loop = loop;
    tr->start = start;
    secs = (float)wb->frames[wave] / ((float)wb->rates[wave] * tr->pitch);
    tr->end = loop ? -1 : start + (int)ceilf(secs * SE_FPS) + 1;
}

/* A track's events (each an 8-byte _XACT_TRACK_EVENT_HEADER, then bSize
 * bytes of its body). */
static void track_events(const SoundBank *b, uint32_t off, int count, Track *tr, WaveBank *const *wbs, int now)
{
    int e;
    for (e = 0; e < count; e++) {
        const uint8_t *p;
        uint32_t w0;
        int type;
        uint32_t ts;
        int size;
        u16 loops;
        if (off + 8u > b->size) {
            return;
        }
        p = b->data + off;
        w0 = rd32(p);
        type = (int)(w0 & 0xFF);
        ts = w0 >> 8;
        size = p[4];
        loops = rd16(p + 6);
        if (off + 8u + (uint32_t)size > b->size) {
            return;
        }
        switch (type) {
        case 0: /* Play */
            if (size >= 4) {
                u16 wave = rd16(p + 8);
                u16 bank = rd16(p + 10);
                track_wave(tr, bank < b->wavebanks ? wbs[bank] : NULL, wave, loops != 0, now + frames_of_ms(ts));
            }
            break;
        case 2: /* Stop */
        case 3: /* StopAndEGRelease */
            tr->stop = now + frames_of_ms(ts);
            if (type == 2) {
                tr->release = 0;
            }
            break;
        case 5: /* SetVolume: a fade to the level over dwDuration */
            if (size >= 8) {
                tr->fade_at = now + frames_of_ms(ts);
                tr->fade_len = frames_of_ms(rd32(p + 12) >> 8);
                tr->fade_to = db100((s16)rd16(p + 8));
            }
            break;
        case 10: /* AmplitudeEG */
            if (size >= 12) {
                tr->eg = 1;
                tr->attack = frames_of_eg(rd16(p + 10));
                tr->hold = frames_of_eg(rd16(p + 12));
                tr->decay = frames_of_eg(rd16(p + 14));
                tr->release = frames_of_eg(rd16(p + 16));
                tr->sustain = (float)p[18] / 255.0f;
            }
            break;
        case 16: /* SetMixBins: the speaker levels */
        {
            int k;
            for (k = 0; k + 4 <= size && k < 32; k += 4) {
                u16 bin = rd16(p + 8 + k);
                if (bin < 6) {
                    tr->bin[bin] = db100((s16)rd16(p + 10 + k));
                }
            }
            /* Some sounds sit low and lean on the rear speakers (the
             * announcer's second call: -9 dB, front -2 dB, rear 0 dB).
             * Folded to stereo the ITU way: centre and rear at -3 dB,
             * LFE left out. */
            tr->l = tr->bin[0] + 0.70710678f * (tr->bin[2] + tr->bin[4]);
            tr->r = tr->bin[1] + 0.70710678f * (tr->bin[2] + tr->bin[5]);
            break;
        }
        case 8: /* PitchLFO */
            if (size >= 4 && p[10] != 0 && !(getenv("SF_SE_LFO") && atoi(getenv("SF_SE_LFO")) == 0)) {
                tr->lfo_delay = (int)((float)rd16(p + 8) * 32.0f / 48000.0f * SE_FPS + 0.5f);
                tr->lfo_rate = 375.0f * (float)p[10] / 16384.0f;
                tr->lfo_depth = (float)(s8)p[11] / 128.0f;
            }
            break;
        default: /* MixBinPan, markers: not applied */
            break;
        }
        off += 8u + (uint32_t)size;
    }
}

static int voice_cue(Voice *v, int slot, int cue_idx, int now)
{
    const SoundBank *b;
    const uint8_t *cue;
    const uint8_t *snd;
    WaveBank *wbs[8];
    u16 sound;
    uint32_t u;
    float gain;
    float pitch;
    int flags;
    int tracks;
    int i;
    int on = 0;
    if (!s_slot[slot].loaded || s_slot[slot].bank < 0 || s_slot[slot].bank >= SE_BANKS) {
        return 0;
    }
    b = &s_sdbk[s_slot[slot].bank];
    if (!b->data || cue_idx >= b->cues) {
        return 0;
    }
    cue = b->data + 0x38 + 20 * cue_idx;
    sound = rd16(cue + 2);
    if (sound >= b->sounds) {
        return 0;
    }
    for (i = 0; i < 8; i++) {
        wbs[i] = i < b->wavebanks ? wb_find((const char *)b->data + b->wavebank_off + 16 * i) : NULL;
    }
    snd = b->data + b->sound_off + 20u * sound;
    u = rd32(snd);
    gain = db100(-16 * (s32)(rd16(snd + 4) & 0x1FF));
    pitch = powf(2.0f, (float)(s16)rd16(snd + 6) / 4096.0f);
    tracks = snd[8];
    flags = snd[11];
    for (i = 0; i < SE_TRACKS; i++) {
        track_reset(&v->tr[i]);
        v->tr[i].gain = gain;
        v->tr[i].pitch = pitch;
    }
    if (flags & 8) {
        /* one wave: the sound holds its play descriptor */
        u16 bank = (u16)(u >> 16);
        track_wave(&v->tr[0], bank < b->wavebanks && bank < 8 ? wbs[bank] : NULL, (int)(u & 0xFFFF), 0, now);
    } else {
        for (i = 0; i < tracks && i < SE_TRACKS; i++) {
            uint32_t te;
            if (u + 4u * (uint32_t)i + 4u > b->size) {
                break;
            }
            te = rd32(b->data + u + 4u * (uint32_t)i);
            track_events(b, te >> 8, (int)(te & 0xFF), &v->tr[i], wbs, now);
        }
    }
    for (i = 0; i < SE_TRACKS; i++) {
        on |= v->tr[i].on;
    }
    return on;
}

static float track_level(const Track *tr, int now)
{
    float env = 1.0f;
    float g = tr->gain;
    int t = now - tr->start;
    if (tr->eg) {
        if (t < tr->attack) {
            env = (float)(t + 1) / (float)(tr->attack + 1);
        } else if (t < tr->attack + tr->hold) {
            env = 1.0f;
        } else if (t < tr->attack + tr->hold + tr->decay) {
            float k = (float)(t - tr->attack - tr->hold) / (float)tr->decay;
            env = 1.0f + (tr->sustain - 1.0f) * k;
        } else {
            env = tr->sustain;
        }
    }
    if (tr->released >= 0) {
        int r = now - tr->released;
        env *= tr->release > 0 ? 1.0f - (float)r / (float)tr->release : 0.0f;
        if (env < 0.0f) {
            env = 0.0f;
        }
    }
    if (tr->fade_at >= 0 && now >= tr->fade_at) {
        float k = tr->fade_len > 0 ? (float)(now - tr->fade_at) / (float)tr->fade_len : 1.0f;
        g *= 1.0f + (tr->fade_to - 1.0f) * (k > 1.0f ? 1.0f : k);
    }
    return g * env;
}

static void track_al(Voice *v, Track *tr, int now)
{
    float l;
    float r;
    float a;
    float level;
    if (!al_ok()) {
        return;
    }
    if (!tr->src) {
        alGenSources(1, &tr->src);
        if (!tr->src) {
            return;
        }
        alSourcei(tr->src, AL_SOURCE_RELATIVE, AL_TRUE);
        alSourcef(tr->src, AL_ROLLOFF_FACTOR, 0.0f);
        alSourcef(tr->src, AL_MAX_GAIN, 4.0f);
    }
    if (D_00797100) {
        l = r = 1.0f;
    } else {
        l = v->pan_l * tr->l;
        r = v->pan_r * tr->r;
    }
    level = track_level(tr, now) * s_master;
    /* a mono source panned by power between the two speakers */
    a = atan2f(r, l) * 2.0f - 1.5707964f;
    alSource3f(tr->src, AL_POSITION, sinf(a), 0.0f, -cosf(a));
    alSourcef(tr->src, AL_GAIN, level * sqrtf(l * l + r * r));
    if (tr->lfo_rate > 0.0f && now >= tr->start + tr->lfo_delay) {
        /* a triangle from 0, up first, by the frame clock */
        float ph = (float)(now - tr->start - tr->lfo_delay) / SE_FPS * tr->lfo_rate;
        float tri;
        ph -= floorf(ph);
        tri = ph < 0.25f ? 4.0f * ph : ph < 0.75f ? 2.0f - 4.0f * ph : 4.0f * ph - 4.0f;
        alSourcef(tr->src, AL_PITCH, tr->pitch * exp2f(tr->lfo_depth * tri));
    }
    if (now == tr->start) {
        alSourcei(tr->src, AL_BUFFER, (ALint)tr->wb->buffers[tr->wave]);
        alSourcei(tr->src, AL_LOOPING, tr->loop ? AL_TRUE : AL_FALSE);
        alSourcef(tr->src, AL_PITCH, tr->pitch);
        alSourcePlay(tr->src);
    }
}

static void voice_stop_al(Voice *v)
{
    int t;
    for (t = 0; t < SE_TRACKS; t++) {
        if (v->tr[t].src && s_al) {
            alSourceStop(v->tr[t].src);
            alSourcei(v->tr[t].src, AL_BUFFER, 0);
        }
        v->tr[t].on = 0;
    }
}

/* Called a frame: advances every voice and frees those that are done. */
static void voices_frame(int now)
{
    int i;
    int t;
    for (i = 0; i < SE_VOICES; i++) {
        Voice *v = &s_voice[i];
        int live = 0;
        if (!v->cue) {
            continue;
        }
        if (v->state == 1) {
            voice_stop_al(v);
            v->cue = 0;
            continue;
        }
        for (t = 0; t < SE_TRACKS; t++) {
            Track *tr = &v->tr[t];
            if (!tr->on) {
                continue;
            }
            if (tr->stop >= 0 && now >= tr->stop && tr->released < 0) {
                tr->released = now;
            }
            if ((tr->end >= 0 && now >= tr->end) || (tr->released >= 0 && now - tr->released >= tr->release)) {
                if (tr->src && s_al) {
                    alSourceStop(tr->src);
                    alSourcei(tr->src, AL_BUFFER, 0);
                }
                tr->on = 0;
                continue;
            }
            live = 1;
            if (now >= tr->start) {
                track_al(v, tr, now);
            }
        }
        if (!live) {
            /* retail's end notification: the voice is free */
            v->cue = 0;
            v->state = 1;
        }
    }
}

/* ---- retail's layer ---- */

/* xb_SeGroupFree: 0 when a playing voice of the group outranks prio. */
static int group_free(u8 grp, u8 prio)
{
    int i;
    for (i = 0; i < SE_VOICES; i++) {
        if (s_voice[i].cue && s_voice[i].state != 1 && s_voice[i].grp == grp && s_voice[i].prio > prio) {
            return 0;
        }
    }
    return 1;
}

static void stop_group(u8 grp)
{
    int i;
    for (i = 0; i < SE_VOICES; i++) {
        if (s_voice[i].cue && s_voice[i].state != 1 && s_voice[i].grp == grp) {
            voice_stop_al(&s_voice[i]);
            s_voice[i].state = 1;
        }
    }
}

/* xb_StartSoundEntry: 0 keeps the request queued (no voice is free). */
static int start_entry(const Request *q, int now)
{
    Voice *v = NULL;
    int i;
    if (q->slot >= SE_SLOTS || !s_slot[q->slot].loaded || !group_free(q->grp, q->prio)) {
        return 1;
    }
    for (i = 0; i < SE_VOICES; i++) {
        if (!s_voice[i].cue) {
            v = &s_voice[i];
            break;
        }
    }
    if (!v) {
        return 0;
    }
    if (q->pan != 0x100) {
        s32 pan = q->pan < -32 ? -32 : q->pan > 32 ? 32 : q->pan;
        v->pan_l = db100(k_pan_l[pan + 32]);
        v->pan_r = db100(k_pan_r[pan + 32]);
    }
    stop_group(q->grp);
    if (!voice_cue(v, q->slot, q->cue_idx, now)) {
        return 1;
    }
    if (getenv("SF_SE_DEBUG")) {
        int t;
        fprintf(stderr, "se: frame %d slot %u cue %u grp %u prio %u pan %d:", now, q->slot, q->cue_idx, q->grp, q->prio, q->pan);
        for (t = 0; t < SE_TRACKS; t++) {
            const Track *tr = &v->tr[t];
            if (tr->on) {
                fprintf(stderr, " [%s %d %dHz x%.3f%s end %d]", tr->wb->name, tr->wave, tr->wb->rates[tr->wave], tr->pitch,
                        tr->loop ? " loop" : "", tr->end);
            }
        }
        fprintf(stderr, "\n");
    }
    v->cue = ++s_cue_serial ? s_cue_serial : ++s_cue_serial;
    v->state = 0;
    v->slot = q->slot;
    v->cue_idx = q->cue_idx;
    v->grp = q->grp;
    v->prio = q->prio;
    return 1;
}

/* Retail's xb_XactFrame: free finished voices, then start what is queued,
 * in order, until one cannot start. */
void sf_se_frame(void)
{
    int now = sf_port_frame;
    u32 k;
    u32 count;
    if (!s_ready) {
        return;
    }
    voices_frame(now);
    count = s_queued;
    for (k = 0; k < count; k++) {
        if (!start_entry(&s_queue[0], now)) {
            break;
        }
        s_queued--;
        memmove(&s_queue[0], &s_queue[1], s_queued * sizeof s_queue[0]);
    }
    /* what started this frame begins playing now */
    if (s_al) {
        int i;
        int t;
        for (i = 0; i < SE_VOICES; i++) {
            for (t = 0; t < SE_TRACKS; t++) {
                Track *tr = &s_voice[i].tr[t];
                if (s_voice[i].cue && s_voice[i].state != 1 && tr->on && tr->start == now) {
                    track_al(&s_voice[i], tr, now);
                }
            }
        }
    }
}

s32 xb_InitSoundBankList(void)
{
    int i;
    int loaded = 0;
    for (i = 0; i < SE_BANKS; i++) {
        if (!s_sdbk[i].data && bank_load(i, &s_sdbk[i]) == 0) {
            loaded++;
        } else if (s_sdbk[i].data) {
            loaded++;
        }
    }
    if (loaded != SE_BANKS) {
        fprintf(stderr, "se: %d of %d sound banks in data/audio/sdbk; no sound effects "
                        "(sf33rdsxb --sound-banks sf33rd.xbe --data DIR writes them)\n",
                loaded, SE_BANKS);
    }
    for (i = 0; i < SE_SLOTS; i++) {
        s_slot[i].loaded = 0;
        s_slot[i].bank = -1;
    }
    for (i = 0; i < SE_VOICES; i++) {
        voice_stop_al(&s_voice[i]);
        s_voice[i].cue = 0;
        s_voice[i].state = 0;
        s_voice[i].pan_l = s_voice[i].pan_r = 1.0f;
    }
    s_queued = 0;
    D_00797100 = 0;
    s_ready = loaded == SE_BANKS;
    return 1;
}

s32 xb_ClearColorRamEntries(void)
{
    s_entry[0] = s_entry[1] = s_entry[2] = -1;
    return 0;
}

/* xb_PurgeListEntry: stops the slot's voices and lets its bank go. */
void xb_PurgeListEntry(u8 id)
{
    int i;
    if (id >= SE_SLOTS) {
        return;
    }
    s_entry[id] = -1;
    if (!s_slot[id].loaded) {
        return;
    }
    for (i = 0; i < SE_VOICES; i++) {
        if (s_voice[i].cue && s_voice[i].state != 1 && s_voice[i].slot == id) {
            voice_stop_al(&s_voice[i]);
            s_voice[i].state = 1;
        }
    }
    if (s_slot[id].own) {
        wb_drop(s_slot[id].own);
        s_slot[id].own = NULL;
    }
    s_slot[id].loaded = 0;
}

/* Retail's resident wave banks (filled by color3rd.c's types 0x50 to 0x52). */
extern u8 D_007C5CE0[];
extern u8 D_00881400[];
extern u8 D_00B84200[];

/* xb_SetColorRamEntry / xb_LoadListEntry: slot 0 is the resident set
 * (sound bank 0 over the three resident wave banks); slots 1 and 2 take
 * sound bank `index` over the wave bank at addr. */
s32 xb_SetColorRamEntry(void *addr, u32 size, s32 id, s32 index)
{
    if (id < 0 || id >= SE_SLOTS) {
        return 0;
    }
    if (id == 0) {
        wb_register(D_007C5CE0, 0xBB6FC);
        wb_register(D_00881400, 0x1490B4);
        wb_register(D_00B84200, 0xC7CC4);
        s_slot[0].bank = 0;
        s_slot[0].loaded = 1;
    } else {
        if (s_slot[id].loaded) {
            xb_PurgeListEntry((u8)id);
        }
        s_slot[id].own = wb_register(addr, size);
        s_slot[id].bank = index;
        s_slot[id].loaded = s_slot[id].own != NULL && index > 0 && index < SE_BANKS;
    }
    s_entry[id] = index;
    return 0;
}

s32 xb_GetColorRamEntry(s32 i)
{
    return i >= 0 && i < SE_SLOTS ? s_entry[i] : -1;
}

/* xb_TsbQueueRequest: slot, cue, group, priority, a flag, the pan
 * (-32..32, 0x100 to keep the voice's), and a value retail passes on. */
s32 xb_TsbQueueRequest(u32 slot, u32 cue_idx, s32 grp, u8 prio, u8 e, s32 pan, s16 g)
{
    Request *q;
    if (s_queued >= SE_QUEUE) {
        return 0;
    }
    q = &s_queue[s_queued++];
    q->slot = (u16)slot;
    q->cue_idx = (u16)cue_idx;
    q->grp = (u8)grp;
    q->prio = prio;
    q->e = e;
    q->pan = pan;
    q->g = (u16)g;
    return 1;
}

s32 xb_StopSeByGroup(u32 bank, u32 code, s32 grp, u8 prio)
{
    int i;
    (void)bank;
    (void)code;
    for (i = 0; i < SE_VOICES; i++) {
        if (s_voice[i].cue && s_voice[i].state != 1 && s_voice[i].grp == (u8)grp && s_voice[i].prio <= prio) {
            voice_stop_al(&s_voice[i]);
            s_voice[i].state = 1;
        }
    }
    return 1;
}

void xb_StopAllSeSlots(void)
{
    int i;
    for (i = 0; i < SE_VOICES; i++) {
        if (s_voice[i].cue && s_voice[i].state != 1) {
            voice_stop_al(&s_voice[i]);
            s_voice[i].state = 1;
        }
    }
}

/* The master effect volume (mlSysSetBankVolume), in hundredths of a dB. */
void IXACTEngine_SetMasterVolume(s32 engine, s32 category, s32 vol)
{
    (void)engine;
    (void)category;
    s_master = db100(vol);
}

/* At exit, before the device closes: the voices stop and the sources and
 * wave buffers go. */
void sf_se_shutdown(void)
{
    int i;
    int t;
    if (!s_al) {
        return;
    }
    for (i = 0; i < SE_VOICES; i++) {
        voice_stop_al(&s_voice[i]);
        s_voice[i].cue = 0;
        for (t = 0; t < SE_TRACKS; t++) {
            if (s_voice[i].tr[t].src) {
                alDeleteSources(1, &s_voice[i].tr[t].src);
                s_voice[i].tr[t].src = 0;
            }
        }
    }
    for (i = 0; i < SE_WAVEBANKS; i++) {
        int w;
        for (w = 0; s_wb[i].buffers && w < s_wb[i].count; w++) {
            if (s_wb[i].buffers[w]) {
                alDeleteBuffers(1, &s_wb[i].buffers[w]);
                s_wb[i].buffers[w] = 0;
            }
        }
    }
    s_al = 0;
    s_ready = 0;
}
