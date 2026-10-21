#ifndef SF_PORT_STORE_H
#define SF_PORT_STORE_H

/* Versioned text for the game's saves and the host's settings. The game
 * keeps using its structs; this is only the file. A file is "key=value"
 * lines under [section] headings. version=1 is required on a file this
 * writes. A bare key=value file (an older display.txt or keyboard.txt)
 * loads with an empty section name and no version. */

#include "structs.h"

typedef struct SfIni SfIni;

SfIni *sf_ini_new(void);
void sf_ini_free(SfIni *ini);
void sf_ini_set_note(SfIni *ini, const char *note);

/* 0 when the file loaded, 1 when there is none, -1 when it is damaged. */
int sf_ini_load(SfIni *ini, const char *path);
/* Replace path: write a temp file beside it, flush and sync, then rename. */
int sf_ini_save(const SfIni *ini, const char *path);

/* 0 when the key is there (*value stays valid until the next set or free),
 * 1 when it is not. */
int sf_ini_find(const SfIni *ini, const char *section, const char *key, const char **value);
int sf_ini_set(SfIni *ini, const char *section, const char *key, const char *value);
int sf_ini_set_int(SfIni *ini, const char *section, const char *key, long long value);

/* Rename path to path.bak, replacing an older bak. */
int sf_file_retire(const char *path);

/* 0 loaded, 1 missing, -1 damaged. A damaged file is not a missing one.
 * On 0 the struct is filled from the fields, with its padding zero and,
 * for a replay, game_infor.fname NULL. */
int sf_store_read_system(const char *path, struct _SAVE_W *sw);
int sf_store_write_system(const char *path, const struct _SAVE_W *sw);
int sf_store_read_directions(const char *path, SystemDir *dir);
int sf_store_write_directions(const char *path, const SystemDir *dir);
int sf_store_read_replay(const char *path, _REPLAY_W *w);
int sf_store_write_replay(const char *path, const _REPLAY_W *w);

#endif
