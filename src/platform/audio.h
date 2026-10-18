#ifndef SF_AUDIO_H
#define SF_AUDIO_H

#include <stddef.h>

int sf_audio_open(char *err, size_t err_n);
void sf_audio_close(void);

#endif
