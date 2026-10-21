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

/* The screen and the output render at 640x480 times scale (1 and up).
 * Changing it waits for the device and remakes both. */
int sf_scene_scale(const SfScene *s);
int sf_scene_set_scale(SfScene *s, int scale, char *err, size_t err_n);

/* Outside a render pass: the uploads, the screen, and the output it is
 * copied onto. */
void sf_scene_record(SfScene *s, VkCommandBuffer cmd, const SfDrawList *list);

/* Inside the window's render pass: the output, kept 4:3. */
/* aspect: the shown screen's width over height, centred; 0 fills. */
/* filter: 0 each texel whole, 1 filtered as it scales, 2 CRT CURVED,
 * 3 CRT FLAT. */
void sf_scene_record_show(SfScene *s, VkCommandBuffer cmd, VkExtent2D extent, float aspect, const SfDrawList *list,
                          int filter);

/* The screen (output 0) or the output (1) back to the host: record the
 * copy, then read it once the commands have finished. The pixels are RGBA
 * at the scale, and stay valid until the next readback. */
void sf_scene_record_readback(SfScene *s, VkCommandBuffer cmd, int output);
const uint8_t *sf_scene_read(SfScene *s, int *w, int *h);

/* Debugging: the next record reads the screen before the PS2-sized
 * picture is copied off it (read with sf_scene_read). */
void sf_scene_read_layer1(SfScene *s);

#endif
