#ifndef SF_AUDIO_H
#define SF_AUDIO_H

#include <stddef.h>
#include <stdint.h>

int sf_audio_open(char *err, size_t err_n);
void sf_audio_close(void);

/* pcm is interleaved s16. samples counts one channel. A loop with
   loop_start at 0 repeats one buffer. A later loop_start plays the
   intro once, then repeats [loop_start, loop_end). */
int sf_audio_play(const int16_t *pcm, int32_t samples, int rate, int channels, int loop, int32_t loop_start, int32_t loop_end);
void sf_audio_pump(void);

#endif
