#include "render/nv2a.h"

#include "common.h"

#include <SDL3/SDL.h>

#include <stdint.h>
#include <string.h>

enum {
    SF_GL_LOWER_LEFT = 0x8CA1,
    SF_GL_UPPER_LEFT = 0x8CA2,
    SF_GL_NEGATIVE_ONE_TO_ONE = 0x935E,
    SF_GL_ZERO_TO_ONE = 0x935F,
    SF_GL_COMPILE_STATUS = 0x8B81,
    SF_SPIRV_MAGIC = 0x07230203
};

int sf_spirv_check(const void *bytes, size_t size, char *err, size_t err_n)
{
    const uint32_t *words;
    if (!bytes || size < 20 || (size % 4) != 0 || ((uintptr_t)bytes % 4) != 0) {
        sf_set_err(err, err_n, "SPIR-V module is empty or misaligned");
        return -1;
    }
    if (size > 16u * 1024u * 1024u) {
        sf_set_err(err, err_n, "SPIR-V module is too large");
        return -1;
    }
    words = bytes;
    if (words[0] != SF_SPIRV_MAGIC) {
        sf_set_err(err, err_n, "SPIR-V magic is missing");
        return -1;
    }
    return 0;
}

int sf_vk_shader_spirv(VkDevice device, const void *bytes, size_t size, VkShaderModule *module, char *err, size_t err_n)
{
    VkShaderModuleCreateInfo info;
    if (!module) {
        sf_set_err(err, err_n, "missing Vulkan shader module");
        return -1;
    }
    *module = VK_NULL_HANDLE;
    if (sf_spirv_check(bytes, size, err, err_n) != 0) {
        return -1;
    }
    memset(&info, 0, sizeof info);
    info.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    info.codeSize = size;
    info.pCode = bytes;
    if (vkCreateShaderModule(device, &info, NULL, module) != VK_SUCCESS) {
        sf_set_err(err, err_n, "Vulkan rejected the SPIR-V module");
        *module = VK_NULL_HANDLE;
        return -1;
    }
    return 0;
}

/* The GL calls past 1.1, through SDL: Windows' opengl32 exports only 1.1. */
static PFNGLCREATESHADERPROC p_glCreateShader;
static PFNGLSHADERBINARYPROC p_glShaderBinary;
static PFNGLGETSHADERIVPROC p_glGetShaderiv;
static PFNGLGETSHADERINFOLOGPROC p_glGetShaderInfoLog;
static PFNGLDELETESHADERPROC p_glDeleteShader;
static PFNGLCLIPCONTROLPROC p_glClipControl;

static void load_gl(void)
{
    if (p_glCreateShader) {
        return;
    }
    p_glCreateShader = (PFNGLCREATESHADERPROC)SDL_GL_GetProcAddress("glCreateShader");
    p_glShaderBinary = (PFNGLSHADERBINARYPROC)SDL_GL_GetProcAddress("glShaderBinary");
    p_glGetShaderiv = (PFNGLGETSHADERIVPROC)SDL_GL_GetProcAddress("glGetShaderiv");
    p_glGetShaderInfoLog = (PFNGLGETSHADERINFOLOGPROC)SDL_GL_GetProcAddress("glGetShaderInfoLog");
    p_glDeleteShader = (PFNGLDELETESHADERPROC)SDL_GL_GetProcAddress("glDeleteShader");
    p_glClipControl = (PFNGLCLIPCONTROLPROC)SDL_GL_GetProcAddress("glClipControl");
}

int sf_gl_shader_spirv(GLenum stage, const void *bytes, size_t size, GLuint *shader, char *err, size_t err_n)
{
    PFNGLSPECIALIZESHADERPROC specialize;
    GLuint id = 0;
    GLint status = 0;
    if (!shader) {
        sf_set_err(err, err_n, "missing OpenGL shader");
        return -1;
    }
    *shader = 0;
    if (stage != GL_VERTEX_SHADER && stage != GL_FRAGMENT_SHADER) {
        sf_set_err(err, err_n, "OpenGL shader stage is not vertex or fragment");
        return -1;
    }
    if (sf_spirv_check(bytes, size, err, err_n) != 0) {
        return -1;
    }
    specialize = (PFNGLSPECIALIZESHADERPROC)SDL_GL_GetProcAddress("glSpecializeShader");
    if (!specialize) {
        specialize = (PFNGLSPECIALIZESHADERPROC)SDL_GL_GetProcAddress("glSpecializeShaderARB");
    }
    load_gl();
    if (!specialize || !p_glCreateShader || !p_glShaderBinary || !p_glGetShaderiv || !p_glGetShaderInfoLog ||
        !p_glDeleteShader) {
        sf_set_err(err, err_n, "this OpenGL context cannot load SPIR-V");
        return -1;
    }
    id = p_glCreateShader(stage);
    if (!id) {
        sf_set_err(err, err_n, "OpenGL shader object failed");
        return -1;
    }
    p_glShaderBinary(1, &id, GL_SHADER_BINARY_FORMAT_SPIR_V, bytes, (GLsizei)size);
    specialize(id, "main", 0, NULL, NULL);
    p_glGetShaderiv(id, SF_GL_COMPILE_STATUS, &status);
    if (!status) {
        char log[1024];
        GLsizei len = 0;
        p_glGetShaderInfoLog(id, (GLsizei)sizeof log, &len, log);
        if (len < 0) {
            len = 0;
        }
        log[len < (GLsizei)sizeof log ? len : (GLsizei)sizeof log - 1] = 0;
        sf_set_err(err, err_n, "OpenGL rejected the SPIR-V module%s%s", log[0] ? ": " : "", log);
        p_glDeleteShader(id);
        return -1;
    }
    *shader = id;
    return 0;
}

void sf_gl_clip_canvas(void)
{
    load_gl();
    if (p_glClipControl) {
        p_glClipControl(SF_GL_LOWER_LEFT, SF_GL_NEGATIVE_ONE_TO_ONE);
    }
}

void sf_gl_clip_nv2a(void)
{
    load_gl();
    if (p_glClipControl) {
        p_glClipControl(SF_GL_UPPER_LEFT, SF_GL_ZERO_TO_ONE);
    }
}
