#ifndef SF_NV2A_H
#define SF_NV2A_H

#define GL_GLEXT_PROTOTYPES
#include <GL/gl.h>
#include <GL/glext.h>
#include <vulkan/vulkan.h>

#include <stddef.h>

/* SPIR-V from tools/spirv.py. One set, so a vertex program and a combiner
   share a Vulkan pipeline layout and link as one desktop OpenGL program.
   OpenGL matches uniform blocks by binding and ignores the set. */
enum {
    SF_NV2A_SET = 0,
    SF_NV2A_VERT_CONST = 0,
    SF_NV2A_VIEWPORT = 1,
    SF_NV2A_FRAG_CONST = 2,
    SF_NV2A_SAMPLER0 = 3
};

#ifndef GL_VERTEX_SHADER
#define GL_VERTEX_SHADER 0x8B31
#endif
#ifndef GL_FRAGMENT_SHADER
#define GL_FRAGMENT_SHADER 0x8B30
#endif

int sf_spirv_check(const void *bytes, size_t size, char *err, size_t err_n);
int sf_vk_shader_spirv(VkDevice device, const void *bytes, size_t size, VkShaderModule *module, char *err, size_t err_n);
int sf_gl_shader_spirv(GLenum stage, const void *bytes, size_t size, GLuint *shader, char *err, size_t err_n);

/* The converted modules use Vulkan clip space. The UI canvas does not. */
void sf_gl_clip_canvas(void);
void sf_gl_clip_nv2a(void);

#endif
