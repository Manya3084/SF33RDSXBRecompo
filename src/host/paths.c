#include "host/paths.h"

#include "common.h"

#include <SDL3/SDL.h>

#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

int sf_exe_dir(char *out, size_t n)
{
#ifdef __ANDROID__
    /* the app's own storage (Android/data/APP/files, which a computer
     * sees over USB), or its internal storage: the install is read-only */
    const char *base = SDL_GetAndroidExternalStoragePath();
    if (!base) {
        base = SDL_GetAndroidInternalStoragePath();
    }
#else
    /* SDL3 owns and caches this string; it is not freed */
    const char *base = SDL_GetBasePath();
#endif
    size_t len;
    if (!base || !out || n == 0) {
        return -1;
    }
    if (snprintf(out, n, "%s", base) >= (int)n) {
        return -1;
    }
    len = strlen(out);
    while (len > 1 && out[len - 1] == '/') {
        out[--len] = 0;
    }
    return 0;
}

int sf_find_shader_tool(const char *start_dir, char *out, size_t n)
{
    char dir[4096];
    int up;
    if (!start_dir || !out) {
        return -1;
    }
    if (strlen(start_dir) >= sizeof dir) {
        return -1;
    }
    memcpy(dir, start_dir, strlen(start_dir) + 1);
    for (up = 0; up < 5; up++) {
        char path[4096];
        struct stat st;
        char *slash;
        if (sf_join(path, sizeof path, dir, "tools/spirv.py") == 0 && stat(path, &st) == 0 && S_ISREG(st.st_mode)) {
            if (strlen(path) >= n) {
                return -1;
            }
            memcpy(out, path, strlen(path) + 1);
            return 0;
        }
        slash = strrchr(dir, '/');
        if (!slash || slash == dir) {
            break;
        }
        *slash = 0;
    }
    return -1;
}
