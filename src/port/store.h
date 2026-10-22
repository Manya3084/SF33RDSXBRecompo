#ifndef SF_PORT_STORE_H
#define SF_PORT_STORE_H

/* SQLite saves. The game keeps its structs; these files are only storage.
 * data/save/game.sqlite holds the system row, the directions, and ten
 * replay slots. config/host.sqlite holds the window and the keyboard.
 * Schema version is 1. A write is one transaction, with the journal
 * synced. A file that is not a database is left as it is.
 *
 * The memory-card header, the network ids, and the old .sav are not
 * stored. A read of those fields is zero, and a replay's fname is NULL.
 *
 * 0 the row was read, 1 there is no row, -1 the file is damaged or the
 * write failed. Screen_Mode is stored as it is. */

#include "structs.h"

int sf_store_read_system(const char *path, struct _SAVE_W *sw);
int sf_store_write_system(const char *path, const struct _SAVE_W *sw);
int sf_store_read_directions(const char *path, SystemDir *dir);
int sf_store_write_directions(const char *path, const SystemDir *dir);

/* slot is 0 .. 9. saved_at may be NULL. It is the unix time of the write. */
int sf_store_read_replay(const char *path, int slot, _REPLAY_W *w, long long *saved_at);
int sf_store_write_replay(const char *path, int slot, const _REPLAY_W *w);

/* mode and aspect are the short names ("windowed", "4:3"). */
int sf_store_read_window(const char *path, char *mode, unsigned mode_n, int *width, int *height, char *aspect,
                         unsigned aspect_n);
int sf_store_write_window(const char *path, const char *mode, int width, int height, const char *aspect);

/* values[i] is the scancode name, or empty when that name has no row.
 * 1 means the key table has no rows at all. */
int sf_store_read_keys(const char *path, const char *const *names, char values[][64], int n);
int sf_store_write_keys(const char *path, const char *const *names, const char *const *values, int n);

/* A named setting's value; 1 means it has no row. */
int sf_store_read_setting(const char *path, const char *name, char *value, unsigned value_n);
int sf_store_write_setting(const char *path, const char *name, const char *value);

#endif
