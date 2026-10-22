#ifndef SF_PORT_H
#define SF_PORT_H

/* The platform the decomp's C runs on: what the Xbox XDK, the kernel and
 * the Xbox Live code gave retail. Each shim is named as the XDK names it;
 * src/port/stubs.c (generated) covers every name no shim defines yet. */

#include <stddef.h>
#include <stdint.h>

/* Reports a call to a name that has no shim yet, once per name. */
void sf_port_missing(const char *name);

/* A stop on an error: the message to stderr and, with a window, in a
 * dialog, then the program ends (src/port/main.c). On Android it ends
 * without running the libraries' exit handlers: OpenAL's mixer thread
 * would lock the mutexes they destroy, and Android aborts on that. */
void sf_fatal(const char *fmt, ...);

/* The game's state as one number, the same on every machine playing the
 * same game (src/port/sync.c); SF_SYNC_LOG=PATH writes it every frame. */
uint32_t sf_sync_hash(void);
void sf_sync_frame(void);
int sf_sync_testing(void);

/* The game frame running, for messages. */
extern int sf_port_frame;

/* The game's heaps (src/port/arena.c). */
void *sf_arena_alloc(size_t size, size_t align);
void sf_arena_extent(uint8_t **base, size_t *used);
/* The game-visible heap in the arena (src/port/gheap.c). */
void *sf_gheap_alloc(size_t n);
void sf_gheap_free(void *p);
void sf_gfx_state_restored(void);
void sf_se_state_restored(void);
void sf_state_restored(void);
/* GAME OPTION's LANGUAGE row (src/port/language.c). */
void sf_language_init(void);
const char *sf_language_text(void);
int sf_language_step(int dir);
int sf_language_japanese(void);
void sf_language_force(int japanese);
/* The LANGUAGE row's restart prompt: -1 closed, 0 English, 1 Japanese. */
int sf_language_asking(void);
void sf_language_ask_close(void);
int sf_language_ask_accept(void);
int sf_language_restart_ready(void);
/* Start this program again (the language prompt's yes). */
void sf_port_restart(void);
/* Netplay (src/port/net.c). */
#define SF_NET_PORT 33133
int sf_net_host(int port);
int sf_net_join(const char *address);
int sf_net_host_delay(int port, int delay);
void sf_net_cancel(void);
void sf_net_at_menu(void);
const char *sf_net_banner(void);
void sf_net_local_address(char *out, size_t n);
/* Games waiting on this network (src/port/lobby.c). announce sends this
 * machine's own beacon (a host waiting for a player); match_port is the
 * port a joiner calls. The list drops a game silent for a few seconds. */
void sf_lobby_poll(int announce, int match_port);
void sf_lobby_close(void);
int sf_lobby_count(void);
int sf_lobby_address(int index, char *out, size_t n);
int sf_net_page(int start);
void sf_net_page_text(const char *text);
int sf_setting_read(const char *name, char *value, unsigned n);
void sf_setting_write(const char *name, const char *value);
void sf_net_stop(void);
void sf_net_quit(void);
void sf_net_tick(void);
void sf_net_env(void);
int sf_net_active(void);
int sf_net_waiting(void);
int sf_net_resim(void);
const char *sf_net_status(void);
uint64_t sf_afs_identity(void);
int sf_retail_image_ok(void); /* src/port/retail.c */
void sf_video_wait_frame(void);
void sf_frame(void);
void sf_arena_free(void *p);

/* Video: the game's 640x448 frame, shown in the SDL3 window. */
void sf_video_init(int w, int h);
void sf_video_set_offset(int x, int y);
void sf_video_present(void);
/* The game's window (NULL when headless), and the shape its screen is
 * shown in (width over height; 0 fills the window). */
struct SDL_Window *sf_video_window(void);
void sf_video_set_aspect(float aspect);
struct SfCanvas;
void sf_video_present_canvas(const struct SfCanvas *canvas);

/* The first start's import screen, and the import on the command line
 * (src/port/setup.c). */
int sf_setup_run(const char *install);
int sf_setup_cli(int argc, char **argv);

/* The window's mode, size and aspect, kept in config/host.sqlite; SCREEN
 * ADJUST's rows 5 to 7 (src/port/display.c). A missing row is created
 * from the original window: 1280x960, 4:3. */
void sf_display_init(void);
const char *sf_display_text(int row);
int sf_display_step(int row, int dir);
void sf_display_default(void);
/* The picture's shape in the window: 4/3, 16/9, or 0 when it fills the window. */
float sf_display_aspect(void);

/* SCREEN ADJUST's FILTER, live: 0 OFF, 1 SMOOTH, 2 CRT CURVED, 3 CRT FLAT. */
int sf_save_screen_filter(void);
/* Writes the system database when the filter, the picture size, or the
 * position stored in save_w[1] differs from the file. The window rows
 * write config/host.sqlite on their own step. */
void sf_system_commit_screen(void);
/* Loads data/save/game.sqlite: the system row (options, rankings, pads,
 * screen) and the directions. A missing file is created from the original
 * defaults. Call once the init task has written those defaults. */
void sf_save_startup(void);

/* Every disc read finished (src/port/files.c). */
void sf_files_sync(void);
/* An archive member's place, and a read of the archive at an offset. */
int sf_afs_member(int32_t fid, uint32_t *offset, uint32_t *size);
long sf_afs_pread(void *buf, uint32_t n, uint32_t offset);

/* Sound: refills the voices; once per frame. */
void sf_sound_pump(void);
/* The app in the background (1) or back (0): the sound device pauses. */
void sf_sound_pause(int paused);
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
 * scancode for an Xbox button, kept in config/host.sqlite. A missing
 * key table is filled from the original bindings. */
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
/* The input tape (rollback): mark starts taping each frame's pads, rewind
 * makes the next reads take them again, off stops. */
void sf_input_tape_mark(void);
void sf_input_tape_rewind(void);
void sf_input_tape_seek(int at);
void sf_input_replay_rewind(void);
void sf_input_tape_off(void);

/* Replays (src/port/save.c): ten slots in data/save/game.sqlite.
 * sf_replay_slot says what a slot holds (1 a replay, 0 none, -1 damaged). */
#define SF_REPLAY_SLOTS 10
int sf_replay_slot(int slot, int chars[2], long long *when);
int sf_replay_save(int slot);
int sf_replay_load(int slot);

/* The game's state as one block (src/port/state.c), for rollback. SF_KEEP
 * puts a variable outside it, even in a test that snapshots the port too:
 * the snapshot's own bookkeeping and the input tape. */
#define SF_KEEP __attribute__((section("sfkeep")))
/* SF_GAME keeps a variable a port file defines in the game's state: the
 * game's own globals whose storage was in retail's platform files. */
#define SF_GAME __attribute__((section("sfgame")))
size_t sf_state_size(void);
void sf_state_save(uint8_t *buf);
void sf_state_restore(const uint8_t *buf);
void sf_state_test_frame(void);
/* The state's memory regions as the last sf_state_size found them. */
typedef struct SfStateRegion {
    uint8_t *p;
    size_t n;
} SfStateRegion;
int sf_state_regions(SfStateRegion *out, int max);
/* Rollback history (src/port/history.c): the last frames' states, kept as
 * the pages each frame wrote. */
int sf_hist_start(int frame);
void sf_hist_save(int frame);
int sf_hist_load(int frame);
int sf_hist_oldest(void);
int sf_hist_on(void);
void sf_hist_stop(void);
void sf_hist_touch(void *p, size_t n);
/* The state as start-up left it (src/port/state.c), for netplay. */
void sf_state_boot_save(void);
int sf_state_boot_ok(void);
int sf_state_boot_restore(void);
void sf_save_restart(void);
/* The music streams' place, which the game waits on (src/port/sound.c). */
size_t sf_sound_state_size(void);
void sf_sound_state_save(void *buf);
void sf_sound_state_restore(const void *buf);

/* The port's additions to the game's menus (src/port/menu_ext.c). */
/* The replay slot page, run by SaveMove each frame: start is set on the
 * first frame, save picks a slot to save to (else to load from). 1 while
 * open, 0 when done, -1 when left. */
int sf_replay_page(int start, int save);
void sf_menu_ext_init(void);
void sf_menu_ext_frame(void);
/* The frame's queued Xbox button pictures, drawn after its other 2D. */
void sf_button_images_draw(void);

#endif
