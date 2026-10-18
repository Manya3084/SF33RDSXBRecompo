#ifndef SF_SETTINGS_H
#define SF_SETTINGS_H

typedef enum SfRendererKind {
    SF_RENDER_VULKAN = 0,
    SF_RENDER_OPENGL = 1
} SfRendererKind;

typedef struct SfSettings {
    int width;
    int height;
    SfRendererKind renderer;
} SfSettings;

typedef struct SfResPreset {
    int width;
    int height;
    const char *label;
} SfResPreset;

#define SF_RES_COUNT 5

extern const SfResPreset sf_res_presets[SF_RES_COUNT];

void sf_settings_default(SfSettings *settings);
int sf_settings_load(SfSettings *settings, const char *install_dir);
int sf_settings_save(const SfSettings *settings, const char *install_dir);

#endif
