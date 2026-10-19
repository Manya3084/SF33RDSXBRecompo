#ifndef SF_RENDER_SCENE_H
#define SF_RENDER_SCENE_H

/* The game's screen on Vulkan: draws an SfDrawList into a 640x480 colour
 * and depth target, then shows the game's part of it in the window. It
 * runs on any device that enables sf_scene_features, the window's or a
 * headless one (the tests). */

#include "render/draw.h"

#include <stddef.h>
#include <vulkan/vulkan.h>

typedef struct SfScene SfScene;

/* The device features the scene needs. */
void sf_scene_features(VkPhysicalDeviceFeatures *features);
int sf_scene_supported(VkPhysicalDevice physical);

/* show_pass is the window's render pass, or VK_NULL_HANDLE headless. */
int sf_scene_create(SfScene **out, VkPhysicalDevice physical, VkDevice device, VkRenderPass show_pass, char *err,
                    size_t err_n);
void sf_scene_destroy(SfScene *s);

/* Host side of a frame, once the previous frame's commands have finished:
 * textures and palettes the game changed, the vertices, the descriptors. */
int sf_scene_prepare(SfScene *s, const SfDrawList *list, char *err, size_t err_n);

/* Outside a render pass: the uploads and the screen. */
void sf_scene_record(SfScene *s, VkCommandBuffer cmd, const SfDrawList *list);

/* Inside the window's render pass: the screen's game area, kept 4:3. */
void sf_scene_record_show(SfScene *s, VkCommandBuffer cmd, VkExtent2D extent, const SfDrawList *list);

/* The screen back to the host (the tests): record the copy, then read it
 * once the commands have finished. rgba is 640x480x4. */
void sf_scene_record_readback(SfScene *s, VkCommandBuffer cmd);
void sf_scene_read(SfScene *s, uint8_t *rgba);

#endif
