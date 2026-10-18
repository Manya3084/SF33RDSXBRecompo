#ifndef SF_TRACKS_H
#define SF_TRACKS_H

#include <stddef.h>
#include <stdint.h>

/* A full ADX mix from the archive index. Stem files such as
   o01_A_NYC_00.adx are left out. */

typedef struct SfTrack {
    uint32_t fnum;
    char name[48];
} SfTrack;

int sf_track_is_full(const char *name);
int sf_tracks_load(const char *install, SfTrack **out, int *count, char *err, size_t err_n);
void sf_tracks_free(SfTrack *tracks);

#endif
