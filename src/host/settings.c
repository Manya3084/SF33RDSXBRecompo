#include "host/settings.h"

#include "common.h"
#include "iso/archive.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

const SfResPreset sf_res_presets[SF_RES_COUNT] = {
    {640, 480, "640x480"},
    {1280, 720, "1280x720"},
    {1920, 1080, "1920x1080"},
    {2560, 1440, "2560x1440"},
    {0, 0, "desktop"},
};

void sf_settings_default(SfSettings *settings)
{
    settings->width = 1280;
    settings->height = 720;
    settings->renderer = SF_RENDER_VULKAN;
}

int sf_settings_load(SfSettings *settings, const char *install_dir)
{
    char path[4096];
    FILE *fp;
    char line[256];
    sf_settings_default(settings);
    if (!install_dir || sf_join(path, sizeof path, install_dir, "config/settings.txt") != 0) {
        return -1;
    }
    fp = fopen(path, "rb");
    if (!fp) {
        return 1;
    }
    while (fgets(line, sizeof line, fp)) {
        char key[64];
        char val[64];
        if (sscanf(line, "%63s %63s", key, val) != 2) {
            continue;
        }
        if (strcmp(key, "width") == 0) {
            settings->width = atoi(val);
        } else if (strcmp(key, "height") == 0) {
            settings->height = atoi(val);
        } else if (strcmp(key, "renderer") == 0) {
            settings->renderer = strcmp(val, "opengl") == 0 ? SF_RENDER_OPENGL : SF_RENDER_VULKAN;
        }
    }
    fclose(fp);
    if (settings->width < 0 || settings->height < 0) {
        sf_settings_default(settings);
    }
    return 0;
}

int sf_settings_save(const SfSettings *settings, const char *install_dir)
{
    char dir[4096];
    char path[4096];
    FILE *fp;
    char err[128];
    if (!settings || !install_dir) {
        return -1;
    }
    if (sf_join(dir, sizeof dir, install_dir, "config") != 0 || sf_mkdir_p(dir, err, sizeof err) != 0) {
        return -1;
    }
    if (sf_join(path, sizeof path, install_dir, "config/settings.txt") != 0) {
        return -1;
    }
    fp = fopen(path, "wb");
    if (!fp) {
        return -1;
    }
    fprintf(fp, "width %d\nheight %d\nrenderer %s\n", settings->width, settings->height,
            settings->renderer == SF_RENDER_OPENGL ? "opengl" : "vulkan");
    if (fclose(fp) != 0) {
        return -1;
    }
    return 0;
}
