#include "game/tracks.h"

#include "common.h"
#include "mem/heap.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>

int sf_track_is_full(const char *name)
{
    size_t n;
    char a;
    char b;
    if (!name) {
        return 0;
    }
    n = strlen(name);
    if (n < 5 || strcmp(name + n - 4, ".adx") != 0) {
        return 0;
    }
    if (n >= 8 && name[n - 7] == '_') {
        a = name[n - 6];
        b = name[n - 5];
        if (isxdigit((unsigned char)a) && isxdigit((unsigned char)b)) {
            return 0;
        }
    }
    return 1;
}

int sf_tracks_load(const char *install, SfTrack **out, int *count, char *err, size_t err_n)
{
    char path[8192];
    FILE *fp;
    char line[256];
    unsigned version = 0;
    unsigned files = 0;
    SfTrack *list = NULL;
    int n = 0;
    if (out) {
        *out = NULL;
    }
    if (count) {
        *count = 0;
    }
    if (!install || !out || !count) {
        sf_set_err(err, err_n, "missing track arguments");
        return -1;
    }
    if (sf_join(path, sizeof path, install, "data/afs/index") != 0) {
        sf_set_err(err, err_n, "archive index path does not fit");
        return -1;
    }
    fp = fopen(path, "r");
    if (!fp) {
        sf_set_err(err, err_n, "archive index is missing");
        return -1;
    }
    if (!fgets(line, (int)sizeof line, fp) || sscanf(line, "sf33rd-afs %u", &version) != 1 || version != 1 || !fgets(line, (int)sizeof line, fp) || sscanf(line, "files %u", &files) != 1) {
        fclose(fp);
        sf_set_err(err, err_n, "archive index header is wrong");
        return -1;
    }
    while (fgets(line, (int)sizeof line, fp)) {
        unsigned num = 0;
        unsigned bytes = 0;
        char name[48];
        SfTrack *next;
        memset(name, 0, sizeof name);
        if (sscanf(line, "%u %47s %u", &num, name, &bytes) != 3 || bytes == 0 || !sf_track_is_full(name)) {
            continue;
        }
        next = sf_realloc(list, ((size_t)n + 1u) * sizeof *list);
        if (!next) {
            sf_free(list);
            fclose(fp);
            sf_set_err(err, err_n, "track memory");
            return -1;
        }
        list = next;
        memset(&list[n], 0, sizeof list[n]);
        list[n].fnum = num;
        memcpy(list[n].name, name, sizeof list[n].name);
        n++;
    }
    fclose(fp);
    *out = list;
    *count = n;
    return 0;
}

void sf_tracks_free(SfTrack *tracks)
{
    sf_free(tracks);
}
