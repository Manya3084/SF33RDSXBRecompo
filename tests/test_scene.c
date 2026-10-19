/* The game's graphics calls through the Vulkan scene, on a headless device
 * (lavapipe under ctest), read back and checked pixel by pixel. */

#include "port/gfx.h"
#include "render/scene.h"

#include "types.h"
#include "structs.h"
#include "sf33rd/AcrSDK/common/plcommon.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

s32 flCreateTextureHandle(void *bits, u32 attribute);
s32 flCreatePaletteHandle(void *bits, s32 flag);
s32 flLockPalette(void *rect, u32 h, plContext *out, s32 flag);
s32 flLockTexture(s16 *r, u32 h, plContext *out, s32 mode);
s32 flUnlockTexture(u32 h);
s32 flSetRenderState(s32 func, u32 value);
s32 ps2SeqsRenderQuadInit_A(void);
s32 ps2SeqsRenderQuadInit_B(void);
void ps2SeqsRenderQuadEnd(void);
void ps2SeqsRenderQuad_A(Sprite *spr, u32 col);
void ps2SeqsRenderQuad_B(Quad *q, u32 col);

static VkInstance s_instance;
static VkPhysicalDevice s_physical;
static VkDevice s_device;
static VkQueue s_queue;
static VkCommandPool s_pool;
static VkCommandBuffer s_cmd;
static VkFence s_fence;
static const uint8_t *s_screen;
static int s_fail;

static int open_device(void)
{
    VkApplicationInfo app;
    VkInstanceCreateInfo ii;
    uint32_t n = 0;
    VkPhysicalDevice devs[8];
    uint32_t i;
    float prio = 1.f;
    VkDeviceQueueCreateInfo qi;
    VkDeviceCreateInfo di;
    VkPhysicalDeviceFeatures features;
    VkCommandPoolCreateInfo pi;
    VkCommandBufferAllocateInfo ci;
    VkFenceCreateInfo fi;
    memset(&app, 0, sizeof app);
    app.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    app.apiVersion = VK_API_VERSION_1_0;
    memset(&ii, 0, sizeof ii);
    ii.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    ii.pApplicationInfo = &app;
    if (vkCreateInstance(&ii, NULL, &s_instance) != VK_SUCCESS) {
        return -1;
    }
    vkEnumeratePhysicalDevices(s_instance, &n, NULL);
    if (n > 8) {
        n = 8;
    }
    vkEnumeratePhysicalDevices(s_instance, &n, devs);
    for (i = 0; i < n; i++) {
        if (sf_scene_supported(devs[i])) {
            s_physical = devs[i];
            break;
        }
    }
    if (!s_physical) {
        return -1;
    }
    memset(&qi, 0, sizeof qi);
    qi.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    qi.queueFamilyIndex = 0;
    qi.queueCount = 1;
    qi.pQueuePriorities = &prio;
    memset(&features, 0, sizeof features);
    sf_scene_features(&features);
    memset(&di, 0, sizeof di);
    di.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    di.queueCreateInfoCount = 1;
    di.pQueueCreateInfos = &qi;
    di.pEnabledFeatures = &features;
    if (vkCreateDevice(s_physical, &di, NULL, &s_device) != VK_SUCCESS) {
        return -1;
    }
    vkGetDeviceQueue(s_device, 0, 0, &s_queue);
    memset(&pi, 0, sizeof pi);
    pi.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    pi.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    vkCreateCommandPool(s_device, &pi, NULL, &s_pool);
    memset(&ci, 0, sizeof ci);
    ci.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    ci.commandPool = s_pool;
    ci.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    ci.commandBufferCount = 1;
    vkAllocateCommandBuffers(s_device, &ci, &s_cmd);
    memset(&fi, 0, sizeof fi);
    fi.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    vkCreateFence(s_device, &fi, NULL, &s_fence);
    return 0;
}

static int draw(SfScene *scene)
{
    char err[256];
    VkCommandBufferBeginInfo bi;
    VkSubmitInfo si;
    SfDrawList *l = sf_gfx_list();
    sf_gfx_frame_ready();
    if (sf_scene_prepare(scene, l, err, sizeof err) != 0) {
        fprintf(stderr, "prepare: %s\n", err);
        return -1;
    }
    vkResetCommandBuffer(s_cmd, 0);
    memset(&bi, 0, sizeof bi);
    bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    vkBeginCommandBuffer(s_cmd, &bi);
    sf_scene_record(scene, s_cmd, l);
    sf_scene_record_readback(scene, s_cmd, 0);
    vkEndCommandBuffer(s_cmd);
    memset(&si, 0, sizeof si);
    si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    si.commandBufferCount = 1;
    si.pCommandBuffers = &s_cmd;
    vkResetFences(s_device, 1, &s_fence);
    vkQueueSubmit(s_queue, 1, &si, s_fence);
    vkWaitForFences(s_device, 1, &s_fence, VK_TRUE, UINT64_MAX);
    {
        int w;
        int h;
        s_screen = sf_scene_read(scene, &w, &h);
    }
    sf_gfx_frame_done();
    return 0;
}

static void expect(const char *what, int x, int y, int r, int g, int b, int tol)
{
    const uint8_t *p = &s_screen[((size_t)y * SF_SCREEN_W + x) * 4];
    if (abs(p[0] - r) > tol || abs(p[1] - g) > tol || abs(p[2] - b) > tol) {
        fprintf(stderr, "%s: pixel (%d, %d) is (%d, %d, %d), expected (%d, %d, %d)\n", what, x, y, p[0], p[1], p[2], r,
                g, b);
        s_fail = 1;
    }
}

static void sprite(Sprite *s, float x0, float y0, float x1, float y1)
{
    memset(s, 0, sizeof *s);
    s->v[0].x = x0;
    s->v[0].y = y0;
    s->v[1].x = x1;
    s->v[1].y = y0;
    s->v[2].x = x0;
    s->v[2].y = y1;
    s->v[3].x = x1;
    s->v[3].y = y1;
    s->t[1].s = 1.f;
    s->t[2].t = 1.f;
    s->t[3].s = 1.f;
    s->t[3].t = 1.f;
}

int main(void)
{
    char err[256];
    SfScene *scene = NULL;
    plContext img;
    plContext lock;
    u8 index[4 * 4];
    u32 colors[16];
    s32 tex;
    s32 pal;
    Sprite spr;
    Quad q;
    int i;

    if (open_device() != 0) {
        fprintf(stderr, "no Vulkan device for the scene; skipped\n");
        return 77;
    }
    if (sf_scene_create(&scene, s_physical, s_device, VK_NULL_HANDLE, err, sizeof err) != 0) {
        fprintf(stderr, "scene: %s\n", err);
        return 1;
    }
    sf_gfx_init();

    /* an empty frame is the start-up clear colour, 0x80808080 */
    draw(scene);
    expect("clear", 0, 0, 0x80, 0x80, 0x80, 0);

    /* a 4x4 8-bit indexed texture through a 16-colour palette */
    for (i = 0; i < 16; i++) {
        index[i] = (u8)i;
        colors[i] = 0xFF000000u | (u32)(i * 16) << 16 | (u32)(255 - i * 16) << 8 | 0x40;
    }
    memset(&img, 0, sizeof img);
    img.desc = 0x44; /* indexed, 8-bit indices */
    img.width = 4;
    img.height = 4;
    img.pitch = 4;
    img.bitdepth = 1;
    img.ptr = index;
    tex = flCreateTextureHandle(&img, 0);
    memset(&img, 0, sizeof img);
    img.width = 16;
    img.height = 1;
    img.pitch = 64;
    img.bitdepth = 4;
    img.pixelformat.rl = 8;
    img.pixelformat.rs = 16;
    img.pixelformat.rm = 0xFF;
    img.pixelformat.gl = 8;
    img.pixelformat.gs = 8;
    img.pixelformat.gm = 0xFF;
    img.pixelformat.bl = 8;
    img.pixelformat.bm = 0xFF;
    img.pixelformat.al = 8;
    img.pixelformat.as = 24;
    img.pixelformat.am = 0xFF;
    img.ptr = colors;
    pal = flCreatePaletteHandle(&img, 0);
    if (tex <= 0 || pal <= 0) {
        fprintf(stderr, "handles: texture %d, palette %d\n", tex, pal);
        return 1;
    }

    /* point filtering is the start-up state (blend word bit 0x10000) */
    flSetRenderState(4, (u32)tex | (u32)pal << 16);
    ps2SeqsRenderQuadInit_A();
    sprite(&spr, 10.f, 20.f, 14.f, 24.f);
    ps2SeqsRenderQuad_A(&spr, 0xFFFFFFFF);
    ps2SeqsRenderQuadEnd();
    draw(scene);
    for (i = 0; i < 16; i++) {
        expect("palette sprite", 10 + i % 4, 20 + i / 4, i * 16, 255 - i * 16, 0x40, 0);
    }
    expect("outside the sprite", 14, 20, 0x80, 0x80, 0x80, 0);

    /* the vertex colour modulates the texel */
    flSetRenderState(4, (u32)tex | (u32)pal << 16);
    ps2SeqsRenderQuadInit_A();
    sprite(&spr, 10.f, 20.f, 14.f, 24.f);
    ps2SeqsRenderQuad_A(&spr, 0xFF808080);
    ps2SeqsRenderQuadEnd();
    draw(scene);
    expect("modulated", 13, 23, 0x78, 0x08, 0x20, 1);

    /* a palette written through flLockPalette shows the next frame */
    flLockPalette(NULL, (u32)pal, &lock, 0);
    ((u32 *)lock.ptr)[0] = 0xFF0000FF;
    flSetRenderState(4, (u32)tex | (u32)pal << 16);
    ps2SeqsRenderQuadInit_A();
    sprite(&spr, 10.f, 20.f, 14.f, 24.f);
    ps2SeqsRenderQuad_A(&spr, 0xFFFFFFFF);
    ps2SeqsRenderQuadEnd();
    draw(scene);
    expect("locked palette", 10, 20, 0, 0, 0xFF, 0);

    /* an untextured quad, half alpha, over the clear colour */
    ps2SeqsRenderQuadInit_B();
    memset(&q, 0, sizeof q);
    q.v[0].x = 100.f;
    q.v[0].y = 100.f;
    q.v[1].x = 110.f;
    q.v[1].y = 100.f;
    q.v[2].x = 100.f;
    q.v[2].y = 110.f;
    q.v[3].x = 110.f;
    q.v[3].y = 110.f;
    ps2SeqsRenderQuad_B(&q, 0x80FF0000);
    ps2SeqsRenderQuadEnd();
    draw(scene);
    expect("blended quad", 105, 105, 0xBF, 0x40, 0x40, 2);
    expect("blended quad edge", 110, 105, 0x80, 0x80, 0x80, 0);

    /* a texture written through a lock in place */
    flLockTexture(NULL, (u32)tex, &lock, 1);
    ((u8 *)lock.ptr)[0] = 15;
    flUnlockTexture((u32)tex);
    flSetRenderState(4, (u32)tex | (u32)pal << 16);
    ps2SeqsRenderQuadInit_A();
    sprite(&spr, 10.f, 20.f, 14.f, 24.f);
    ps2SeqsRenderQuad_A(&spr, 0xFFFFFFFF);
    ps2SeqsRenderQuadEnd();
    draw(scene);
    expect("locked texture", 10, 20, 15 * 16, 255 - 15 * 16, 0x40, 0);

    sf_scene_destroy(scene);
    vkDestroyFence(s_device, s_fence, NULL);
    vkDestroyCommandPool(s_device, s_pool, NULL);
    vkDestroyDevice(s_device, NULL);
    vkDestroyInstance(s_instance, NULL);
    if (!s_fail) {
        printf("scene: ok\n");
    }
    return s_fail;
}
