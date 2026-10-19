#ifndef SF_PORT_H
#define SF_PORT_H

/* The platform the decomp's C runs on: what the Xbox XDK, the kernel and
 * the Xbox Live code gave retail. Each shim is named as the XDK names it;
 * src/port/stubs.c (generated) covers every name no shim defines yet. */

#include <stddef.h>
#include <stdint.h>

/* Reports a call to a name that has no shim yet, once per name. */
void sf_port_missing(const char *name);

/* The game frame running, for messages. */
extern int sf_port_frame;

/* Allocation below 4 GB, for memory whose address the game keeps in a u32. */
void *sf_low_alloc(size_t size, size_t align);
void sf_low_free(void *p);

/* Video: the game's 640x448 frame, shown in the SDL3 window. */
void sf_video_init(int w, int h);
void sf_video_set_offset(int x, int y);
void sf_video_present(void);

/* A whole member of the imported archive, malloc'd. */
int sf_afs_read(int32_t fid, uint8_t **data, uint32_t *size);

/* Sound: refills the voices; once per frame. */
void sf_sound_pump(void);
/* The sound effects: retail's per-frame XACT work (src/port/se.c). */
void sf_se_frame(void);

/* The game: start-up, then one call per frame (src/port/frame.c). */
void sf_game_init(void);
void sf_game_frame(void);

#endif
