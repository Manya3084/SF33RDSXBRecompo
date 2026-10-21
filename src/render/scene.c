#include "render/scene.h"

#include "common.h"
#include "render/copy_frag_spv.h"
#include "render/scene_frag_spv.h"
#include "render/scene_vert_spv.h"
#include "render/show_frag_spv.h"
#include "render/show_vert_spv.h"

#include <stdlib.h>
#include <string.h>

#define SF_PIPE_MAX 64

typedef struct SceneImage {
    VkImage image;
    VkDeviceMemory mem;
    VkImageView view;
    int w;
    int h;
    int ready; /* in SHADER_READ_ONLY_OPTIMAL */
} SceneImage;

typedef struct Copy {
    int target; /* texture handle, or -1 for the palettes */
    VkDeviceSize offset;
    uint32_t y;
    uint32_t w;
    uint32_t h;
} Copy;

typedef struct ScenePipe {
    SfBlendKey key;
    VkPipeline pipe;
} ScenePipe;

typedef struct Push {
    uint32_t tex;
    uint32_t pal;
    uint32_t ops;
    uint32_t misc;
    float size[2];
    float scale;
    float pad;
} Push;

struct SfScene {
    VkPhysicalDevice physical;
    VkDevice device;
    VkFormat depth_format;
    int scale; /* the screen and the output are 640x480 times this */
    SceneImage color;
    SceneImage depth;
    SceneImage back; /* the output: retail's back buffer */
    VkRenderPass pass;
    VkFramebuffer frame;
    VkRenderPass back_clear;
    VkRenderPass back_load;
    VkFramebuffer back_frame;
    VkDescriptorSetLayout set_layout;
    VkPipelineLayout layout;
    VkDescriptorPool desc_pool;
    VkDescriptorSet set;
    VkSampler nearest;
    VkSampler linear;
    VkShaderModule vert;
    VkShaderModule frag;
    ScenePipe pipes[SF_PIPE_MAX];
    int pipe_n;
    SceneImage tex[SF_TEX_MAX];
    uint32_t tex_version[SF_TEX_MAX];
    SceneImage dummy;
    SceneImage pal;
    uint32_t pal_version[SF_PAL_MAX];
    VkBuffer staging;
    VkDeviceMemory staging_mem;
    VkDeviceSize staging_cap;
    uint8_t *staging_map;
    Copy *copies;
    int copy_n;
    int copy_cap;
    VkBuffer vbuf;
    VkDeviceMemory vbuf_mem;
    VkDeviceSize vbuf_cap;
    void *vbuf_map;
    VkBuffer ibuf;
    VkDeviceMemory ibuf_mem;
    uint32_t ibuf_quads;
    VkBuffer readback;
    VkDeviceMemory readback_mem;
    void *readback_map;
    /* showing the screen in the window */
    VkRenderPass show_pass;
    VkDescriptorSetLayout show_set_layout;
    VkPipelineLayout show_layout;
    VkDescriptorSet show_set;  /* the output, for the window */
    VkDescriptorSet copy_set;  /* the screen, for the output */
    VkPipeline show_pipe;
    VkPipeline copy_opaque;
    VkPipeline copy_blend;
    VkDeviceSize readback_size;
    int read_layer1; /* debugging: read the PS2-sized picture before its copy */
    int back_valid;  /* the output holds a frame (it starts undefined) */
};

static uint32_t mem_index(VkPhysicalDevice physical, uint32_t bits, VkMemoryPropertyFlags flags)
{
    VkPhysicalDeviceMemoryProperties props;
    uint32_t i;
    vkGetPhysicalDeviceMemoryProperties(physical, &props);
    for (i = 0; i < props.memoryTypeCount; i++) {
        if ((bits & (1u << i)) && (props.memoryTypes[i].propertyFlags & flags) == flags) {
            return i;
        }
    }
    return UINT32_MAX;
}

void sf_scene_features(VkPhysicalDeviceFeatures *features)
{
    features->shaderSampledImageArrayDynamicIndexing = VK_TRUE;
}

int sf_scene_supported(VkPhysicalDevice physical)
{
    VkPhysicalDeviceFeatures f;
    VkPhysicalDeviceProperties p;
    vkGetPhysicalDeviceFeatures(physical, &f);
    vkGetPhysicalDeviceProperties(physical, &p);
    return f.shaderSampledImageArrayDynamicIndexing && p.limits.maxPerStageDescriptorSampledImages >= SF_TEX_MAX + 1 &&
           p.limits.maxImageDimension2D >= SF_PAL_MAX;
}

static int make_buffer(SfScene *s, VkDeviceSize size, VkBufferUsageFlags usage, VkBuffer *buf, VkDeviceMemory *mem,
                       void **map)
{
    VkBufferCreateInfo info;
    VkMemoryRequirements req;
    VkMemoryAllocateInfo alloc;
    memset(&info, 0, sizeof info);
    info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    info.size = size;
    info.usage = usage;
    info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (vkCreateBuffer(s->device, &info, NULL, buf) != VK_SUCCESS) {
        return -1;
    }
    vkGetBufferMemoryRequirements(s->device, *buf, &req);
    memset(&alloc, 0, sizeof alloc);
    alloc.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    alloc.allocationSize = req.size;
    alloc.memoryTypeIndex = mem_index(s->physical, req.memoryTypeBits,
                                      VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    if (alloc.memoryTypeIndex == UINT32_MAX || vkAllocateMemory(s->device, &alloc, NULL, mem) != VK_SUCCESS) {
        vkDestroyBuffer(s->device, *buf, NULL);
        *buf = VK_NULL_HANDLE;
        return -1;
    }
    vkBindBufferMemory(s->device, *buf, *mem, 0);
    if (vkMapMemory(s->device, *mem, 0, VK_WHOLE_SIZE, 0, map) != VK_SUCCESS) {
        return -1;
    }
    return 0;
}

static void free_buffer(SfScene *s, VkBuffer *buf, VkDeviceMemory *mem)
{
    if (*buf) {
        vkDestroyBuffer(s->device, *buf, NULL);
    }
    if (*mem) {
        vkFreeMemory(s->device, *mem, NULL);
    }
    *buf = VK_NULL_HANDLE;
    *mem = VK_NULL_HANDLE;
}

static int make_image(SfScene *s, SceneImage *img, int w, int h, VkFormat format, VkImageUsageFlags usage,
                      VkImageAspectFlags aspect)
{
    VkImageCreateInfo info;
    VkMemoryRequirements req;
    VkMemoryAllocateInfo alloc;
    VkImageViewCreateInfo view;
    memset(img, 0, sizeof *img);
    memset(&info, 0, sizeof info);
    info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    info.imageType = VK_IMAGE_TYPE_2D;
    info.format = format;
    info.extent.width = (uint32_t)w;
    info.extent.height = (uint32_t)h;
    info.extent.depth = 1;
    info.mipLevels = 1;
    info.arrayLayers = 1;
    info.samples = VK_SAMPLE_COUNT_1_BIT;
    info.tiling = VK_IMAGE_TILING_OPTIMAL;
    info.usage = usage;
    info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (vkCreateImage(s->device, &info, NULL, &img->image) != VK_SUCCESS) {
        return -1;
    }
    vkGetImageMemoryRequirements(s->device, img->image, &req);
    memset(&alloc, 0, sizeof alloc);
    alloc.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    alloc.allocationSize = req.size;
    alloc.memoryTypeIndex = mem_index(s->physical, req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (alloc.memoryTypeIndex == UINT32_MAX) {
        alloc.memoryTypeIndex = mem_index(s->physical, req.memoryTypeBits, 0);
    }
    if (vkAllocateMemory(s->device, &alloc, NULL, &img->mem) != VK_SUCCESS) {
        return -1;
    }
    vkBindImageMemory(s->device, img->image, img->mem, 0);
    memset(&view, 0, sizeof view);
    view.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    view.image = img->image;
    view.viewType = VK_IMAGE_VIEW_TYPE_2D;
    view.format = format;
    view.subresourceRange.aspectMask = aspect;
    view.subresourceRange.levelCount = 1;
    view.subresourceRange.layerCount = 1;
    if (vkCreateImageView(s->device, &view, NULL, &img->view) != VK_SUCCESS) {
        return -1;
    }
    img->w = w;
    img->h = h;
    return 0;
}

static void free_image(SfScene *s, SceneImage *img)
{
    if (img->view) {
        vkDestroyImageView(s->device, img->view, NULL);
    }
    if (img->image) {
        vkDestroyImage(s->device, img->image, NULL);
    }
    if (img->mem) {
        vkFreeMemory(s->device, img->mem, NULL);
    }
    memset(img, 0, sizeof *img);
}

static VkShaderModule make_shader(VkDevice device, const uint32_t *code, size_t size)
{
    VkShaderModuleCreateInfo info;
    VkShaderModule m = VK_NULL_HANDLE;
    memset(&info, 0, sizeof info);
    info.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    info.codeSize = size;
    info.pCode = code;
    if (vkCreateShaderModule(device, &info, NULL, &m) != VK_SUCCESS) {
        return VK_NULL_HANDLE;
    }
    return m;
}

static VkFormat pick_depth(VkPhysicalDevice physical)
{
    static const VkFormat k[] = { VK_FORMAT_D32_SFLOAT, VK_FORMAT_D24_UNORM_S8_UINT, VK_FORMAT_D16_UNORM };
    size_t i;
    for (i = 0; i < sizeof k / sizeof k[0]; i++) {
        VkFormatProperties p;
        vkGetPhysicalDeviceFormatProperties(physical, k[i], &p);
        if (p.optimalTilingFeatures & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT) {
            return k[i];
        }
    }
    return VK_FORMAT_UNDEFINED;
}

static int make_pass(SfScene *s)
{
    VkAttachmentDescription att[2];
    VkAttachmentReference color_ref;
    VkAttachmentReference depth_ref;
    VkSubpassDescription sub;
    VkSubpassDependency dep[2];
    VkRenderPassCreateInfo info;
    VkImageView views[2];
    VkFramebufferCreateInfo fb;
    memset(att, 0, sizeof att);
    att[0].format = VK_FORMAT_R8G8B8A8_UNORM;
    att[0].samples = VK_SAMPLE_COUNT_1_BIT;
    att[0].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    att[0].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    att[0].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    att[0].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    att[0].initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    att[0].finalLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    att[1].format = s->depth_format;
    att[1].samples = VK_SAMPLE_COUNT_1_BIT;
    att[1].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    att[1].storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    att[1].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    att[1].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    att[1].initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    att[1].finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
    color_ref.attachment = 0;
    color_ref.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    depth_ref.attachment = 1;
    depth_ref.layout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
    memset(&sub, 0, sizeof sub);
    sub.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    sub.colorAttachmentCount = 1;
    sub.pColorAttachments = &color_ref;
    sub.pDepthStencilAttachment = &depth_ref;
    memset(dep, 0, sizeof dep);
    dep[0].srcSubpass = VK_SUBPASS_EXTERNAL;
    dep[0].dstSubpass = 0;
    dep[0].srcStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT |
                          VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dep[0].dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT;
    dep[0].srcAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    dep[0].dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT |
                           VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    dep[1].srcSubpass = 0;
    dep[1].dstSubpass = VK_SUBPASS_EXTERNAL;
    dep[1].srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dep[1].dstStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT;
    dep[1].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    dep[1].dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_TRANSFER_READ_BIT;
    memset(&info, 0, sizeof info);
    info.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
    info.attachmentCount = 2;
    info.pAttachments = att;
    info.subpassCount = 1;
    info.pSubpasses = &sub;
    info.dependencyCount = 2;
    info.pDependencies = dep;
    if (vkCreateRenderPass(s->device, &info, NULL, &s->pass) != VK_SUCCESS) {
        return -1;
    }
    /* the output's passes: one clears it, one draws over what it holds */
    info.attachmentCount = 1;
    sub.pDepthStencilAttachment = NULL;
    att[0].initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (vkCreateRenderPass(s->device, &info, NULL, &s->back_clear) != VK_SUCCESS) {
        return -1;
    }
    att[0].loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
    att[0].initialLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    if (vkCreateRenderPass(s->device, &info, NULL, &s->back_load) != VK_SUCCESS) {
        return -1;
    }
    (void)views;
    (void)fb;
    return 0;
}

static void write_copy_sets(SfScene *s);

static void free_targets(SfScene *s)
{
    if (s->frame) {
        vkDestroyFramebuffer(s->device, s->frame, NULL);
        s->frame = VK_NULL_HANDLE;
    }
    if (s->back_frame) {
        vkDestroyFramebuffer(s->device, s->back_frame, NULL);
        s->back_frame = VK_NULL_HANDLE;
    }
    free_image(s, &s->color);
    free_image(s, &s->depth);
    free_image(s, &s->back);
}

/* The screen (colour and depth) and the output, at the scale. */
static int make_targets(SfScene *s, int scale)
{
    int w = SF_SCREEN_W * scale;
    int h = SF_SCREEN_H * scale;
    VkImageView views[2];
    VkFramebufferCreateInfo fb;
    s->scale = scale;
    if (make_image(s, &s->color, w, h, VK_FORMAT_R8G8B8A8_UNORM,
                   VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
                   VK_IMAGE_ASPECT_COLOR_BIT) != 0 ||
        make_image(s, &s->depth, w, h, s->depth_format, VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT,
                   s->depth_format == VK_FORMAT_D24_UNORM_S8_UINT ? VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT
                                                                  : VK_IMAGE_ASPECT_DEPTH_BIT) != 0 ||
        make_image(s, &s->back, w, h, VK_FORMAT_R8G8B8A8_UNORM,
                   VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
                   VK_IMAGE_ASPECT_COLOR_BIT) != 0) {
        return -1;
    }
    views[0] = s->color.view;
    views[1] = s->depth.view;
    memset(&fb, 0, sizeof fb);
    fb.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
    fb.renderPass = s->pass;
    fb.attachmentCount = 2;
    fb.pAttachments = views;
    fb.width = (uint32_t)w;
    fb.height = (uint32_t)h;
    fb.layers = 1;
    if (vkCreateFramebuffer(s->device, &fb, NULL, &s->frame) != VK_SUCCESS) {
        return -1;
    }
    fb.renderPass = s->back_clear;
    fb.attachmentCount = 1;
    fb.pAttachments = &s->back.view;
    if (vkCreateFramebuffer(s->device, &fb, NULL, &s->back_frame) != VK_SUCCESS) {
        return -1;
    }
    if (s->copy_set) {
        write_copy_sets(s);
    }
    s->back_valid = 0;
    return 0;
}

static int make_layouts(SfScene *s)
{
    VkDescriptorSetLayoutBinding b[3];
    VkDescriptorSetLayoutCreateInfo set_info;
    VkPushConstantRange push;
    VkPipelineLayoutCreateInfo layout_info;
    VkDescriptorPoolSize sizes[3];
    VkDescriptorPoolCreateInfo pool;
    VkDescriptorSetAllocateInfo alloc;
    memset(b, 0, sizeof b);
    b[0].binding = 0;
    b[0].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
    b[0].descriptorCount = SF_TEX_MAX;
    b[0].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    b[1].binding = 1;
    b[1].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
    b[1].descriptorCount = 1;
    b[1].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    b[2].binding = 2;
    b[2].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER;
    b[2].descriptorCount = 1;
    b[2].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    memset(&set_info, 0, sizeof set_info);
    set_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    set_info.bindingCount = 3;
    set_info.pBindings = b;
    if (vkCreateDescriptorSetLayout(s->device, &set_info, NULL, &s->set_layout) != VK_SUCCESS) {
        return -1;
    }
    memset(&push, 0, sizeof push);
    push.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    push.size = sizeof(Push);
    memset(&layout_info, 0, sizeof layout_info);
    layout_info.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    layout_info.setLayoutCount = 1;
    layout_info.pSetLayouts = &s->set_layout;
    layout_info.pushConstantRangeCount = 1;
    layout_info.pPushConstantRanges = &push;
    if (vkCreatePipelineLayout(s->device, &layout_info, NULL, &s->layout) != VK_SUCCESS) {
        return -1;
    }
    sizes[0].type = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
    sizes[0].descriptorCount = SF_TEX_MAX + 1;
    sizes[1].type = VK_DESCRIPTOR_TYPE_SAMPLER;
    sizes[1].descriptorCount = 1;
    sizes[2].type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    sizes[2].descriptorCount = 2;
    memset(&pool, 0, sizeof pool);
    pool.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    pool.maxSets = 3;
    pool.poolSizeCount = 3;
    pool.pPoolSizes = sizes;
    if (vkCreateDescriptorPool(s->device, &pool, NULL, &s->desc_pool) != VK_SUCCESS) {
        return -1;
    }
    memset(&alloc, 0, sizeof alloc);
    alloc.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    alloc.descriptorPool = s->desc_pool;
    alloc.descriptorSetCount = 1;
    alloc.pSetLayouts = &s->set_layout;
    return vkAllocateDescriptorSets(s->device, &alloc, &s->set) == VK_SUCCESS ? 0 : -1;
}

static VkSampler make_sampler(SfScene *s, VkFilter filter)
{
    VkSamplerCreateInfo info;
    VkSampler smp = VK_NULL_HANDLE;
    memset(&info, 0, sizeof info);
    info.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    info.magFilter = filter;
    info.minFilter = filter;
    info.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    info.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    info.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    vkCreateSampler(s->device, &info, NULL, &smp);
    return smp;
}

static const VkBlendFactor k_factor[SF_BF_COUNT] = {
    VK_BLEND_FACTOR_ZERO,      VK_BLEND_FACTOR_ONE,           VK_BLEND_FACTOR_SRC_COLOR,
    VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR, VK_BLEND_FACTOR_SRC_ALPHA, VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA,
    VK_BLEND_FACTOR_DST_ALPHA, VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA, VK_BLEND_FACTOR_DST_COLOR,
    VK_BLEND_FACTOR_ONE_MINUS_DST_COLOR,
};

static const VkCompareOp k_compare[8] = {
    VK_COMPARE_OP_NEVER,   VK_COMPARE_OP_LESS,      VK_COMPARE_OP_EQUAL,          VK_COMPARE_OP_LESS_OR_EQUAL,
    VK_COMPARE_OP_GREATER, VK_COMPARE_OP_NOT_EQUAL, VK_COMPARE_OP_GREATER_OR_EQUAL, VK_COMPARE_OP_ALWAYS,
};

static VkPipeline make_pipe(SfScene *s, const SfBlendKey *k)
{
    VkPipelineShaderStageCreateInfo stages[2];
    VkVertexInputBindingDescription vb;
    VkVertexInputAttributeDescription va[4];
    VkPipelineVertexInputStateCreateInfo vertex;
    VkPipelineInputAssemblyStateCreateInfo assembly;
    VkPipelineViewportStateCreateInfo viewport;
    VkPipelineRasterizationStateCreateInfo raster;
    VkPipelineMultisampleStateCreateInfo multi;
    VkPipelineDepthStencilStateCreateInfo depth;
    VkPipelineColorBlendAttachmentState blend;
    VkPipelineColorBlendStateCreateInfo blending;
    VkDynamicState dyn[2] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
    VkPipelineDynamicStateCreateInfo dynamic;
    VkGraphicsPipelineCreateInfo info;
    VkPipeline pipe = VK_NULL_HANDLE;
    VkBlendOp op = k->op == SF_BOP_SUB ? VK_BLEND_OP_SUBTRACT : k->op == SF_BOP_REV_SUB ? VK_BLEND_OP_REVERSE_SUBTRACT
                                                                                        : VK_BLEND_OP_ADD;
    memset(stages, 0, sizeof stages);
    stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = s->vert;
    stages[0].pName = "main";
    stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = s->frag;
    stages[1].pName = "main";
    vb.binding = 0;
    vb.stride = sizeof(SfVertex);
    vb.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;
    va[0].location = 0;
    va[0].binding = 0;
    va[0].format = VK_FORMAT_R32G32B32_SFLOAT;
    va[0].offset = offsetof(SfVertex, x);
    va[1].location = 1;
    va[1].binding = 0;
    va[1].format = VK_FORMAT_B8G8R8A8_UNORM; /* 0xAARRGGBB in memory order */
    va[1].offset = offsetof(SfVertex, color);
    va[2].location = 2;
    va[2].binding = 0;
    va[2].format = VK_FORMAT_R32G32_SFLOAT;
    va[2].offset = offsetof(SfVertex, u);
    va[3].location = 3;
    va[3].binding = 0;
    va[3].format = VK_FORMAT_R32G32B32A32_SFLOAT;
    va[3].offset = offsetof(SfVertex, u0);
    memset(&vertex, 0, sizeof vertex);
    vertex.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    vertex.vertexBindingDescriptionCount = 1;
    vertex.pVertexBindingDescriptions = &vb;
    vertex.vertexAttributeDescriptionCount = 4;
    vertex.pVertexAttributeDescriptions = va;
    memset(&assembly, 0, sizeof assembly);
    assembly.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    assembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    memset(&viewport, 0, sizeof viewport);
    viewport.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    viewport.viewportCount = 1;
    viewport.scissorCount = 1;
    memset(&raster, 0, sizeof raster);
    raster.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    raster.polygonMode = VK_POLYGON_MODE_FILL;
    raster.cullMode = VK_CULL_MODE_NONE;
    raster.frontFace = VK_FRONT_FACE_CLOCKWISE;
    raster.lineWidth = 1.f;
    memset(&multi, 0, sizeof multi);
    multi.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    multi.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    memset(&depth, 0, sizeof depth);
    depth.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
    depth.depthTestEnable = k->zfunc != SF_CMP_ALWAYS || k->zwrite;
    depth.depthWriteEnable = k->zwrite;
    depth.depthCompareOp = k_compare[k->zfunc & 7];
    memset(&blend, 0, sizeof blend);
    blend.blendEnable = k->blend;
    blend.srcColorBlendFactor = k_factor[k->src];
    blend.dstColorBlendFactor = k_factor[k->dst];
    blend.colorBlendOp = op;
    blend.srcAlphaBlendFactor = k_factor[k->src];
    blend.dstAlphaBlendFactor = k_factor[k->dst];
    blend.alphaBlendOp = op;
    blend.colorWriteMask =
        VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    memset(&blending, 0, sizeof blending);
    blending.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    blending.attachmentCount = 1;
    blending.pAttachments = &blend;
    memset(&dynamic, 0, sizeof dynamic);
    dynamic.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    dynamic.dynamicStateCount = 2;
    dynamic.pDynamicStates = dyn;
    memset(&info, 0, sizeof info);
    info.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    info.stageCount = 2;
    info.pStages = stages;
    info.pVertexInputState = &vertex;
    info.pInputAssemblyState = &assembly;
    info.pViewportState = &viewport;
    info.pRasterizationState = &raster;
    info.pMultisampleState = &multi;
    info.pDepthStencilState = &depth;
    info.pColorBlendState = &blending;
    info.pDynamicState = &dynamic;
    info.layout = s->layout;
    info.renderPass = s->pass;
    if (vkCreateGraphicsPipelines(s->device, VK_NULL_HANDLE, 1, &info, NULL, &pipe) != VK_SUCCESS) {
        return VK_NULL_HANDLE;
    }
    return pipe;
}

static VkPipeline pipe_for(SfScene *s, const SfBlendKey *k)
{
    int i;
    for (i = 0; i < s->pipe_n; i++) {
        if (memcmp(&s->pipes[i].key, k, sizeof *k) == 0) {
            return s->pipes[i].pipe;
        }
    }
    if (s->pipe_n == SF_PIPE_MAX) {
        return s->pipes[0].pipe;
    }
    s->pipes[s->pipe_n].key = *k;
    s->pipes[s->pipe_n].pipe = make_pipe(s, k);
    return s->pipes[s->pipe_n++].pipe;
}

/* A full-viewport quad sampling one image (show.vert): the window's view
 * of the output, and the screen's copies onto the output. */
static VkPipeline make_quad_pipe(SfScene *s, VkShaderModule vert, VkShaderModule frag, VkRenderPass pass, int blend_on)
{
    VkPipelineShaderStageCreateInfo stages[2];
    VkPipelineVertexInputStateCreateInfo vertex;
    VkPipelineInputAssemblyStateCreateInfo assembly;
    VkPipelineViewportStateCreateInfo viewport;
    VkPipelineRasterizationStateCreateInfo raster;
    VkPipelineMultisampleStateCreateInfo multi;
    VkPipelineColorBlendAttachmentState blend;
    VkPipelineColorBlendStateCreateInfo blending;
    VkDynamicState dyn[2] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
    VkPipelineDynamicStateCreateInfo dynamic;
    VkGraphicsPipelineCreateInfo info;
    VkPipeline pipe = VK_NULL_HANDLE;
    memset(stages, 0, sizeof stages);
    stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = vert;
    stages[0].pName = "main";
    stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = frag;
    stages[1].pName = "main";
    memset(&vertex, 0, sizeof vertex);
    vertex.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    memset(&assembly, 0, sizeof assembly);
    assembly.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    assembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    memset(&viewport, 0, sizeof viewport);
    viewport.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    viewport.viewportCount = 1;
    viewport.scissorCount = 1;
    memset(&raster, 0, sizeof raster);
    raster.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    raster.polygonMode = VK_POLYGON_MODE_FILL;
    raster.cullMode = VK_CULL_MODE_NONE;
    raster.lineWidth = 1.f;
    memset(&multi, 0, sizeof multi);
    multi.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    multi.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    memset(&blend, 0, sizeof blend);
    blend.blendEnable = blend_on ? VK_TRUE : VK_FALSE;
    blend.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
    blend.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    blend.colorBlendOp = VK_BLEND_OP_ADD;
    blend.srcAlphaBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
    blend.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    blend.alphaBlendOp = VK_BLEND_OP_ADD;
    blend.colorWriteMask =
        VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    memset(&blending, 0, sizeof blending);
    blending.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    blending.attachmentCount = 1;
    blending.pAttachments = &blend;
    memset(&dynamic, 0, sizeof dynamic);
    dynamic.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    dynamic.dynamicStateCount = 2;
    dynamic.pDynamicStates = dyn;
    memset(&info, 0, sizeof info);
    info.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    info.stageCount = 2;
    info.pStages = stages;
    info.pVertexInputState = &vertex;
    info.pInputAssemblyState = &assembly;
    info.pViewportState = &viewport;
    info.pRasterizationState = &raster;
    info.pMultisampleState = &multi;
    info.pColorBlendState = &blending;
    info.pDynamicState = &dynamic;
    info.layout = s->show_layout;
    info.renderPass = pass;
    if (vkCreateGraphicsPipelines(s->device, VK_NULL_HANDLE, 1, &info, NULL, &pipe) != VK_SUCCESS) {
        return VK_NULL_HANDLE;
    }
    return pipe;
}

static void write_set(SfScene *s, VkDescriptorSet set, VkImageView view, VkSampler smp)
{
    VkDescriptorImageInfo image;
    VkWriteDescriptorSet write;
    image.sampler = smp;
    image.imageView = view;
    image.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    memset(&write, 0, sizeof write);
    write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    write.dstSet = set;
    write.descriptorCount = 1;
    write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    write.pImageInfo = &image;
    vkUpdateDescriptorSets(s->device, 1, &write, 0, NULL);
}

/* Retail copies the screen with point sampling unless its filter option
 * is on; the window scales the output smoothly. */
static void write_copy_sets(SfScene *s)
{
    write_set(s, s->copy_set, s->color.view, s->nearest);
    write_set(s, s->show_set, s->back.view, s->linear);
}

static int make_copies(SfScene *s, VkRenderPass show_pass)
{
    VkDescriptorSetLayoutBinding b;
    VkDescriptorSetLayoutCreateInfo set_info;
    VkPushConstantRange push[2];
    VkPipelineLayoutCreateInfo layout_info;
    VkDescriptorSetAllocateInfo alloc;
    VkDescriptorSetLayout layouts[2];
    VkDescriptorSet sets[2];
    VkShaderModule vert;
    VkShaderModule show_frag;
    VkShaderModule copy_frag;
    int ok;
    s->show_pass = show_pass;
    memset(&b, 0, sizeof b);
    b.binding = 0;
    b.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    b.descriptorCount = 1;
    b.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    memset(&set_info, 0, sizeof set_info);
    set_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    set_info.bindingCount = 1;
    set_info.pBindings = &b;
    if (vkCreateDescriptorSetLayout(s->device, &set_info, NULL, &s->show_set_layout) != VK_SUCCESS) {
        return -1;
    }
    memset(push, 0, sizeof push);
    /* src at 0 for both stages; the window filter's mode follows it */
    push[0].stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
    push[0].size = 16;
    push[1].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    push[1].offset = 16;
    push[1].size = 16;
    memset(&layout_info, 0, sizeof layout_info);
    layout_info.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    layout_info.setLayoutCount = 1;
    layout_info.pSetLayouts = &s->show_set_layout;
    layout_info.pushConstantRangeCount = 2;
    layout_info.pPushConstantRanges = push;
    if (vkCreatePipelineLayout(s->device, &layout_info, NULL, &s->show_layout) != VK_SUCCESS) {
        return -1;
    }
    layouts[0] = s->show_set_layout;
    layouts[1] = s->show_set_layout;
    memset(&alloc, 0, sizeof alloc);
    alloc.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    alloc.descriptorPool = s->desc_pool;
    alloc.descriptorSetCount = 2;
    alloc.pSetLayouts = layouts;
    if (vkAllocateDescriptorSets(s->device, &alloc, sets) != VK_SUCCESS) {
        return -1;
    }
    s->show_set = sets[0];
    s->copy_set = sets[1];
    write_copy_sets(s);

    vert = make_shader(s->device, sf_show_vert_spv, sf_show_vert_spv_size);
    show_frag = make_shader(s->device, sf_show_frag_spv, sf_show_frag_spv_size);
    copy_frag = make_shader(s->device, sf_copy_frag_spv, sf_copy_frag_spv_size);
    ok = vert && show_frag && copy_frag;
    if (ok) {
        s->copy_opaque = make_quad_pipe(s, vert, copy_frag, s->back_clear, 0);
        s->copy_blend = make_quad_pipe(s, vert, copy_frag, s->back_clear, 1);
        if (show_pass) {
            s->show_pipe = make_quad_pipe(s, vert, show_frag, show_pass, 0);
        }
        ok = s->copy_opaque && s->copy_blend && (!show_pass || s->show_pipe);
    }
    if (vert) {
        vkDestroyShaderModule(s->device, vert, NULL);
    }
    if (show_frag) {
        vkDestroyShaderModule(s->device, show_frag, NULL);
    }
    if (copy_frag) {
        vkDestroyShaderModule(s->device, copy_frag, NULL);
    }
    return ok ? 0 : -1;
}

static void write_texture_descriptor(SfScene *s, int h)
{
    VkDescriptorImageInfo image;
    VkWriteDescriptorSet write;
    image.sampler = VK_NULL_HANDLE;
    image.imageView = s->tex[h].view ? s->tex[h].view : s->dummy.view;
    image.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    memset(&write, 0, sizeof write);
    write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    write.dstSet = s->set;
    write.dstBinding = 0;
    write.dstArrayElement = (uint32_t)h;
    write.descriptorCount = 1;
    write.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
    write.pImageInfo = &image;
    vkUpdateDescriptorSets(s->device, 1, &write, 0, NULL);
}

int sf_scene_create(SfScene **out, VkPhysicalDevice physical, VkDevice device, VkRenderPass show_pass, char *err,
                    size_t err_n)
{
    SfScene *s = calloc(1, sizeof *s);
    VkDescriptorImageInfo images[2];
    VkWriteDescriptorSet writes[2];
    int h;
    *out = NULL;
    if (!s) {
        sf_set_err(err, err_n, "out of memory");
        return -1;
    }
    s->physical = physical;
    s->device = device;
    s->depth_format = pick_depth(physical);
    if (s->depth_format == VK_FORMAT_UNDEFINED) {
        sf_set_err(err, err_n, "no depth format");
        sf_scene_destroy(s);
        return -1;
    }
    if (make_image(s, &s->dummy, 1, 1, VK_FORMAT_R8G8B8A8_UNORM,
                   VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, VK_IMAGE_ASPECT_COLOR_BIT) != 0 ||
        make_image(s, &s->pal, SF_PAL_ENTRIES, SF_PAL_MAX, VK_FORMAT_R8G8B8A8_UNORM,
                   VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, VK_IMAGE_ASPECT_COLOR_BIT) != 0) {
        sf_set_err(err, err_n, "Vulkan scene images failed");
        sf_scene_destroy(s);
        return -1;
    }
    if (make_pass(s) != 0 || make_layouts(s) != 0 || make_targets(s, 1) != 0) {
        sf_set_err(err, err_n, "Vulkan scene pass failed");
        sf_scene_destroy(s);
        return -1;
    }
    s->nearest = make_sampler(s, VK_FILTER_NEAREST);
    s->linear = make_sampler(s, VK_FILTER_LINEAR);
    s->vert = make_shader(device, sf_scene_vert_spv, sf_scene_vert_spv_size);
    s->frag = make_shader(device, sf_scene_frag_spv, sf_scene_frag_spv_size);
    if (!s->nearest || !s->linear || !s->vert || !s->frag) {
        sf_set_err(err, err_n, "Vulkan scene shaders failed");
        sf_scene_destroy(s);
        return -1;
    }
    for (h = 0; h < SF_TEX_MAX; h++) {
        write_texture_descriptor(s, h);
    }
    memset(images, 0, sizeof images);
    images[0].imageView = s->pal.view;
    images[0].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    images[1].sampler = s->nearest;
    memset(writes, 0, sizeof writes);
    writes[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[0].dstSet = s->set;
    writes[0].dstBinding = 1;
    writes[0].descriptorCount = 1;
    writes[0].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
    writes[0].pImageInfo = &images[0];
    writes[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[1].dstSet = s->set;
    writes[1].dstBinding = 2;
    writes[1].descriptorCount = 1;
    writes[1].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER;
    writes[1].pImageInfo = &images[1];
    vkUpdateDescriptorSets(device, 2, writes, 0, NULL);
    if (make_copies(s, show_pass) != 0) {
        sf_set_err(err, err_n, "Vulkan copy pipelines failed");
        sf_scene_destroy(s);
        return -1;
    }
    *out = s;
    return 0;
}

void sf_scene_destroy(SfScene *s)
{
    int i;
    if (!s) {
        return;
    }
    vkDeviceWaitIdle(s->device);
    for (i = 0; i < s->pipe_n; i++) {
        if (s->pipes[i].pipe) {
            vkDestroyPipeline(s->device, s->pipes[i].pipe, NULL);
        }
    }
    if (s->show_pipe) {
        vkDestroyPipeline(s->device, s->show_pipe, NULL);
    }
    if (s->copy_opaque) {
        vkDestroyPipeline(s->device, s->copy_opaque, NULL);
    }
    if (s->copy_blend) {
        vkDestroyPipeline(s->device, s->copy_blend, NULL);
    }
    if (s->show_layout) {
        vkDestroyPipelineLayout(s->device, s->show_layout, NULL);
    }
    if (s->show_set_layout) {
        vkDestroyDescriptorSetLayout(s->device, s->show_set_layout, NULL);
    }
    if (s->vert) {
        vkDestroyShaderModule(s->device, s->vert, NULL);
    }
    if (s->frag) {
        vkDestroyShaderModule(s->device, s->frag, NULL);
    }
    if (s->nearest) {
        vkDestroySampler(s->device, s->nearest, NULL);
    }
    if (s->linear) {
        vkDestroySampler(s->device, s->linear, NULL);
    }
    if (s->desc_pool) {
        vkDestroyDescriptorPool(s->device, s->desc_pool, NULL);
    }
    if (s->layout) {
        vkDestroyPipelineLayout(s->device, s->layout, NULL);
    }
    if (s->set_layout) {
        vkDestroyDescriptorSetLayout(s->device, s->set_layout, NULL);
    }
    free_targets(s);
    if (s->pass) {
        vkDestroyRenderPass(s->device, s->pass, NULL);
    }
    if (s->back_clear) {
        vkDestroyRenderPass(s->device, s->back_clear, NULL);
    }
    if (s->back_load) {
        vkDestroyRenderPass(s->device, s->back_load, NULL);
    }
    for (i = 0; i < SF_TEX_MAX; i++) {
        free_image(s, &s->tex[i]);
    }
    free_image(s, &s->dummy);
    free_image(s, &s->pal);
    free_buffer(s, &s->staging, &s->staging_mem);
    free_buffer(s, &s->vbuf, &s->vbuf_mem);
    free_buffer(s, &s->ibuf, &s->ibuf_mem);
    free_buffer(s, &s->readback, &s->readback_mem);
    free(s->copies);
    free(s);
}

static int add_copy(SfScene *s, int target, VkDeviceSize offset, uint32_t y, uint32_t w, uint32_t h)
{
    if (s->copy_n == s->copy_cap) {
        int cap = s->copy_cap ? s->copy_cap * 2 : 64;
        Copy *c = realloc(s->copies, (size_t)cap * sizeof *c);
        if (!c) {
            return -1;
        }
        s->copies = c;
        s->copy_cap = cap;
    }
    s->copies[s->copy_n].target = target;
    s->copies[s->copy_n].offset = offset;
    s->copies[s->copy_n].y = y;
    s->copies[s->copy_n].w = w;
    s->copies[s->copy_n].h = h;
    s->copy_n++;
    return 0;
}

static int ensure_staging(SfScene *s, VkDeviceSize need)
{
    VkDeviceSize cap;
    void *map = NULL;
    if (need <= s->staging_cap) {
        return 0;
    }
    free_buffer(s, &s->staging, &s->staging_mem);
    cap = s->staging_cap ? s->staging_cap : (VkDeviceSize)4 << 20;
    while (cap < need) {
        cap *= 2;
    }
    if (make_buffer(s, cap, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, &s->staging, &s->staging_mem, &map) != 0) {
        s->staging_cap = 0;
        return -1;
    }
    s->staging_map = map;
    s->staging_cap = cap;
    return 0;
}

static int ensure_vertices(SfScene *s, uint32_t quads)
{
    VkDeviceSize need = (VkDeviceSize)quads * 4 * sizeof(SfVertex);
    if (need > s->vbuf_cap) {
        VkDeviceSize cap = s->vbuf_cap ? s->vbuf_cap : 4096 * sizeof(SfVertex);
        while (cap < need) {
            cap *= 2;
        }
        free_buffer(s, &s->vbuf, &s->vbuf_mem);
        if (make_buffer(s, cap, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, &s->vbuf, &s->vbuf_mem, &s->vbuf_map) != 0) {
            s->vbuf_cap = 0;
            return -1;
        }
        s->vbuf_cap = cap;
    }
    if (quads > s->ibuf_quads) {
        uint32_t n = s->ibuf_quads ? s->ibuf_quads : 1024;
        uint32_t *idx;
        uint32_t q;
        void *map = NULL;
        while (n < quads) {
            n *= 2;
        }
        free_buffer(s, &s->ibuf, &s->ibuf_mem);
        if (make_buffer(s, (VkDeviceSize)n * 6 * sizeof(uint32_t), VK_BUFFER_USAGE_INDEX_BUFFER_BIT, &s->ibuf,
                        &s->ibuf_mem, &map) != 0) {
            s->ibuf_quads = 0;
            return -1;
        }
        idx = map;
        for (q = 0; q < n; q++) {
            idx[q * 6 + 0] = q * 4 + 0;
            idx[q * 6 + 1] = q * 4 + 1;
            idx[q * 6 + 2] = q * 4 + 2;
            idx[q * 6 + 3] = q * 4 + 2;
            idx[q * 6 + 4] = q * 4 + 1;
            idx[q * 6 + 5] = q * 4 + 3;
        }
        s->ibuf_quads = n;
    }
    return 0;
}

int sf_scene_prepare(SfScene *s, const SfDrawList *l, char *err, size_t err_n)
{
    VkDeviceSize need = 4;
    VkDeviceSize at = 0;
    int h;
    int pal_dirty = 0;
    s->copy_n = 0;
    for (h = 1; h < SF_TEX_MAX; h++) {
        const SfTexture *t = &l->tex[h];
        if (!t->rgba) {
            if (s->tex[h].image) {
                free_image(s, &s->tex[h]);
                write_texture_descriptor(s, h);
            }
            s->tex_version[h] = 0;
            continue;
        }
        if (s->tex[h].image && s->tex_version[h] == t->version && s->tex[h].w == t->w && s->tex[h].h == t->h) {
            continue;
        }
        need += (VkDeviceSize)t->w * t->h * 4;
    }
    for (h = 0; h < SF_PAL_MAX; h++) {
        if (l->pal[h].version != s->pal_version[h]) {
            pal_dirty = 1;
        }
    }
    if (pal_dirty || !s->pal.ready) {
        need += (VkDeviceSize)SF_PAL_ENTRIES * SF_PAL_MAX * 4;
    }
    if (ensure_staging(s, need) != 0) {
        sf_set_err(err, err_n, "Vulkan staging failed");
        return -1;
    }
    if (!s->dummy.ready) {
        memset(s->staging_map, 0, 4);
        add_copy(s, -2, 0, 0, 1, 1);
        at = 4;
    }
    for (h = 1; h < SF_TEX_MAX; h++) {
        const SfTexture *t = &l->tex[h];
        VkDeviceSize n;
        if (!t->rgba || (s->tex[h].image && s->tex_version[h] == t->version && s->tex[h].w == t->w &&
                         s->tex[h].h == t->h)) {
            continue;
        }
        if (s->tex[h].image && (s->tex[h].w != t->w || s->tex[h].h != t->h)) {
            free_image(s, &s->tex[h]);
        }
        if (!s->tex[h].image) {
            if (make_image(s, &s->tex[h], t->w, t->h, VK_FORMAT_R8G8B8A8_UNORM,
                           VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                           VK_IMAGE_ASPECT_COLOR_BIT) != 0) {
                sf_set_err(err, err_n, "Vulkan texture %d failed", h);
                return -1;
            }
            write_texture_descriptor(s, h);
        }
        n = (VkDeviceSize)t->w * t->h * 4;
        memcpy(s->staging_map + at, t->rgba, (size_t)n);
        add_copy(s, h, at, 0, (uint32_t)t->w, (uint32_t)t->h);
        at += n;
        s->tex_version[h] = t->version;
    }
    if (pal_dirty || !s->pal.ready) {
        for (h = 0; h < SF_PAL_MAX; h++) {
            memcpy(s->staging_map + at + (size_t)h * SF_PAL_ENTRIES * 4, l->pal[h].rgba, SF_PAL_ENTRIES * 4);
            s->pal_version[h] = l->pal[h].version;
        }
        add_copy(s, -1, at, 0, SF_PAL_ENTRIES, SF_PAL_MAX);
        at += (VkDeviceSize)SF_PAL_ENTRIES * SF_PAL_MAX * 4;
    }
    if (ensure_vertices(s, l->v_n / 4 + 1) != 0) {
        sf_set_err(err, err_n, "Vulkan vertex buffer failed");
        return -1;
    }
    if (l->v_n) {
        memcpy(s->vbuf_map, l->v, (size_t)l->v_n * sizeof(SfVertex));
    }
    return 0;
}

static void barrier(VkCommandBuffer cmd, VkImage image, VkImageLayout from, VkImageLayout to, VkAccessFlags src_access,
                    VkAccessFlags dst_access, VkPipelineStageFlags src, VkPipelineStageFlags dst)
{
    VkImageMemoryBarrier b;
    memset(&b, 0, sizeof b);
    b.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    b.oldLayout = from;
    b.newLayout = to;
    b.srcAccessMask = src_access;
    b.dstAccessMask = dst_access;
    b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = image;
    b.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    b.subresourceRange.levelCount = 1;
    b.subresourceRange.layerCount = 1;
    vkCmdPipelineBarrier(cmd, src, dst, 0, 0, NULL, 0, NULL, 1, &b);
}

static void record_uploads(SfScene *s, VkCommandBuffer cmd)
{
    int i;
    for (i = 0; i < s->copy_n; i++) {
        const Copy *c = &s->copies[i];
        SceneImage *img = c->target == -1 ? &s->pal : c->target == -2 ? &s->dummy : &s->tex[c->target];
        VkBufferImageCopy region;
        barrier(cmd, img->image, img->ready ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL : VK_IMAGE_LAYOUT_UNDEFINED,
                VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_ACCESS_SHADER_READ_BIT, VK_ACCESS_TRANSFER_WRITE_BIT,
                VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                VK_PIPELINE_STAGE_TRANSFER_BIT);
        memset(&region, 0, sizeof region);
        region.bufferOffset = c->offset;
        region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        region.imageSubresource.layerCount = 1;
        region.imageOffset.y = (int32_t)c->y;
        region.imageExtent.width = c->w;
        region.imageExtent.height = c->h;
        region.imageExtent.depth = 1;
        vkCmdCopyBufferToImage(cmd, s->staging, img->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
        barrier(cmd, img->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
        img->ready = 1;
    }
    /* a handle that was created but never uploaded still needs a layout */
    for (i = 1; i < SF_TEX_MAX; i++) {
        if (s->tex[i].image && !s->tex[i].ready) {
            barrier(cmd, s->tex[i].image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, 0,
                    VK_ACCESS_SHADER_READ_BIT, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                    VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
            s->tex[i].ready = 1;
        }
    }
    s->copy_n = 0;
}

int sf_scene_scale(const SfScene *s)
{
    return s->scale;
}

int sf_scene_set_scale(SfScene *s, int scale, char *err, size_t err_n)
{
    if (scale < 1) {
        scale = 1;
    }
    if (scale == s->scale) {
        return 0;
    }
    vkDeviceWaitIdle(s->device);
    free_targets(s);
    free_buffer(s, &s->readback, &s->readback_mem);
    s->readback_map = NULL;
    if (make_targets(s, scale) != 0) {
        sf_set_err(err, err_n, "Vulkan screen at %dx failed", scale);
        return -1;
    }
    return 0;
}

static VkClearValue clear_of(uint32_t c)
{
    VkClearValue v;
    memset(&v, 0, sizeof v);
    v.color.float32[0] = (float)((c >> 16) & 0xFF) / 255.f;
    v.color.float32[1] = (float)((c >> 8) & 0xFF) / 255.f;
    v.color.float32[2] = (float)(c & 0xFF) / 255.f;
    v.color.float32[3] = (float)(c >> 24) / 255.f;
    return v;
}

/* Batches [from, to) onto the screen, which starts as clear. Positions
 * are in 640x480 screen pixels at any scale: the vertex shader maps them,
 * and the scissor scales. */
static void record_screen(SfScene *s, VkCommandBuffer cmd, const SfDrawList *l, uint32_t from, uint32_t to,
                          uint32_t clear)
{
    VkRenderPassBeginInfo rp;
    VkClearValue clears[2];
    VkViewport viewport;
    VkDeviceSize zero = 0;
    VkPipeline bound = VK_NULL_HANDLE;
    int k = s->scale;
    uint32_t i;
    clears[0] = clear_of(clear);
    memset(&clears[1], 0, sizeof clears[1]);
    clears[1].depthStencil.depth = 1.f;
    memset(&rp, 0, sizeof rp);
    rp.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    rp.renderPass = s->pass;
    rp.framebuffer = s->frame;
    rp.renderArea.extent.width = (uint32_t)(SF_SCREEN_W * k);
    rp.renderArea.extent.height = (uint32_t)(SF_SCREEN_H * k);
    rp.clearValueCount = 2;
    rp.pClearValues = clears;
    vkCmdBeginRenderPass(cmd, &rp, VK_SUBPASS_CONTENTS_INLINE);
    viewport.x = 0.f;
    viewport.y = 0.f;
    viewport.width = (float)(SF_SCREEN_W * k);
    viewport.height = (float)(SF_SCREEN_H * k);
    viewport.minDepth = 0.f;
    viewport.maxDepth = 1.f;
    vkCmdSetViewport(cmd, 0, 1, &viewport);
    if (to > from) {
        vkCmdBindVertexBuffers(cmd, 0, 1, &s->vbuf, &zero);
        vkCmdBindIndexBuffer(cmd, s->ibuf, 0, VK_INDEX_TYPE_UINT32);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, s->layout, 0, 1, &s->set, 0, NULL);
    }
    for (i = from; i < to; i++) {
        const SfBatch *b = &l->b[i];
        const SfShadeState *st = &b->shade;
        VkPipeline p = pipe_for(s, &b->blend);
        VkRect2D scissor;
        Push push;
        int x0 = b->clip[0] < 0 ? 0 : b->clip[0];
        int y0 = b->clip[1] < 0 ? 0 : b->clip[1];
        int x1 = b->clip[2] > SF_SCREEN_W ? SF_SCREEN_W : b->clip[2];
        int y1 = b->clip[3] > SF_SCREEN_H ? SF_SCREEN_H : b->clip[3];
        if (!p || x1 <= x0 || y1 <= y0) {
            continue;
        }
        if (p != bound) {
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, p);
            bound = p;
        }
        scissor.offset.x = x0 * k;
        scissor.offset.y = y0 * k;
        scissor.extent.width = (uint32_t)((x1 - x0) * k);
        scissor.extent.height = (uint32_t)((y1 - y0) * k);
        vkCmdSetScissor(cmd, 0, 1, &scissor);
        memset(&push, 0, sizeof push);
        push.tex = st->tex;
        push.pal = st->pal;
        push.ops = st->color_op | (uint32_t)st->alpha_op << 8 | (uint32_t)st->address_u << 16 |
                   (uint32_t)st->address_v << 24;
        push.misc = (st->point ? 1u : 0u) | (st->tex && l->tex[st->tex].indexed ? 2u : 0u) |
                    (st->texel_uv ? 4u : 0u) | (st->tex && l->tex[st->tex].rgba ? 8u : 0u) |
                    (st->alpha_test ? 16u : 0u) | (uint32_t)(st->alpha_func & 7) << 5 |
                    (uint32_t)st->alpha_ref << 8;
        push.size[0] = st->tex ? (float)l->tex[st->tex].w : 1.f;
        push.size[1] = st->tex ? (float)l->tex[st->tex].h : 1.f;
        push.scale = (float)k;
        vkCmdPushConstants(cmd, s->layout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof push, &push);
        vkCmdDrawIndexed(cmd, b->quads * 6, 1, (b->first / 4) * 6, 0, 0);
    }
    vkCmdEndRenderPass(cmd);
}

/* The screen's rect (in 640x480 pixels) onto the output's 640x448 game
 * area at the display offset, 16 lines down, as retail's frame end draws
 * it: opaque, or blended by the screen's alpha. */
static void record_copy(SfScene *s, VkCommandBuffer cmd, int clear_out, uint32_t clear, VkPipeline pipe,
                        const int rect[4], const int at[2])
{
    VkRenderPassBeginInfo rp;
    VkClearValue cv = clear_of(clear);
    VkViewport viewport;
    VkRect2D scissor;
    float src[4];
    int k = s->scale;
    memset(&rp, 0, sizeof rp);
    rp.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    rp.renderPass = clear_out ? s->back_clear : s->back_load;
    rp.framebuffer = s->back_frame;
    rp.renderArea.extent.width = (uint32_t)(SF_SCREEN_W * k);
    rp.renderArea.extent.height = (uint32_t)(SF_SCREEN_H * k);
    rp.clearValueCount = 1;
    rp.pClearValues = &cv;
    vkCmdBeginRenderPass(cmd, &rp, VK_SUBPASS_CONTENTS_INLINE);
    viewport.x = (float)(at[0] * k);
    viewport.y = (float)((at[1] + 16) * k);
    viewport.width = (float)(640 * k);
    viewport.height = (float)(448 * k);
    viewport.minDepth = 0.f;
    viewport.maxDepth = 1.f;
    scissor.offset.x = 0;
    scissor.offset.y = 0;
    scissor.extent = rp.renderArea.extent;
    src[0] = (float)rect[0] / SF_SCREEN_W;
    src[1] = (float)rect[1] / SF_SCREEN_H;
    src[2] = (float)rect[2] / SF_SCREEN_W;
    src[3] = (float)rect[3] / SF_SCREEN_H;
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
    vkCmdSetViewport(cmd, 0, 1, &viewport);
    vkCmdSetScissor(cmd, 0, 1, &scissor);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, s->show_layout, 0, 1, &s->copy_set, 0, NULL);
    vkCmdPushConstants(cmd, s->show_layout, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof src, src);
    vkCmdDraw(cmd, 3, 1, 0, 0);
    vkCmdEndRenderPass(cmd);
}

void sf_scene_read_layer1(SfScene *s)
{
    s->read_layer1 = 1;
}

void sf_scene_record(SfScene *s, VkCommandBuffer cmd, const SfDrawList *l)
{
    record_uploads(s, cmd);
    if (l->cp3) {
        uint32_t at = l->cp3_at < l->b_n ? l->cp3_at : l->b_n;
        record_screen(s, cmd, l, 0, at, l->clear);
        if (s->read_layer1) {
            sf_scene_record_readback(s, cmd, 0);
            s->read_layer1 = 0;
        }
        record_copy(s, cmd, 1, l->clear, s->copy_opaque, l->cp3_rect, l->show_at);
        record_screen(s, cmd, l, at, l->b_n, 0);
        record_copy(s, cmd, 0, 0, s->copy_blend, l->show, l->show_at);
    } else {
        /* no PS2-sized picture this frame: the output keeps the last one,
         * as retail's back buffer does, and the screen is blended over it */
        record_screen(s, cmd, l, 0, l->b_n, l->clear);
        record_copy(s, cmd, !s->back_valid, 0xFF000000, s->copy_blend, l->show, l->show_at);
    }
    s->back_valid = 1;
}

void sf_scene_record_show(SfScene *s, VkCommandBuffer cmd, VkExtent2D extent, float aspect, const SfDrawList *l,
                          int filter)
{
    VkViewport viewport;
    VkRect2D scissor;
    float block[8];
    float frame_w;
    float frame_h;
    /* the output's game area, 640x448 at line 16 (record_copy puts it
     * there): the bands above and below are black on every screen, and a
     * 4:3 TV showed the 448 lines from top to bottom */
    static const float src[4] = { 0.f, 16.f / SF_SCREEN_H, 1.f, 464.f / SF_SCREEN_H };
    (void)l;
    if (!s->show_pipe) {
        return;
    }
    /* the game area in the asked shape, centred in the window */
    frame_w = (float)extent.width;
    frame_h = (float)extent.height;
    if (aspect > 0.f) {
        frame_h = frame_w / aspect;
        if (frame_h > (float)extent.height) {
            frame_h = (float)extent.height;
            frame_w = frame_h * aspect;
        }
    }
    viewport.x = ((float)extent.width - frame_w) / 2.f;
    viewport.y = ((float)extent.height - frame_h) / 2.f;
    viewport.width = frame_w;
    viewport.height = frame_h;
    viewport.minDepth = 0.f;
    viewport.maxDepth = 1.f;
    scissor.offset.x = (int32_t)viewport.x;
    scissor.offset.y = (int32_t)viewport.y;
    scissor.extent.width = (uint32_t)frame_w;
    scissor.extent.height = (uint32_t)frame_h;
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, s->show_pipe);
    vkCmdSetViewport(cmd, 0, 1, &viewport);
    vkCmdSetScissor(cmd, 0, 1, &scissor);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, s->show_layout, 0, 1, &s->show_set, 0, NULL);
    memset(block, 0, sizeof block);
    memcpy(block, src, sizeof src);
    block[4] = (float)(filter < 0 ? 0 : (filter > 3 ? 3 : filter));
    vkCmdPushConstants(cmd, s->show_layout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, 16, block);
    vkCmdPushConstants(cmd, s->show_layout, VK_SHADER_STAGE_FRAGMENT_BIT, 16, 16, block + 4);
    vkCmdDraw(cmd, 3, 1, 0, 0);
}

void sf_scene_record_readback(SfScene *s, VkCommandBuffer cmd, int output)
{
    VkBufferImageCopy region;
    VkImage image = output ? s->back.image : s->color.image;
    VkDeviceSize size = (VkDeviceSize)SF_SCREEN_W * SF_SCREEN_H * 4 * (VkDeviceSize)(s->scale * s->scale);
    if (!s->readback || s->readback_size != size) {
        free_buffer(s, &s->readback, &s->readback_mem);
        if (make_buffer(s, size, VK_BUFFER_USAGE_TRANSFER_DST_BIT, &s->readback, &s->readback_mem,
                        &s->readback_map) != 0) {
            s->readback_map = NULL;
            return;
        }
        s->readback_size = size;
    }
    barrier(cmd, image, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
            VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT,
            VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
    memset(&region, 0, sizeof region);
    region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    region.imageSubresource.layerCount = 1;
    region.imageExtent.width = (uint32_t)(SF_SCREEN_W * s->scale);
    region.imageExtent.height = (uint32_t)(SF_SCREEN_H * s->scale);
    region.imageExtent.depth = 1;
    vkCmdCopyImageToBuffer(cmd, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, s->readback, 1, &region);
    barrier(cmd, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
            VK_ACCESS_TRANSFER_READ_BIT, VK_ACCESS_SHADER_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
            VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
}

const uint8_t *sf_scene_read(SfScene *s, int *w, int *h)
{
    *w = SF_SCREEN_W * s->scale;
    *h = SF_SCREEN_H * s->scale;
    return s->readback_map;
}
