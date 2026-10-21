/* The game's screen on OpenGL 3.3: the fallback where Vulkan is missing.
 *
 * It draws the same draw list (render/draw.h) the same way render/scene.c
 * does on Vulkan: the batches onto a 640x480 screen (colour and depth) at
 * a whole multiple of its size, the screen onto the output (opaque for the
 * PS2-sized picture, blended for what is over it), and the output into
 * the window at the asked shape. The shaders are scene.c's, written for
 * GLSL 330: the push constants are uniforms, and the texture is bound per
 * batch, as the fragment shader fetches texels itself.
 *
 * GL's window coordinates count rows from the bottom; here the screen's
 * top is row 0 of each framebuffer, so gl_FragCoord, the scissor and the
 * viewports have the same numbers as on Vulkan, and only the last draw,
 * into the window, turns the picture over. Depth: the vertex shader gives
 * z * 2 - 1, which GL clips and stores as Vulkan does z. */

#include "render/renderer_internal.h"

#include "common.h"
#include "render/draw.h"

#include <SDL3/SDL_opengl.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- the GL calls past 1.1, through SDL ---- */

#define GL_FUNCS(X)                                                                                                    \
    X(PFNGLCREATESHADERPROC, glCreateShader)                                                                           \
    X(PFNGLSHADERSOURCEPROC, glShaderSource)                                                                           \
    X(PFNGLCOMPILESHADERPROC, glCompileShader)                                                                         \
    X(PFNGLGETSHADERIVPROC, glGetShaderiv)                                                                             \
    X(PFNGLGETSHADERINFOLOGPROC, glGetShaderInfoLog)                                                                   \
    X(PFNGLDELETESHADERPROC, glDeleteShader)                                                                           \
    X(PFNGLCREATEPROGRAMPROC, glCreateProgram)                                                                         \
    X(PFNGLATTACHSHADERPROC, glAttachShader)                                                                           \
    X(PFNGLLINKPROGRAMPROC, glLinkProgram)                                                                             \
    X(PFNGLGETPROGRAMIVPROC, glGetProgramiv)                                                                           \
    X(PFNGLGETPROGRAMINFOLOGPROC, glGetProgramInfoLog)                                                                 \
    X(PFNGLDELETEPROGRAMPROC, glDeleteProgram)                                                                         \
    X(PFNGLUSEPROGRAMPROC, glUseProgram)                                                                               \
    X(PFNGLGETUNIFORMLOCATIONPROC, glGetUniformLocation)                                                               \
    X(PFNGLUNIFORM1IPROC, glUniform1i)                                                                                 \
    X(PFNGLUNIFORM1UIPROC, glUniform1ui)                                                                               \
    X(PFNGLUNIFORM1FPROC, glUniform1f)                                                                                 \
    X(PFNGLUNIFORM2FPROC, glUniform2f)                                                                                 \
    X(PFNGLUNIFORM4FPROC, glUniform4f)                                                                                 \
    X(PFNGLGENVERTEXARRAYSPROC, glGenVertexArrays)                                                                     \
    X(PFNGLBINDVERTEXARRAYPROC, glBindVertexArray)                                                                     \
    X(PFNGLDELETEVERTEXARRAYSPROC, glDeleteVertexArrays)                                                               \
    X(PFNGLGENBUFFERSPROC, glGenBuffers)                                                                               \
    X(PFNGLBINDBUFFERPROC, glBindBuffer)                                                                               \
    X(PFNGLBUFFERDATAPROC, glBufferData)                                                                               \
    X(PFNGLDELETEBUFFERSPROC, glDeleteBuffers)                                                                         \
    X(PFNGLVERTEXATTRIBPOINTERPROC, glVertexAttribPointer)                                                             \
    X(PFNGLENABLEVERTEXATTRIBARRAYPROC, glEnableVertexAttribArray)                                                     \
    X(PFNGLGENFRAMEBUFFERSPROC, glGenFramebuffers)                                                                     \
    X(PFNGLBINDFRAMEBUFFERPROC, glBindFramebuffer)                                                                     \
    X(PFNGLFRAMEBUFFERTEXTURE2DPROC, glFramebufferTexture2D)                                                           \
    X(PFNGLCHECKFRAMEBUFFERSTATUSPROC, glCheckFramebufferStatus)                                                       \
    X(PFNGLDELETEFRAMEBUFFERSPROC, glDeleteFramebuffers)                                                               \
    X(PFNGLBLENDEQUATIONPROC, glBlendEquation)                                                                         \
    X(PFNGLACTIVETEXTUREPROC, glActiveTexture)

#define SF_GL_DECLARE(type, name) static type p_##name;
GL_FUNCS(SF_GL_DECLARE)
#undef SF_GL_DECLARE

static int load_gl(void)
{
#define SF_GL_LOAD(type, name)                                                                                            \
    p_##name = (type)SDL_GL_GetProcAddress(#name);                                                                     \
    if (!p_##name) {                                                                                                   \
        return -1;                                                                                                     \
    }
    GL_FUNCS(SF_GL_LOAD)
#undef SF_GL_LOAD
    return 0;
}

/* ---- the shaders: render/scene.c's, for GLSL 330 ---- */

static const char k_scene_vert[] =
    "#version 330 core\n"
    "layout(location = 0) in vec3 pos;\n"
    "layout(location = 1) in vec4 color;\n"
    "layout(location = 2) in vec2 uv;\n"
    "layout(location = 3) in vec4 rect;\n"
    "out vec4 v_color;\n"
    "out vec2 v_uv;\n"
    "flat out vec4 v_rect;\n"
    "void main()\n"
    "{\n"
    "    gl_Position = vec4((pos.x + 0.5) / 320.0 - 1.0, (pos.y + 0.5) / 240.0 - 1.0, pos.z * 2.0 - 1.0, 1.0);\n"
    "    v_color = color;\n"
    "    v_uv = uv;\n"
    "    v_rect = rect;\n"
    "}\n";

/* shaders/scene.frag, one texture bound at a time */
static const char k_scene_frag[] =
    "#version 330 core\n"
    "uniform sampler2D tex;\n"
    "uniform sampler2D pal;\n"
    "uniform uint u_pal;\n"
    "uniform uint u_ops;\n"
    "uniform uint u_misc;\n"
    "uniform vec2 u_size;\n"
    "uniform float u_scale;\n"
    "in vec4 v_color;\n"
    "in vec2 v_uv;\n"
    "flat in vec4 v_rect;\n"
    "out vec4 o;\n"
    "int address(int i, int n, uint mode)\n"
    "{\n"
    "    if (mode == 3u) return clamp(i, 0, n - 1);\n"
    "    if (mode == 2u) {\n"
    "        int m = i % (2 * n);\n"
    "        if (m < 0) m += 2 * n;\n"
    "        return m < n ? m : 2 * n - 1 - m;\n"
    "    }\n"
    "    int w = i % n;\n"
    "    return w < 0 ? w + n : w;\n"
    "}\n"
    "ivec2 lo;\n"
    "ivec2 hi;\n"
    "vec4 fetch(ivec2 t)\n"
    "{\n"
    "    ivec2 n = ivec2(u_size);\n"
    "    t = clamp(t, lo, hi);\n"
    "    ivec2 a = ivec2(address(t.x, n.x, (u_ops >> 16) & 0xFFu), address(t.y, n.y, (u_ops >> 24) & 0xFFu));\n"
    "    vec4 c = texelFetch(tex, a, 0);\n"
    "    if ((u_misc & 2u) != 0u) {\n"
    "        int index = int(c.r * 255.0 + 0.5);\n"
    "        c = texelFetch(pal, ivec2(index, int(u_pal)), 0);\n"
    "    }\n"
    "    return c;\n"
    "}\n"
    "vec4 sample_texture()\n"
    "{\n"
    "    vec2 scale = (u_misc & 4u) != 0u ? vec2(1.0) : u_size;\n"
    "    vec2 base = (floor(gl_FragCoord.xy / u_scale) + 0.5) * u_scale;\n"
    "    vec2 d = base - gl_FragCoord.xy;\n"
    "    vec2 uv = v_uv + dFdx(v_uv) * d.x + dFdy(v_uv) * d.y;\n"
    "    vec2 t = uv * scale;\n"
    "    vec2 r0 = v_rect.xy * scale;\n"
    "    vec2 r1 = v_rect.zw * scale;\n"
    "    lo = ivec2(floor(r0 + 1.0 / 1024.0));\n"
    "    hi = max(lo, ivec2(ceil(r1 - 1.0 / 1024.0)) - 1);\n"
    "    if ((u_misc & 1u) != 0u) {\n"
    "        return fetch(ivec2(floor(t + 1.0 / 1024.0)));\n"
    "    }\n"
    "    t -= 0.5;\n"
    "    ivec2 i = ivec2(floor(t));\n"
    "    vec2 f = t - floor(t);\n"
    "    vec4 a = mix(fetch(i), fetch(i + ivec2(1, 0)), f.x);\n"
    "    vec4 b = mix(fetch(i + ivec2(0, 1)), fetch(i + ivec2(1, 1)), f.x);\n"
    "    return mix(a, b, f.y);\n"
    "}\n"
    "float op1(uint op, float t, float d)\n"
    "{\n"
    "    if (op == 2u) return t;\n"
    "    if (op == 3u) return d;\n"
    "    if (op == 7u) return min(t + d, 1.0);\n"
    "    return t * d;\n"
    "}\n"
    "vec3 op3(uint op, vec3 t, vec3 d)\n"
    "{\n"
    "    if (op == 2u) return t;\n"
    "    if (op == 3u) return d;\n"
    "    if (op == 7u) return min(t + d, vec3(1.0));\n"
    "    return t * d;\n"
    "}\n"
    "bool passes(uint func, float a, float ref)\n"
    "{\n"
    "    if (func == 0u) return false;\n"
    "    if (func == 1u) return a < ref;\n"
    "    if (func == 2u) return a == ref;\n"
    "    if (func == 3u) return a <= ref;\n"
    "    if (func == 4u) return a > ref;\n"
    "    if (func == 5u) return a != ref;\n"
    "    if (func == 6u) return a >= ref;\n"
    "    return true;\n"
    "}\n"
    "void main()\n"
    "{\n"
    "    uint cop = u_ops & 0xFFu;\n"
    "    uint aop = (u_ops >> 8) & 0xFFu;\n"
    "    vec4 c = v_color;\n"
    "    if ((u_misc & 8u) != 0u && cop != 1u) {\n"
    "        vec4 t = sample_texture();\n"
    "        c.rgb = op3(cop, t.rgb, v_color.rgb);\n"
    "        if (aop != 1u) c.a = op1(aop, t.a, v_color.a);\n"
    "    }\n"
    "    if ((u_misc & 16u) != 0u) {\n"
    "        float a = floor(c.a * 255.0 + 0.5);\n"
    "        if (!passes((u_misc >> 5) & 7u, a, float((u_misc >> 8) & 0xFFu))) discard;\n"
    "    }\n"
    "    o = c;\n"
    "}\n";

/* shaders/show.vert: one triangle over the viewport, src the part shown */
static const char k_quad_vert[] =
    "#version 330 core\n"
    "uniform vec4 src;\n"
    "out vec2 v_uv;\n"
    "void main()\n"
    "{\n"
    "    vec2 p = vec2(float((gl_VertexID << 1) & 2), float(gl_VertexID & 2));\n"
    "    gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);\n"
    "    v_uv = mix(src.xy, src.zw, p);\n"
    "}\n";

/* shaders/copy.frag when opaque is 0. The window (opaque) is show.frag:
 * filter_mode 0 keeps each texel, 1 filters the scale, 2 is CRT CURVED
 * and 3 is CRT FLAT. */
static const char k_quad_frag[] =
    "#version 330 core\n"
    "uniform sampler2D screen;\n"
    "uniform vec4 src;\n"
    "uniform int opaque;\n"
    "uniform float filter_mode;\n"
    "in vec2 v_uv;\n"
    "out vec4 o;\n"
    "vec3 taken(vec2 uv, int sharp)\n"
    "{\n"
    "    if (sharp != 0) {\n"
    "        vec2 size = vec2(textureSize(screen, 0));\n"
    "        uv = (floor(uv * size) + 0.5) / size;\n"
    "    }\n"
    "    return texture(screen, uv).rgb;\n"
    "}\n"
    "void main()\n"
    "{\n"
    "    int mode;\n"
    "    vec2 span;\n"
    "    vec2 p;\n"
    "    vec2 c;\n"
    "    vec2 uv;\n"
    "    vec2 px;\n"
    "    vec3 col;\n"
    "    vec3 glow;\n"
    "    vec3 mask;\n"
    "    float scan;\n"
    "    float mask_x;\n"
    "    float vig;\n"
    "    if (opaque == 0) {\n"
    "        o = texture(screen, v_uv);\n"
    "        return;\n"
    "    }\n"
    "    mode = int(filter_mode + 0.5);\n"
    "    if (mode < 2) {\n"
    "        o = vec4(taken(v_uv, mode == 0 ? 1 : 0), 1.0);\n"
    "        return;\n"
    "    }\n"
    "    span = src.zw - src.xy;\n"
    "    p = (v_uv - src.xy) / span;\n"
    "    if (mode == 2) {\n"
    "        c = p * 2.0 - 1.0;\n"
    "        c *= 1.0 + dot(c, c) * 0.08;\n"
    "        p = c * 0.5 + 0.5;\n"
    "        if (p.x < 0.0 || p.y < 0.0 || p.x > 1.0 || p.y > 1.0) {\n"
    "            o = vec4(0.0, 0.0, 0.0, 1.0);\n"
    "            return;\n"
    "        }\n"
    "    }\n"
    "    uv = src.xy + span * p;\n"
    "    col = taken(uv, 0);\n"
    "    scan = sin(fract(p.y * 448.0) * 3.14159265);\n"
    "    col *= mix(0.68, 1.0, scan * scan);\n"
    "    mask_x = mod(gl_FragCoord.x, 3.0);\n"
    "    mask = mask_x < 1.0 ? vec3(1.0, 0.80, 0.80)\n"
    "         : mask_x < 2.0 ? vec3(0.80, 1.0, 0.80)\n"
    "                        : vec3(0.80, 0.80, 1.0);\n"
    "    col *= mask;\n"
    "    vig = smoothstep(1.15, 0.35, length((p - 0.5) * vec2(1.12, 1.0)));\n"
    "    col *= mix(0.78, 1.0, vig);\n"
    "    px = span / vec2(640.0, 448.0);\n"
    "    glow = taken(uv + vec2(px.x, 0.0), 0) + taken(uv - vec2(px.x, 0.0), 0) +\n"
    "           taken(uv + vec2(0.0, px.y), 0) + taken(uv - vec2(0.0, px.y), 0);\n"
    "    o = vec4(col + glow * 0.03, 1.0);\n"
    "}\n";

/* ---- the scene ---- */

typedef struct GlScene {
    GLuint prog;
    GLint u_pal;
    GLint u_ops;
    GLint u_misc;
    GLint u_size;
    GLint u_scale;
    GLuint quad;
    GLint q_src;
    GLint q_opaque;
    GLint q_filter;
    GLuint vao;
    GLuint vbuf;
    GLuint ibuf;
    uint32_t quads; /* the index buffer's */
    GLuint tex[SF_TEX_MAX];
    int tex_w[SF_TEX_MAX];
    int tex_h[SF_TEX_MAX];
    uint32_t tex_version[SF_TEX_MAX];
    GLuint pal;
    uint32_t pal_version[SF_PAL_MAX];
    int pal_ready;
    GLuint dummy;
    int scale;
    GLuint color; /* the screen */
    GLuint depth;
    GLuint fb;
    GLuint back; /* the output */
    GLuint back_fb;
    int back_valid;
    uint8_t *readback;
} GlScene;

static GLuint compile(GLenum kind, const char *src, char *err, size_t err_n)
{
    GLuint sh = p_glCreateShader(kind);
    GLint ok = 0;
    p_glShaderSource(sh, 1, &src, NULL);
    p_glCompileShader(sh);
    p_glGetShaderiv(sh, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[1024];
        p_glGetShaderInfoLog(sh, sizeof log, NULL, log);
        sf_set_err(err, err_n, "OpenGL shader: %s", log);
        p_glDeleteShader(sh);
        return 0;
    }
    return sh;
}

static GLuint program(const char *vs, const char *fs, char *err, size_t err_n)
{
    GLuint v = compile(GL_VERTEX_SHADER, vs, err, err_n);
    GLuint f = v ? compile(GL_FRAGMENT_SHADER, fs, err, err_n) : 0;
    GLuint p;
    GLint ok = 0;
    if (!f) {
        if (v) {
            p_glDeleteShader(v);
        }
        return 0;
    }
    p = p_glCreateProgram();
    p_glAttachShader(p, v);
    p_glAttachShader(p, f);
    p_glLinkProgram(p);
    p_glDeleteShader(v);
    p_glDeleteShader(f);
    p_glGetProgramiv(p, GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[1024];
        p_glGetProgramInfoLog(p, sizeof log, NULL, log);
        sf_set_err(err, err_n, "OpenGL program: %s", log);
        p_glDeleteProgram(p);
        return 0;
    }
    return p;
}

static GLuint make_texture(int w, int h, GLenum internal, GLenum format, GLenum type, const void *data)
{
    GLuint t;
    glGenTextures(1, &t);
    glBindTexture(GL_TEXTURE_2D, t);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexImage2D(GL_TEXTURE_2D, 0, (GLint)internal, w, h, 0, format, type, data);
    return t;
}

static void free_targets(GlScene *s)
{
    if (s->fb) {
        p_glDeleteFramebuffers(1, &s->fb);
    }
    if (s->back_fb) {
        p_glDeleteFramebuffers(1, &s->back_fb);
    }
    if (s->color) {
        glDeleteTextures(1, &s->color);
    }
    if (s->depth) {
        glDeleteTextures(1, &s->depth);
    }
    if (s->back) {
        glDeleteTextures(1, &s->back);
    }
    s->fb = s->back_fb = s->color = s->depth = s->back = 0;
    s->back_valid = 0;
    free(s->readback);
    s->readback = NULL;
}

static int make_targets(GlScene *s, int scale)
{
    int w = SF_SCREEN_W * scale;
    int h = SF_SCREEN_H * scale;
    free_targets(s);
    s->scale = scale;
    s->color = make_texture(w, h, GL_RGBA8, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
    s->depth = make_texture(w, h, GL_DEPTH_COMPONENT32F, GL_DEPTH_COMPONENT, GL_FLOAT, NULL);
    s->back = make_texture(w, h, GL_RGBA8, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
    p_glGenFramebuffers(1, &s->fb);
    p_glBindFramebuffer(GL_FRAMEBUFFER, s->fb);
    p_glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, s->color, 0);
    p_glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_TEXTURE_2D, s->depth, 0);
    if (p_glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
        return -1;
    }
    p_glGenFramebuffers(1, &s->back_fb);
    p_glBindFramebuffer(GL_FRAMEBUFFER, s->back_fb);
    p_glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, s->back, 0);
    if (p_glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
        return -1;
    }
    p_glBindFramebuffer(GL_FRAMEBUFFER, 0);
    return 0;
}

static void scene_destroy(GlScene *s)
{
    int i;
    if (!s) {
        return;
    }
    free_targets(s);
    for (i = 0; i < SF_TEX_MAX; i++) {
        if (s->tex[i]) {
            glDeleteTextures(1, &s->tex[i]);
        }
    }
    if (s->pal) {
        glDeleteTextures(1, &s->pal);
    }
    if (s->dummy) {
        glDeleteTextures(1, &s->dummy);
    }
    if (s->vbuf) {
        p_glDeleteBuffers(1, &s->vbuf);
    }
    if (s->ibuf) {
        p_glDeleteBuffers(1, &s->ibuf);
    }
    if (s->vao) {
        p_glDeleteVertexArrays(1, &s->vao);
    }
    if (s->prog) {
        p_glDeleteProgram(s->prog);
    }
    if (s->quad) {
        p_glDeleteProgram(s->quad);
    }
    free(s);
}

static GlScene *scene_create(char *err, size_t err_n)
{
    GlScene *s = calloc(1, sizeof *s);
    static const uint8_t zero[4] = { 0, 0, 0, 0 };
    if (!s) {
        sf_set_err(err, err_n, "out of memory");
        return NULL;
    }
    s->prog = program(k_scene_vert, k_scene_frag, err, err_n);
    s->quad = s->prog ? program(k_quad_vert, k_quad_frag, err, err_n) : 0;
    if (!s->quad) {
        scene_destroy(s);
        return NULL;
    }
    p_glUseProgram(s->prog);
    p_glUniform1i(p_glGetUniformLocation(s->prog, "tex"), 0);
    p_glUniform1i(p_glGetUniformLocation(s->prog, "pal"), 1);
    s->u_pal = p_glGetUniformLocation(s->prog, "u_pal");
    s->u_ops = p_glGetUniformLocation(s->prog, "u_ops");
    s->u_misc = p_glGetUniformLocation(s->prog, "u_misc");
    s->u_size = p_glGetUniformLocation(s->prog, "u_size");
    s->u_scale = p_glGetUniformLocation(s->prog, "u_scale");
    p_glUseProgram(s->quad);
    p_glUniform1i(p_glGetUniformLocation(s->quad, "screen"), 0);
    s->q_src = p_glGetUniformLocation(s->quad, "src");
    s->q_opaque = p_glGetUniformLocation(s->quad, "opaque");
    s->q_filter = p_glGetUniformLocation(s->quad, "filter_mode");
    p_glGenVertexArrays(1, &s->vao);
    p_glBindVertexArray(s->vao);
    p_glGenBuffers(1, &s->vbuf);
    p_glGenBuffers(1, &s->ibuf);
    p_glBindBuffer(GL_ARRAY_BUFFER, s->vbuf);
    p_glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, s->ibuf);
    p_glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(SfVertex), (void *)offsetof(SfVertex, x));
    /* 0xAARRGGBB, as Vulkan's B8G8R8A8 reads it */
    p_glVertexAttribPointer(1, GL_BGRA, GL_UNSIGNED_BYTE, GL_TRUE, sizeof(SfVertex), (void *)offsetof(SfVertex, color));
    p_glVertexAttribPointer(2, 2, GL_FLOAT, GL_FALSE, sizeof(SfVertex), (void *)offsetof(SfVertex, u));
    p_glVertexAttribPointer(3, 4, GL_FLOAT, GL_FALSE, sizeof(SfVertex), (void *)offsetof(SfVertex, u0));
    p_glEnableVertexAttribArray(0);
    p_glEnableVertexAttribArray(1);
    p_glEnableVertexAttribArray(2);
    p_glEnableVertexAttribArray(3);
    p_glBindVertexArray(0);
    s->pal = make_texture(SF_PAL_ENTRIES, SF_PAL_MAX, GL_RGBA8, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
    s->dummy = make_texture(1, 1, GL_RGBA8, GL_RGBA, GL_UNSIGNED_BYTE, zero);
    return s;
}

/* The textures and palettes that changed, and the frame's quads. */
static void prepare(GlScene *s, const SfDrawList *l)
{
    int h;
    int pal_dirty = !s->pal_ready;
    for (h = 1; h < SF_TEX_MAX; h++) {
        const SfTexture *t = &l->tex[h];
        if (!t->rgba) {
            if (s->tex[h]) {
                glDeleteTextures(1, &s->tex[h]);
                s->tex[h] = 0;
            }
            s->tex_version[h] = 0;
            continue;
        }
        if (s->tex[h] && s->tex_version[h] == t->version && s->tex_w[h] == t->w && s->tex_h[h] == t->h) {
            continue;
        }
        if (s->tex[h] && (s->tex_w[h] != t->w || s->tex_h[h] != t->h)) {
            glDeleteTextures(1, &s->tex[h]);
            s->tex[h] = 0;
        }
        if (!s->tex[h]) {
            s->tex[h] = make_texture(t->w, t->h, GL_RGBA8, GL_RGBA, GL_UNSIGNED_BYTE, t->rgba);
            s->tex_w[h] = t->w;
            s->tex_h[h] = t->h;
        } else {
            glBindTexture(GL_TEXTURE_2D, s->tex[h]);
            glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
            glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, t->w, t->h, GL_RGBA, GL_UNSIGNED_BYTE, t->rgba);
        }
        s->tex_version[h] = t->version;
    }
    for (h = 0; h < SF_PAL_MAX && !pal_dirty; h++) {
        pal_dirty = l->pal[h].version != s->pal_version[h];
    }
    if (pal_dirty) {
        static uint8_t all[SF_PAL_MAX * SF_PAL_ENTRIES * 4];
        for (h = 0; h < SF_PAL_MAX; h++) {
            memcpy(all + (size_t)h * SF_PAL_ENTRIES * 4, l->pal[h].rgba, SF_PAL_ENTRIES * 4);
            s->pal_version[h] = l->pal[h].version;
        }
        glBindTexture(GL_TEXTURE_2D, s->pal);
        glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, SF_PAL_ENTRIES, SF_PAL_MAX, GL_RGBA, GL_UNSIGNED_BYTE, all);
        s->pal_ready = 1;
    }
    p_glBindVertexArray(s->vao);
    p_glBindBuffer(GL_ARRAY_BUFFER, s->vbuf);
    p_glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)((size_t)l->v_n * sizeof(SfVertex)), l->v, GL_STREAM_DRAW);
    if (l->v_n / 4 > s->quads) {
        uint32_t n = l->v_n / 4 + 1024;
        uint32_t *idx = malloc((size_t)n * 6 * sizeof *idx);
        uint32_t q;
        for (q = 0; idx && q < n; q++) {
            idx[q * 6 + 0] = q * 4 + 0;
            idx[q * 6 + 1] = q * 4 + 1;
            idx[q * 6 + 2] = q * 4 + 2;
            idx[q * 6 + 3] = q * 4 + 2;
            idx[q * 6 + 4] = q * 4 + 1;
            idx[q * 6 + 5] = q * 4 + 3;
        }
        if (idx) {
            p_glBufferData(GL_ELEMENT_ARRAY_BUFFER, (GLsizeiptr)((size_t)n * 6 * sizeof *idx), idx, GL_STATIC_DRAW);
            s->quads = n;
            free(idx);
        }
    }
}

static const GLenum k_factor[SF_BF_COUNT] = {
    GL_ZERO,      GL_ONE,           GL_SRC_COLOR,           GL_ONE_MINUS_SRC_COLOR, GL_SRC_ALPHA,
    GL_ONE_MINUS_SRC_ALPHA, GL_DST_ALPHA, GL_ONE_MINUS_DST_ALPHA, GL_DST_COLOR,         GL_ONE_MINUS_DST_COLOR,
};

static const GLenum k_compare[8] = { GL_NEVER,   GL_LESS,     GL_EQUAL,  GL_LEQUAL,
                                     GL_GREATER, GL_NOTEQUAL, GL_GEQUAL, GL_ALWAYS };

static void clear_to(uint32_t c, int depth)
{
    glClearColor((float)((c >> 16) & 0xFF) / 255.f, (float)((c >> 8) & 0xFF) / 255.f, (float)(c & 0xFF) / 255.f,
                 (float)(c >> 24) / 255.f);
    glClearDepth(1.0);
    glDepthMask(GL_TRUE);
    glDisable(GL_SCISSOR_TEST);
    glClear(GL_COLOR_BUFFER_BIT | (depth ? GL_DEPTH_BUFFER_BIT : 0));
}

/* Batches [from, to) onto the screen, which starts as clear (scene.c's
 * record_screen). */
static void draw_screen(GlScene *s, const SfDrawList *l, uint32_t from, uint32_t to, uint32_t clear)
{
    int k = s->scale;
    uint32_t i;
    p_glBindFramebuffer(GL_FRAMEBUFFER, s->fb);
    glViewport(0, 0, SF_SCREEN_W * k, SF_SCREEN_H * k);
    clear_to(clear, 1);
    p_glUseProgram(s->prog);
    p_glBindVertexArray(s->vao);
    p_glUniform1f(s->u_scale, (float)k);
    p_glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, s->pal);
    p_glActiveTexture(GL_TEXTURE0);
    glEnable(GL_SCISSOR_TEST);
    glDisable(GL_CULL_FACE);
    for (i = from; i < to; i++) {
        const SfBatch *b = &l->b[i];
        const SfShadeState *st = &b->shade;
        const SfBlendKey *bk = &b->blend;
        int x0 = b->clip[0] < 0 ? 0 : b->clip[0];
        int y0 = b->clip[1] < 0 ? 0 : b->clip[1];
        int x1 = b->clip[2] > SF_SCREEN_W ? SF_SCREEN_W : b->clip[2];
        int y1 = b->clip[3] > SF_SCREEN_H ? SF_SCREEN_H : b->clip[3];
        GLenum op;
        if (x1 <= x0 || y1 <= y0 || b->quads == 0) {
            continue;
        }
        glScissor(x0 * k, y0 * k, (x1 - x0) * k, (y1 - y0) * k);
        if (bk->blend) {
            op = bk->op == SF_BOP_SUB ? GL_FUNC_SUBTRACT : bk->op == SF_BOP_REV_SUB ? GL_FUNC_REVERSE_SUBTRACT
                                                                                   : GL_FUNC_ADD;
            glEnable(GL_BLEND);
            glBlendFunc(k_factor[bk->src % SF_BF_COUNT], k_factor[bk->dst % SF_BF_COUNT]);
            p_glBlendEquation(op);
        } else {
            glDisable(GL_BLEND);
        }
        if (bk->zfunc != SF_CMP_ALWAYS || bk->zwrite) {
            glEnable(GL_DEPTH_TEST);
            glDepthFunc(k_compare[bk->zfunc & 7]);
        } else {
            glDisable(GL_DEPTH_TEST);
        }
        glDepthMask(bk->zwrite ? GL_TRUE : GL_FALSE);
        glBindTexture(GL_TEXTURE_2D, st->tex && s->tex[st->tex] ? s->tex[st->tex] : s->dummy);
        p_glUniform1ui(s->u_pal, st->pal);
        p_glUniform1ui(s->u_ops, st->color_op | (uint32_t)st->alpha_op << 8 | (uint32_t)st->address_u << 16 |
                                     (uint32_t)st->address_v << 24);
        p_glUniform1ui(s->u_misc, (st->point ? 1u : 0u) | (st->tex && l->tex[st->tex].indexed ? 2u : 0u) |
                                      (st->texel_uv ? 4u : 0u) | (st->tex && l->tex[st->tex].rgba ? 8u : 0u) |
                                      (st->alpha_test ? 16u : 0u) | (uint32_t)(st->alpha_func & 7) << 5 |
                                      (uint32_t)st->alpha_ref << 8);
        p_glUniform2f(s->u_size, st->tex ? (float)l->tex[st->tex].w : 1.f, st->tex ? (float)l->tex[st->tex].h : 1.f);
        glDrawElements(GL_TRIANGLES, (GLsizei)(b->quads * 6), GL_UNSIGNED_INT,
                       (void *)((size_t)(b->first / 4) * 6 * sizeof(uint32_t)));
    }
    glDisable(GL_SCISSOR_TEST);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_BLEND);
    glDepthMask(GL_TRUE);
}

/* The screen's rect onto the output's 640x448 game area at the display
 * offset, 16 lines down (scene.c's record_copy). */
static void copy_screen(GlScene *s, int clear_out, uint32_t clear, int blend, const int rect[4], const int at[2])
{
    int k = s->scale;
    p_glBindFramebuffer(GL_FRAMEBUFFER, s->back_fb);
    glViewport(0, 0, SF_SCREEN_W * k, SF_SCREEN_H * k);
    if (clear_out) {
        clear_to(clear, 0);
    }
    glViewport(at[0] * k, (at[1] + 16) * k, 640 * k, 448 * k);
    p_glUseProgram(s->quad);
    p_glBindVertexArray(s->vao);
    p_glUniform4f(s->q_src, (float)rect[0] / SF_SCREEN_W, (float)rect[1] / SF_SCREEN_H, (float)rect[2] / SF_SCREEN_W,
                  (float)rect[3] / SF_SCREEN_H);
    p_glUniform1i(s->q_opaque, 0);
    p_glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, s->color);
    if (blend) {
        glEnable(GL_BLEND);
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
        p_glBlendEquation(GL_FUNC_ADD);
    } else {
        glDisable(GL_BLEND);
    }
    glDrawArrays(GL_TRIANGLES, 0, 3);
    glDisable(GL_BLEND);
}

/* The frame, as scene.c's sf_scene_record. */
static void draw_frame(GlScene *s, const SfDrawList *l)
{
    if (l->cp3) {
        uint32_t at = l->cp3_at < l->b_n ? l->cp3_at : l->b_n;
        draw_screen(s, l, 0, at, l->clear);
        copy_screen(s, 1, l->clear, 0, l->cp3_rect, l->show_at);
        draw_screen(s, l, at, l->b_n, 0);
        copy_screen(s, 0, 0, 1, l->show, l->show_at);
    } else {
        draw_screen(s, l, 0, l->b_n, l->clear);
        copy_screen(s, !s->back_valid, 0xFF000000, 1, l->show, l->show_at);
    }
    s->back_valid = 1;
}

/* The output into the window at the asked shape, turned over: GL's rows
 * count from the bottom. */
static void show(GlScene *s, int w, int h, float aspect, int filter)
{
    float fw = (float)w;
    float fh = (float)h;
    p_glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glViewport(0, 0, w, h);
    clear_to(0xFF000000, 0);
    if (aspect > 0.f) {
        fh = fw / aspect;
        if (fh > (float)h) {
            fh = (float)h;
            fw = fh * aspect;
        }
    }
    glViewport((int)(((float)w - fw) / 2.f), (int)(((float)h - fh) / 2.f), (int)fw, (int)fh);
    p_glUseProgram(s->quad);
    p_glBindVertexArray(s->vao);
    /* the game area, 448 lines from line 16, as scene.c shows it */
    p_glUniform4f(s->q_src, 0.f, 464.f / SF_SCREEN_H, 1.f, 16.f / SF_SCREEN_H);
    p_glUniform1i(s->q_opaque, 1);
    p_glUniform1f(s->q_filter, (float)(filter < 0 ? 0 : (filter > 3 ? 3 : filter)));
    p_glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, s->back);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, filter == 0 ? GL_NEAREST : GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, filter == 0 ? GL_NEAREST : GL_LINEAR);
    glDrawArrays(GL_TRIANGLES, 0, 3);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
}

/* The scale the screen renders at: SF_SCALE, or the largest whole multiple
 * of 640x480 the window holds, up to 8 (vulkan.c's render_scale). */
static int render_scale(int w, int h)
{
    const char *env = getenv("SF_SCALE");
    int k;
    if (env && atoi(env) > 0) {
        k = atoi(env);
    } else {
        int kw = w / 640;
        int kh = h / 480;
        k = kw < kh ? kw : kh;
    }
    if (k < 1) {
        k = 1;
    }
    return k > 8 ? 8 : k;
}

int sf_gl_present_list(SfRenderer *r, const SfDrawList *list, char *err, size_t err_n)
{
    GlScene *s;
    int w = 0;
    int h = 0;
    int k;
    SDL_GL_MakeCurrent(r->window, r->gl);
    if (!r->glscene) {
        if (load_gl() != 0) {
            sf_set_err(err, err_n, "OpenGL 3.3 is missing calls the game's screen needs");
            return -1;
        }
        r->glscene = scene_create(err, err_n);
        if (!r->glscene) {
            return -1;
        }
    }
    s = r->glscene;
    SDL_GetWindowSizeInPixels(r->window, &w, &h);
    k = render_scale(w, h);
    if (k != s->scale) {
        fprintf(stderr, "video: window %dx%d, the game drawn at %dx%d (OpenGL)\n", w, h, 640 * k, 480 * k);
        if (make_targets(s, k) != 0) {
            sf_set_err(err, err_n, "OpenGL screen at %dx failed", k);
            return -1;
        }
    }
    prepare(s, list);
    draw_frame(s, list);
    if (r->shot_pending == 1) {
        /* the output, as sf_render_read_screen gives it: top row first */
        size_t n = (size_t)SF_SCREEN_W * SF_SCREEN_H * 4 * (size_t)(s->scale * s->scale);
        free(s->readback);
        s->readback = malloc(n);
        if (s->readback) {
            p_glBindFramebuffer(GL_FRAMEBUFFER, s->back_fb);
            glPixelStorei(GL_PACK_ALIGNMENT, 1);
            glReadPixels(0, 0, SF_SCREEN_W * s->scale, SF_SCREEN_H * s->scale, GL_RGBA, GL_UNSIGNED_BYTE, s->readback);
        }
        r->shot_pending = 2;
    }
    if (w > 0 && h > 0) {
        show(s, w, h, r->aspect, r->filter);
        SDL_GL_SwapWindow(r->window);
    }
    return 0;
}

int sf_gl_read_screen(SfRenderer *r, const uint8_t **rgba, int *w, int *h)
{
    GlScene *s = r->glscene;
    if (!s) {
        return -1;
    }
    if (r->shot_pending != 2) {
        r->shot_pending = 1;
        return 1;
    }
    r->shot_pending = 0;
    if (!s->readback) {
        return -1;
    }
    *rgba = s->readback;
    *w = SF_SCREEN_W * s->scale;
    *h = SF_SCREEN_H * s->scale;
    return 0;
}

void sf_gl_scene_destroy(SfRenderer *r)
{
    if (r && r->glscene) {
        SDL_GL_MakeCurrent(r->window, r->gl);
        scene_destroy(r->glscene);
        r->glscene = NULL;
    }
}
