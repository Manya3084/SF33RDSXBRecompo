#ifndef SF_PORT_GFX_H
#define SF_PORT_GFX_H

#include "render/draw.h"

/* The draw list the game's graphics calls fill (src/port/gfx.c). */
SfDrawList *sf_gfx_list(void);

/* The start-up states, before the game's first call. */
void sf_gfx_init(void);

/* Before the backend draws the list: palettes the game wrote are published. */
void sf_gfx_frame_ready(void);

/* After the backend drew the list: the next frame starts empty. */
void sf_gfx_frame_done(void);

#endif
