/* The game's language: GAME OPTION's LANGUAGE row (sf_language_step,
 * sf_language_text). Retail took it from the console: a Japanese Xbox
 * set to Japanese ran the game as the Japanese release (Country 1, its
 * text and names, its CPU difficulty and music ducking), and any other
 * as the American one here (Country 4). JAPANESE is that Japanese Xbox.
 * config/host.sqlite keeps the choice; SF_COUNTRY still overrides the
 * region for tests.
 *
 * Retail's text tables read the language at 0x7AA5F1, which is also
 * mpp_w.language; the port has them as two variables and sets both. */

#include "host/compat.h"
#include "host/paths.h"
#include "port/port.h"
#include "port/store.h"

#include "types.h"
#include "structs.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

extern MPP mpp_w;
extern u8 Country;
extern s8 D_007AA5F1;
void Setup_Difficult_V(void);

static int s_japanese;

static int host_db(char *out, size_t n)
{
    char base[4096];
    int wrote;
    if (sf_exe_dir(base, sizeof base) != 0) {
        return -1;
    }
    wrote = snprintf(out, n, "%s/config", base);
    if (wrote < 0 || (size_t)wrote >= n) {
        return -1;
    }
    if (mkdir(out, 0755) != 0 && errno != EEXIST) {
        return -1;
    }
    wrote = snprintf(out, n, "%s/config/host.sqlite", base);
    return wrote < 0 || (size_t)wrote >= n ? -1 : 0;
}

/* The game's variables as the choice has them. */
static void apply(void)
{
    const char *env = getenv("SF_COUNTRY");
    if (env && env[0] >= '1' && env[0] <= '4' && env[1] == 0) {
        Country = (u8)(env[0] - '0');
    } else {
        Country = s_japanese ? 1 : 4;
    }
    mpp_w.language = (s8)(Country == 1);
    D_007AA5F1 = (s8)(Country == 1);
}

/* A setting of the port's in config/host.sqlite: 0 with its value. */
int sf_setting_read(const char *name, char *value, unsigned n)
{
    char path[4096 + 64];
    if (host_db(path, sizeof path) != 0) {
        return -1;
    }
    return sf_store_read_setting(path, name, value, n);
}

void sf_setting_write(const char *name, const char *value)
{
    char path[4096 + 64];
    if (host_db(path, sizeof path) == 0) {
        sf_store_write_setting(path, name, value);
    }
}

/* At start-up, from the region setup (src/port/system.c). */
void sf_language_init(void)
{
    char path[4096 + 64];
    char value[32];
    if (host_db(path, sizeof path) == 0 && sf_store_read_setting(path, "language", value, sizeof value) == 0) {
        s_japanese = strcmp(value, "japanese") == 0;
    }
    apply();
}

const char *sf_language_text(void)
{
    return s_japanese ? "JAPANESE" : "ENGLISH";
}

/* Left or right on the row: the other language, kept. */
int sf_language_step(int dir)
{
    char path[4096 + 64];
    (void)dir;
    s_japanese = !s_japanese;
    apply();
    Setup_Difficult_V(); /* the CPU's difficulty values are the region's */
    if (host_db(path, sizeof path) == 0) {
        sf_store_write_setting(path, "language", s_japanese ? "japanese" : "english");
    }
    return 1;
}

/* A netplay game plays in the host's language (not kept): the game's
 * variables as that has them, until the session ends. */
void sf_language_force(int japanese)
{
    int mine = s_japanese;
    s_japanese = japanese;
    apply();
    Setup_Difficult_V();
    s_japanese = mine;
}

/* For netplay: the language both machines play in. */
int sf_language_japanese(void)
{
    return s_japanese;
}
