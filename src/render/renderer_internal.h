#ifndef SF_RENDERER_INTERNAL_H
#define SF_RENDERER_INTERNAL_H

#include "render/renderer.h"

#include <GL/gl.h>
#include <vulkan/vulkan.h>

typedef struct SfScene SfScene;

struct SfRenderer {
    SfRendererKind kind;
    SDL_Window *window;
    float aspect; /* the game screen's shape in the window; 0 fills it */
    int sized_w, sized_h; /* the window's pixel size the swapchain was last made for */
    VkInstance instance;
    VkPhysicalDevice physical;
    VkDevice device;
    VkQueue queue;
    uint32_t family;
    VkSurfaceKHR surface;
    VkSwapchainKHR swapchain;
    VkFormat format;
    VkExtent2D extent;
    VkRenderPass pass;
    VkPipelineLayout layout;
    VkPipeline pipeline;
    VkCommandPool pool;
    VkCommandBuffer cmd;
    VkFence fence;
    VkSemaphore image_ok;
    VkSemaphore render_ok;
    VkImage *images;
    VkImageView *views;
    VkFramebuffer *frames;
    uint32_t image_count;
    VkDescriptorSetLayout set_layout;
    VkDescriptorPool desc_pool;
    VkDescriptorSet set;
    VkSampler sampler;
    VkImage tex;
    VkDeviceMemory tex_mem;
    VkImageView tex_view;
    VkBuffer staging;
    VkDeviceMemory staging_mem;
    VkDeviceSize staging_cap;
    int tex_w;
    int tex_h;
    SfScene *scene;
    int shot_pending;
    SDL_GLContext gl;
    GLuint gl_tex;
    void *glscene; /* the game's screen on OpenGL (glscene.c) */
};

int sf_vk_create(SfRenderer **out, SDL_Window *window, char *err, size_t err_n);
void sf_vk_destroy(SfRenderer *renderer);
int sf_vk_present(SfRenderer *renderer, const SfCanvas *canvas, char *err, size_t err_n);
int sf_vk_present_list(SfRenderer *renderer, const SfDrawList *list, char *err, size_t err_n);
int sf_vk_read_screen(SfRenderer *renderer, const uint8_t **rgba, int *w, int *h);
int sf_vk_create_headless(SfRenderer **out, char *err, size_t err_n);

int sf_gl_create(SfRenderer **out, SDL_Window *window, char *err, size_t err_n);
void sf_gl_destroy(SfRenderer *renderer);
int sf_gl_present(SfRenderer *renderer, const SfCanvas *canvas, char *err, size_t err_n);
int sf_gl_present_list(SfRenderer *renderer, const SfDrawList *list, char *err, size_t err_n);
int sf_gl_read_screen(SfRenderer *renderer, const uint8_t **rgba, int *w, int *h);
void sf_gl_scene_destroy(SfRenderer *renderer);

#endif
