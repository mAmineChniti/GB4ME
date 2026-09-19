// Minimal Vulkan renderer: uploads the 160x144 GB framebuffer to a texture
// and draws a fullscreen quad. One pipeline, one texture update per frame.
// https://wiki.libsdl.org/SDL3/SDL_Vulkan_CreateSurface

#include "gb/vulkan_renderer.h"

#include "gb/font.h"
#include "gb/gui_console.h"
#include <SDL3/SDL_vulkan.h>
#include <algorithm>
#include <cmath>
#include <string>
#include <array>
#include <cstring>
#include <fstream>
#include <limits>
#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"

namespace gb {

namespace {

constexpr u32 kTexWidth = SCREEN_WIDTH;
constexpr u32 kTexHeight = SCREEN_HEIGHT;
constexpr VkFormat kTexFormat = VK_FORMAT_B8G8R8A8_SRGB;

u32 find_memory_type(VkPhysicalDevice phys, u32 filter, VkMemoryPropertyFlags props) {
    VkPhysicalDeviceMemoryProperties mem{};
    vkGetPhysicalDeviceMemoryProperties(phys, &mem);
    for (u32 i = 0; i < mem.memoryTypeCount; i++) {
        if ((filter & (1u << i)) && (mem.memoryTypes[i].propertyFlags & props) == props) {
            return i;
        }
    }
    return std::numeric_limits<u32>::max();
}

bool create_buffer(VkPhysicalDevice phys, VkDevice dev, VkDeviceSize size,
                   VkBufferUsageFlags usage, VkMemoryPropertyFlags props,
                   VkBuffer& buf, VkDeviceMemory& mem) {
    VkBufferCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    info.size = size;
    info.usage = usage;
    info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (vkCreateBuffer(dev, &info, nullptr, &buf) != VK_SUCCESS) return false;
    VkMemoryRequirements req{};
    vkGetBufferMemoryRequirements(dev, buf, &req);
    const u32 idx = find_memory_type(phys, req.memoryTypeBits, props);
    if (idx == std::numeric_limits<u32>::max()) return false;
    VkMemoryAllocateInfo alloc{};
    alloc.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    alloc.allocationSize = req.size;
    alloc.memoryTypeIndex = idx;
    if (vkAllocateMemory(dev, &alloc, nullptr, &mem) != VK_SUCCESS) return false;
    vkBindBufferMemory(dev, buf, mem, 0);
    return true;
}

VkCommandBuffer begin_single_time(VkDevice dev, VkCommandPool pool) {
    VkCommandBufferAllocateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    info.commandPool = pool;
    info.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    info.commandBufferCount = 1;
    VkCommandBuffer cmd{};
    vkAllocateCommandBuffers(dev, &info, &cmd);
    VkCommandBufferBeginInfo begin{};
    begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cmd, &begin);
    return cmd;
}

void end_single_time(VkDevice dev, VkCommandPool pool, VkQueue queue, VkCommandBuffer cmd) {
    vkEndCommandBuffer(cmd);
    VkSubmitInfo submit{};
    submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &cmd;
    vkQueueSubmit(queue, 1, &submit, VK_NULL_HANDLE);
    vkQueueWaitIdle(queue);
    vkFreeCommandBuffers(dev, pool, 1, &cmd);
}

}  // namespace

VulkanRenderer::VulkanRenderer() = default;

VulkanRenderer::~VulkanRenderer() {
    shutdown();
}

bool VulkanRenderer::initialize(SDL_Window* win) {
    window = win;
    if (!create_instance()) return false;
    if (!create_surface()) return false;
    if (!pick_physical_device()) return false;
    if (!create_logical_device()) return false;
    if (!create_swapchain()) return false;
    if (!create_image_views()) return false;
    if (!create_render_pass()) return false;
    if (!create_descriptor_set_layout()) return false;
    if (!create_graphics_pipeline()) return false;
    if (!create_framebuffers()) return false;
    if (!create_command_pool()) return false;
    if (!create_texture()) return false;
    // Photo shell is best-effort: without it the game still runs on the
    // clear color with overlay UI.
    body_photo_ok_ = create_body_texture();
    if (!body_photo_ok_) SDL_Log("Body photo missing; running without shell texture");
    if (!create_vertex_buffer()) return false;
    if (!create_index_buffer()) return false;
    if (!create_uniform_buffers()) return false;
    if (!create_gui_buffers()) return false;
    if (!create_gui_pipeline()) return false;
    if (!create_descriptor_pool()) return false;
    if (!create_descriptor_sets()) return false;
    if (!create_body_photo_descriptors()) return false;
    if (!create_command_buffers()) return false;
    if (!create_sync_objects()) return false;
    return true;
}

void VulkanRenderer::shutdown() {
    if (device == VK_NULL_HANDLE) return;
    wait_idle();
    for (size_t i = 0; i < image_available_semaphores.size(); i++) {
        vkDestroySemaphore(device, image_available_semaphores[i], nullptr);
        vkDestroySemaphore(device, render_finished_semaphores[i], nullptr);
        vkDestroyFence(device, in_flight_fences[i], nullptr);
    }
    image_available_semaphores.clear();
    render_finished_semaphores.clear();
    in_flight_fences.clear();
    vkDestroyCommandPool(device, command_pool, nullptr);
    command_pool = VK_NULL_HANDLE;
    for (auto fb : swapchain_framebuffers) vkDestroyFramebuffer(device, fb, nullptr);
    swapchain_framebuffers.clear();
    vkDestroyPipeline(device, gui_pipeline, nullptr);
    vkDestroyPipelineLayout(device, gui_pipeline_layout, nullptr);
    vkDestroyPipeline(device, graphics_pipeline, nullptr);
    vkDestroyPipelineLayout(device, pipeline_layout, nullptr);
    vkDestroyRenderPass(device, render_pass, nullptr);
    for (auto view : swapchain_image_views) vkDestroyImageView(device, view, nullptr);
    swapchain_image_views.clear();
    vkDestroySwapchainKHR(device, swapchain, nullptr);
    vkDestroySampler(device, texture_sampler, nullptr);
    vkDestroyImageView(device, texture_image_view, nullptr);
    vkDestroyImage(device, texture_image, nullptr);
    vkFreeMemory(device, texture_image_memory, nullptr);
    vkDestroyBuffer(device, texture_buffer, nullptr);
    vkFreeMemory(device, texture_buffer_memory, nullptr);
    // Body texture resources (pool/sets/layout stay VK_NULL_HANDLE unless the
    // textured-body path is enabled; destroying null handles is a no-op).
    vkDestroySampler(device, body_sampler, nullptr);
    vkDestroyImageView(device, body_image_view, nullptr);
    vkDestroyImage(device, body_image, nullptr);
    vkFreeMemory(device, body_image_memory, nullptr);
    vkDestroyDescriptorPool(device, body_descriptor_pool, nullptr);
    vkDestroyDescriptorSetLayout(device, body_descriptor_set_layout, nullptr);
    vkDestroyBuffer(device, gui_vertex_buffer, nullptr);
    vkFreeMemory(device, gui_vertex_buffer_memory, nullptr);
    vkDestroyBuffer(device, gui_index_buffer, nullptr);
    vkFreeMemory(device, gui_index_buffer_memory, nullptr);
    vkDestroyBuffer(device, vertex_buffer, nullptr);
    vkFreeMemory(device, vertex_buffer_memory, nullptr);
    vkDestroyBuffer(device, index_buffer, nullptr);
    vkFreeMemory(device, index_buffer_memory, nullptr);
    for (size_t i = 0; i < uniform_buffers.size(); i++) {
        vkDestroyBuffer(device, uniform_buffers[i], nullptr);
        vkFreeMemory(device, uniform_buffers_memory[i], nullptr);
    }
    uniform_buffers.clear();
    uniform_buffers_memory.clear();
    uniform_buffers_mapped.clear();
    vkDestroyDescriptorPool(device, descriptor_pool, nullptr);
    vkDestroyDescriptorSetLayout(device, descriptor_set_layout, nullptr);
    vkDestroyDevice(device, nullptr);
    device = VK_NULL_HANDLE;
    vkDestroySurfaceKHR(instance, surface, nullptr);
    vkDestroyInstance(instance, nullptr);
    instance = VK_NULL_HANDLE;
}

void VulkanRenderer::wait_idle() {
    if (device != VK_NULL_HANDLE) vkDeviceWaitIdle(device);
}

void VulkanRenderer::resize(u32 /*width*/, u32 /*height*/) {
    recreate_swapchain();
}

bool VulkanRenderer::create_instance() {
    VkApplicationInfo app{};
    app.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    app.pApplicationName = "GB4ME";
    app.applicationVersion = VK_MAKE_VERSION(1, 0, 0);
    app.apiVersion = VK_API_VERSION_1_0;

    u32 ext_count = 0;
    const char* const* exts = SDL_Vulkan_GetInstanceExtensions(&ext_count);
    if (!exts) return false;

    VkInstanceCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    info.pApplicationInfo = &app;
    info.enabledExtensionCount = ext_count;
    info.ppEnabledExtensionNames = exts;
    return vkCreateInstance(&info, nullptr, &instance) == VK_SUCCESS;
}

bool VulkanRenderer::create_surface() {
    return SDL_Vulkan_CreateSurface(window, instance, nullptr, &surface);
}

bool VulkanRenderer::pick_physical_device() {
    u32 count = 0;
    vkEnumeratePhysicalDevices(instance, &count, nullptr);
    if (count == 0) return false;
    std::vector<VkPhysicalDevice> devs(count);
    vkEnumeratePhysicalDevices(instance, &count, devs.data());
    for (auto d : devs) {
        if (find_queue_families(d, surface).is_complete()) {
            physical_device = d;
            return true;
        }
    }
    return false;
}

bool VulkanRenderer::create_logical_device() {
    const QueueFamilyIndices idx = find_queue_families(physical_device, surface);
    std::vector<VkDeviceQueueCreateInfo> queues;
    const float prio = 1.0f;
    u32 families[2] = {idx.graphics_family.value(), idx.present_family.value()};
    for (u32 f : families) {
        bool dup = false;
        for (const auto& q : queues) {
            if (q.queueFamilyIndex == f) dup = true;
        }
        if (dup) continue;
        VkDeviceQueueCreateInfo q{};
        q.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
        q.queueFamilyIndex = f;
        q.queueCount = 1;
        q.pQueuePriorities = &prio;
        queues.push_back(q);
    }
    VkPhysicalDeviceFeatures feats{};
    VkDeviceCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    info.queueCreateInfoCount = static_cast<u32>(queues.size());
    info.pQueueCreateInfos = queues.data();
    info.pEnabledFeatures = &feats;
    const char* swap_ext = VK_KHR_SWAPCHAIN_EXTENSION_NAME;
    info.enabledExtensionCount = 1;
    info.ppEnabledExtensionNames = &swap_ext;
    if (vkCreateDevice(physical_device, &info, nullptr, &device) != VK_SUCCESS) {
        return false;
    }
    vkGetDeviceQueue(device, idx.graphics_family.value(), 0, &graphics_queue);
    vkGetDeviceQueue(device, idx.present_family.value(), 0, &present_queue);
    return true;
}

bool VulkanRenderer::create_swapchain() {
    const SwapChainSupportDetails sup = query_swapchain_support(physical_device, surface);
    const VkSurfaceFormatKHR fmt = choose_swap_surface_format(sup.formats);
    const VkPresentModeKHR mode = choose_swap_present_mode(sup.present_modes);
    swapchain_extent = choose_swap_extent(sup.capabilities, window);
    u32 count = sup.capabilities.minImageCount + 1;
    if (sup.capabilities.maxImageCount > 0 && count > sup.capabilities.maxImageCount) {
        count = sup.capabilities.maxImageCount;
    }
    VkSwapchainCreateInfoKHR info{};
    info.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
    info.surface = surface;
    info.minImageCount = count;
    info.imageFormat = fmt.format;
    info.imageColorSpace = fmt.colorSpace;
    info.imageExtent = swapchain_extent;
    info.imageArrayLayers = 1;
    info.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    const QueueFamilyIndices idx = find_queue_families(physical_device, surface);
    const u32 fam[2] = {idx.graphics_family.value(), idx.present_family.value()};
    if (fam[0] != fam[1]) {
        info.imageSharingMode = VK_SHARING_MODE_CONCURRENT;
        info.queueFamilyIndexCount = 2;
        info.pQueueFamilyIndices = fam;
    } else {
        info.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    }
    info.preTransform = sup.capabilities.currentTransform;
    info.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    info.presentMode = mode;
    info.clipped = VK_TRUE;
    if (vkCreateSwapchainKHR(device, &info, nullptr, &swapchain) != VK_SUCCESS) {
        return false;
    }
    swapchain_image_format = fmt.format;
    vkGetSwapchainImagesKHR(device, swapchain, &count, nullptr);
    swapchain_images.resize(count);
    vkGetSwapchainImagesKHR(device, swapchain, &count, swapchain_images.data());
    return true;
}

bool VulkanRenderer::create_image_views() {
    swapchain_image_views.resize(swapchain_images.size());
    for (size_t i = 0; i < swapchain_images.size(); i++) {
        VkImageViewCreateInfo info{};
        info.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        info.image = swapchain_images[i];
        info.viewType = VK_IMAGE_VIEW_TYPE_2D;
        info.format = swapchain_image_format;
        info.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        info.subresourceRange.levelCount = 1;
        info.subresourceRange.layerCount = 1;
        if (vkCreateImageView(device, &info, nullptr, &swapchain_image_views[i]) != VK_SUCCESS) {
            return false;
        }
    }
    return true;
}

bool VulkanRenderer::create_render_pass() {
    VkAttachmentDescription color{};
    color.format = swapchain_image_format;
    color.samples = VK_SAMPLE_COUNT_1_BIT;
    color.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    color.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    color.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    color.finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    VkAttachmentReference ref{};
    ref.attachment = 0;
    ref.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    VkSubpassDescription sub{};
    sub.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    sub.colorAttachmentCount = 1;
    sub.pColorAttachments = &ref;
    VkSubpassDependency dep{};
    dep.srcSubpass = VK_SUBPASS_EXTERNAL;
    dep.dstSubpass = 0;
    dep.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dep.dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    VkRenderPassCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
    info.attachmentCount = 1;
    info.pAttachments = &color;
    info.subpassCount = 1;
    info.pSubpasses = &sub;
    info.dependencyCount = 1;
    info.pDependencies = &dep;
    return vkCreateRenderPass(device, &info, nullptr, &render_pass) == VK_SUCCESS;
}

bool VulkanRenderer::create_descriptor_set_layout() {
    VkDescriptorSetLayoutBinding ubo{};
    ubo.binding = 0;
    ubo.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    ubo.descriptorCount = 1;
    ubo.stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
    VkDescriptorSetLayoutBinding sampler{};
    sampler.binding = 1;
    sampler.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    sampler.descriptorCount = 1;
    sampler.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    const VkDescriptorSetLayoutBinding bindings[2] = {ubo, sampler};
    VkDescriptorSetLayoutCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    info.bindingCount = 2;
    info.pBindings = bindings;
    return vkCreateDescriptorSetLayout(device, &info, nullptr, &descriptor_set_layout) ==
           VK_SUCCESS;
}

bool VulkanRenderer::create_graphics_pipeline() {
    // Resolve shaders relative to the executable first (SDL_GetBasePath),
    // then relative to the working directory, so `./GB4ME rom.gb` works
    // from anywhere.
    const char* base = SDL_GetBasePath();
    const std::string base_dir = base ? base : "";
    const std::string candidates[5] = {
        base_dir + "shaders/quad.vert.spv",
        base_dir + "../share/GB4ME/shaders/quad.vert.spv",
        "/usr/share/GB4ME/shaders/quad.vert.spv",
        "shaders/quad.vert.spv",
        "bin/shaders/quad.vert.spv",
    };
    std::vector<char> vert, frag;
    for (const auto& c : candidates) {
        vert = read_file(c);
        if (vert.empty()) continue;
        std::string f = c;
        f.replace(f.size() - 8, 8, "frag.spv");  // quad.vert.spv -> quad.frag.spv
        frag = read_file(f);
        if (!frag.empty()) break;
        vert.clear();
    }
    if (vert.empty() || frag.empty()) {
        SDL_Log("Missing shaders (expected shaders/quad.{vert,frag}.spv next to the binary)");
        return false;
    }
    const VkShaderModule vert_mod = create_shader_module(device, vert);
    const VkShaderModule frag_mod = create_shader_module(device, frag);
    VkPipelineShaderStageCreateInfo stages[2] = {};
    stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = vert_mod;
    stages[0].pName = "main";
    stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = frag_mod;
    stages[1].pName = "main";

    VkVertexInputBindingDescription bind{};
    bind.binding = 0;
    bind.stride = sizeof(Vertex);
    bind.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;
    VkVertexInputAttributeDescription attrs[2] = {};
    attrs[0].location = 0;
    attrs[0].format = VK_FORMAT_R32G32_SFLOAT;
    attrs[0].offset = 0;
    attrs[1].location = 1;
    attrs[1].format = VK_FORMAT_R32G32_SFLOAT;
    attrs[1].offset = sizeof(Vec2);
    VkPipelineVertexInputStateCreateInfo vertex{};
    vertex.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    vertex.vertexBindingDescriptionCount = 1;
    vertex.pVertexBindingDescriptions = &bind;
    vertex.vertexAttributeDescriptionCount = 2;
    vertex.pVertexAttributeDescriptions = attrs;

    VkPipelineInputAssemblyStateCreateInfo assembly{};
    assembly.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    assembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkPipelineViewportStateCreateInfo vp{};
    vp.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    vp.viewportCount = 1;
    vp.scissorCount = 1;
    // Viewport/scissor are dynamic: set per frame from the real swapchain
    // extent with aspect-preserving letterboxing (see record_command_buffer).
    // A viewport baked at pipeline creation goes stale on window resize and
    // renders into the wrong area.
    const VkDynamicState dyn_states[2] = {VK_DYNAMIC_STATE_VIEWPORT,
                                          VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dyn{};
    dyn.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    dyn.dynamicStateCount = 2;
    dyn.pDynamicStates = dyn_states;
    VkPipelineRasterizationStateCreateInfo raster{};
    raster.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    raster.polygonMode = VK_POLYGON_MODE_FILL;
    raster.cullMode = VK_CULL_MODE_BACK_BIT;
    raster.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    raster.lineWidth = 1.0f;
    VkPipelineMultisampleStateCreateInfo ms{};
    ms.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    VkPipelineColorBlendAttachmentState blend_att{};
    // Blending on: the photo shell has transparent rounded corners; the game
    // quad is opaque (alpha 1) so it renders identically either way.
    blend_att.blendEnable = VK_TRUE;
    blend_att.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
    blend_att.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    blend_att.colorBlendOp = VK_BLEND_OP_ADD;
    blend_att.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
    blend_att.dstAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
    blend_att.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                               VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    VkPipelineColorBlendStateCreateInfo blend{};
    blend.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    blend.attachmentCount = 1;
    blend.pAttachments = &blend_att;
    VkPipelineLayoutCreateInfo layout{};
    layout.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    layout.setLayoutCount = 1;
    layout.pSetLayouts = &descriptor_set_layout;
    if (vkCreatePipelineLayout(device, &layout, nullptr, &pipeline_layout) != VK_SUCCESS) {
        return false;
    }
    VkGraphicsPipelineCreateInfo pipe{};
    pipe.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    pipe.stageCount = 2;
    pipe.pStages = stages;
    pipe.pVertexInputState = &vertex;
    pipe.pInputAssemblyState = &assembly;
    pipe.pViewportState = &vp;
    pipe.pRasterizationState = &raster;
    pipe.pMultisampleState = &ms;
    pipe.pColorBlendState = &blend;
    pipe.pDynamicState = &dyn;
    pipe.layout = pipeline_layout;
    pipe.renderPass = render_pass;
    const bool ok =
        vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &pipe, nullptr, &graphics_pipeline) ==
        VK_SUCCESS;
    vkDestroyShaderModule(device, vert_mod, nullptr);
    vkDestroyShaderModule(device, frag_mod, nullptr);
    return ok;
}

bool VulkanRenderer::create_gui_buffers() {
    const VkDeviceSize vsize = 16384 * sizeof(GuiVertex);
    const VkDeviceSize isize = 32768 * sizeof(u16);
    if (!create_buffer(physical_device, device, vsize,
                       VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
                       VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                       gui_vertex_buffer, gui_vertex_buffer_memory)) {
        return false;
    }
    if (!create_buffer(physical_device, device, isize,
                       VK_BUFFER_USAGE_INDEX_BUFFER_BIT,
                       VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                       gui_index_buffer, gui_index_buffer_memory)) {
        return false;
    }
    return true;
}

bool VulkanRenderer::create_gui_pipeline() {
    auto try_gui = [&](const std::string &base) -> std::pair<std::vector<char>, std::vector<char>> {
        auto v = read_file(base + "gui.vert.spv");
        auto f = read_file(base + "gui.frag.spv");
        return {std::move(v), std::move(f)};
    };
    const char* sdl_base = SDL_GetBasePath();
    std::string base_dir = sdl_base ? sdl_base : "";
    const std::string gui_bases[6] = {
        "shaders/",
        "bin/shaders/",
        base_dir + "shaders/",
        base_dir + "../share/GB4ME/shaders/",
        "/usr/share/GB4ME/shaders/",
        "/usr/local/share/GB4ME/shaders/"
    };
    std::vector<char> vert, frag;
    for (auto &b : gui_bases) {
        auto p = try_gui(b);
        if (!p.first.empty() && !p.second.empty()) { vert = std::move(p.first); frag = std::move(p.second); break; }
    }
    if (vert.empty() || frag.empty()) {
        // Fallback to second candidate path (when running from build dir)
        const auto vert2 = read_file("bin/shaders/gui.vert.spv");
        const auto frag2 = read_file("bin/shaders/gui.frag.spv");
        if (vert2.empty() || frag2.empty()) {
            SDL_Log("Missing GUI shaders");
            return false;
        }
        const VkShaderModule vmod = create_shader_module(device, vert2);
        const VkShaderModule fmod = create_shader_module(device, frag2);
        // ... pipeline creation using vmod/fmod (duplicated below); to avoid duplication, just retry loading via base path logic
        vkDestroyShaderModule(device, vmod, nullptr);
        vkDestroyShaderModule(device, fmod, nullptr);
        // Fallback not yet implemented as pipeline; try primary again via absolute path
        const char* base = SDL_GetBasePath();
        std::string b = base ? base : "";
        const auto vert3 = read_file(b + "shaders/gui.vert.spv");
        const auto frag3 = read_file(b + "shaders/gui.frag.spv");
        if (vert3.empty() || frag3.empty()) return false;
        VkShaderModule vm = create_shader_module(device, vert3);
        VkShaderModule fm = create_shader_module(device, frag3);
        VkPipelineShaderStageCreateInfo stages[2] = {};
        stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
        stages[0].module = vm;
        stages[0].pName = "main";
        stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
        stages[1].module = fm;
        stages[1].pName = "main";
        VkVertexInputBindingDescription bind{};
        bind.binding = 0;
        bind.stride = sizeof(GuiVertex);
        bind.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;
        VkVertexInputAttributeDescription attrs[2] = {};
        attrs[0].location = 0; attrs[0].format = VK_FORMAT_R32G32_SFLOAT; attrs[0].offset = offsetof(GuiVertex, pos);
        attrs[1].location = 1; attrs[1].format = VK_FORMAT_R32G32B32A32_SFLOAT; attrs[1].offset = offsetof(GuiVertex, color);
        VkPipelineVertexInputStateCreateInfo vertex{}; vertex.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO; vertex.vertexBindingDescriptionCount = 1; vertex.pVertexBindingDescriptions = &bind; vertex.vertexAttributeDescriptionCount = 2; vertex.pVertexAttributeDescriptions = attrs;
        VkPipelineInputAssemblyStateCreateInfo assembly{}; assembly.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO; assembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
        VkPipelineViewportStateCreateInfo vp{}; vp.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO; vp.viewportCount = 1; vp.scissorCount = 1;
        const VkDynamicState dyn_states[2] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
        VkPipelineDynamicStateCreateInfo dyn{}; dyn.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO; dyn.dynamicStateCount = 2; dyn.pDynamicStates = dyn_states;
        VkPipelineRasterizationStateCreateInfo raster{}; raster.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO; raster.polygonMode = VK_POLYGON_MODE_FILL; raster.cullMode = VK_CULL_MODE_NONE; raster.lineWidth = 1.0f;
        VkPipelineMultisampleStateCreateInfo ms{}; ms.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO; ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
        VkPipelineColorBlendAttachmentState blend_att{}; blend_att.blendEnable = VK_TRUE; blend_att.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA; blend_att.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA; blend_att.colorBlendOp = VK_BLEND_OP_ADD; blend_att.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE; blend_att.dstAlphaBlendFactor = VK_BLEND_FACTOR_ZERO; blend_att.colorWriteMask = VK_COLOR_COMPONENT_R_BIT|VK_COLOR_COMPONENT_G_BIT|VK_COLOR_COMPONENT_B_BIT|VK_COLOR_COMPONENT_A_BIT;
        VkPipelineColorBlendStateCreateInfo blend{}; blend.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO; blend.attachmentCount = 1; blend.pAttachments = &blend_att;
        VkPipelineLayoutCreateInfo layout{}; layout.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        if (vkCreatePipelineLayout(device, &layout, nullptr, &gui_pipeline_layout) != VK_SUCCESS) { vkDestroyShaderModule(device, vm, nullptr); vkDestroyShaderModule(device, fm, nullptr); return false; }
        VkGraphicsPipelineCreateInfo pipe{}; pipe.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO; pipe.stageCount = 2; pipe.pStages = stages; pipe.pVertexInputState = &vertex; pipe.pInputAssemblyState = &assembly; pipe.pViewportState = &vp; pipe.pRasterizationState = &raster; pipe.pMultisampleState = &ms; pipe.pColorBlendState = &blend; pipe.pDynamicState = &dyn; pipe.layout = gui_pipeline_layout; pipe.renderPass = render_pass;
        bool ok = vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &pipe, nullptr, &gui_pipeline) == VK_SUCCESS;
        vkDestroyShaderModule(device, vm, nullptr); vkDestroyShaderModule(device, fm, nullptr);
        return ok;
    }
    const VkShaderModule vert_mod = create_shader_module(device, vert);
    const VkShaderModule frag_mod = create_shader_module(device, frag);
    VkPipelineShaderStageCreateInfo stages[2] = {};
    stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = vert_mod;
    stages[0].pName = "main";
    stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = frag_mod;
    stages[1].pName = "main";
    VkVertexInputBindingDescription bind{};
    bind.binding = 0;
    bind.stride = sizeof(GuiVertex);
    bind.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;
    VkVertexInputAttributeDescription attrs[2] = {};
    attrs[0].location = 0; attrs[0].format = VK_FORMAT_R32G32_SFLOAT; attrs[0].offset = offsetof(GuiVertex, pos);
    attrs[1].location = 1; attrs[1].format = VK_FORMAT_R32G32B32A32_SFLOAT; attrs[1].offset = offsetof(GuiVertex, color);
    VkPipelineVertexInputStateCreateInfo vertex{}; vertex.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO; vertex.vertexBindingDescriptionCount = 1; vertex.pVertexBindingDescriptions = &bind; vertex.vertexAttributeDescriptionCount = 2; vertex.pVertexAttributeDescriptions = attrs;
    VkPipelineInputAssemblyStateCreateInfo assembly{}; assembly.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO; assembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkPipelineViewportStateCreateInfo vp{}; vp.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO; vp.viewportCount = 1; vp.scissorCount = 1;
    const VkDynamicState dyn_states[2] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dyn{}; dyn.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO; dyn.dynamicStateCount = 2; dyn.pDynamicStates = dyn_states;
    VkPipelineRasterizationStateCreateInfo raster{}; raster.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO; raster.polygonMode = VK_POLYGON_MODE_FILL; raster.cullMode = VK_CULL_MODE_NONE; raster.lineWidth = 1.0f;
    VkPipelineMultisampleStateCreateInfo ms{}; ms.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO; ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    VkPipelineColorBlendAttachmentState blend_att{}; blend_att.blendEnable = VK_TRUE; blend_att.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA; blend_att.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA; blend_att.colorBlendOp = VK_BLEND_OP_ADD; blend_att.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE; blend_att.dstAlphaBlendFactor = VK_BLEND_FACTOR_ZERO; blend_att.colorWriteMask = VK_COLOR_COMPONENT_R_BIT|VK_COLOR_COMPONENT_G_BIT|VK_COLOR_COMPONENT_B_BIT|VK_COLOR_COMPONENT_A_BIT;
    VkPipelineColorBlendStateCreateInfo blend{}; blend.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO; blend.attachmentCount = 1; blend.pAttachments = &blend_att;
    VkPipelineLayoutCreateInfo layout{}; layout.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    if (vkCreatePipelineLayout(device, &layout, nullptr, &gui_pipeline_layout) != VK_SUCCESS) { vkDestroyShaderModule(device, vert_mod, nullptr); vkDestroyShaderModule(device, frag_mod, nullptr); return false; }
    VkGraphicsPipelineCreateInfo pipe{}; pipe.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO; pipe.stageCount = 2; pipe.pStages = stages; pipe.pVertexInputState = &vertex; pipe.pInputAssemblyState = &assembly; pipe.pViewportState = &vp; pipe.pRasterizationState = &raster; pipe.pMultisampleState = &ms; pipe.pColorBlendState = &blend; pipe.pDynamicState = &dyn; pipe.layout = gui_pipeline_layout; pipe.renderPass = render_pass;
    bool ok = vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &pipe, nullptr, &gui_pipeline) == VK_SUCCESS;
    vkDestroyShaderModule(device, vert_mod, nullptr);
    vkDestroyShaderModule(device, frag_mod, nullptr);
    return ok;
}

bool VulkanRenderer::create_framebuffers() {
    swapchain_framebuffers.resize(swapchain_image_views.size());
    for (size_t i = 0; i < swapchain_image_views.size(); i++) {
        VkFramebufferCreateInfo info{};
        info.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
        info.renderPass = render_pass;
        info.attachmentCount = 1;
        info.pAttachments = &swapchain_image_views[i];
        info.width = swapchain_extent.width;
        info.height = swapchain_extent.height;
        info.layers = 1;
        if (vkCreateFramebuffer(device, &info, nullptr, &swapchain_framebuffers[i]) !=
            VK_SUCCESS) {
            return false;
        }
    }
    return true;
}

bool VulkanRenderer::create_command_pool() {
    const QueueFamilyIndices idx = find_queue_families(physical_device, surface);
    VkCommandPoolCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    info.queueFamilyIndex = idx.graphics_family.value();
    info.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    return vkCreateCommandPool(device, &info, nullptr, &command_pool) == VK_SUCCESS;
}

bool VulkanRenderer::create_command_buffers() {
    command_buffers.resize(max_frames_in_flight);
    VkCommandBufferAllocateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    info.commandPool = command_pool;
    info.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    info.commandBufferCount = max_frames_in_flight;
    return vkAllocateCommandBuffers(device, &info, command_buffers.data()) == VK_SUCCESS;
}

bool VulkanRenderer::create_sync_objects() {
    image_available_semaphores.resize(max_frames_in_flight);
    render_finished_semaphores.resize(max_frames_in_flight);
    in_flight_fences.resize(max_frames_in_flight);
    VkSemaphoreCreateInfo sem{};
    sem.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
    VkFenceCreateInfo fence{};
    fence.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    fence.flags = VK_FENCE_CREATE_SIGNALED_BIT;
    for (u32 i = 0; i < max_frames_in_flight; i++) {
        if (vkCreateSemaphore(device, &sem, nullptr, &image_available_semaphores[i]) !=
                VK_SUCCESS ||
            vkCreateSemaphore(device, &sem, nullptr, &render_finished_semaphores[i]) !=
                VK_SUCCESS ||
            vkCreateFence(device, &fence, nullptr, &in_flight_fences[i]) != VK_SUCCESS) {
            return false;
        }
    }
    return true;
}

bool VulkanRenderer::create_vertex_buffer() {
    // Fullscreen quad (CCW). UVs are V-flipped relative to the naive mapping:
    // Vulkan samples texel row 0 at v=0, our upload puts the game's TOP row
    // first, and the standard viewport maps NDC y=+1 to the framebuffer
    // BOTTOM -- so the top vertices must sample v=1 to show the image
    // upright. (uv origin is the game's top-left pixel.)
    const std::array<Vertex, 4> verts = {{
        {{-1.0f, 1.0f}, {0.0f, 1.0f}},
        {{1.0f, 1.0f}, {1.0f, 1.0f}},
        {{1.0f, -1.0f}, {1.0f, 0.0f}},
        {{-1.0f, -1.0f}, {0.0f, 0.0f}},
    }};
    const VkDeviceSize size = sizeof(verts);
    VkBuffer staging{};
    VkDeviceMemory staging_mem{};
    if (!create_buffer(physical_device, device, size, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                       VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                           VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                       staging, staging_mem)) {
        return false;
    }
    void* dst = nullptr;
    vkMapMemory(device, staging_mem, 0, size, 0, &dst);
    std::memcpy(dst, verts.data(), size);
    vkUnmapMemory(device, staging_mem);
    if (!create_buffer(physical_device, device, size,
                       VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
                       VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, vertex_buffer,
                       vertex_buffer_memory)) {
        return false;
    }
    const VkCommandBuffer cmd = begin_single_time(device, command_pool);
    VkBufferCopy copy{};
    copy.size = size;
    vkCmdCopyBuffer(cmd, staging, vertex_buffer, 1, &copy);
    end_single_time(device, command_pool, graphics_queue, cmd);
    vkDestroyBuffer(device, staging, nullptr);
    vkFreeMemory(device, staging_mem, nullptr);
    return true;
}

bool VulkanRenderer::create_index_buffer() {
    const std::array<u16, 6> idx = {0, 1, 2, 2, 3, 0};
    const VkDeviceSize size = sizeof(idx);
    VkBuffer staging{};
    VkDeviceMemory staging_mem{};
    if (!create_buffer(physical_device, device, size, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                       VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                           VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                       staging, staging_mem)) {
        return false;
    }
    void* dst = nullptr;
    vkMapMemory(device, staging_mem, 0, size, 0, &dst);
    std::memcpy(dst, idx.data(), size);
    vkUnmapMemory(device, staging_mem);
    if (!create_buffer(physical_device, device, size,
                       VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT,
                       VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, index_buffer,
                       index_buffer_memory)) {
        return false;
    }
    const VkCommandBuffer cmd = begin_single_time(device, command_pool);
    VkBufferCopy copy{};
    copy.size = size;
    vkCmdCopyBuffer(cmd, staging, index_buffer, 1, &copy);
    end_single_time(device, command_pool, graphics_queue, cmd);
    vkDestroyBuffer(device, staging, nullptr);
    vkFreeMemory(device, staging_mem, nullptr);
    return true;
}

bool VulkanRenderer::create_texture() {
    const VkDeviceSize size = kTexWidth * kTexHeight * 4;
    if (!create_buffer(physical_device, device, size, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                       VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                           VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                       texture_buffer, texture_buffer_memory)) {
        return false;
    }
    VkImageCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    info.imageType = VK_IMAGE_TYPE_2D;
    info.format = kTexFormat;
    info.extent = {kTexWidth, kTexHeight, 1};
    info.mipLevels = 1;
    info.arrayLayers = 1;
    info.samples = VK_SAMPLE_COUNT_1_BIT;
    info.tiling = VK_IMAGE_TILING_OPTIMAL;
    info.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (vkCreateImage(device, &info, nullptr, &texture_image) != VK_SUCCESS) return false;
    VkMemoryRequirements req{};
    vkGetImageMemoryRequirements(device, texture_image, &req);
    const u32 idx =
        find_memory_type(physical_device, req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (idx == std::numeric_limits<u32>::max()) return false;
    VkMemoryAllocateInfo alloc{};
    alloc.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    alloc.allocationSize = req.size;
    alloc.memoryTypeIndex = idx;
    if (vkAllocateMemory(device, &alloc, nullptr, &texture_image_memory) != VK_SUCCESS) {
        return false;
    }
    vkBindImageMemory(device, texture_image, texture_image_memory, 0);

    VkImageViewCreateInfo view{};
    view.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    view.image = texture_image;
    view.viewType = VK_IMAGE_VIEW_TYPE_2D;
    view.format = kTexFormat;
    view.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    view.subresourceRange.levelCount = 1;
    view.subresourceRange.layerCount = 1;
    if (vkCreateImageView(device, &view, nullptr, &texture_image_view) != VK_SUCCESS) {
        return false;
    }
    VkSamplerCreateInfo sampler{};
    sampler.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    sampler.magFilter = VK_FILTER_NEAREST;  // Crisp pixels, no blur.
    sampler.minFilter = VK_FILTER_NEAREST;
    sampler.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sampler.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sampler.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    return vkCreateSampler(device, &sampler, nullptr, &texture_sampler) == VK_SUCCESS;
}

bool VulkanRenderer::create_body_texture() {
    // Load the GBC body texture — try installed share first, then dev assets
    int tex_width, tex_height, tex_channels;
    unsigned char* pixels = nullptr;
    const char* base = SDL_GetBasePath();
    std::string base_dir = base ? base : "";
    const std::string candidates[4] = {
        base_dir + "../share/GB4ME/assets/gbc_body.png",
        "/usr/share/GB4ME/assets/gbc_body.png",
        base_dir + "assets/gbc_body.png",
        "assets/gbc_body.png"
    };
    for (auto &p : candidates) {
        pixels = stbi_load(p.c_str(), &tex_width, &tex_height, &tex_channels, 4);
        if (pixels) break;
    }
    if (!pixels) {
        SDL_Log("Failed to load body texture: %s", stbi_failure_reason());
        return false;
    }
    
    const VkDeviceSize size = tex_width * tex_height * 4;
    VkBuffer body_staging_buffer;
    VkDeviceMemory body_staging_buffer_memory;
    if (!create_buffer(physical_device, device, size, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                       VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                       body_staging_buffer, body_staging_buffer_memory)) {
        stbi_image_free(pixels);
        return false;
    }
    
    void* dst = nullptr;
    vkMapMemory(device, body_staging_buffer_memory, 0, size, 0, &dst);
    std::memcpy(dst, pixels, size);
    vkUnmapMemory(device, body_staging_buffer_memory);
    stbi_image_free(pixels);
    
    VkImageCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    info.imageType = VK_IMAGE_TYPE_2D;
    info.format = VK_FORMAT_R8G8B8A8_SRGB;
    info.extent = {static_cast<u32>(tex_width), static_cast<u32>(tex_height), 1};
    info.mipLevels = 1;
    info.arrayLayers = 1;
    info.samples = VK_SAMPLE_COUNT_1_BIT;
    info.tiling = VK_IMAGE_TILING_OPTIMAL;
    info.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (vkCreateImage(device, &info, nullptr, &body_image) != VK_SUCCESS) return false;
    
    VkMemoryRequirements req{};
    vkGetImageMemoryRequirements(device, body_image, &req);
    const u32 idx = find_memory_type(physical_device, req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (idx == std::numeric_limits<u32>::max()) return false;
    
    VkMemoryAllocateInfo alloc{};
    alloc.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    alloc.allocationSize = req.size;
    alloc.memoryTypeIndex = idx;
    if (vkAllocateMemory(device, &alloc, nullptr, &body_image_memory) != VK_SUCCESS) {
        return false;
    }
    vkBindImageMemory(device, body_image, body_image_memory, 0);
    
    // Copy staging buffer to image
    VkCommandBuffer cmd = begin_single_time(device, command_pool);
    VkImageMemoryBarrier barrier{};
    barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    barrier.srcAccessMask = 0;
    barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    barrier.image = body_image;
    barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    barrier.subresourceRange.levelCount = 1;
    barrier.subresourceRange.layerCount = 1;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &barrier);
    
    VkBufferImageCopy region{};
    region.imageExtent = {static_cast<u32>(tex_width), static_cast<u32>(tex_height), 1};
    region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    region.imageSubresource.layerCount = 1;
    vkCmdCopyBufferToImage(cmd, body_staging_buffer, body_image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
    
    VkImageMemoryBarrier to_shader = barrier;
    to_shader.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    to_shader.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    to_shader.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    to_shader.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, nullptr, 0, nullptr, 1, &to_shader);
    
    end_single_time(device, command_pool, graphics_queue, cmd);
    
    // Clean up staging buffer
    vkDestroyBuffer(device, body_staging_buffer, nullptr);
    vkFreeMemory(device, body_staging_buffer_memory, nullptr);
    
    // Create image view
    VkImageViewCreateInfo view{};
    view.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    view.image = body_image;
    view.viewType = VK_IMAGE_VIEW_TYPE_2D;
    view.format = VK_FORMAT_R8G8B8A8_SRGB;
    view.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    view.subresourceRange.levelCount = 1;
    view.subresourceRange.layerCount = 1;
    if (vkCreateImageView(device, &view, nullptr, &body_image_view) != VK_SUCCESS) {
        return false;
    }
    
    // Create sampler
    VkSamplerCreateInfo sampler{};
    sampler.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    sampler.magFilter = VK_FILTER_LINEAR;
    sampler.minFilter = VK_FILTER_LINEAR;
    sampler.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sampler.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sampler.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    return vkCreateSampler(device, &sampler, nullptr, &body_sampler) == VK_SUCCESS;
}

// Photo shell: one descriptor set reusing the main set layout (UBO binding
// 0 + combined sampler binding 1), with the body photo bound as sampler.
bool VulkanRenderer::create_body_photo_descriptors() {
    if (!body_photo_ok_) return true; // no photo, nothing to bind
    VkDescriptorPoolSize sizes[2] = {};
    sizes[0].type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    sizes[0].descriptorCount = 1;
    sizes[1].type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    sizes[1].descriptorCount = 1;
    VkDescriptorPoolCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    info.maxSets = 1;
    info.poolSizeCount = 2;
    info.pPoolSizes = sizes;
    if (vkCreateDescriptorPool(device, &info, nullptr, &body_descriptor_pool) != VK_SUCCESS) {
        return false;
    }
    VkDescriptorSetAllocateInfo alloc{};
    alloc.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    alloc.descriptorPool = body_descriptor_pool;
    alloc.descriptorSetCount = 1;
    alloc.pSetLayouts = &descriptor_set_layout;
    body_descriptor_sets.resize(1);
    if (vkAllocateDescriptorSets(device, &alloc, body_descriptor_sets.data()) != VK_SUCCESS) {
        return false;
    }
    VkDescriptorBufferInfo buf{};
    buf.buffer = uniform_buffers[0];
    buf.range = sizeof(UniformBufferObject);
    VkDescriptorImageInfo img{};
    img.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    img.imageView = body_image_view;
    img.sampler = body_sampler;
    VkWriteDescriptorSet writes[2] = {};
    writes[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[0].dstSet = body_descriptor_sets[0];
    writes[0].dstBinding = 0;
    writes[0].descriptorCount = 1;
    writes[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    writes[0].pBufferInfo = &buf;
    writes[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[1].dstSet = body_descriptor_sets[0];
    writes[1].dstBinding = 1;
    writes[1].descriptorCount = 1;
    writes[1].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    writes[1].pImageInfo = &img;
    vkUpdateDescriptorSets(device, 2, writes, 0, nullptr);
    return true;
}


bool VulkanRenderer::create_uniform_buffers() {
    const VkDeviceSize size = sizeof(UniformBufferObject);
    uniform_buffers.resize(max_frames_in_flight);
    uniform_buffers_memory.resize(max_frames_in_flight);
    uniform_buffers_mapped.resize(max_frames_in_flight);
    for (u32 i = 0; i < max_frames_in_flight; i++) {
        if (!create_buffer(physical_device, device, size, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
                           VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                               VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                           uniform_buffers[i], uniform_buffers_memory[i])) {
            return false;
        }
        vkMapMemory(device, uniform_buffers_memory[i], 0, size, 0, &uniform_buffers_mapped[i]);
    }
    return true;
}

bool VulkanRenderer::create_descriptor_pool() {
    VkDescriptorPoolSize sizes[2] = {};
    sizes[0].type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    sizes[0].descriptorCount = max_frames_in_flight;
    sizes[1].type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    sizes[1].descriptorCount = max_frames_in_flight;
    VkDescriptorPoolCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    info.maxSets = max_frames_in_flight;
    info.poolSizeCount = 2;
    info.pPoolSizes = sizes;
    return vkCreateDescriptorPool(device, &info, nullptr, &descriptor_pool) == VK_SUCCESS;
}

bool VulkanRenderer::create_descriptor_sets() {
    std::vector<VkDescriptorSetLayout> layouts(max_frames_in_flight, descriptor_set_layout);
    VkDescriptorSetAllocateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    info.descriptorPool = descriptor_pool;
    info.descriptorSetCount = max_frames_in_flight;
    info.pSetLayouts = layouts.data();
    descriptor_sets.resize(max_frames_in_flight);
    if (vkAllocateDescriptorSets(device, &info, descriptor_sets.data()) != VK_SUCCESS) {
        return false;
    }
    for (u32 i = 0; i < max_frames_in_flight; i++) {
        VkDescriptorBufferInfo buf{};
        buf.buffer = uniform_buffers[i];
        buf.range = sizeof(UniformBufferObject);
        VkDescriptorImageInfo img{};
        img.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        img.imageView = texture_image_view;
        img.sampler = texture_sampler;
        VkWriteDescriptorSet writes[2] = {};
        writes[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[0].dstSet = descriptor_sets[i];
        writes[0].dstBinding = 0;
        writes[0].descriptorCount = 1;
        writes[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        writes[0].pBufferInfo = &buf;
        writes[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[1].dstSet = descriptor_sets[i];
        writes[1].dstBinding = 1;
        writes[1].descriptorCount = 1;
        writes[1].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        writes[1].pImageInfo = &img;
        vkUpdateDescriptorSets(device, 2, writes, 0, nullptr);
    }
    return true;
}

void VulkanRenderer::recreate_swapchain() {
    wait_idle();
    for (auto fb : swapchain_framebuffers) vkDestroyFramebuffer(device, fb, nullptr);
    swapchain_framebuffers.clear();
    for (auto view : swapchain_image_views) vkDestroyImageView(device, view, nullptr);
    swapchain_image_views.clear();
    vkDestroySwapchainKHR(device, swapchain, nullptr);
    create_swapchain();
    create_image_views();
    create_framebuffers();
}

void VulkanRenderer::update_uniform_buffer(u32 current_image) {
    UniformBufferObject ubo{};
    // Identity MVP: quad vertices are already in NDC.
    ubo.mvp.rows[0] = {1.0f, 0.0f, 0.0f, 0.0f};
    ubo.mvp.rows[1] = {0.0f, 1.0f, 0.0f, 0.0f};
    ubo.mvp.rows[2] = {0.0f, 0.0f, 1.0f, 0.0f};
    ubo.mvp.rows[3] = {0.0f, 0.0f, 0.0f, 1.0f};
    std::memcpy(uniform_buffers_mapped[current_image], &ubo, sizeof(ubo));
}

void VulkanRenderer::record_command_buffer(VkCommandBuffer cmd, u32 image_index) {
    VkCommandBufferBeginInfo begin{};
    begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    vkBeginCommandBuffer(cmd, &begin);

    // Upload: staging buffer -> texture image.
    const bool first_upload = (texture_layout_ == VK_IMAGE_LAYOUT_UNDEFINED);
    VkImageMemoryBarrier to_transfer{};
    to_transfer.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    to_transfer.oldLayout = texture_layout_;
    to_transfer.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    to_transfer.srcAccessMask = first_upload ? 0 : VK_ACCESS_SHADER_READ_BIT;
    to_transfer.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    to_transfer.image = texture_image;
    to_transfer.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    to_transfer.subresourceRange.levelCount = 1;
    to_transfer.subresourceRange.layerCount = 1;
    vkCmdPipelineBarrier(cmd, first_upload ? VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT
                                           : VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                         VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1,
                         &to_transfer);
    VkBufferImageCopy region{};
    region.imageExtent = {kTexWidth, kTexHeight, 1};
    region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    region.imageSubresource.layerCount = 1;
    vkCmdCopyBufferToImage(cmd, texture_buffer, texture_image,
                           VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
    VkImageMemoryBarrier to_shader = to_transfer;
    to_shader.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    to_shader.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    to_shader.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    to_shader.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, nullptr, 0, nullptr, 1,
                         &to_shader);
    texture_layout_ = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

    VkClearValue clear{};
    clear.color = {{0.0f, 0.0f, 0.0f, 1.0f}};
    VkRenderPassBeginInfo rp{};
    rp.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    rp.renderPass = render_pass;
    rp.framebuffer = swapchain_framebuffers[image_index];
    rp.renderArea.extent = swapchain_extent;
    rp.clearValueCount = 1;
    rp.pClearValues = &clear;
    vkCmdBeginRenderPass(cmd, &rp, VK_SUBPASS_CONTENTS_INLINE);
    // Letterbox the 10:9 GB image into the window: largest integer-friendly
    // fit centered, black bars elsewhere (bars show the clear color).
    {
        const float win_w = static_cast<float>(swapchain_extent.width);
        const float win_h = static_cast<float>(swapchain_extent.height);
        const float scale = std::min(win_w / kTexWidth, win_h / kTexHeight);
        const float dw = kTexWidth * scale;
        const float dh = kTexHeight * scale;
        VkViewport viewport{};
        viewport.x = (win_w - dw) * 0.5f;
        viewport.y = (win_h - dh) * 0.5f;
        viewport.width = dw;
        viewport.height = dh;
        viewport.minDepth = 0.0f;
        viewport.maxDepth = 1.0f;
        vkCmdSetViewport(cmd, 0, 1, &viewport);
        VkRect2D scissor{};
        scissor.offset = {static_cast<i32>(viewport.x), static_cast<i32>(viewport.y)};
        scissor.extent = {static_cast<u32>(dw), static_cast<u32>(dh)};
        vkCmdSetScissor(cmd, 0, 1, &scissor);
    }
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, graphics_pipeline);
    VkBuffer vbs[1] = {vertex_buffer};
    VkDeviceSize offs[1] = {0};
    vkCmdBindVertexBuffers(cmd, 0, 1, vbs, offs);
    vkCmdBindIndexBuffer(cmd, index_buffer, 0, VK_INDEX_TYPE_UINT16);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_layout, 0, 1,
                            &descriptor_sets[current_frame], 0, nullptr);
    vkCmdDrawIndexed(cmd, 6, 1, 0, 0, 0);
    vkCmdEndRenderPass(cmd);
    vkEndCommandBuffer(cmd);
}

void VulkanRenderer::render(const PPU::FrameData& frame) {
    vkWaitForFences(device, 1, &in_flight_fences[current_frame], VK_TRUE,
                    std::numeric_limits<u64>::max());
    u32 image_index = 0;
    VkResult acq = vkAcquireNextImageKHR(device, swapchain,
                                         std::numeric_limits<u64>::max(),
                                         image_available_semaphores[current_frame],
                                         VK_NULL_HANDLE, &image_index);
    if (acq == VK_ERROR_OUT_OF_DATE_KHR) {
        recreate_swapchain();
        return;
    }
    vkResetFences(device, 1, &in_flight_fences[current_frame]);

    // PPU framebuffer is 0xAARRGGBB, matching B8G8R8A8 byte order: direct copy.
    void* dst = nullptr;
    vkMapMemory(device, texture_buffer_memory, 0, kTexWidth * kTexHeight * 4, 0, &dst);
    std::memcpy(dst, frame.pixels.data(), kTexWidth * kTexHeight * 4);
    vkUnmapMemory(device, texture_buffer_memory);

    update_uniform_buffer(current_frame);
    vkResetCommandBuffer(command_buffers[current_frame], 0);
    record_command_buffer(command_buffers[current_frame], image_index);

    VkSemaphore wait[1] = {image_available_semaphores[current_frame]};
    VkPipelineStageFlags stages[1] = {VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT};
    VkSemaphore signal[1] = {render_finished_semaphores[current_frame]};
    VkSubmitInfo submit{};
    submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submit.waitSemaphoreCount = 1;
    submit.pWaitSemaphores = wait;
    submit.pWaitDstStageMask = stages;
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &command_buffers[current_frame];
    submit.signalSemaphoreCount = 1;
    submit.pSignalSemaphores = signal;
    vkQueueSubmit(graphics_queue, 1, &submit, in_flight_fences[current_frame]);

    VkPresentInfoKHR present{};
    present.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
    present.waitSemaphoreCount = 1;
    present.pWaitSemaphores = signal;
    present.swapchainCount = 1;
    present.pSwapchains = &swapchain;
    present.pImageIndices = &image_index;
    const VkResult res = vkQueuePresentKHR(present_queue, &present);
    if (res == VK_ERROR_OUT_OF_DATE_KHR || res == VK_SUBOPTIMAL_KHR) {
        recreate_swapchain();
    }
    current_frame = (current_frame + 1) % max_frames_in_flight;
}

void VulkanRenderer::update_gui_buffers(const GUIConsole& gui) {
    // Build vertex/index data for GUI rects (body, bezel, buttons, etc.)
    std::vector<GuiVertex> verts;
    std::vector<u16> indices;
    auto push_rect = [&](float x, float y, float w, float h, u32 rgba, float rounding) {
        // Rounded rectangle: for now, just axis-aligned rect (rounding ignored, but keep param for future).
        // Convert pixel to NDC
        float win_w = static_cast<float>(swapchain_extent.width);
        float win_h = static_cast<float>(swapchain_extent.height);
        auto to_ndc = [&](float px, float py) -> Vec2 {
            // Window pixels use top-left origin (y down). Vulkan NDC y=-1 maps
            // to the framebuffer top, so py=0 must yield -1 (NOT 1.0f - ...,
            // which rendered the whole GUI upside down).
            return { (px / win_w) * 2.0f - 1.0f, (py / win_h) * 2.0f - 1.0f };
        };
        (void)rounding;
        Vec2 p0 = to_ndc(x, y);
        Vec2 p1 = to_ndc(x + w, y);
        Vec2 p2 = to_ndc(x + w, y + h);
        Vec2 p3 = to_ndc(x, y + h);
        Vec4 col = { ((rgba>>16)&0xFF)/255.0f, ((rgba>>8)&0xFF)/255.0f, (rgba&0xFF)/255.0f, ((rgba>>24)&0xFF)/255.0f };
        // Reorder to match shader's RGBA order? Our Vec4 is x,y,z,w as r,g,b,a.
        u16 base = static_cast<u16>(verts.size());
        verts.push_back({p0, {col.x, col.y, col.z, col.w}});
        verts.push_back({p1, {col.x, col.y, col.z, col.w}});
        verts.push_back({p2, {col.x, col.y, col.z, col.w}});
        verts.push_back({p3, {col.x, col.y, col.z, col.w}});
        indices.push_back(base+0); indices.push_back(base+1); indices.push_back(base+2);
        indices.push_back(base+2); indices.push_back(base+3); indices.push_back(base+0);
    };

    auto push_circle = [&](float cx, float cy, float r, u32 rgba) {
        float win_w = static_cast<float>(swapchain_extent.width);
        float win_h = static_cast<float>(swapchain_extent.height);
        Vec4 col = { ((rgba>>16)&0xFF)/255.0f, ((rgba>>8)&0xFF)/255.0f, (rgba&0xFF)/255.0f, ((rgba>>24)&0xFF)/255.0f };
        const int segs = 16;
        u16 center_idx = static_cast<u16>(verts.size());
        Vec2 center = { (cx / win_w)*2.0f -1.0f, (cy / win_h)*2.0f -1.0f };
        verts.push_back({center, {col.x, col.y, col.z, col.w}});
        for (int i=0;i<segs;i++) {
            float a = (i / static_cast<float>(segs)) * 2.0f * 3.1415926f;
            float x = cx + std::cos(a)*r;
            float y = cy + std::sin(a)*r;
            Vec2 p = { (x / win_w)*2.0f -1.0f, (y / win_h)*2.0f -1.0f };
            verts.push_back({p, {col.x, col.y, col.z, col.w}});
        }
        for (int i=0;i<segs;i++) {
            u16 i0 = center_idx;
            u16 i1 = center_idx + 1 + i;
            u16 i2 = center_idx + 1 + ((i+1)%segs);
            indices.push_back(i0); indices.push_back(i1); indices.push_back(i2);
        }
    };
    auto push_text = [&](float x, float y, const std::string& str, float scale, u32 rgba) {
        float cursor_x = x;
        for (char ch : str) {
            if (ch < 32 || ch > 126) { cursor_x += 8*scale; continue; }
            const uint8_t* glyph = font::kFont[static_cast<int>(ch)];
            for (int row=0; row<8; row++) {
                uint8_t bits = glyph[row];
                // Font is LSB-first (bit 0 = leftmost pixel), row 0 = top row.
                for (int col=0; col<8; col++) {
                    if (bits & (1 << col)) {
                        push_rect(cursor_x + col*scale, y + row*scale, scale, scale, rgba, 0);
                    }
                }
            }
            cursor_x += 8*scale;
        }
    };
    // (procedural shell removed: photo body is drawn textured)
    // Photo shell: the GBC body photo (assets/gbc_body.png, Evan-Amos public
    // domain) is drawn as a textured quad in record_command_buffer_with_gui
    // before the game. All printed artwork (Nintendo / GAME BOY COLOR /
    // SELECT / START / POWER / speaker / LEDs / screws) comes from the photo,
    // so only interactive feedback is drawn here: button press highlights,
    // the settings gear, the settings panel, and the mapping modal.
    auto body = gui.body_rect();
    // Press highlights over the photo buttons.
    for (const auto& b : gui.buttons()) {
        if (b.w <= 0) continue;
        if (!gui.is_pressed(b.key)) continue;
        if (b.is_circle) {
            push_circle(b.x + b.w * 0.5f, b.y + b.h * 0.5f, b.w * 0.5f, 0x60FFFFFF);
        } else {
            push_rect(b.x, b.y, b.w, b.h, 0x50FFFFFF, 2.0f);
        }
    }
    // ROM menu on the game screen (no ROM loaded): ROM list from rom_folder.
    if (gui.menu_active()) {
        auto scr = gui.screen_rect();
        push_rect(scr.x, scr.y, scr.w, scr.h, 0xFF101418, 0);
        const auto& roms = gui.rom_list();
        const float cx = scr.x + scr.w * 0.5f;
        push_text(cx - 5 * 4.0f, scr.y + 6, "GB4ME", 1.0f, 0xFFFFFFFF);
        const std::string sub = "SELECT ROM (" + std::to_string(roms.size()) + ")";
        push_text(cx - sub.size() * 4.0f, scr.y + 18, sub, 1.0f, 0xFFAAAAAA);
        const auto list = gui.menu_list_rect();
        const int cols = gui.menu_grid_cols();
        const float cw = gui.menu_cell_w();
        const float ch = gui.menu_cell_h();
        const int vis = gui.menu_grid_visible_count();
        const int first = gui.menu_first_visible();
        const float gap = 8.0f;
        const float badge_w = 3 * 8.0f + 6.0f;
        const float badge_h = 10.0f;
        for (int vi = 0; vi < vis; vi++) {
            const int i = first + vi;
            if (i < 0 || i >= static_cast<int>(roms.size())) break;
            const int col = vi % cols;
            const int row = vi / cols;
            const float cx = list.x + col * (cw + gap);
            const float cy = list.y + row * ch;
            // Last row may be partial — skip empty cells.
            if (cx + cw > list.x + list.w + 1.0f) continue;
            const bool sel = (i == gui.menu_selected());
            // Cell background
            push_rect(cx, cy, cw, ch, sel ? 0xFF3A3A8F : 0xFF22252B, 3.0f);
            if (sel) push_rect(cx + 1, cy + 1, cw - 2, ch - 2, 0xFF2E3190, 3.0f);
            // CGB/DMG badge — top-right inside cell
            const std::string badge = roms[static_cast<size_t>(i)].cgb ? "CGB" : "DMG";
            const u32 bcol = roms[static_cast<size_t>(i)].cgb ? 0xFF3FA06B : 0xFF5A5A5A;
            const float bx = cx + cw - badge_w - 4;
            const float by = cy + 4;
            push_rect(bx - 2, by, badge_w, badge_h, bcol, 2.0f);
            push_text(bx, by + 1, badge, 0.85f, 0xFFFFFFFF);
            // Large cartridge icon — centered top of cell
            const float icon_w = std::clamp(cw * 0.38f, 28.0f, 42.0f);
            const float icon_h = icon_w * 0.92f;
            const float ix = cx + (cw - icon_w) * 0.5f;
            const float iy = cy + 8;
            const u32 cart_body = sel ? 0xFFD6D6D6 : 0xFFB0B0B0;
            const u32 cart_top  = sel ? 0xFF9A9A9A : 0xFF6E6E6E;
            const u32 label_bg  = sel ? 0xFF1A1A1A : 0xFF252525;
            push_rect(ix, iy + 4, icon_w, icon_h, cart_body, 2.0f);
            push_rect(ix + icon_w * 0.22f, iy, icon_w * 0.56f, 5.0f, cart_top, 2.0f);
            // notch
            push_rect(ix + icon_w * 0.42f, iy + 5, icon_w * 0.16f, 2.0f, cart_top, 0);
            push_rect(ix + 3, iy + 10, icon_w - 6, icon_h - 12, label_bg, 1.0f);
            // GB emboss on label
            const float lbx = ix + (icon_w - 16) * 0.5f;
            push_rect(lbx, iy + 13, 16, 1, sel ? 0xFF4A4A4A : 0xFF3A3A3A, 0);
            // Title — centered below icon, wrapped to 2 lines max
            std::string title = roms[static_cast<size_t>(i)].title;
            // Split " - " suffix (region) from stem to keep main title larger:
            // keep full title but allow wrapping; word-aware.
            const float text_max_w = cw - 10.0f;
            const float tscale = 0.85f;
            const float char_w = 8.0f * tscale;
            size_t maxc_line = text_max_w > 4 ? static_cast<size_t>((text_max_w) / char_w) : 1;
            if (maxc_line < 4) maxc_line = 4;
            // Wrap into up to 2 lines
            auto trim = [](const std::string& s) -> std::string {
                size_t a = 0;
                while (a < s.size() && std::isspace(static_cast<unsigned char>(s[a]))) a++;
                size_t b = s.size();
                while (b > a && std::isspace(static_cast<unsigned char>(s[b - 1]))) b--;
                return s.substr(a, b - a);
            };
            std::vector<std::string> lines;
            std::string remaining = title;
            for (int line = 0; line < 2 && !remaining.empty(); line++) {
                if (remaining.size() <= maxc_line) { lines.push_back(remaining); break; }
                size_t cut = maxc_line;
                if (line == 1) {
                    // Last line truncates with ...
                    if (remaining.size() > maxc_line) {
                        if (maxc_line > 3) {
                            size_t sp = remaining.rfind(' ', maxc_line - 3);
                            if (sp != std::string::npos && sp > maxc_line / 2) remaining = remaining.substr(0, sp) + "...";
                            else remaining = remaining.substr(0, maxc_line - 3) + "...";
                        } else remaining = remaining.substr(0, maxc_line);
                    }
                    lines.push_back(remaining);
                    break;
                }
                size_t sp = remaining.rfind(' ', cut);
                if (sp != std::string::npos && sp > maxc_line / 2) {
                    lines.push_back(remaining.substr(0, sp));
                    remaining = trim(remaining.substr(sp + 1));
                } else {
                    size_t dash = remaining.rfind(" - ", cut);
                    if (dash != std::string::npos && dash > maxc_line / 2) {
                        lines.push_back(remaining.substr(0, dash));
                        remaining = trim(remaining.substr(dash + 3));
                    } else {
                        lines.push_back(remaining.substr(0, cut));
                        remaining = trim(remaining.substr(cut));
                    }
                }
            }
            const float title_y0 = iy + icon_h + 8;
            for (size_t li = 0; li < lines.size(); li++) {
                const std::string& line = lines[li];
                const float tw = line.size() * char_w;
                const float tx = cx + (cw - tw) * 0.5f;
                const float ty = title_y0 + li * 9.0f;
                if (ty + 8 > cy + ch - 4) break;
                push_text(tx, ty, line, tscale, sel ? 0xFFFFFFFF : 0xFFD8D8D8);
            }
        }
        if (roms.empty()) {
            const std::string e1 = "NO ROMS IN FOLDER";
            push_text(cx - e1.size() * 4.0f, list.y + 10, e1, 1.0f, 0xFFAAAAAA);
            std::string folder = gui.settings() ? gui.settings()->rom_folder : ".";
            if (folder.size() > 24) folder = "..." + folder.substr(folder.size() - 21);
            push_text(cx - folder.size() * 4.0f, list.y + 24, folder, 1.0f, 0xFF777777);
        }
        // scroll indicators — grid scrolls by row, show track + arrows
        if (first > 0) push_text(list.x + list.w - 12, list.y - 10, "^", 1.0f, 0xFFAAAAAA);
        if (first + vis < static_cast<int>(roms.size())) {
            push_text(list.x + list.w - 12, list.y + list.h + 2, "v", 1.0f, 0xFFAAAAAA);
        }
        // vertical scrollbar for large libraries (50+ games)
        if (static_cast<int>(roms.size()) > vis) {
            const float track_x = list.x + list.w - 4;
            const float track_y = list.y;
            const float track_h = list.h;
            push_rect(track_x, track_y, 3.0f, track_h, 0xFF1E2025, 1.0f);
            const float frac_vis = static_cast<float>(vis) / roms.size();
            const float frac_first = static_cast<float>(first) / roms.size();
            const float thumb_h = std::clamp(track_h * frac_vis, 12.0f, track_h);
            const float thumb_y = track_y + frac_first * track_h;
            const float clamped_thumb_y = std::min(thumb_y, track_y + track_h - thumb_h);
            push_rect(track_x, clamped_thumb_y, 3.0f, thumb_h, 0xFF6B6B8A, 1.0f);
        }
        // footer hints — grid navigation
        const std::string f1 = "ARROWS:MOVE ENTER:PLAY";
        const std::string f2 = "O:OPEN FILE S:SETTINGS R:RESCAN";
        push_text(cx - f1.size() * 4.0f, scr.y + scr.h - 24, f1, 1.0f, 0xFF888888);
        push_text(cx - f2.size() * 4.0f, scr.y + scr.h - 13, f2, 1.0f, 0xFF888888);
    }
    // OPEN button (file dialog, top-left) + settings gear (top-right).
    // The photo has neither; both stay clickable (also O / S keys).
    {
        auto openr = gui.open_button_rect();
        const float ocx = openr.x + openr.w * 0.5f, ocy = openr.y + openr.h * 0.5f;
        push_circle(ocx, ocy, openr.w * 0.5f, 0xFF5A5A5A);
        // folder glyph
        const float fs = openr.w / 30.0f;
        push_rect(ocx - 7 * fs, ocy - 5 * fs, 6 * fs, 3 * fs, 0xFFFFFFFF, 0);
        push_rect(ocx - 7 * fs, ocy - 3 * fs, 14 * fs, 8 * fs, 0xFFFFFFFF, 0);
        push_rect(ocx - 5 * fs, ocy - 1 * fs, 10 * fs, 2 * fs, 0xFF5A5A5A, 0);
    }
    // Settings gear (photo has none; kept clickable, also toggled by S key)
    {
        auto setr = gui.settings_button_rect();
        push_circle(setr.x + setr.w * 0.5f, setr.y + setr.h * 0.5f, setr.w * 0.5f, 0xFF5A5A5A);
        const float gs = setr.w / 28.0f;
        push_text(setr.x + setr.w * 0.5f - 4 * gs, setr.y + setr.h * 0.5f - 4 * gs,
                  "*", gs, 0xFFFFFFFF);
    }
    // Settings panel
    if (gui.show_settings()) {
        // Darken background
        push_rect(0, 0, static_cast<float>(swapchain_extent.width), static_cast<float>(swapchain_extent.height), 0xAA000000, 0);
        float panel_w = body.w * 0.85f, panel_h = body.h * 0.70f;
        float panel_x = body.x + (body.w - panel_w)*0.5f;
        float panel_y = body.y + (body.h - panel_h)*0.5f;
        push_rect(panel_x, panel_y, panel_w, panel_h, 0xFFD8D8D8, 8.0f);
        push_rect(panel_x+2, panel_y+2, panel_w-4, panel_h-4, 0xFF1A1A1A, 8.0f);
        // Title bar
        push_rect(panel_x+4, panel_y+6, panel_w-8, body.h*0.04f, 0xFF3A3A8F, 3.0f);
        push_text(panel_x+panel_w*0.5f - 28, panel_y+10, "SETTINGS", 1.0f, 0xFFFFFFFF);
        // Tabs GENERAL / INPUTS + close box (all clickable).
        {
            auto draw_tab = [&](GUIConsole::SettingsTab t, const std::string& name) {
                const auto r = gui.settings_tab_rect(t);
                const bool active = gui.settings_tab() == t;
                push_rect(r.x, r.y, r.w, r.h, active ? 0xFF3A3A8F : 0xFF2A2A2A, 2.0f);
                push_text(r.x + (r.w - name.size() * 8.0f) * 0.5f,
                          r.y + (r.h - 8.0f) * 0.5f, name, 1.0f,
                          active ? 0xFFFFFFFF : 0xFFAAAAAA);
            };
            draw_tab(GUIConsole::SettingsTab::General, "GENERAL");
            draw_tab(GUIConsole::SettingsTab::Inputs, "INPUTS");
            const auto cr = gui.settings_close_rect();
            push_rect(cr.x, cr.y, cr.w, cr.h, 0xFF6B2A2A, 2.0f);
            push_text(cr.x + (cr.w - 8.0f) * 0.5f, cr.y + (cr.h - 8.0f) * 0.5f,
                      "X", 1.0f, 0xFFFFFFFF);
        }
        // Rows of the active tab (rects from GUIConsole, values live).
        // Text scale follows the window: full-size font on large windows,
        // smaller on tiny ones; values are truncated to their cells.
        const float tscale = body.w > 420.0f ? 1.0f : 0.6f;
        const float tchar = 8.0f * tscale;
        const auto fit = [&](const std::string& s, float w) {
            size_t maxc = w > tchar + 4.0f ? static_cast<size_t>((w - 4.0f) / tchar) : 1;
            if (s.size() <= maxc) return s;
            if (maxc <= 3) return s.substr(0, maxc);
            return std::string("...") + s.substr(s.size() - (maxc - 3));
        };
        for (const auto& row : gui.settings_rows()) {
            push_rect(row.label_rect.x, row.label_rect.y, row.label_rect.w,
                      row.label_rect.h, 0xFF3A3A3A, 2.0f);
            push_rect(row.value_rect.x, row.value_rect.y, row.value_rect.w,
                      row.value_rect.h, 0xFF2A2A2A, 2.0f);
            if (row.id == "volume") {
                const float vol = gui.settings() ? gui.settings()->volume : 1.0f;
                if (vol > 0.0f) {
                    push_rect(row.value_rect.x + 2, row.value_rect.y + 2,
                              (row.value_rect.w - 4) * vol, row.value_rect.h - 4,
                              0xFF3FA06B, 2.0f);
                }
                // Slider knob at the fill edge.
                const float kx = row.value_rect.x + 2 + (row.value_rect.w - 4) * vol;
                push_rect(kx - 3, row.value_rect.y + 1, 6, row.value_rect.h - 2,
                          0xFFFFFFFF, 2.0f);
            }
            // Highlight the input row currently awaiting a remap key.
            if (row.id.rfind("map", 0) == 0 && gui.awaiting_input() &&
                gui.pending_key()) {
                const int idx = std::stoi(row.id.substr(3));
                if (idx == static_cast<int>(*gui.pending_key())) {
                    push_rect(row.value_rect.x, row.value_rect.y, row.value_rect.w,
                              row.value_rect.h, 0x806B3FA0, 2.0f);
                }
            }
            push_text(row.label_rect.x + 4,
                      row.label_rect.y + (row.label_rect.h - 8.0f * tscale) * 0.5f,
                      fit(row.label, row.label_rect.w), tscale, 0xFFFFFFFF);
            push_text(row.value_rect.x + 4,
                      row.value_rect.y + (row.value_rect.h - 8.0f * tscale) * 0.5f,
                      fit(row.value, row.value_rect.w), tscale, 0xFFFFFFFF);
        }
        // Bottom hint
        push_rect(panel_x+6, panel_y+panel_h-20, panel_w-12, 14, 0xFF333333, 2.0f);
        push_text(panel_x+8, panel_y+panel_h-18, "CLICK ROW - DRAG VOLUME - L/R:VOL - S/ESC:CLOSE", 0.5f, 0xFFCCCCCC);
    }
    // Mapping modal overlay (on top of settings if both)
    if (gui.awaiting_input()) {
        // semi-transparent dark overlay
        push_rect(0, 0, static_cast<float>(swapchain_extent.width), static_cast<float>(swapchain_extent.height), 0xAA000000, 0);
        float modal_w = body.w * 0.6f, modal_h = body.h * 0.12f;
        float modal_x = body.x + (body.w - modal_w)*0.5f;
        float modal_y = body.y + (body.h - modal_h)*0.5f;
        push_rect(modal_x, modal_y, modal_w, modal_h, 0xFFFFFFFF, 6.0f);
        push_rect(modal_x+2, modal_y+2, modal_w-4, modal_h-4, 0xFF202020, 6.0f);
        // Prompt text: which button is being remapped + how to cancel.
        // (Previously the modal was a blank box, so users had no idea it was
        // waiting for a keypress and their keys were silently remapped.)
        std::string who = "button";
        if (gui.pending_key()) {
            switch (*gui.pending_key()) {
                case Joypad::Key::Up: who = "Up"; break;
                case Joypad::Key::Down: who = "Down"; break;
                case Joypad::Key::Left: who = "Left"; break;
                case Joypad::Key::Right: who = "Right"; break;
                case Joypad::Key::A: who = "A"; break;
                case Joypad::Key::B: who = "B"; break;
                case Joypad::Key::Start: who = "Start"; break;
                case Joypad::Key::Select: who = "Select"; break;
            }
        }
        const std::string line1 = "Press a key for " + who;
        const std::string line2 = "Esc cancels";
        const float cx = modal_x + modal_w * 0.5f;
        const float cy = modal_y + modal_h * 0.5f;
        push_text(cx - line1.size() * 4.0f, cy - 13, line1, 1.0f, 0xFFFFFFFF);
        push_text(cx - line2.size() * 4.0f, cy + 5, line2, 1.0f, 0xFFAAAAAA);
    }
    // Upload
    void* vdst = nullptr;
    vkMapMemory(device, gui_vertex_buffer_memory, 0, verts.size()*sizeof(GuiVertex), 0, &vdst);
    std::memcpy(vdst, verts.data(), verts.size()*sizeof(GuiVertex));
    vkUnmapMemory(device, gui_vertex_buffer_memory);
    void* idst = nullptr;
    vkMapMemory(device, gui_index_buffer_memory, 0, indices.size()*sizeof(u16), 0, &idst);
    std::memcpy(idst, indices.data(), indices.size()*sizeof(u16));
    vkUnmapMemory(device, gui_index_buffer_memory);
    gui_index_count = static_cast<u32>(indices.size());
}

void VulkanRenderer::record_command_buffer_with_gui(VkCommandBuffer cmd, u32 image_index, const GUIConsole& gui) {
    VkCommandBufferBeginInfo begin{}; begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO; vkBeginCommandBuffer(cmd, &begin);
    // Upload game texture as before
    const bool first_upload = (texture_layout_ == VK_IMAGE_LAYOUT_UNDEFINED);
    VkImageMemoryBarrier to_transfer{}; to_transfer.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER; to_transfer.oldLayout = texture_layout_; to_transfer.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL; to_transfer.srcAccessMask = first_upload ? 0 : VK_ACCESS_SHADER_READ_BIT; to_transfer.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT; to_transfer.image = texture_image; to_transfer.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT; to_transfer.subresourceRange.levelCount = 1; to_transfer.subresourceRange.layerCount = 1;
    vkCmdPipelineBarrier(cmd, first_upload ? VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT : VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0,nullptr,0,nullptr,1,&to_transfer);
    VkBufferImageCopy region{}; region.imageExtent = {kTexWidth, kTexHeight, 1}; region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT; region.imageSubresource.layerCount = 1; vkCmdCopyBufferToImage(cmd, texture_buffer, texture_image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
    VkImageMemoryBarrier to_shader = to_transfer; to_shader.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL; to_shader.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL; to_shader.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT; to_shader.dstAccessMask = VK_ACCESS_SHADER_READ_BIT; vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0,0,nullptr,0,nullptr,1,&to_shader); texture_layout_ = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    VkClearValue clear{}; // Body color as clear? Use dark gray, but GBC body will be drawn as rect, so clear to black is fine.
    clear.color = {{0.08f, 0.08f, 0.08f, 1.0f}};
    VkRenderPassBeginInfo rp{}; rp.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO; rp.renderPass = render_pass; rp.framebuffer = swapchain_framebuffers[image_index]; rp.renderArea.extent = swapchain_extent; rp.clearValueCount = 1; rp.pClearValues = &clear;
    vkCmdBeginRenderPass(cmd, &rp, VK_SUBPASS_CONTENTS_INLINE);
    // Draw order (back to front): photo shell, game screen, GUI overlay.
    // (HiDPI: GUI coords are window pixels; scale to swapchain pixels.)
    int win_w = gui.window_w(), win_h = gui.window_h();
    const float scale_x = swapchain_extent.width / static_cast<float>(win_w);
    const float scale_y = swapchain_extent.height / static_cast<float>(win_h);
    // 1) Photo shell body (textured fullscreen quad clipped to body rect).
    if (body_photo_ok_ && !body_descriptor_sets.empty()) {
        auto bd = gui.body_rect();
        VkViewport vp_body{};
        vp_body.x = bd.x * scale_x; vp_body.y = bd.y * scale_y;
        vp_body.width = bd.w * scale_x; vp_body.height = bd.h * scale_y;
        vp_body.minDepth = 0; vp_body.maxDepth = 1;
        vkCmdSetViewport(cmd, 0, 1, &vp_body);
        VkRect2D sc_body{};
        sc_body.offset = {static_cast<i32>(vp_body.x), static_cast<i32>(vp_body.y)};
        sc_body.extent = {static_cast<u32>(vp_body.width), static_cast<u32>(vp_body.height)};
        vkCmdSetScissor(cmd, 0, 1, &sc_body);
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, graphics_pipeline);
        VkBuffer vbs[1] = {vertex_buffer}; VkDeviceSize offs[1] = {0};
        vkCmdBindVertexBuffers(cmd, 0, 1, vbs, offs);
        vkCmdBindIndexBuffer(cmd, index_buffer, 0, VK_INDEX_TYPE_UINT16);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_layout, 0, 1, &body_descriptor_sets[0], 0, nullptr);
        vkCmdDrawIndexed(cmd, 6, 1, 0, 0, 0);
    }
    // 2) Game texture inside the photo's LCD rect.
    auto scr = gui.screen_rect();
    VkViewport vp_game{};
    vp_game.x = scr.x * scale_x;
    vp_game.y = scr.y * scale_y;
    vp_game.width = scr.w * scale_x;
    vp_game.height = scr.h * scale_y;
    vp_game.minDepth = 0; vp_game.maxDepth = 1;
    vkCmdSetViewport(cmd, 0, 1, &vp_game);
    VkRect2D sc_game{};
    sc_game.offset = {static_cast<i32>(vp_game.x), static_cast<i32>(vp_game.y)};
    sc_game.extent = {static_cast<u32>(vp_game.width), static_cast<u32>(vp_game.height)};
    vkCmdSetScissor(cmd, 0, 1, &sc_game);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, graphics_pipeline);
    VkBuffer vbs[1] = {vertex_buffer}; VkDeviceSize offs[1]={0}; vkCmdBindVertexBuffers(cmd,0,1,vbs,offs); vkCmdBindIndexBuffer(cmd, index_buffer, 0, VK_INDEX_TYPE_UINT16); vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_layout, 0,1,&descriptor_sets[current_frame],0,nullptr);
    vkCmdDrawIndexed(cmd, 6,1,0,0,0);
    // 3) GUI overlay full-window: press highlights, gear, settings, modal.
    VkViewport vp_full{}; vp_full.x=0; vp_full.y=0; vp_full.width=static_cast<float>(swapchain_extent.width); vp_full.height=static_cast<float>(swapchain_extent.height); vp_full.minDepth=0; vp_full.maxDepth=1; vkCmdSetViewport(cmd, 0,1,&vp_full);
    VkRect2D sc_full{}; sc_full.extent = swapchain_extent; vkCmdSetScissor(cmd, 0,1,&sc_full);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, gui_pipeline);
    VkBuffer gvbs[1] = {gui_vertex_buffer}; VkDeviceSize goffs[1]={0}; vkCmdBindVertexBuffers(cmd,0,1,gvbs,goffs); vkCmdBindIndexBuffer(cmd, gui_index_buffer, 0, VK_INDEX_TYPE_UINT16);
    vkCmdDrawIndexed(cmd, gui_index_count, 1, 0,0,0);
    vkCmdEndRenderPass(cmd);
    vkEndCommandBuffer(cmd);
}

void VulkanRenderer::render_with_gui(const PPU::FrameData& frame, const GUIConsole& gui) {
    vkWaitForFences(device, 1, &in_flight_fences[current_frame], VK_TRUE, std::numeric_limits<u64>::max());
    u32 image_index = 0;
    VkResult acq = vkAcquireNextImageKHR(device, swapchain, std::numeric_limits<u64>::max(), image_available_semaphores[current_frame], VK_NULL_HANDLE, &image_index);
    if (acq == VK_ERROR_OUT_OF_DATE_KHR) { recreate_swapchain(); return; }
    vkResetFences(device, 1, &in_flight_fences[current_frame]);
    void* dst = nullptr;
    vkMapMemory(device, texture_buffer_memory, 0, kTexWidth*kTexHeight*4, 0, &dst);
    std::memcpy(dst, frame.pixels.data(), kTexWidth*kTexHeight*4);
    vkUnmapMemory(device, texture_buffer_memory);
    update_uniform_buffer(current_frame);
    update_gui_buffers(gui);
    vkResetCommandBuffer(command_buffers[current_frame], 0);
    record_command_buffer_with_gui(command_buffers[current_frame], image_index, gui);
    VkSemaphore wait[1] = {image_available_semaphores[current_frame]};
    VkPipelineStageFlags stages[1] = {VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT};
    VkSemaphore signal[1] = {render_finished_semaphores[current_frame]};
    VkSubmitInfo submit{}; submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO; submit.waitSemaphoreCount = 1; submit.pWaitSemaphores = wait; submit.pWaitDstStageMask = stages; submit.commandBufferCount = 1; submit.pCommandBuffers = &command_buffers[current_frame]; submit.signalSemaphoreCount = 1; submit.pSignalSemaphores = signal;
    vkQueueSubmit(graphics_queue, 1, &submit, in_flight_fences[current_frame]);
    VkPresentInfoKHR present{}; present.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR; present.waitSemaphoreCount = 1; present.pWaitSemaphores = signal; present.swapchainCount = 1; present.pSwapchains = &swapchain; present.pImageIndices = &image_index;
    VkResult res = vkQueuePresentKHR(present_queue, &present);
    if (res == VK_ERROR_OUT_OF_DATE_KHR || res == VK_SUBOPTIMAL_KHR) recreate_swapchain();
    current_frame = (current_frame + 1) % max_frames_in_flight;
}

void VulkanRenderer::record_command_buffer_menu(VkCommandBuffer cmd, u32 image_index, const GUIConsole& gui) {
    VkCommandBufferBeginInfo begin{}; begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO; vkBeginCommandBuffer(cmd, &begin);
    VkClearValue clear{};
    clear.color = {{0.08f, 0.08f, 0.08f, 1.0f}};
    VkRenderPassBeginInfo rp{}; rp.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO; rp.renderPass = render_pass; rp.framebuffer = swapchain_framebuffers[image_index]; rp.renderArea.extent = swapchain_extent; rp.clearValueCount = 1; rp.pClearValues = &clear;
    vkCmdBeginRenderPass(cmd, &rp, VK_SUBPASS_CONTENTS_INLINE);
    // Draw order: photo shell, then GUI overlay (menu list is part of it).
    // (HiDPI: GUI coords are window pixels; scale to swapchain pixels.)
    int win_w = gui.window_w(), win_h = gui.window_h();
    const float scale_x = swapchain_extent.width / static_cast<float>(win_w);
    const float scale_y = swapchain_extent.height / static_cast<float>(win_h);
    // 1) Photo shell body (textured quad clipped to body rect).
    if (body_photo_ok_ && !body_descriptor_sets.empty()) {
        auto bd = gui.body_rect();
        VkViewport vp_body{};
        vp_body.x = bd.x * scale_x; vp_body.y = bd.y * scale_y;
        vp_body.width = bd.w * scale_x; vp_body.height = bd.h * scale_y;
        vp_body.minDepth = 0; vp_body.maxDepth = 1;
        vkCmdSetViewport(cmd, 0, 1, &vp_body);
        VkRect2D sc_body{};
        sc_body.offset = {static_cast<i32>(vp_body.x), static_cast<i32>(vp_body.y)};
        sc_body.extent = {static_cast<u32>(vp_body.width), static_cast<u32>(vp_body.height)};
        vkCmdSetScissor(cmd, 0, 1, &sc_body);
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, graphics_pipeline);
        VkBuffer vbs[1] = {vertex_buffer}; VkDeviceSize offs[1] = {0};
        vkCmdBindVertexBuffers(cmd, 0, 1, vbs, offs);
        vkCmdBindIndexBuffer(cmd, index_buffer, 0, VK_INDEX_TYPE_UINT16);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_layout, 0, 1, &body_descriptor_sets[0], 0, nullptr);
        vkCmdDrawIndexed(cmd, 6, 1, 0, 0, 0);
    }
    // 2) GUI overlay full-window: menu, buttons, settings, modal.
    VkViewport vp_full{}; vp_full.x=0; vp_full.y=0; vp_full.width=static_cast<float>(swapchain_extent.width); vp_full.height=static_cast<float>(swapchain_extent.height); vp_full.minDepth=0; vp_full.maxDepth=1; vkCmdSetViewport(cmd, 0,1,&vp_full);
    VkRect2D sc_full{}; sc_full.extent = swapchain_extent; vkCmdSetScissor(cmd, 0,1,&sc_full);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, gui_pipeline);
    VkBuffer gvbs[1] = {gui_vertex_buffer}; VkDeviceSize goffs[1]={0}; vkCmdBindVertexBuffers(cmd,0,1,gvbs,goffs); vkCmdBindIndexBuffer(cmd, gui_index_buffer, 0, VK_INDEX_TYPE_UINT16);
    vkCmdDrawIndexed(cmd, gui_index_count, 1, 0,0,0);
    vkCmdEndRenderPass(cmd);
    vkEndCommandBuffer(cmd);
}

void VulkanRenderer::render_menu(const GUIConsole& gui) {
    vkWaitForFences(device, 1, &in_flight_fences[current_frame], VK_TRUE, std::numeric_limits<u64>::max());
    u32 image_index = 0;
    VkResult acq = vkAcquireNextImageKHR(device, swapchain, std::numeric_limits<u64>::max(), image_available_semaphores[current_frame], VK_NULL_HANDLE, &image_index);
    if (acq == VK_ERROR_OUT_OF_DATE_KHR) { recreate_swapchain(); return; }
    vkResetFences(device, 1, &in_flight_fences[current_frame]);
    update_uniform_buffer(current_frame);
    update_gui_buffers(gui);
    vkResetCommandBuffer(command_buffers[current_frame], 0);
    record_command_buffer_menu(command_buffers[current_frame], image_index, gui);
    VkSemaphore wait[1] = {image_available_semaphores[current_frame]};
    VkPipelineStageFlags stages[1] = {VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT};
    VkSemaphore signal[1] = {render_finished_semaphores[current_frame]};
    VkSubmitInfo submit{}; submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO; submit.waitSemaphoreCount = 1; submit.pWaitSemaphores = wait; submit.pWaitDstStageMask = stages; submit.commandBufferCount = 1; submit.pCommandBuffers = &command_buffers[current_frame]; submit.signalSemaphoreCount = 1; submit.pSignalSemaphores = signal;
    vkQueueSubmit(graphics_queue, 1, &submit, in_flight_fences[current_frame]);
    VkPresentInfoKHR present{}; present.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR; present.waitSemaphoreCount = 1; present.pWaitSemaphores = signal; present.swapchainCount = 1; present.pSwapchains = &swapchain; present.pImageIndices = &image_index;
    VkResult res = vkQueuePresentKHR(present_queue, &present);
    if (res == VK_ERROR_OUT_OF_DATE_KHR || res == VK_SUBOPTIMAL_KHR) recreate_swapchain();
    current_frame = (current_frame + 1) % max_frames_in_flight;
}

std::vector<char> VulkanRenderer::read_file(const std::string& filename) {
    std::ifstream file(filename, std::ios::ate | std::ios::binary);
    if (!file) return {};
    const auto size = file.tellg();
    std::vector<char> bytes(static_cast<size_t>(size));
    file.seekg(0);
    file.read(bytes.data(), size);
    return bytes;
}

VkShaderModule VulkanRenderer::create_shader_module(VkDevice dev,
                                                    const std::vector<char>& code) {
    VkShaderModuleCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    info.codeSize = code.size();
    info.pCode = reinterpret_cast<const u32*>(code.data());
    VkShaderModule mod{};
    vkCreateShaderModule(dev, &info, nullptr, &mod);
    return mod;
}

bool VulkanRenderer::check_validation_layer_support() {
    return false;  // Validation layers off for now; enable with VK_LAYER_KHRONOS_validation.
}

VulkanRenderer::QueueFamilyIndices VulkanRenderer::find_queue_families(VkPhysicalDevice dev,
                                                                      VkSurfaceKHR surf) {
    QueueFamilyIndices idx;
    u32 count = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(dev, &count, nullptr);
    std::vector<VkQueueFamilyProperties> props(count);
    vkGetPhysicalDeviceQueueFamilyProperties(dev, &count, props.data());
    for (u32 i = 0; i < count; i++) {
        if (props[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) idx.graphics_family = i;
        VkBool32 present = VK_FALSE;
        vkGetPhysicalDeviceSurfaceSupportKHR(dev, i, surf, &present);
        if (present) idx.present_family = i;
        if (idx.is_complete()) break;
    }
    return idx;
}

VulkanRenderer::SwapChainSupportDetails VulkanRenderer::query_swapchain_support(
    VkPhysicalDevice dev, VkSurfaceKHR surf) {
    SwapChainSupportDetails details{};
    vkGetPhysicalDeviceSurfaceCapabilitiesKHR(dev, surf, &details.capabilities);
    u32 count = 0;
    vkGetPhysicalDeviceSurfaceFormatsKHR(dev, surf, &count, nullptr);
    if (count != 0) {
        details.formats.resize(count);
        vkGetPhysicalDeviceSurfaceFormatsKHR(dev, surf, &count, details.formats.data());
    }
    vkGetPhysicalDeviceSurfacePresentModesKHR(dev, surf, &count, nullptr);
    if (count != 0) {
        details.present_modes.resize(count);
        vkGetPhysicalDeviceSurfacePresentModesKHR(dev, surf, &count,
                                                 details.present_modes.data());
    }
    return details;
}

VkSurfaceFormatKHR VulkanRenderer::choose_swap_surface_format(
    const std::vector<VkSurfaceFormatKHR>& available) {
    for (const auto& f : available) {
        if (f.format == VK_FORMAT_B8G8R8A8_SRGB &&
            f.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) {
            return f;
        }
    }
    return available[0];
}

VkPresentModeKHR VulkanRenderer::choose_swap_present_mode(
    const std::vector<VkPresentModeKHR>& available) {
    for (const auto& m : available) {
        if (m == VK_PRESENT_MODE_MAILBOX_KHR) return m;
    }
    return VK_PRESENT_MODE_FIFO_KHR;
}

VkExtent2D VulkanRenderer::choose_swap_extent(const VkSurfaceCapabilitiesKHR& caps,
                                              SDL_Window* win) {
    if (caps.currentExtent.width != std::numeric_limits<u32>::max()) {
        return caps.currentExtent;
    }
    int w = 0, h = 0;
    SDL_GetWindowSizeInPixels(win, &w, &h);
    VkExtent2D extent{static_cast<u32>(w), static_cast<u32>(h)};
    extent.width = std::clamp(extent.width, caps.minImageExtent.width,
                              caps.maxImageExtent.width);
    extent.height = std::clamp(extent.height, caps.minImageExtent.height,
                               caps.maxImageExtent.height);
    return extent;
}

VKAPI_ATTR VkBool32 VKAPI_CALL VulkanRenderer::debug_callback(
    VkDebugUtilsMessageSeverityFlagBitsEXT /*severity*/,
    VkDebugUtilsMessageTypeFlagsEXT /*types*/,
    const VkDebugUtilsMessengerCallbackDataEXT* /*data*/, void* /*user*/) {
    return VK_FALSE;
}

}  // namespace gb
