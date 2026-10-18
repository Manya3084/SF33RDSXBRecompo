#ifndef SF_PATHS_H
#define SF_PATHS_H

#include <stddef.h>

int sf_exe_dir(char *out, size_t n);
int sf_find_shader_tool(const char *start_dir, char *out, size_t n);

#endif
