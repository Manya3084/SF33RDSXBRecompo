#include "render/nv2a.h"

#include <SDL3/SDL.h>

#include <errno.h>
#include <spawn.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

extern char **environ;

static int g_fail;

static void expect(int cond, const char *msg)
{
    if (!cond) {
        fprintf(stderr, "FAIL %s\n", msg);
        g_fail++;
    } else {
        printf("ok %s\n", msg);
    }
}

static int run_cmd(char *const argv[])
{
    pid_t pid;
    int status = 0;
    if (posix_spawnp(&pid, argv[0], NULL, NULL, argv, environ) != 0) {
        fprintf(stderr, "spawn %s: %s\n", argv[0], strerror(errno));
        return -1;
    }
    for (;;) {
        pid_t w = waitpid(pid, &status, 0);
        if (w < 0 && errno == EINTR) {
            continue;
        }
        if (w < 0) {
            return -1;
        }
        break;
    }
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        return -1;
    }
    return 0;
}

static void *read_all(const char *path, size_t *size)
{
    FILE *fp = fopen(path, "rb");
    void *buf;
    long len;
    if (!fp) {
        return NULL;
    }
    if (fseek(fp, 0, SEEK_END) != 0) {
        fclose(fp);
        return NULL;
    }
    len = ftell(fp);
    if (len < 0 || fseek(fp, 0, SEEK_SET) != 0) {
        fclose(fp);
        return NULL;
    }
    buf = malloc((size_t)len);
    if (!buf || fread(buf, 1, (size_t)len, fp) != (size_t)len) {
        free(buf);
        fclose(fp);
        return NULL;
    }
    fclose(fp);
    *size = (size_t)len;
    return buf;
}

static int vk_device(VkInstance *instance, VkDevice *device)
{
    VkApplicationInfo app;
    VkInstanceCreateInfo ici;
    VkPhysicalDevice phys[8];
    uint32_t count = 8;
    uint32_t i;
    memset(&app, 0, sizeof app);
    app.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    app.apiVersion = VK_API_VERSION_1_0;
    memset(&ici, 0, sizeof ici);
    ici.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    ici.pApplicationInfo = &app;
    *instance = VK_NULL_HANDLE;
    *device = VK_NULL_HANDLE;
    if (vkCreateInstance(&ici, NULL, instance) != VK_SUCCESS) {
        return -1;
    }
    if (vkEnumeratePhysicalDevices(*instance, &count, NULL) != VK_SUCCESS || count == 0) {
        return -1;
    }
    if (count > 8) {
        count = 8;
    }
    if (vkEnumeratePhysicalDevices(*instance, &count, phys) != VK_SUCCESS) {
        return -1;
    }
    for (i = 0; i < count; i++) {
        VkQueueFamilyProperties fam[8];
        uint32_t fam_n = 8;
        uint32_t q;
        vkGetPhysicalDeviceQueueFamilyProperties(phys[i], &fam_n, NULL);
        if (fam_n > 8) {
            fam_n = 8;
        }
        vkGetPhysicalDeviceQueueFamilyProperties(phys[i], &fam_n, fam);
        for (q = 0; q < fam_n; q++) {
            float priority = 1.0f;
            VkDeviceQueueCreateInfo qi;
            VkDeviceCreateInfo di;
            if ((fam[q].queueFlags & VK_QUEUE_GRAPHICS_BIT) == 0) {
                continue;
            }
            memset(&qi, 0, sizeof qi);
            qi.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
            qi.queueFamilyIndex = q;
            qi.queueCount = 1;
            qi.pQueuePriorities = &priority;
            memset(&di, 0, sizeof di);
            di.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
            di.queueCreateInfoCount = 1;
            di.pQueueCreateInfos = &qi;
            if (vkCreateDevice(phys[i], &di, NULL, device) == VK_SUCCESS) {
                return 0;
            }
        }
    }
    return -1;
}

static int gl_link(GLuint vert, GLuint frag)
{
    GLuint prog = glCreateProgram();
    GLint status = 0;
    glAttachShader(prog, vert);
    glAttachShader(prog, frag);
    glLinkProgram(prog);
    glGetProgramiv(prog, GL_LINK_STATUS, &status);
    if (!status) {
        char log[2048];
        GLsizei len = 0;
        glGetProgramInfoLog(prog, (GLsizei)sizeof log, &len, log);
        fprintf(stderr, "link: %s\n", log);
    }
    glDeleteProgram(prog);
    return status ? 0 : -1;
}

int main(void)
{
    char dir[] = "/tmp/sf33rd-shader-XXXXXX";
    char xvu[256];
    char xpu[256];
    char vert_path[256];
    char frag_path[256];
    uint8_t nop[20];
    uint8_t zero[244];
    void *vert_bytes = NULL;
    void *frag_bytes = NULL;
    size_t vert_size = 0;
    size_t frag_size = 0;
    SDL_Window *window = NULL;
    SDL_GLContext gl = NULL;
    VkInstance instance = VK_NULL_HANDLE;
    VkDevice device = VK_NULL_HANDLE;
    VkShaderModule vert_mod = VK_NULL_HANDLE;
    VkShaderModule frag_mod = VK_NULL_HANDLE;
    GLuint vert = 0;
    GLuint frag = 0;
    char err[512];
    char *mk;
    int i;
    const char *envs[] = {"vulkan1.0", "opengl4.5"};

    memset(nop, 0, sizeof nop);
    nop[0] = 0x78;
    nop[1] = 0x20;
    nop[2] = 1;
    nop[16] = 1;
    memset(zero, 0, sizeof zero);
    memcpy(zero, "PSB0", 4);

    mk = mkdtemp(dir);
    expect(mk != NULL, "temp dir");
    if (!mk) {
        return 1;
    }
    snprintf(xvu, sizeof xvu, "%s/nop.xvu", dir);
    snprintf(xpu, sizeof xpu, "%s/zero.xpu", dir);
    snprintf(vert_path, sizeof vert_path, "%s/nop.spv", dir);
    snprintf(frag_path, sizeof frag_path, "%s/zero.spv", dir);
    {
        FILE *fp = fopen(xvu, "wb");
        expect(fp && fwrite(nop, 1, sizeof nop, fp) == sizeof nop, "write nop.xvu");
        if (fp) {
            fclose(fp);
        }
        fp = fopen(xpu, "wb");
        expect(fp && fwrite(zero, 1, sizeof zero, fp) == sizeof zero, "write zero.xpu");
        if (fp) {
            fclose(fp);
        }
    }
    {
        char *argv[] = {"python3", SF_SHADER_TOOL, xvu, "-o", vert_path, NULL};
        expect(run_cmd(argv) == 0, "convert vertex program");
    }
    {
        char *argv[] = {"python3", SF_SHADER_TOOL, xpu, "-o", frag_path, NULL};
        expect(run_cmd(argv) == 0, "convert combiner");
    }
    for (i = 0; i < 2; i++) {
        char *vargv[] = {"spirv-val", "--target-env", (char *)envs[i], vert_path, NULL};
        char *fargv[] = {"spirv-val", "--target-env", (char *)envs[i], frag_path, NULL};
        expect(run_cmd(vargv) == 0, envs[i]);
        expect(run_cmd(fargv) == 0, envs[i]);
    }
    vert_bytes = read_all(vert_path, &vert_size);
    frag_bytes = read_all(frag_path, &frag_size);
    expect(vert_bytes && frag_bytes, "read SPIR-V");

    expect(SDL_Init(SDL_INIT_VIDEO), "SDL video");
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 4);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 6);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_COMPATIBILITY);
    window = SDL_CreateWindow("sf33rd shader", 64, 64, SDL_WINDOW_OPENGL | SDL_WINDOW_HIDDEN);
    expect(window != NULL, "OpenGL window");
    if (window) {
        gl = SDL_GL_CreateContext(window);
        expect(gl != NULL, "OpenGL 4.6 context");
    }
    if (gl && vert_bytes && frag_bytes) {
        expect(sf_gl_shader_spirv(GL_VERTEX_SHADER, vert_bytes, vert_size, &vert, err, sizeof err) == 0, "OpenGL vertex SPIR-V");
        if (!vert) {
            fprintf(stderr, "%s\n", err);
        }
        expect(sf_gl_shader_spirv(GL_FRAGMENT_SHADER, frag_bytes, frag_size, &frag, err, sizeof err) == 0, "OpenGL fragment SPIR-V");
        if (!frag) {
            fprintf(stderr, "%s\n", err);
        }
        if (vert && frag) {
            expect(gl_link(vert, frag) == 0, "OpenGL links both stages");
        }
    }
    expect(vk_device(&instance, &device) == 0, "Vulkan device");
    if (device && vert_bytes && frag_bytes) {
        expect(sf_vk_shader_spirv(device, vert_bytes, vert_size, &vert_mod, err, sizeof err) == 0, "Vulkan vertex SPIR-V");
        expect(sf_vk_shader_spirv(device, frag_bytes, frag_size, &frag_mod, err, sizeof err) == 0, "Vulkan fragment SPIR-V");
    }

    if (vert) {
        glDeleteShader(vert);
    }
    if (frag) {
        glDeleteShader(frag);
    }
    if (gl) {
        SDL_GL_DestroyContext(gl);
    }
    if (window) {
        SDL_DestroyWindow(window);
    }
    SDL_Quit();
    if (device) {
        if (vert_mod) {
            vkDestroyShaderModule(device, vert_mod, NULL);
        }
        if (frag_mod) {
            vkDestroyShaderModule(device, frag_mod, NULL);
        }
        vkDestroyDevice(device, NULL);
    }
    if (instance) {
        vkDestroyInstance(instance, NULL);
    }
    free(vert_bytes);
    free(frag_bytes);
    unlink(xvu);
    unlink(xpu);
    unlink(vert_path);
    unlink(frag_path);
    rmdir(dir);
    return g_fail ? 1 : 0;
}
