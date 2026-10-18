#ifndef SF_EXTRACT_H
#define SF_EXTRACT_H

#include <stddef.h>

typedef struct SfExtractResult {
    int files;
    int shaders;
    int spirv;
    int spirv_failed;
    char error[512];
    char note[512];
} SfExtractResult;

typedef void (*SfExtractProgress)(const char *path, void *user);

int sf_data_ready(const char *install_dir);

/* source is an Xbox XDVDFS image, a zip or 7z of that Xbox disc, or an extracted Xbox folder. */
int sf_extract(const char *source, const char *install_dir, const char *shader_tool,
               SfExtractProgress progress, void *user, SfExtractResult *result);

int sf_retry_shaders(const char *install_dir, const char *shader_tool, SfExtractResult *result);

#endif
