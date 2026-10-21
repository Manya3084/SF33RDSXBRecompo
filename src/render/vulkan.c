#include "render/renderer_internal.h"

#include "common.h"
#include "render/scene.h"
#include "render/ui_spv.h"

#include <SDL3/SDL_vulkan.h>

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

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

static void destroy_swapchain(SfRenderer *r)
{
    uint32_t i;
    if (!r->device) {
        return;
    }
    for (i = 0; i < r->image_count; i++) {
        if (r->frames && r->frames[i]) {
            vkDestroyFramebuffer(r->device, r->frames[i], NULL);
        }
        if (r->views && r->views[i]) {
            vkDestroyImageView(r->device, r->views[i], NULL);
        }
    }
    free(r->frames);
    free(r->views);
    free(r->images);
    r->frames = NULL;
    r->views = NULL;
    r->images = NULL;
    r->image_count = 0;
    if (r->swapchain) {
        vkDestroySwapchainKHR(r->device, r->swapchain, NULL);
        r->swapchain = VK_NULL_HANDLE;
    }
}

static int make_swapchain(SfRenderer *r, char *err, size_t err_n)
{
    VkSurfaceCapabilitiesKHR caps;
    VkSurfaceFormatKHR *formats = NULL;
    uint32_t format_count = 0;
    VkPresentModeKHR *modes = NULL;
    uint32_t mode_count = 0;
    VkExtent2D extent;
    VkSwapchainCreateInfoKHR info;
    VkFormat chosen = VK_FORMAT_B8G8R8A8_UNORM;
    VkColorSpaceKHR space = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
    VkPresentModeKHR mode = VK_PRESENT_MODE_FIFO_KHR;
    uint32_t count;
    uint32_t i;
    VkImageViewCreateInfo view_info;
    int w = 0;
    int h = 0;

    if (vkGetPhysicalDeviceSurfaceCapabilitiesKHR(r->physical, r->surface, &caps) != VK_SUCCESS) {
        sf_set_err(err, err_n, "Vulkan surface caps failed");
        return -1;
    }
    vkGetPhysicalDeviceSurfaceFormatsKHR(r->physical, r->surface, &format_count, NULL);
    if (format_count == 0) {
        sf_set_err(err, err_n, "Vulkan surface has no formats");
        return -1;
    }
    formats = calloc(format_count, sizeof(*formats));
    if (!formats) {
        sf_set_err(err, err_n, "out of memory");
        return -1;
    }
    vkGetPhysicalDeviceSurfaceFormatsKHR(r->physical, r->surface, &format_count, formats);
    if (!(format_count == 1 && formats[0].format == VK_FORMAT_UNDEFINED)) {
        chosen = formats[0].format;
        space = formats[0].colorSpace;
        for (i = 0; i < format_count; i++) {
            if (formats[i].format == VK_FORMAT_B8G8R8A8_UNORM || formats[i].format == VK_FORMAT_R8G8B8A8_UNORM) {
                chosen = formats[i].format;
                space = formats[i].colorSpace;
                break;
            }
        }
    }
    free(formats);
    vkGetPhysicalDeviceSurfacePresentModesKHR(r->physical, r->surface, &mode_count, NULL);
    if (mode_count > 0) {
        modes = calloc(mode_count, sizeof(*modes));
        if (!modes) {
            sf_set_err(err, err_n, "out of memory");
            return -1;
        }
        vkGetPhysicalDeviceSurfacePresentModesKHR(r->physical, r->surface, &mode_count, modes);
        for (i = 0; i < mode_count; i++) {
            if (modes[i] == VK_PRESENT_MODE_FIFO_KHR) {
                mode = modes[i];
                break;
            }
        }
        free(modes);
    }
    if (caps.currentExtent.width != UINT32_MAX) {
        extent = caps.currentExtent;
    } else {
        SDL_GetWindowSizeInPixels(r->window, &w, &h);
        if (w < 1) {
            w = 1;
        }
        if (h < 1) {
            h = 1;
        }
        extent.width = (uint32_t)w;
        extent.height = (uint32_t)h;
        if (extent.width < caps.minImageExtent.width) {
            extent.width = caps.minImageExtent.width;
        }
        if (extent.height < caps.minImageExtent.height) {
            extent.height = caps.minImageExtent.height;
        }
        if (extent.width > caps.maxImageExtent.width) {
            extent.width = caps.maxImageExtent.width;
        }
        if (extent.height > caps.maxImageExtent.height) {
            extent.height = caps.maxImageExtent.height;
        }
    }
    count = caps.minImageCount + 1;
    if (caps.maxImageCount > 0 && count > caps.maxImageCount) {
        count = caps.maxImageCount;
    }
    memset(&info, 0, sizeof info);
    info.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
    info.surface = r->surface;
    info.minImageCount = count;
    info.imageFormat = chosen;
    info.imageColorSpace = space;
    info.imageExtent = extent;
    info.imageArrayLayers = 1;
    info.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    info.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    info.preTransform = caps.currentTransform;
    info.compositeAlpha = (caps.supportedCompositeAlpha & VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR) ? VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR : VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR;
    info.presentMode = mode;
    info.clipped = VK_TRUE;
    if (vkCreateSwapchainKHR(r->device, &info, NULL, &r->swapchain) != VK_SUCCESS) {
        sf_set_err(err, err_n, "Vulkan swapchain failed");
        return -1;
    }
    r->format = chosen;
    r->extent = extent;
    vkGetSwapchainImagesKHR(r->device, r->swapchain, &r->image_count, NULL);
    r->images = calloc(r->image_count, sizeof(*r->images));
    r->views = calloc(r->image_count, sizeof(*r->views));
    r->frames = calloc(r->image_count, sizeof(*r->frames));
    if (!r->images || !r->views || !r->frames) {
        sf_set_err(err, err_n, "out of memory");
        return -1;
    }
    vkGetSwapchainImagesKHR(r->device, r->swapchain, &r->image_count, r->images);
    memset(&view_info, 0, sizeof view_info);
    view_info.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    view_info.viewType = VK_IMAGE_VIEW_TYPE_2D;
    view_info.format = chosen;
    view_info.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    view_info.subresourceRange.levelCount = 1;
    view_info.subresourceRange.layerCount = 1;
    for (i = 0; i < r->image_count; i++) {
        VkFramebufferCreateInfo fb;
        view_info.image = r->images[i];
        if (vkCreateImageView(r->device, &view_info, NULL, &r->views[i]) != VK_SUCCESS) {
            sf_set_err(err, err_n, "Vulkan image view failed");
            return -1;
        }
        memset(&fb, 0, sizeof fb);
        fb.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
        fb.renderPass = r->pass;
        fb.attachmentCount = 1;
        fb.pAttachments = &r->views[i];
        fb.width = extent.width;
        fb.height = extent.height;
        fb.layers = 1;
        if (vkCreateFramebuffer(r->device, &fb, NULL, &r->frames[i]) != VK_SUCCESS) {
            sf_set_err(err, err_n, "Vulkan framebuffer failed");
            return -1;
        }
    }
    return 0;
}

static VkShaderModule make_shader(VkDevice device, const uint32_t *code, size_t size)
{
    VkShaderModuleCreateInfo info;
    VkShaderModule module = VK_NULL_HANDLE;
    memset(&info, 0, sizeof info);
    info.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    info.codeSize = size;
    info.pCode = code;
    if (vkCreateShaderModule(device, &info, NULL, &module) != VK_SUCCESS) {
        return VK_NULL_HANDLE;
    }
    return module;
}

void sf_vk_destroy(SfRenderer *r)
{
    if (!r) {
        return;
    }
    if (r->device) {
        vkDeviceWaitIdle(r->device);
    }
    sf_scene_destroy(r->scene);
    r->scene = NULL;
    destroy_swapchain(r);
    if (r->device && r->pipeline) {
        vkDestroyPipeline(r->device, r->pipeline, NULL);
    }
    if (r->device && r->layout) {
        vkDestroyPipelineLayout(r->device, r->layout, NULL);
    }
    if (r->device && r->pass) {
        vkDestroyRenderPass(r->device, r->pass, NULL);
    }
    if (r->device && r->desc_pool) {
        vkDestroyDescriptorPool(r->device, r->desc_pool, NULL);
    }
    if (r->device && r->set_layout) {
        vkDestroyDescriptorSetLayout(r->device, r->set_layout, NULL);
    }
    if (r->device && r->sampler) {
        vkDestroySampler(r->device, r->sampler, NULL);
    }
    if (r->device && r->tex_view) {
        vkDestroyImageView(r->device, r->tex_view, NULL);
    }
    if (r->device && r->tex) {
        vkDestroyImage(r->device, r->tex, NULL);
    }
    if (r->device && r->tex_mem) {
        vkFreeMemory(r->device, r->tex_mem, NULL);
    }
    if (r->device && r->staging) {
        vkDestroyBuffer(r->device, r->staging, NULL);
    }
    if (r->device && r->staging_mem) {
        vkFreeMemory(r->device, r->staging_mem, NULL);
    }
    if (r->device && r->pool) {
        vkDestroyCommandPool(r->device, r->pool, NULL);
    }
    if (r->device && r->fence) {
        vkDestroyFence(r->device, r->fence, NULL);
    }
    if (r->device && r->image_ok) {
        vkDestroySemaphore(r->device, r->image_ok, NULL);
    }
    if (r->device && r->render_ok) {
        vkDestroySemaphore(r->device, r->render_ok, NULL);
    }
    if (r->device) {
        vkDestroyDevice(r->device, NULL);
    }
    if (r->instance && r->surface) {
        SDL_Vulkan_DestroySurface(r->instance, r->surface, NULL);
    }
    if (r->instance) {
        vkDestroyInstance(r->instance, NULL);
    }
    free(r);
}

int sf_vk_create(SfRenderer **out, SDL_Window *window, char *err, size_t err_n)
{
    SfRenderer *r;
    Uint32 ext_count = 0;
    const char *const *exts;
    VkApplicationInfo app;
    VkInstanceCreateInfo instance_info;
    uint32_t dev_count = 0;
    VkPhysicalDevice *devs = NULL;
    uint32_t i;
    int picked = 0;
    uint32_t family = 0;
    float priority = 1.f;
    VkDeviceQueueCreateInfo queue_info;
    VkDeviceCreateInfo device_info;
    VkPhysicalDeviceFeatures features;
    const char *swap_ext = VK_KHR_SWAPCHAIN_EXTENSION_NAME;
    VkAttachmentDescription color;
    VkAttachmentReference ref;
    VkSubpassDescription sub;
    VkSubpassDependency dep;
    VkRenderPassCreateInfo pass_info;
    VkDescriptorSetLayoutBinding bind;
    VkDescriptorSetLayoutCreateInfo set_info;
    VkPipelineLayoutCreateInfo layout_info;
    VkShaderModule vert;
    VkShaderModule frag;
    VkPipelineShaderStageCreateInfo stages[2];
    VkPipelineVertexInputStateCreateInfo vertex;
    VkPipelineInputAssemblyStateCreateInfo assembly;
    VkPipelineViewportStateCreateInfo viewport;
    VkPipelineRasterizationStateCreateInfo raster;
    VkPipelineMultisampleStateCreateInfo multi;
    VkPipelineColorBlendAttachmentState blend;
    VkPipelineColorBlendStateCreateInfo blending;
    VkDynamicState dynamics[2];
    VkPipelineDynamicStateCreateInfo dynamic;
    VkGraphicsPipelineCreateInfo pipe;
    VkCommandPoolCreateInfo pool_info;
    VkCommandBufferAllocateInfo cmd_info;
    VkSemaphoreCreateInfo sem_info;
    VkFenceCreateInfo fence_info;
    VkDescriptorPoolSize pool_size;
    VkDescriptorPoolCreateInfo dpool;
    VkDescriptorSetAllocateInfo dset;

    r = calloc(1, sizeof(*r));
    if (!r) {
        sf_set_err(err, err_n, "out of memory");
        return -1;
    }
    r->kind = SF_RENDER_VULKAN;
    r->aspect = 4.f / 3.f;
    r->window = window;
    exts = SDL_Vulkan_GetInstanceExtensions(&ext_count);
    if (!exts || ext_count == 0) {
        sf_set_err(err, err_n, "Vulkan instance extensions failed: %s", SDL_GetError());
        sf_vk_destroy(r);
        return -1;
    }
    memset(&app, 0, sizeof app);
    app.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    app.pApplicationName = "Street Fighter III: 3rd Strike";
    app.apiVersion = VK_API_VERSION_1_0;
    memset(&instance_info, 0, sizeof instance_info);
    instance_info.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    instance_info.pApplicationInfo = &app;
    instance_info.enabledExtensionCount = ext_count;
    instance_info.ppEnabledExtensionNames = exts;
    if (vkCreateInstance(&instance_info, NULL, &r->instance) != VK_SUCCESS) {
        sf_set_err(err, err_n, "Vulkan instance failed");
        sf_vk_destroy(r);
        return -1;
    }
    if (!SDL_Vulkan_CreateSurface(window, r->instance, NULL, &r->surface)) {
        sf_set_err(err, err_n, "Vulkan surface failed: %s", SDL_GetError());
        sf_vk_destroy(r);
        return -1;
    }
    if (vkEnumeratePhysicalDevices(r->instance, &dev_count, NULL) != VK_SUCCESS || dev_count == 0) {
        sf_set_err(err, err_n, "no Vulkan device");
        sf_vk_destroy(r);
        return -1;
    }
    devs = calloc(dev_count, sizeof(*devs));
    if (!devs) {
        sf_set_err(err, err_n, "out of memory");
        sf_vk_destroy(r);
        return -1;
    }
    vkEnumeratePhysicalDevices(r->instance, &dev_count, devs);
    for (i = 0; i < dev_count; i++) {
        uint32_t qcount = 0;
        VkQueueFamilyProperties *qprops;
        uint32_t q;
        VkPhysicalDeviceProperties props;
        int graphics = -1;
        vkGetPhysicalDeviceQueueFamilyProperties(devs[i], &qcount, NULL);
        qprops = calloc(qcount, sizeof(*qprops));
        if (!qprops) {
            continue;
        }
        vkGetPhysicalDeviceQueueFamilyProperties(devs[i], &qcount, qprops);
        for (q = 0; q < qcount; q++) {
            VkBool32 present = VK_FALSE;
            if ((qprops[q].queueFlags & VK_QUEUE_GRAPHICS_BIT) == 0) {
                continue;
            }
            vkGetPhysicalDeviceSurfaceSupportKHR(devs[i], q, r->surface, &present);
            if (present) {
                graphics = (int)q;
                break;
            }
        }
        free(qprops);
        if (graphics < 0) {
            continue;
        }
        vkGetPhysicalDeviceProperties(devs[i], &props);
        r->physical = devs[i];
        family = (uint32_t)graphics;
        picked = 1;
        if (props.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU) {
            break;
        }
    }
    free(devs);
    if (!picked) {
        sf_set_err(err, err_n, "no Vulkan device can draw to the window");
        sf_vk_destroy(r);
        return -1;
    }
    r->family = family;
    memset(&queue_info, 0, sizeof queue_info);
    queue_info.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    queue_info.queueFamilyIndex = family;
    queue_info.queueCount = 1;
    queue_info.pQueuePriorities = &priority;
    memset(&device_info, 0, sizeof device_info);
    device_info.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    device_info.queueCreateInfoCount = 1;
    device_info.pQueueCreateInfos = &queue_info;
    device_info.enabledExtensionCount = 1;
    device_info.ppEnabledExtensionNames = &swap_ext;
    memset(&features, 0, sizeof features);
    if (sf_scene_supported(r->physical)) {
        sf_scene_features(&features);
    }
    device_info.pEnabledFeatures = &features;
    if (vkCreateDevice(r->physical, &device_info, NULL, &r->device) != VK_SUCCESS) {
        sf_set_err(err, err_n, "Vulkan device failed");
        sf_vk_destroy(r);
        return -1;
    }
    vkGetDeviceQueue(r->device, family, 0, &r->queue);

    memset(&color, 0, sizeof color);
    color.format = VK_FORMAT_B8G8R8A8_UNORM;
    color.samples = VK_SAMPLE_COUNT_1_BIT;
    color.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    color.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    color.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    color.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    color.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    color.finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    memset(&ref, 0, sizeof ref);
    ref.attachment = 0;
    ref.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    memset(&sub, 0, sizeof sub);
    sub.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    sub.colorAttachmentCount = 1;
    sub.pColorAttachments = &ref;
    memset(&dep, 0, sizeof dep);
    dep.srcSubpass = VK_SUBPASS_EXTERNAL;
    dep.dstSubpass = 0;
    dep.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dep.dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dep.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    memset(&pass_info, 0, sizeof pass_info);
    pass_info.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
    pass_info.attachmentCount = 1;
    pass_info.pAttachments = &color;
    pass_info.subpassCount = 1;
    pass_info.pSubpasses = &sub;
    pass_info.dependencyCount = 1;
    pass_info.pDependencies = &dep;
    /* The real format is chosen with the swapchain. Create the pass after the format is known. */
    (void)pass_info;

    memset(&bind, 0, sizeof bind);
    bind.binding = 0;
    bind.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    bind.descriptorCount = 1;
    bind.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    memset(&set_info, 0, sizeof set_info);
    set_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    set_info.bindingCount = 1;
    set_info.pBindings = &bind;
    if (vkCreateDescriptorSetLayout(r->device, &set_info, NULL, &r->set_layout) != VK_SUCCESS) {
        sf_set_err(err, err_n, "Vulkan descriptor layout failed");
        sf_vk_destroy(r);
        return -1;
    }
    memset(&layout_info, 0, sizeof layout_info);
    layout_info.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    layout_info.setLayoutCount = 1;
    layout_info.pSetLayouts = &r->set_layout;
    if (vkCreatePipelineLayout(r->device, &layout_info, NULL, &r->layout) != VK_SUCCESS) {
        sf_set_err(err, err_n, "Vulkan pipeline layout failed");
        sf_vk_destroy(r);
        return -1;
    }

    /* Swapchain first so the render pass matches its format. Layout does not depend on it. */
    if (vkGetPhysicalDeviceSurfaceFormatsKHR(r->physical, r->surface, &i, NULL) != VK_SUCCESS) {
        sf_set_err(err, err_n, "Vulkan formats failed");
        sf_vk_destroy(r);
        return -1;
    }
    {
        VkSurfaceFormatKHR *formats;
        uint32_t format_count = i;
        VkFormat chosen = VK_FORMAT_B8G8R8A8_UNORM;
        formats = calloc(format_count ? format_count : 1, sizeof(*formats));
        if (!formats) {
            sf_set_err(err, err_n, "out of memory");
            sf_vk_destroy(r);
            return -1;
        }
        if (format_count > 0) {
            vkGetPhysicalDeviceSurfaceFormatsKHR(r->physical, r->surface, &format_count, formats);
            if (!(format_count == 1 && formats[0].format == VK_FORMAT_UNDEFINED)) {
                chosen = formats[0].format;
                for (i = 0; i < format_count; i++) {
                    if (formats[i].format == VK_FORMAT_B8G8R8A8_UNORM || formats[i].format == VK_FORMAT_R8G8B8A8_UNORM) {
                        chosen = formats[i].format;
                        break;
                    }
                }
            }
        }
        free(formats);
        color.format = chosen;
    }
    if (vkCreateRenderPass(r->device, &pass_info, NULL, &r->pass) != VK_SUCCESS) {
        sf_set_err(err, err_n, "Vulkan render pass failed");
        sf_vk_destroy(r);
        return -1;
    }
    if (make_swapchain(r, err, err_n) != 0) {
        sf_vk_destroy(r);
        return -1;
    }
    if (r->format != color.format) {
        sf_set_err(err, err_n, "Vulkan swapchain format changed");
        sf_vk_destroy(r);
        return -1;
    }

    vert = make_shader(r->device, sf_ui_vert_spv, sf_ui_vert_spv_size);
    frag = make_shader(r->device, sf_ui_frag_spv, sf_ui_frag_spv_size);
    if (!vert || !frag) {
        if (vert) {
            vkDestroyShaderModule(r->device, vert, NULL);
        }
        if (frag) {
            vkDestroyShaderModule(r->device, frag, NULL);
        }
        sf_set_err(err, err_n, "Vulkan UI shader failed");
        sf_vk_destroy(r);
        return -1;
    }
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
    raster.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    raster.lineWidth = 1.f;
    memset(&multi, 0, sizeof multi);
    multi.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    multi.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    memset(&blend, 0, sizeof blend);
    blend.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    memset(&blending, 0, sizeof blending);
    blending.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    blending.attachmentCount = 1;
    blending.pAttachments = &blend;
    dynamics[0] = VK_DYNAMIC_STATE_VIEWPORT;
    dynamics[1] = VK_DYNAMIC_STATE_SCISSOR;
    memset(&dynamic, 0, sizeof dynamic);
    dynamic.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    dynamic.dynamicStateCount = 2;
    dynamic.pDynamicStates = dynamics;
    memset(&pipe, 0, sizeof pipe);
    pipe.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    pipe.stageCount = 2;
    pipe.pStages = stages;
    pipe.pVertexInputState = &vertex;
    pipe.pInputAssemblyState = &assembly;
    pipe.pViewportState = &viewport;
    pipe.pRasterizationState = &raster;
    pipe.pMultisampleState = &multi;
    pipe.pColorBlendState = &blending;
    pipe.pDynamicState = &dynamic;
    pipe.layout = r->layout;
    pipe.renderPass = r->pass;
    if (vkCreateGraphicsPipelines(r->device, VK_NULL_HANDLE, 1, &pipe, NULL, &r->pipeline) != VK_SUCCESS) {
        vkDestroyShaderModule(r->device, vert, NULL);
        vkDestroyShaderModule(r->device, frag, NULL);
        sf_set_err(err, err_n, "Vulkan pipeline failed");
        sf_vk_destroy(r);
        return -1;
    }
    vkDestroyShaderModule(r->device, vert, NULL);
    vkDestroyShaderModule(r->device, frag, NULL);

    memset(&pool_info, 0, sizeof pool_info);
    pool_info.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    pool_info.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    pool_info.queueFamilyIndex = r->family;
    if (vkCreateCommandPool(r->device, &pool_info, NULL, &r->pool) != VK_SUCCESS) {
        sf_set_err(err, err_n, "Vulkan command pool failed");
        sf_vk_destroy(r);
        return -1;
    }
    memset(&cmd_info, 0, sizeof cmd_info);
    cmd_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    cmd_info.commandPool = r->pool;
    cmd_info.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cmd_info.commandBufferCount = 1;
    if (vkAllocateCommandBuffers(r->device, &cmd_info, &r->cmd) != VK_SUCCESS) {
        sf_set_err(err, err_n, "Vulkan command buffer failed");
        sf_vk_destroy(r);
        return -1;
    }
    memset(&sem_info, 0, sizeof sem_info);
    sem_info.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
    memset(&fence_info, 0, sizeof fence_info);
    fence_info.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    fence_info.flags = VK_FENCE_CREATE_SIGNALED_BIT;
    if (vkCreateSemaphore(r->device, &sem_info, NULL, &r->image_ok) != VK_SUCCESS ||
        vkCreateSemaphore(r->device, &sem_info, NULL, &r->render_ok) != VK_SUCCESS ||
        vkCreateFence(r->device, &fence_info, NULL, &r->fence) != VK_SUCCESS) {
        sf_set_err(err, err_n, "Vulkan sync failed");
        sf_vk_destroy(r);
        return -1;
    }
    memset(&pool_size, 0, sizeof pool_size);
    pool_size.type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    pool_size.descriptorCount = 1;
    memset(&dpool, 0, sizeof dpool);
    dpool.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    dpool.maxSets = 1;
    dpool.poolSizeCount = 1;
    dpool.pPoolSizes = &pool_size;
    if (vkCreateDescriptorPool(r->device, &dpool, NULL, &r->desc_pool) != VK_SUCCESS) {
        sf_set_err(err, err_n, "Vulkan descriptor pool failed");
        sf_vk_destroy(r);
        return -1;
    }
    memset(&dset, 0, sizeof dset);
    dset.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    dset.descriptorPool = r->desc_pool;
    dset.descriptorSetCount = 1;
    dset.pSetLayouts = &r->set_layout;
    if (vkAllocateDescriptorSets(r->device, &dset, &r->set) != VK_SUCCESS) {
        sf_set_err(err, err_n, "Vulkan descriptor set failed");
        sf_vk_destroy(r);
        return -1;
    }
    {
        VkSamplerCreateInfo sampler;
        memset(&sampler, 0, sizeof sampler);
        sampler.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
        sampler.magFilter = VK_FILTER_NEAREST;
        sampler.minFilter = VK_FILTER_NEAREST;
        sampler.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        sampler.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        sampler.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        if (vkCreateSampler(r->device, &sampler, NULL, &r->sampler) != VK_SUCCESS) {
            sf_set_err(err, err_n, "Vulkan sampler failed");
            sf_vk_destroy(r);
            return -1;
        }
    }
    *out = r;
    return 0;
}

static int ensure_texture(SfRenderer *r, const SfCanvas *canvas, char *err, size_t err_n)
{
    VkDeviceSize bytes;
    VkBufferCreateInfo buf_info;
    VkMemoryRequirements req;
    VkMemoryAllocateInfo alloc;
    uint32_t index;
    VkImageCreateInfo image;
    VkImageViewCreateInfo view;
    void *mapped = NULL;
    if (r->tex && r->tex_w == canvas->width && r->tex_h == canvas->height && r->staging_cap >= (VkDeviceSize)canvas->width * canvas->height * 4) {
        if (vkMapMemory(r->device, r->staging_mem, 0, (VkDeviceSize)canvas->width * canvas->height * 4, 0, &mapped) != VK_SUCCESS) {
            sf_set_err(err, err_n, "Vulkan map failed");
            return -1;
        }
        memcpy(mapped, canvas->rgba, (size_t)canvas->width * (size_t)canvas->height * 4);
        vkUnmapMemory(r->device, r->staging_mem);
        return 0;
    }
    vkDeviceWaitIdle(r->device);
    if (r->tex_view) {
        vkDestroyImageView(r->device, r->tex_view, NULL);
        r->tex_view = VK_NULL_HANDLE;
    }
    if (r->tex) {
        vkDestroyImage(r->device, r->tex, NULL);
        r->tex = VK_NULL_HANDLE;
    }
    if (r->tex_mem) {
        vkFreeMemory(r->device, r->tex_mem, NULL);
        r->tex_mem = VK_NULL_HANDLE;
    }
    if (r->staging) {
        vkDestroyBuffer(r->device, r->staging, NULL);
        r->staging = VK_NULL_HANDLE;
    }
    if (r->staging_mem) {
        vkFreeMemory(r->device, r->staging_mem, NULL);
        r->staging_mem = VK_NULL_HANDLE;
    }
    bytes = (VkDeviceSize)canvas->width * (VkDeviceSize)canvas->height * 4;
    memset(&buf_info, 0, sizeof buf_info);
    buf_info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    buf_info.size = bytes;
    buf_info.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    if (vkCreateBuffer(r->device, &buf_info, NULL, &r->staging) != VK_SUCCESS) {
        sf_set_err(err, err_n, "Vulkan staging buffer failed");
        return -1;
    }
    vkGetBufferMemoryRequirements(r->device, r->staging, &req);
    index = mem_index(r->physical, req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    if (index == UINT32_MAX) {
        sf_set_err(err, err_n, "Vulkan has no staging memory");
        return -1;
    }
    memset(&alloc, 0, sizeof alloc);
    alloc.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    alloc.allocationSize = req.size;
    alloc.memoryTypeIndex = index;
    if (vkAllocateMemory(r->device, &alloc, NULL, &r->staging_mem) != VK_SUCCESS || vkBindBufferMemory(r->device, r->staging, r->staging_mem, 0) != VK_SUCCESS) {
        sf_set_err(err, err_n, "Vulkan staging memory failed");
        return -1;
    }
    r->staging_cap = bytes;
    memset(&image, 0, sizeof image);
    image.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    image.imageType = VK_IMAGE_TYPE_2D;
    image.format = VK_FORMAT_R8G8B8A8_UNORM;
    image.extent.width = (uint32_t)canvas->width;
    image.extent.height = (uint32_t)canvas->height;
    image.extent.depth = 1;
    image.mipLevels = 1;
    image.arrayLayers = 1;
    image.samples = VK_SAMPLE_COUNT_1_BIT;
    image.tiling = VK_IMAGE_TILING_OPTIMAL;
    image.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    image.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (vkCreateImage(r->device, &image, NULL, &r->tex) != VK_SUCCESS) {
        sf_set_err(err, err_n, "Vulkan image failed");
        return -1;
    }
    vkGetImageMemoryRequirements(r->device, r->tex, &req);
    index = mem_index(r->physical, req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (index == UINT32_MAX) {
        index = mem_index(r->physical, req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT);
    }
    if (index == UINT32_MAX) {
        sf_set_err(err, err_n, "Vulkan has no image memory");
        return -1;
    }
    alloc.allocationSize = req.size;
    alloc.memoryTypeIndex = index;
    if (vkAllocateMemory(r->device, &alloc, NULL, &r->tex_mem) != VK_SUCCESS || vkBindImageMemory(r->device, r->tex, r->tex_mem, 0) != VK_SUCCESS) {
        sf_set_err(err, err_n, "Vulkan image memory failed");
        return -1;
    }
    memset(&view, 0, sizeof view);
    view.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    view.image = r->tex;
    view.viewType = VK_IMAGE_VIEW_TYPE_2D;
    view.format = VK_FORMAT_R8G8B8A8_UNORM;
    view.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    view.subresourceRange.levelCount = 1;
    view.subresourceRange.layerCount = 1;
    if (vkCreateImageView(r->device, &view, NULL, &r->tex_view) != VK_SUCCESS) {
        sf_set_err(err, err_n, "Vulkan texture view failed");
        return -1;
    }
    {
        VkDescriptorImageInfo image_info;
        VkWriteDescriptorSet write;
        memset(&image_info, 0, sizeof image_info);
        image_info.sampler = r->sampler;
        image_info.imageView = r->tex_view;
        image_info.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        memset(&write, 0, sizeof write);
        write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        write.dstSet = r->set;
        write.dstBinding = 0;
        write.descriptorCount = 1;
        write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        write.pImageInfo = &image_info;
        vkUpdateDescriptorSets(r->device, 1, &write, 0, NULL);
    }
    r->tex_w = canvas->width;
    r->tex_h = canvas->height;
    if (vkMapMemory(r->device, r->staging_mem, 0, bytes, 0, &mapped) != VK_SUCCESS) {
        sf_set_err(err, err_n, "Vulkan map failed");
        return -1;
    }
    memcpy(mapped, canvas->rgba, (size_t)bytes);
    vkUnmapMemory(r->device, r->staging_mem);
    return 0;
}

static int record(SfRenderer *r, uint32_t image_index, const SfCanvas *canvas)
{
    VkCommandBufferBeginInfo begin;
    VkImageMemoryBarrier barrier;
    VkBufferImageCopy copy;
    VkRenderPassBeginInfo rp;
    VkClearValue clear;
    VkViewport viewport;
    VkRect2D scissor;
    VkDeviceSize bytes = (VkDeviceSize)canvas->width * (VkDeviceSize)canvas->height * 4;
    memset(&begin, 0, sizeof begin);
    begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    if (vkBeginCommandBuffer(r->cmd, &begin) != VK_SUCCESS) {
        return -1;
    }
    memset(&barrier, 0, sizeof barrier);
    barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = r->tex;
    barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    barrier.subresourceRange.levelCount = 1;
    barrier.subresourceRange.layerCount = 1;
    barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    vkCmdPipelineBarrier(r->cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1, &barrier);
    memset(&copy, 0, sizeof copy);
    copy.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    copy.imageSubresource.layerCount = 1;
    copy.imageExtent.width = (uint32_t)canvas->width;
    copy.imageExtent.height = (uint32_t)canvas->height;
    copy.imageExtent.depth = 1;
    vkCmdCopyBufferToImage(r->cmd, r->staging, r->tex, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
    (void)bytes;
    barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    vkCmdPipelineBarrier(r->cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, NULL, 0, NULL, 1, &barrier);
    memset(&clear, 0, sizeof clear);
    clear.color.float32[3] = 1.f;
    memset(&rp, 0, sizeof rp);
    rp.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    rp.renderPass = r->pass;
    rp.framebuffer = r->frames[image_index];
    rp.renderArea.extent = r->extent;
    rp.clearValueCount = 1;
    rp.pClearValues = &clear;
    vkCmdBeginRenderPass(r->cmd, &rp, VK_SUBPASS_CONTENTS_INLINE);
    vkCmdBindPipeline(r->cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, r->pipeline);
    viewport.x = 0.f;
    viewport.y = 0.f;
    viewport.width = (float)r->extent.width;
    viewport.height = (float)r->extent.height;
    viewport.minDepth = 0.f;
    viewport.maxDepth = 1.f;
    scissor.offset.x = 0;
    scissor.offset.y = 0;
    scissor.extent = r->extent;
    vkCmdSetViewport(r->cmd, 0, 1, &viewport);
    vkCmdSetScissor(r->cmd, 0, 1, &scissor);
    vkCmdBindDescriptorSets(r->cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, r->layout, 0, 1, &r->set, 0, NULL);
    vkCmdDraw(r->cmd, 3, 1, 0, 0);
    vkCmdEndRenderPass(r->cmd);
    return vkEndCommandBuffer(r->cmd) == VK_SUCCESS ? 0 : -1;
}

int sf_vk_present(SfRenderer *r, const SfCanvas *canvas, char *err, size_t err_n)
{
    uint32_t image_index = 0;
    VkResult ar;
    VkPipelineStageFlags wait_stage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    VkSubmitInfo submit;
    VkPresentInfoKHR present;
    if (!r || !canvas || !canvas->rgba) {
        sf_set_err(err, err_n, "missing frame");
        return -1;
    }
    if (vkWaitForFences(r->device, 1, &r->fence, VK_TRUE, UINT64_MAX) != VK_SUCCESS) {
        sf_set_err(err, err_n, "Vulkan fence wait failed");
        return -1;
    }
    ar = vkAcquireNextImageKHR(r->device, r->swapchain, UINT64_MAX, r->image_ok, VK_NULL_HANDLE, &image_index);
    if (ar == VK_ERROR_OUT_OF_DATE_KHR) {
        vkDeviceWaitIdle(r->device);
        destroy_swapchain(r);
        if (make_swapchain(r, err, err_n) != 0) {
            return -1;
        }
        return 0;
    }
    if (ar != VK_SUCCESS && ar != VK_SUBOPTIMAL_KHR) {
        sf_set_err(err, err_n, "Vulkan acquire failed");
        return -1;
    }
    if (ensure_texture(r, canvas, err, err_n) != 0) {
        return -1;
    }
    vkResetFences(r->device, 1, &r->fence);
    vkResetCommandBuffer(r->cmd, 0);
    if (record(r, image_index, canvas) != 0) {
        sf_set_err(err, err_n, "Vulkan record failed");
        return -1;
    }
    memset(&submit, 0, sizeof submit);
    submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submit.waitSemaphoreCount = 1;
    submit.pWaitSemaphores = &r->image_ok;
    submit.pWaitDstStageMask = &wait_stage;
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &r->cmd;
    submit.signalSemaphoreCount = 1;
    submit.pSignalSemaphores = &r->render_ok;
    if (vkQueueSubmit(r->queue, 1, &submit, r->fence) != VK_SUCCESS) {
        sf_set_err(err, err_n, "Vulkan submit failed");
        return -1;
    }
    memset(&present, 0, sizeof present);
    present.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
    present.waitSemaphoreCount = 1;
    present.pWaitSemaphores = &r->render_ok;
    present.swapchainCount = 1;
    present.pSwapchains = &r->swapchain;
    present.pImageIndices = &image_index;
    ar = vkQueuePresentKHR(r->queue, &present);
    if (ar == VK_ERROR_OUT_OF_DATE_KHR || ar == VK_SUBOPTIMAL_KHR) {
        vkDeviceWaitIdle(r->device);
        destroy_swapchain(r);
        if (make_swapchain(r, err, err_n) != 0) {
            return -1;
        }
        return 0;
    }
    if (ar != VK_SUCCESS) {
        sf_set_err(err, err_n, "Vulkan present failed");
        return -1;
    }
    return 0;
}

/* The scale the game's screen renders at: SF_SCALE when it is set, or the
 * largest whole multiple of 640x480 the window holds, up to 8. The window
 * shows the result at 4:3 either way. */
static int render_scale(const SfRenderer *r)
{
    const char *env = getenv("SF_SCALE");
    int k;
    if (env && atoi(env) > 0) {
        k = atoi(env);
    } else {
        int kw = (int)r->extent.width / 640;
        int kh = (int)r->extent.height / 480;
        k = kw < kh ? kw : kh;
    }
    if (k < 1) {
        k = 1;
    }
    return k > 8 ? 8 : k;
}

/* The game's screen: the scene draws the list offscreen, then shows it in
 * the window's pass. The scene is made on the first call. */
static int record_list(SfRenderer *r, uint32_t image_index, const SfDrawList *list)
{
    VkCommandBufferBeginInfo begin;
    VkRenderPassBeginInfo rp;
    VkClearValue clear;
    memset(&begin, 0, sizeof begin);
    begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    if (vkBeginCommandBuffer(r->cmd, &begin) != VK_SUCCESS) {
        return -1;
    }
    /* SF_SHOT_SCREEN: 1 reads the PS2-sized picture, 2 the layer drawn over it */
    const char *which = getenv("SF_SHOT_SCREEN");
    if (r->shot_pending && which && which[0] == '1') {
        sf_scene_read_layer1(r->scene);
    }
    sf_scene_record(r->scene, r->cmd, list);
    if (r->shot_pending && !(which && which[0] == '1')) {
        sf_scene_record_readback(r->scene, r->cmd, which && which[0] == '2' ? 0 : 1);
    }
    memset(&clear, 0, sizeof clear);
    clear.color.float32[3] = 1.f;
    memset(&rp, 0, sizeof rp);
    rp.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    rp.renderPass = r->pass;
    rp.framebuffer = r->frames[image_index];
    rp.renderArea.extent = r->extent;
    rp.clearValueCount = 1;
    rp.pClearValues = &clear;
    vkCmdBeginRenderPass(r->cmd, &rp, VK_SUBPASS_CONTENTS_INLINE);
    sf_scene_record_show(r->scene, r->cmd, r->extent, r->aspect, list, r->filter);
    vkCmdEndRenderPass(r->cmd);
    return vkEndCommandBuffer(r->cmd) == VK_SUCCESS ? 0 : -1;
}

int sf_vk_present_list(SfRenderer *r, const SfDrawList *list, char *err, size_t err_n)
{
    uint32_t image_index = 0;
    VkResult ar;
    VkPipelineStageFlags wait_stage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    VkSubmitInfo submit;
    VkPresentInfoKHR present;
    if (!r->scene) {
        if (!sf_scene_supported(r->physical)) {
            sf_set_err(err, err_n, "this Vulkan device cannot index 256 textures in a shader");
            return -1;
        }
        if (sf_scene_create(&r->scene, r->physical, r->device, r->pass, err, err_n) != 0) {
            return -1;
        }
    }
    if (vkWaitForFences(r->device, 1, &r->fence, VK_TRUE, UINT64_MAX) != VK_SUCCESS) {
        sf_set_err(err, err_n, "Vulkan fence wait failed");
        return -1;
    }
    if (render_scale(r) != sf_scene_scale(r->scene) && r->swapchain) {
        fprintf(stderr, "video: window %ux%u, the game drawn at %dx%d\n", r->extent.width, r->extent.height,
                640 * render_scale(r), 480 * render_scale(r));
    }
    if (sf_scene_set_scale(r->scene, render_scale(r), err, err_n) != 0) {
        return -1;
    }
    if (sf_scene_prepare(r->scene, list, err, err_n) != 0) {
        return -1;
    }
    if (!r->swapchain) {
        /* headless: the screen is drawn, read back when asked, not shown */
        VkCommandBufferBeginInfo begin;
        vkResetFences(r->device, 1, &r->fence);
        vkResetCommandBuffer(r->cmd, 0);
        memset(&begin, 0, sizeof begin);
        begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        vkBeginCommandBuffer(r->cmd, &begin);
        sf_scene_record(r->scene, r->cmd, list);
        if (r->shot_pending) {
            sf_scene_record_readback(r->scene, r->cmd, 1);
        }
        vkEndCommandBuffer(r->cmd);
        memset(&submit, 0, sizeof submit);
        submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        submit.commandBufferCount = 1;
        submit.pCommandBuffers = &r->cmd;
        if (vkQueueSubmit(r->queue, 1, &submit, r->fence) != VK_SUCCESS) {
            sf_set_err(err, err_n, "Vulkan submit failed");
            return -1;
        }
        return 0;
    }
    /* Wayland does not report a swapchain out of date when the window
     * changes size (fullscreen, a new resolution, a resize): the old images
     * would be stretched over the new size. The size is compared here. */
    {
        int w = 0;
        int h = 0;
        SDL_GetWindowSizeInPixels(r->window, &w, &h);
        if (w > 0 && h > 0 && (w != r->sized_w || h != r->sized_h)) {
            r->sized_w = w;
            r->sized_h = h;
            if ((uint32_t)w != r->extent.width || (uint32_t)h != r->extent.height) {
                vkDeviceWaitIdle(r->device);
                destroy_swapchain(r);
                if (make_swapchain(r, err, err_n) != 0) {
                    return -1;
                }
            }
        }
    }
    ar = vkAcquireNextImageKHR(r->device, r->swapchain, UINT64_MAX, r->image_ok, VK_NULL_HANDLE, &image_index);
    if (ar == VK_ERROR_OUT_OF_DATE_KHR) {
        vkDeviceWaitIdle(r->device);
        destroy_swapchain(r);
        return make_swapchain(r, err, err_n);
    }
    if (ar != VK_SUCCESS && ar != VK_SUBOPTIMAL_KHR) {
        sf_set_err(err, err_n, "Vulkan acquire failed");
        return -1;
    }
    vkResetFences(r->device, 1, &r->fence);
    vkResetCommandBuffer(r->cmd, 0);
    if (record_list(r, image_index, list) != 0) {
        sf_set_err(err, err_n, "Vulkan record failed");
        return -1;
    }
    memset(&submit, 0, sizeof submit);
    submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submit.waitSemaphoreCount = 1;
    submit.pWaitSemaphores = &r->image_ok;
    submit.pWaitDstStageMask = &wait_stage;
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &r->cmd;
    submit.signalSemaphoreCount = 1;
    submit.pSignalSemaphores = &r->render_ok;
    if (vkQueueSubmit(r->queue, 1, &submit, r->fence) != VK_SUCCESS) {
        sf_set_err(err, err_n, "Vulkan submit failed");
        return -1;
    }
    memset(&present, 0, sizeof present);
    present.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
    present.waitSemaphoreCount = 1;
    present.pWaitSemaphores = &r->render_ok;
    present.swapchainCount = 1;
    present.pSwapchains = &r->swapchain;
    present.pImageIndices = &image_index;
    ar = vkQueuePresentKHR(r->queue, &present);
    if (ar == VK_ERROR_OUT_OF_DATE_KHR || ar == VK_SUBOPTIMAL_KHR) {
        vkDeviceWaitIdle(r->device);
        destroy_swapchain(r);
        return make_swapchain(r, err, err_n);
    }
    if (ar != VK_SUCCESS) {
        sf_set_err(err, err_n, "Vulkan present failed");
        return -1;
    }
    return 0;
}

int sf_vk_read_screen(SfRenderer *r, const uint8_t **rgba, int *w, int *h)
{
    if (!r->scene) {
        return -1;
    }
    if (!r->shot_pending) {
        r->shot_pending = 1;
        return 1;
    }
    vkWaitForFences(r->device, 1, &r->fence, VK_TRUE, UINT64_MAX);
    *rgba = sf_scene_read(r->scene, w, h);
    r->shot_pending = 0;
    return *rgba ? 0 : -1;
}

/* A renderer with no window (tests): a device that can draw the game's
 * screen, its queue, one command buffer and a fence. */
int sf_vk_create_headless(SfRenderer **out, char *err, size_t err_n)
{
    SfRenderer *r = calloc(1, sizeof *r);
    VkApplicationInfo app;
    VkInstanceCreateInfo ii;
    uint32_t n = 0;
    VkPhysicalDevice devs[16];
    uint32_t i;
    float prio = 1.f;
    VkDeviceQueueCreateInfo qi;
    VkDeviceCreateInfo di;
    VkPhysicalDeviceFeatures features;
    VkCommandPoolCreateInfo pi;
    VkCommandBufferAllocateInfo ci;
    VkFenceCreateInfo fi;
    *out = NULL;
    if (!r) {
        sf_set_err(err, err_n, "out of memory");
        return -1;
    }
    r->kind = SF_RENDER_VULKAN;
    r->aspect = 4.f / 3.f;
    memset(&app, 0, sizeof app);
    app.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    app.apiVersion = VK_API_VERSION_1_0;
    memset(&ii, 0, sizeof ii);
    ii.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    ii.pApplicationInfo = &app;
    if (vkCreateInstance(&ii, NULL, &r->instance) != VK_SUCCESS) {
        sf_set_err(err, err_n, "Vulkan instance failed");
        sf_vk_destroy(r);
        return -1;
    }
    vkEnumeratePhysicalDevices(r->instance, &n, NULL);
    if (n > 16) {
        n = 16;
    }
    vkEnumeratePhysicalDevices(r->instance, &n, devs);
    for (i = 0; i < n && !r->physical; i++) {
        uint32_t qn = 0;
        VkQueueFamilyProperties q[16];
        uint32_t k;
        if (!sf_scene_supported(devs[i])) {
            continue;
        }
        vkGetPhysicalDeviceQueueFamilyProperties(devs[i], &qn, NULL);
        if (qn > 16) {
            qn = 16;
        }
        vkGetPhysicalDeviceQueueFamilyProperties(devs[i], &qn, q);
        for (k = 0; k < qn; k++) {
            if (q[k].queueFlags & VK_QUEUE_GRAPHICS_BIT) {
                r->physical = devs[i];
                r->family = k;
                break;
            }
        }
    }
    if (!r->physical) {
        sf_set_err(err, err_n, "no Vulkan device can draw the game's screen");
        sf_vk_destroy(r);
        return -1;
    }
    memset(&qi, 0, sizeof qi);
    qi.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    qi.queueFamilyIndex = r->family;
    qi.queueCount = 1;
    qi.pQueuePriorities = &prio;
    memset(&features, 0, sizeof features);
    sf_scene_features(&features);
    memset(&di, 0, sizeof di);
    di.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    di.queueCreateInfoCount = 1;
    di.pQueueCreateInfos = &qi;
    di.pEnabledFeatures = &features;
    if (vkCreateDevice(r->physical, &di, NULL, &r->device) != VK_SUCCESS) {
        sf_set_err(err, err_n, "Vulkan device failed");
        sf_vk_destroy(r);
        return -1;
    }
    vkGetDeviceQueue(r->device, r->family, 0, &r->queue);
    memset(&pi, 0, sizeof pi);
    pi.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    pi.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    pi.queueFamilyIndex = r->family;
    memset(&ci, 0, sizeof ci);
    ci.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    ci.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    ci.commandBufferCount = 1;
    memset(&fi, 0, sizeof fi);
    fi.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    fi.flags = VK_FENCE_CREATE_SIGNALED_BIT;
    if (vkCreateCommandPool(r->device, &pi, NULL, &r->pool) != VK_SUCCESS ||
        (ci.commandPool = r->pool, vkAllocateCommandBuffers(r->device, &ci, &r->cmd)) != VK_SUCCESS ||
        vkCreateFence(r->device, &fi, NULL, &r->fence) != VK_SUCCESS) {
        sf_set_err(err, err_n, "Vulkan command setup failed");
        sf_vk_destroy(r);
        return -1;
    }
    *out = r;
    return 0;
}
