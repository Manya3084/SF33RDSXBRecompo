#include "platform/audio.h"

#include "common.h"

#include <AL/al.h>
#include <AL/alc.h>

static ALCdevice *g_dev;
static ALCcontext *g_ctx;
static ALuint g_source;
static ALuint g_intro;
static ALuint g_loop;
static int g_streaming;
static int g_keep;

static void drop_source(void)
{
    if (!g_ctx) {
        return;
    }
    if (g_source) {
        alSourceStop(g_source);
        alSourcei(g_source, AL_BUFFER, 0);
        alDeleteSources(1, &g_source);
        g_source = 0;
    }
    if (g_intro) {
        alDeleteBuffers(1, &g_intro);
        g_intro = 0;
    }
    if (g_loop) {
        alDeleteBuffers(1, &g_loop);
        g_loop = 0;
    }
    g_streaming = 0;
    g_keep = 0;
}

int sf_audio_open(char *err, size_t err_n)
{
    g_dev = alcOpenDevice(NULL);
    if (!g_dev) {
        sf_set_err(err, err_n, "OpenAL device did not open");
        return -1;
    }
    g_ctx = alcCreateContext(g_dev, NULL);
    if (!g_ctx || !alcMakeContextCurrent(g_ctx)) {
        if (g_ctx) {
            alcDestroyContext(g_ctx);
        }
        alcCloseDevice(g_dev);
        g_ctx = NULL;
        g_dev = NULL;
        sf_set_err(err, err_n, "OpenAL context did not open");
        return -1;
    }
    return 0;
}

void sf_audio_close(void)
{
    if (g_ctx) {
        alcMakeContextCurrent(g_ctx);
        drop_source();
        alcMakeContextCurrent(NULL);
        alcDestroyContext(g_ctx);
        g_ctx = NULL;
    }
    if (g_dev) {
        alcCloseDevice(g_dev);
        g_dev = NULL;
    }
}

static int bytes_ok(int32_t frames, int channels)
{
    if (frames <= 0 || (channels != 1 && channels != 2)) {
        return 0;
    }
    if (frames > (0x7FFFFFFF / (channels * 2))) {
        return 0;
    }
    return 1;
}

static int upload(ALuint buf, const int16_t *pcm, int32_t frames, int channels, int rate)
{
    ALenum format = channels == 2 ? AL_FORMAT_STEREO16 : AL_FORMAT_MONO16;
    ALsizei bytes = (ALsizei)frames * channels * (ALsizei)sizeof(int16_t);
    alBufferData(buf, format, pcm, bytes, rate);
    return alGetError() == AL_NO_ERROR ? 0 : -1;
}

int sf_audio_play(const int16_t *pcm, int32_t samples, int rate, int channels, int loop, int32_t loop_start, int32_t loop_end)
{
    int streaming = 0;
    int32_t end = samples;
    ALuint queued[3];
    int nq = 0;
    if (!g_ctx || !pcm || !bytes_ok(samples, channels) || rate <= 0) {
        return -1;
    }
    if (!alcMakeContextCurrent(g_ctx)) {
        return -1;
    }
    if (loop && loop_end > loop_start && loop_start >= 0 && loop_start < samples) {
        if (loop_end > samples) {
            loop_end = samples;
        }
        end = loop_end;
        if (loop_start > 0) {
            streaming = 1;
        }
    } else {
        loop = 0;
    }
    if (streaming) {
        if (!bytes_ok(loop_start, channels) || !bytes_ok(end - loop_start, channels)) {
            return -1;
        }
    } else if (!bytes_ok(end, channels)) {
        return -1;
    }
    drop_source();
    alGetError();
    alGenSources(1, &g_source);
    if (alGetError() != AL_NO_ERROR || g_source == 0) {
        g_source = 0;
        return -1;
    }
    if (streaming) {
        alGenBuffers(1, &g_intro);
        alGenBuffers(1, &g_loop);
    } else {
        alGenBuffers(1, &g_loop);
    }
    if (alGetError() != AL_NO_ERROR || g_loop == 0 || (streaming && g_intro == 0)) {
        drop_source();
        return -1;
    }
    if (streaming) {
        if (upload(g_intro, pcm, loop_start, channels, rate) != 0 || upload(g_loop, pcm + (size_t)loop_start * (size_t)channels, end - loop_start, channels, rate) != 0) {
            drop_source();
            return -1;
        }
        queued[nq++] = g_intro;
        queued[nq++] = g_loop;
        queued[nq++] = g_loop;
        alSourcei(g_source, AL_LOOPING, AL_FALSE);
        alSourceQueueBuffers(g_source, nq, queued);
    } else {
        if (upload(g_loop, pcm, end, channels, rate) != 0) {
            drop_source();
            return -1;
        }
        alSourcei(g_source, AL_LOOPING, loop ? AL_TRUE : AL_FALSE);
        alSourcei(g_source, AL_BUFFER, (ALint)g_loop);
    }
    alSourcef(g_source, AL_GAIN, 1.0f);
    alSourcePlay(g_source);
    if (alGetError() != AL_NO_ERROR) {
        drop_source();
        return -1;
    }
    g_streaming = streaming;
    g_keep = streaming || loop;
    return 0;
}

void sf_audio_pump(void)
{
    ALint processed = 0;
    ALint state = 0;
    if (!g_ctx || !g_source) {
        return;
    }
    if (g_streaming) {
        alGetSourcei(g_source, AL_BUFFERS_PROCESSED, &processed);
        while (processed > 0) {
            ALuint buf = 0;
            alSourceUnqueueBuffers(g_source, 1, &buf);
            if (buf == g_loop) {
                alSourceQueueBuffers(g_source, 1, &g_loop);
            }
            processed--;
        }
    }
    if (!g_keep) {
        return;
    }
    alGetSourcei(g_source, AL_SOURCE_STATE, &state);
    if (state != AL_PLAYING) {
        alSourcePlay(g_source);
    }
}
