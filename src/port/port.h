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
/* The game's window (NULL when headless), and the shape its screen is
 * shown in (width over height; 0 fills the window). */
struct SDL_Window *sf_video_window(void);
void sf_video_set_aspect(float aspect);

/* The window's mode, size and aspect, kept in config/display.txt; SCREEN
 * ADJUST's rows 5 to 7 (src/port/display.c). */
void sf_display_init(void);
const char *sf_display_text(int row);
int sf_display_step(int row, int dir);
void sf_display_default(void);

/* A whole member of the imported archive, malloc'd. */
int sf_afs_read(int32_t fid, uint8_t **data, uint32_t *size);

/* Sound: refills the voices; once per frame. */
void sf_sound_pump(void);
/* The sound effects: retail's per-frame XACT work (src/port/se.c). */
void sf_se_frame(void);
void sf_se_shutdown(void);

/* The game: start-up, then one call per frame (src/port/frame.c). */
void sf_game_init(void);
void sf_game_frame(void);

/* Quitting: the game asks (the mode menu's EXIT GAME, the window closing),
 * and the main loop shuts down after the frame (src/port/main.c). */
void sf_port_quit(void);
int sf_port_quitting(void);
void sf_video_shutdown(void);
void sf_sound_shutdown(void);

/* The keyboard's bindings (src/port/input.c): 14 rows, each an SDL
 * scancode for an Xbox button, kept in config/keyboard.txt. */
int sf_keys_count(void);
const char *sf_keys_label(int i);
int sf_keys_get(int i);
void sf_keys_set(int i, int key);
void sf_keys_default(void);
int sf_keys_save(void);
/* While on, the game sees no pad or key pressed. */
void sf_input_block(int on);
/* 1 while SF_REPLAY plays a recording in place of the devices. */
int sf_input_replaying(void);

/* The port's additions to the game's menus (src/port/menu_ext.c). */
void sf_menu_ext_init(void);
void sf_menu_ext_frame(void);

#endif
