#include "platform/audio.h"

#include "common.h"

#include <AL/alc.h>

static ALCdevice *g_dev;
static ALCcontext *g_ctx;

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
        alcMakeContextCurrent(NULL);
        alcDestroyContext(g_ctx);
        g_ctx = NULL;
    }
    if (g_dev) {
        alcCloseDevice(g_dev);
        g_dev = NULL;
    }
}
