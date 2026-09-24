// Minimal Vulkan renderer: uploads the 160x144 GB framebuffer to a texture
// and draws a fullscreen quad. One pipeline, one texture update per frame.
// https://wiki.libsdl.org/SDL3/SDL_Vulkan_CreateSurface

#include "gb/vulkan_renderer.h"

#include "gb/font.h"
#include "gb/gui_console.h"
#include <SDL3/SDL_vulkan.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <limits>
#include <string>
#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"

namespace gb
{

namespace
{

constexpr u32 kTexWidth = SCREEN_WIDTH;
constexpr u32 kTexHeight = SCREEN_HEIGHT;
constexpr VkFormat kTexFormat = VK_FORMAT_B8G8R8A8_SRGB;

u32 find_memory_type(VkPhysicalDevice phys, u32 filter, VkMemoryPropertyFlags props)
{
	VkPhysicalDeviceMemoryProperties mem{};
	vkGetPhysicalDeviceMemoryProperties(phys, &mem);
	for (u32 i = 0; i < mem.memoryTypeCount; i++) {
		if ((filter & (1u << i)) && (mem.memoryTypes[i].propertyFlags & props) == props) {
			return i;
		}
	}
	return std::numeric_limits<u32>::max();
}

bool create_buffer(VkPhysicalDevice phys, VkDevice dev, VkDeviceSize size, VkBufferUsageFlags usage,
				   VkMemoryPropertyFlags props, VkBuffer &buf, VkDeviceMemory &mem)
{
	VkBufferCreateInfo info{};
	info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
	info.size = size;
	info.usage = usage;
	info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
	if (vkCreateBuffer(dev, &info, nullptr, &buf) != VK_SUCCESS)
		return false;
	VkMemoryRequirements req{};
	vkGetBufferMemoryRequirements(dev, buf, &req);
	const u32 idx = find_memory_type(phys, req.memoryTypeBits, props);
	if (idx == std::numeric_limits<u32>::max())
		return false;
	VkMemoryAllocateInfo alloc{};
	alloc.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
	alloc.allocationSize = req.size;
	alloc.memoryTypeIndex = idx;
	if (vkAllocateMemory(dev, &alloc, nullptr, &mem) != VK_SUCCESS)
		return false;
	vkBindBufferMemory(dev, buf, mem, 0);
	return true;
}

VkCommandBuffer begin_single_time(VkDevice dev, VkCommandPool pool)
{
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

void end_single_time(VkDevice dev, VkCommandPool pool, VkQueue queue, VkCommandBuffer cmd)
{
	vkEndCommandBuffer(cmd);
	VkSubmitInfo submit{};
	submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
	submit.commandBufferCount = 1;
	submit.pCommandBuffers = &cmd;
	vkQueueSubmit(queue, 1, &submit, VK_NULL_HANDLE);
	vkQueueWaitIdle(queue);
	vkFreeCommandBuffers(dev, pool, 1, &cmd);
}

} // namespace

VulkanRenderer::VulkanRenderer() = default;

VulkanRenderer::~VulkanRenderer()
{
	shutdown();
}

bool VulkanRenderer::initialize(SDL_Window *win)
{
	window = win;
	if (!create_instance())
		return false;
	if (!create_surface())
		return false;
	if (!pick_physical_device())
		return false;
	if (!create_logical_device())
		return false;
	if (!create_swapchain())
		return false;
	if (!create_image_views())
		return false;
	if (!create_render_pass())
		return false;
	if (!create_descriptor_set_layout())
		return false;
	if (!create_graphics_pipeline())
		return false;
	if (!create_framebuffers())
		return false;
	if (!create_command_pool())
		return false;
	if (!create_texture(kTexWidth, kTexHeight))
		return false;
	// Photo shell is best-effort: without it the game still runs on the
	// clear color with overlay UI.
	body_photo_ok_ = create_body_texture();
	if (!body_photo_ok_)
		SDL_Log("Body photo missing; running without shell texture");
	if (!create_vertex_buffer())
		return false;
	if (!create_index_buffer())
		return false;
	if (!create_uniform_buffers())
		return false;
	if (!create_gui_buffers())
		return false;
	if (!create_gui_pipeline())
		return false;
	if (!create_descriptor_pool())
		return false;
	if (!create_descriptor_sets())
		return false;
	if (!create_body_photo_descriptors())
		return false;
	if (!create_command_buffers())
		return false;
	if (!create_sync_objects())
		return false;
	return true;
}

void VulkanRenderer::shutdown()
{
	if (device == VK_NULL_HANDLE)
		return;
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
	for (auto fb : swapchain_framebuffers)
		vkDestroyFramebuffer(device, fb, nullptr);
	swapchain_framebuffers.clear();
	vkDestroyPipeline(device, gui_pipeline, nullptr);
	vkDestroyPipelineLayout(device, gui_pipeline_layout, nullptr);
	vkDestroyPipeline(device, graphics_pipeline, nullptr);
	vkDestroyPipelineLayout(device, pipeline_layout, nullptr);
	vkDestroyRenderPass(device, render_pass, nullptr);
	for (auto view : swapchain_image_views)
		vkDestroyImageView(device, view, nullptr);
	swapchain_image_views.clear();
	vkDestroySwapchainKHR(device, swapchain, nullptr);
	destroy_texture();
	// Body texture resources (pool/sets/layout stay VK_NULL_HANDLE unless the
	// textured-body path is enabled; destroying null handles is a no-op).
	vkDestroySampler(device, body_sampler, nullptr);
	vkDestroyImageView(device, body_image_view, nullptr);
	vkDestroyImage(device, body_image, nullptr);
	vkFreeMemory(device, body_image_memory, nullptr);
	vkDestroyDescriptorPool(device, body_descriptor_pool, nullptr);
	vkDestroyDescriptorSetLayout(device, body_descriptor_set_layout, nullptr);
	destroy_gui_buffers();
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

void VulkanRenderer::wait_idle()
{
	if (device != VK_NULL_HANDLE)
		vkDeviceWaitIdle(device);
}

void VulkanRenderer::resize(u32 /*width*/, u32 /*height*/)
{
	recreate_swapchain();
}

bool VulkanRenderer::create_instance()
{
	VkApplicationInfo app{};
	app.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
	app.pApplicationName = "GB4ME";
	app.applicationVersion = VK_MAKE_VERSION(1, 0, 0);
	app.apiVersion = VK_API_VERSION_1_0;

	u32 ext_count = 0;
	const char *const *exts = SDL_Vulkan_GetInstanceExtensions(&ext_count);
	if (!exts)
		return false;

	VkInstanceCreateInfo info{};
	info.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
	info.pApplicationInfo = &app;
	info.enabledExtensionCount = ext_count;
	info.ppEnabledExtensionNames = exts;
	return vkCreateInstance(&info, nullptr, &instance) == VK_SUCCESS;
}

bool VulkanRenderer::create_surface()
{
	return SDL_Vulkan_CreateSurface(window, instance, nullptr, &surface);
}

bool VulkanRenderer::pick_physical_device()
{
	u32 count = 0;
	vkEnumeratePhysicalDevices(instance, &count, nullptr);
	if (count == 0)
		return false;
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

bool VulkanRenderer::create_logical_device()
{
	const QueueFamilyIndices idx = find_queue_families(physical_device, surface);
	std::vector<VkDeviceQueueCreateInfo> queues;
	const float prio = 1.0f;
	u32 families[2] = {idx.graphics_family.value(), idx.present_family.value()};
	for (u32 f : families) {
		bool dup = false;
		for (const auto &q : queues) {
			if (q.queueFamilyIndex == f)
				dup = true;
		}
		if (dup)
			continue;
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
	const char *swap_ext = VK_KHR_SWAPCHAIN_EXTENSION_NAME;
	info.enabledExtensionCount = 1;
	info.ppEnabledExtensionNames = &swap_ext;
	if (vkCreateDevice(physical_device, &info, nullptr, &device) != VK_SUCCESS) {
		return false;
	}
	vkGetDeviceQueue(device, idx.graphics_family.value(), 0, &graphics_queue);
	vkGetDeviceQueue(device, idx.present_family.value(), 0, &present_queue);
	return true;
}

bool VulkanRenderer::create_swapchain()
{
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

bool VulkanRenderer::create_image_views()
{
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

bool VulkanRenderer::create_render_pass()
{
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

bool VulkanRenderer::create_descriptor_set_layout()
{
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

bool VulkanRenderer::create_graphics_pipeline()
{
	// Resolve shaders relative to the executable first (SDL_GetBasePath),
	// then relative to the working directory, so `./GB4ME rom.gb` works
	// from anywhere.
	const char *base = SDL_GetBasePath();
	const std::string base_dir = base ? base : "";
	const std::string candidates[5] = {
		base_dir + "shaders/quad.vert.spv",
		base_dir + "../share/GB4ME/shaders/quad.vert.spv",
		"/usr/share/GB4ME/shaders/quad.vert.spv",
		"shaders/quad.vert.spv",
		"bin/shaders/quad.vert.spv",
	};
	std::vector<char> vert, frag;
	for (const auto &c : candidates) {
		vert = read_file(c);
		if (vert.empty())
			continue;
		std::string f = c;
		f.replace(f.size() - 8, 8, "frag.spv"); // quad.vert.spv -> quad.frag.spv
		frag = read_file(f);
		if (!frag.empty())
			break;
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
	const VkDynamicState dyn_states[2] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
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
	// The filter/grid state is per-draw and tiny, so it rides in push
	// constants. The GUI pipeline uses its own layout and is unaffected.
	VkPushConstantRange push{};
	push.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
	push.offset = 0;
	push.size = sizeof(VideoPush);
	layout.pushConstantRangeCount = 1;
	layout.pPushConstantRanges = &push;
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
	const bool ok = vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &pipe, nullptr,
											  &graphics_pipeline) == VK_SUCCESS;
	vkDestroyShaderModule(device, vert_mod, nullptr);
	vkDestroyShaderModule(device, frag_mod, nullptr);
	return ok;
}

bool VulkanRenderer::create_gui_buffers()
{
	// Generous initial sizes (typical menus use ~11k verts); growth in
	// ensure_gui_buffers() covers huge libraries.
	gui_vert_cap_ = 32768;
	gui_idx_cap_ = 65536;
	const VkDeviceSize vsize = static_cast<VkDeviceSize>(gui_vert_cap_) * sizeof(GuiVertex);
	const VkDeviceSize isize = static_cast<VkDeviceSize>(gui_idx_cap_) * sizeof(u32);
	if (!create_buffer(physical_device, device, vsize, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
					   VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
					   gui_vertex_buffer, gui_vertex_buffer_memory)) {
		return false;
	}
	if (!create_buffer(physical_device, device, isize, VK_BUFFER_USAGE_INDEX_BUFFER_BIT,
					   VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
					   gui_index_buffer, gui_index_buffer_memory)) {
		return false;
	}
	return true;
}

void VulkanRenderer::destroy_gui_buffers()
{
	if (gui_vertex_buffer != VK_NULL_HANDLE) {
		vkDestroyBuffer(device, gui_vertex_buffer, nullptr);
		gui_vertex_buffer = VK_NULL_HANDLE;
	}
	if (gui_vertex_buffer_memory != VK_NULL_HANDLE) {
		vkFreeMemory(device, gui_vertex_buffer_memory, nullptr);
		gui_vertex_buffer_memory = VK_NULL_HANDLE;
	}
	if (gui_index_buffer != VK_NULL_HANDLE) {
		vkDestroyBuffer(device, gui_index_buffer, nullptr);
		gui_index_buffer = VK_NULL_HANDLE;
	}
	if (gui_index_buffer_memory != VK_NULL_HANDLE) {
		vkFreeMemory(device, gui_index_buffer_memory, nullptr);
		gui_index_buffer_memory = VK_NULL_HANDLE;
	}
	gui_vert_cap_ = 0;
	gui_idx_cap_ = 0;
}

bool VulkanRenderer::ensure_gui_buffers(u32 verts, u32 indices)
{
	if (verts <= gui_vert_cap_ && indices <= gui_idx_cap_)
		return true;
	u32 nv = std::max(gui_vert_cap_ * 2u, verts);
	u32 ni = std::max(gui_idx_cap_ * 2u, indices);
	if (nv < 32768)
		nv = 32768;
	if (ni < 65536)
		ni = 65536;
	wait_idle();
	destroy_gui_buffers();
	gui_vert_cap_ = nv;
	gui_idx_cap_ = ni;
	const VkDeviceSize vsize = static_cast<VkDeviceSize>(nv) * sizeof(GuiVertex);
	const VkDeviceSize isize = static_cast<VkDeviceSize>(ni) * sizeof(u32);
	if (!create_buffer(physical_device, device, vsize, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
					   VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
					   gui_vertex_buffer, gui_vertex_buffer_memory)) {
		return false;
	}
	if (!create_buffer(physical_device, device, isize, VK_BUFFER_USAGE_INDEX_BUFFER_BIT,
					   VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
					   gui_index_buffer, gui_index_buffer_memory)) {
		return false;
	}
	return true;
}

bool VulkanRenderer::create_gui_pipeline()
{
	auto try_gui = [&](const std::string &base) -> std::pair<std::vector<char>, std::vector<char>> {
		auto v = read_file(base + "gui.vert.spv");
		auto f = read_file(base + "gui.frag.spv");
		return {std::move(v), std::move(f)};
	};
	const char *sdl_base = SDL_GetBasePath();
	std::string base_dir = sdl_base ? sdl_base : "";
	const std::string gui_bases[6] = {"shaders/",
									  "bin/shaders/",
									  base_dir + "shaders/",
									  base_dir + "../share/GB4ME/shaders/",
									  "/usr/share/GB4ME/shaders/",
									  "/usr/local/share/GB4ME/shaders/"};
	std::vector<char> vert, frag;
	for (auto &b : gui_bases) {
		auto p = try_gui(b);
		if (!p.first.empty() && !p.second.empty()) {
			vert = std::move(p.first);
			frag = std::move(p.second);
			break;
		}
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
		// ... pipeline creation using vmod/fmod (duplicated below); to avoid duplication, just
		// retry loading via base path logic
		vkDestroyShaderModule(device, vmod, nullptr);
		vkDestroyShaderModule(device, fmod, nullptr);
		// Fallback not yet implemented as pipeline; try primary again via absolute path
		const char *base = SDL_GetBasePath();
		std::string b = base ? base : "";
		const auto vert3 = read_file(b + "shaders/gui.vert.spv");
		const auto frag3 = read_file(b + "shaders/gui.frag.spv");
		if (vert3.empty() || frag3.empty())
			return false;
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
		attrs[0].location = 0;
		attrs[0].format = VK_FORMAT_R32G32_SFLOAT;
		attrs[0].offset = offsetof(GuiVertex, pos);
		attrs[1].location = 1;
		attrs[1].format = VK_FORMAT_R32G32B32A32_SFLOAT;
		attrs[1].offset = offsetof(GuiVertex, color);
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
		const VkDynamicState dyn_states[2] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
		VkPipelineDynamicStateCreateInfo dyn{};
		dyn.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
		dyn.dynamicStateCount = 2;
		dyn.pDynamicStates = dyn_states;
		VkPipelineRasterizationStateCreateInfo raster{};
		raster.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
		raster.polygonMode = VK_POLYGON_MODE_FILL;
		raster.cullMode = VK_CULL_MODE_NONE;
		raster.lineWidth = 1.0f;
		VkPipelineMultisampleStateCreateInfo ms{};
		ms.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
		ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
		VkPipelineColorBlendAttachmentState blend_att{};
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
		if (vkCreatePipelineLayout(device, &layout, nullptr, &gui_pipeline_layout) != VK_SUCCESS) {
			vkDestroyShaderModule(device, vm, nullptr);
			vkDestroyShaderModule(device, fm, nullptr);
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
		pipe.layout = gui_pipeline_layout;
		pipe.renderPass = render_pass;
		bool ok = vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &pipe, nullptr,
											&gui_pipeline) == VK_SUCCESS;
		vkDestroyShaderModule(device, vm, nullptr);
		vkDestroyShaderModule(device, fm, nullptr);
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
	attrs[0].location = 0;
	attrs[0].format = VK_FORMAT_R32G32_SFLOAT;
	attrs[0].offset = offsetof(GuiVertex, pos);
	attrs[1].location = 1;
	attrs[1].format = VK_FORMAT_R32G32B32A32_SFLOAT;
	attrs[1].offset = offsetof(GuiVertex, color);
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
	const VkDynamicState dyn_states[2] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
	VkPipelineDynamicStateCreateInfo dyn{};
	dyn.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
	dyn.dynamicStateCount = 2;
	dyn.pDynamicStates = dyn_states;
	VkPipelineRasterizationStateCreateInfo raster{};
	raster.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
	raster.polygonMode = VK_POLYGON_MODE_FILL;
	raster.cullMode = VK_CULL_MODE_NONE;
	raster.lineWidth = 1.0f;
	VkPipelineMultisampleStateCreateInfo ms{};
	ms.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
	ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
	VkPipelineColorBlendAttachmentState blend_att{};
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
	if (vkCreatePipelineLayout(device, &layout, nullptr, &gui_pipeline_layout) != VK_SUCCESS) {
		vkDestroyShaderModule(device, vert_mod, nullptr);
		vkDestroyShaderModule(device, frag_mod, nullptr);
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
	pipe.layout = gui_pipeline_layout;
	pipe.renderPass = render_pass;
	bool ok = vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &pipe, nullptr, &gui_pipeline) ==
			  VK_SUCCESS;
	vkDestroyShaderModule(device, vert_mod, nullptr);
	vkDestroyShaderModule(device, frag_mod, nullptr);
	return ok;
}

bool VulkanRenderer::create_framebuffers()
{
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
		if (vkCreateFramebuffer(device, &info, nullptr, &swapchain_framebuffers[i]) != VK_SUCCESS) {
			return false;
		}
	}
	return true;
}

bool VulkanRenderer::create_command_pool()
{
	const QueueFamilyIndices idx = find_queue_families(physical_device, surface);
	VkCommandPoolCreateInfo info{};
	info.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
	info.queueFamilyIndex = idx.graphics_family.value();
	info.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
	return vkCreateCommandPool(device, &info, nullptr, &command_pool) == VK_SUCCESS;
}

bool VulkanRenderer::create_command_buffers()
{
	command_buffers.resize(max_frames_in_flight);
	VkCommandBufferAllocateInfo info{};
	info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
	info.commandPool = command_pool;
	info.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
	info.commandBufferCount = max_frames_in_flight;
	return vkAllocateCommandBuffers(device, &info, command_buffers.data()) == VK_SUCCESS;
}

bool VulkanRenderer::create_sync_objects()
{
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

bool VulkanRenderer::create_vertex_buffer()
{
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
					   VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
					   staging, staging_mem)) {
		return false;
	}
	void *dst = nullptr;
	vkMapMemory(device, staging_mem, 0, size, 0, &dst);
	std::memcpy(dst, verts.data(), size);
	vkUnmapMemory(device, staging_mem);
	if (!create_buffer(physical_device, device, size,
					   VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
					   VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, vertex_buffer, vertex_buffer_memory)) {
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

bool VulkanRenderer::create_index_buffer()
{
	const std::array<u16, 6> idx = {0, 1, 2, 2, 3, 0};
	const VkDeviceSize size = sizeof(idx);
	VkBuffer staging{};
	VkDeviceMemory staging_mem{};
	if (!create_buffer(physical_device, device, size, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
					   VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
					   staging, staging_mem)) {
		return false;
	}
	void *dst = nullptr;
	vkMapMemory(device, staging_mem, 0, size, 0, &dst);
	std::memcpy(dst, idx.data(), size);
	vkUnmapMemory(device, staging_mem);
	if (!create_buffer(physical_device, device, size,
					   VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT,
					   VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, index_buffer, index_buffer_memory)) {
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

bool VulkanRenderer::create_texture(u32 w, u32 h)
{
	const VkDeviceSize size = static_cast<VkDeviceSize>(w) * h * 4;
	if (!create_buffer(physical_device, device, size, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
					   VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
					   texture_buffer, texture_buffer_memory)) {
		return false;
	}
	VkImageCreateInfo info{};
	info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
	info.imageType = VK_IMAGE_TYPE_2D;
	info.format = kTexFormat;
	info.extent = {w, h, 1};
	info.mipLevels = 1;
	info.arrayLayers = 1;
	info.samples = VK_SAMPLE_COUNT_1_BIT;
	info.tiling = VK_IMAGE_TILING_OPTIMAL;
	info.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
	info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
	if (vkCreateImage(device, &info, nullptr, &texture_image) != VK_SUCCESS)
		return false;
	VkMemoryRequirements req{};
	vkGetImageMemoryRequirements(device, texture_image, &req);
	const u32 idx =
		find_memory_type(physical_device, req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
	if (idx == std::numeric_limits<u32>::max())
		return false;
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
	sampler.magFilter = VK_FILTER_NEAREST; // Crisp pixels, no blur.
	sampler.minFilter = VK_FILTER_NEAREST;
	sampler.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
	sampler.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
	sampler.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
	if (vkCreateSampler(device, &sampler, nullptr, &texture_sampler) != VK_SUCCESS) {
		return false;
	}
	tex_w_ = w;
	tex_h_ = h;
	texture_layout_ = VK_IMAGE_LAYOUT_UNDEFINED;
	return true;
}

void VulkanRenderer::destroy_texture()
{
	if (texture_sampler != VK_NULL_HANDLE) {
		vkDestroySampler(device, texture_sampler, nullptr);
		texture_sampler = VK_NULL_HANDLE;
	}
	if (texture_image_view != VK_NULL_HANDLE) {
		vkDestroyImageView(device, texture_image_view, nullptr);
		texture_image_view = VK_NULL_HANDLE;
	}
	if (texture_image != VK_NULL_HANDLE) {
		vkDestroyImage(device, texture_image, nullptr);
		texture_image = VK_NULL_HANDLE;
	}
	if (texture_image_memory != VK_NULL_HANDLE) {
		vkFreeMemory(device, texture_image_memory, nullptr);
		texture_image_memory = VK_NULL_HANDLE;
	}
	if (texture_buffer != VK_NULL_HANDLE) {
		vkDestroyBuffer(device, texture_buffer, nullptr);
		texture_buffer = VK_NULL_HANDLE;
	}
	if (texture_buffer_memory != VK_NULL_HANDLE) {
		vkFreeMemory(device, texture_buffer_memory, nullptr);
		texture_buffer_memory = VK_NULL_HANDLE;
	}
	tex_w_ = 0;
	tex_h_ = 0;
}

void VulkanRenderer::update_texture_descriptors()
{
	for (u32 i = 0; i < max_frames_in_flight; i++) {
		VkDescriptorImageInfo img{};
		img.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
		img.imageView = texture_image_view;
		img.sampler = texture_sampler;
		VkWriteDescriptorSet write{};
		write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
		write.dstSet = descriptor_sets[i];
		write.dstBinding = 1;
		write.descriptorCount = 1;
		write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
		write.pImageInfo = &img;
		vkUpdateDescriptorSets(device, 1, &write, 0, nullptr);
	}
}

bool VulkanRenderer::ensure_texture(u32 w, u32 h)
{
	if (texture_image != VK_NULL_HANDLE && tex_w_ == w && tex_h_ == h)
		return true;
	wait_idle();
	destroy_texture();
	if (!create_texture(w, h))
		return false;
	update_texture_descriptors();
	return true;
}

bool VulkanRenderer::create_body_texture()
{
	// Load the GBC body texture — try installed share first, then dev assets
	int tex_width, tex_height, tex_channels;
	unsigned char *pixels = nullptr;
	const char *base = SDL_GetBasePath();
	std::string base_dir = base ? base : "";
	const std::string candidates[4] = {base_dir + "../share/GB4ME/assets/gbc_body.png",
									   "/usr/share/GB4ME/assets/gbc_body.png",
									   base_dir + "assets/gbc_body.png", "assets/gbc_body.png"};
	for (auto &p : candidates) {
		pixels = stbi_load(p.c_str(), &tex_width, &tex_height, &tex_channels, 4);
		if (pixels)
			break;
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

	void *dst = nullptr;
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
	if (vkCreateImage(device, &info, nullptr, &body_image) != VK_SUCCESS)
		return false;

	VkMemoryRequirements req{};
	vkGetImageMemoryRequirements(device, body_image, &req);
	const u32 idx =
		find_memory_type(physical_device, req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
	if (idx == std::numeric_limits<u32>::max())
		return false;

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
	vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0,
						 0, nullptr, 0, nullptr, 1, &barrier);

	VkBufferImageCopy region{};
	region.imageExtent = {static_cast<u32>(tex_width), static_cast<u32>(tex_height), 1};
	region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
	region.imageSubresource.layerCount = 1;
	vkCmdCopyBufferToImage(cmd, body_staging_buffer, body_image,
						   VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);

	VkImageMemoryBarrier to_shader = barrier;
	to_shader.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
	to_shader.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
	to_shader.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
	to_shader.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
	vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
						 0, 0, nullptr, 0, nullptr, 1, &to_shader);

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
bool VulkanRenderer::create_body_photo_descriptors()
{
	if (!body_photo_ok_)
		return true; // no photo, nothing to bind
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

bool VulkanRenderer::create_uniform_buffers()
{
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

bool VulkanRenderer::create_descriptor_pool()
{
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

bool VulkanRenderer::create_descriptor_sets()
{
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

void VulkanRenderer::recreate_swapchain()
{
	wait_idle();
	for (auto fb : swapchain_framebuffers)
		vkDestroyFramebuffer(device, fb, nullptr);
	swapchain_framebuffers.clear();
	for (auto view : swapchain_image_views)
		vkDestroyImageView(device, view, nullptr);
	swapchain_image_views.clear();
	vkDestroySwapchainKHR(device, swapchain, nullptr);
	create_swapchain();
	create_image_views();
	create_framebuffers();
}

void VulkanRenderer::update_uniform_buffer(u32 current_image)
{
	UniformBufferObject ubo{};
	// Identity MVP: quad vertices are already in NDC.
	ubo.mvp.rows[0] = {1.0f, 0.0f, 0.0f, 0.0f};
	ubo.mvp.rows[1] = {0.0f, 1.0f, 0.0f, 0.0f};
	ubo.mvp.rows[2] = {0.0f, 0.0f, 1.0f, 0.0f};
	ubo.mvp.rows[3] = {0.0f, 0.0f, 0.0f, 1.0f};
	std::memcpy(uniform_buffers_mapped[current_image], &ubo, sizeof(ubo));
}

void VulkanRenderer::record_command_buffer(VkCommandBuffer cmd, u32 image_index)
{
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
	vkCmdPipelineBarrier(
		cmd,
		first_upload ? VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT : VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
		VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &to_transfer);
	VkBufferImageCopy region{};
	region.imageExtent = {tex_w_, tex_h_, 1};
	region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
	region.imageSubresource.layerCount = 1;
	vkCmdCopyBufferToImage(cmd, texture_buffer, texture_image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
						   1, &region);
	VkImageMemoryBarrier to_shader = to_transfer;
	to_shader.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
	to_shader.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
	to_shader.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
	to_shader.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
	vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
						 0, 0, nullptr, 0, nullptr, 1, &to_shader);
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
		const PixelRect pr = fit_aspect(0.0f, 0.0f, win_w, win_h);
		VkViewport viewport{};
		viewport.x = pr.x;
		viewport.y = pr.y;
		viewport.width = pr.w;
		viewport.height = pr.h;
		viewport.minDepth = 0.0f;
		viewport.maxDepth = 1.0f;
		vkCmdSetViewport(cmd, 0, 1, &viewport);
		// Floor/ceil rather than truncate: a fractional viewport offset with a
		// truncated scissor clips the right/bottom edge of the image.
		VkRect2D scissor{};
		scissor.offset = {static_cast<i32>(std::floor(viewport.x)),
						  static_cast<i32>(std::floor(viewport.y))};
		scissor.extent = {static_cast<u32>(std::ceil(viewport.x + viewport.width)) -
								  static_cast<u32>(std::floor(viewport.x)),
						  static_cast<u32>(std::ceil(viewport.y + viewport.height)) -
								  static_cast<u32>(std::floor(viewport.y))};
		vkCmdSetScissor(cmd, 0, 1, &scissor);
	}
	push_video_constants(cmd);
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

void VulkanRenderer::render(const PPU::FrameData &frame)
{
	if (!ensure_texture(kTexWidth, kTexHeight))
		return;
	vkWaitForFences(device, 1, &in_flight_fences[current_frame], VK_TRUE,
					std::numeric_limits<u64>::max());
	u32 image_index = 0;
	VkResult acq = vkAcquireNextImageKHR(device, swapchain, std::numeric_limits<u64>::max(),
										 image_available_semaphores[current_frame], VK_NULL_HANDLE,
										 &image_index);
	if (acq == VK_ERROR_OUT_OF_DATE_KHR) {
		recreate_swapchain();
		return;
	}
	vkResetFences(device, 1, &in_flight_fences[current_frame]);

	// PPU framebuffer is 0xAARRGGBB, matching B8G8R8A8 byte order: direct copy.
	void *dst = nullptr;
	vkMapMemory(device, texture_buffer_memory, 0, static_cast<VkDeviceSize>(tex_w_) * tex_h_ * 4, 0,
				&dst);
	std::memcpy(dst, frame.pixels.data(), static_cast<size_t>(tex_w_) * tex_h_ * 4);
	vkUnmapMemory(device, texture_buffer_memory);

	update_uniform_buffer(current_frame);
	vkResetCommandBuffer(command_buffers[current_frame], 0);
	record_command_buffer(command_buffers[current_frame], image_index);
	present_frame(image_index);
}

void VulkanRenderer::render_gba(const u32 *pixels, u32 w, u32 h)
{
	if (pixels == nullptr || w == 0 || h == 0)
		return;
	if (!ensure_texture(w, h))
		return;
	vkWaitForFences(device, 1, &in_flight_fences[current_frame], VK_TRUE,
					std::numeric_limits<u64>::max());
	u32 image_index = 0;
	VkResult acq = vkAcquireNextImageKHR(device, swapchain, std::numeric_limits<u64>::max(),
										 image_available_semaphores[current_frame], VK_NULL_HANDLE,
										 &image_index);
	if (acq == VK_ERROR_OUT_OF_DATE_KHR) {
		recreate_swapchain();
		return;
	}
	vkResetFences(device, 1, &in_flight_fences[current_frame]);

	// GBA framebuffer is 0xAARRGGBB like the GB one: direct copy.
	void *dst = nullptr;
	vkMapMemory(device, texture_buffer_memory, 0, static_cast<VkDeviceSize>(tex_w_) * tex_h_ * 4, 0,
				&dst);
	std::memcpy(dst, pixels, static_cast<size_t>(tex_w_) * tex_h_ * 4);
	vkUnmapMemory(device, texture_buffer_memory);

	update_uniform_buffer(current_frame);
	vkResetCommandBuffer(command_buffers[current_frame], 0);
	record_command_buffer(command_buffers[current_frame], image_index);
	present_frame(image_index);
}

// Snap a draw rect to an exact integer multiple of the source size so every
// source pixel covers the same number of destination pixels. Returns the rect
// unchanged when integer scaling is off (Fit).
VulkanRenderer::PixelRect VulkanRenderer::fit_aspect(f32 x, f32 y, f32 w, f32 h) const
{
	// Pure geometry lives in types.h so the selftest can verify the aspect
	// property without a Vulkan device.
	const PixelRectF r = fit_aspect_rect(x, y, w, h, tex_w_, tex_h_);
	return {r.x, r.y, r.w, r.h};
}

void VulkanRenderer::push_video_state(VkCommandBuffer cmd, VideoFilter f, f32 grid)
{
	VideoPush p{};
	p.mode = static_cast<u32>(f);
	p.grid = grid;
	p.flags = 0;
	p.pad = 0;
	vkCmdPushConstants(cmd, pipeline_layout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(p), &p);
}

// Push the current upscaling filter + LCD grid depth for the next game draw.
void VulkanRenderer::push_video_constants(VkCommandBuffer cmd)
{
	push_video_state(cmd, video_filter_, lcd_grid_);
}

// The bezel photo is drawn with the SAME pipeline and fragment shader as the
// game, so it needs a neutral (Nearest, no grid) state: otherwise the LCD grid
// would be drawn over the entire shell photo, and the menu path (which draws
// the photo but no game) would read uninitialised push constants.
void VulkanRenderer::push_neutral_video_constants(VkCommandBuffer cmd)
{
	push_video_state(cmd, VideoFilter::Nearest, 0.0f);
}

void VulkanRenderer::present_frame(u32 image_index)
{
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

void VulkanRenderer::update_gui_buffers(const GUIConsole &gui, const GUIConsole::Rect *game_rect)
{
	// Build vertex/index data for GUI rects (body, bezel, buttons, etc.)
	// Indices are 32-bit: long ROM lists generate tens of thousands of
	// vertices, and 16-bit indices would wrap and silently drop the tail
	// of the overlay (footer text, gear/open buttons).
	std::vector<GuiVertex> verts;
	std::vector<u32> indices;
	auto push_rect = [&](float x, float y, float w, float h, u32 rgba, float rounding) {
		// Rounded rectangle: for now, just axis-aligned rect (rounding ignored, but keep param for
		// future). Convert to NDC. Layout rects live in WINDOW coordinates (same space as
		// mouse events; see handle_events), so NDC must use the window size — NOT the
		// swapchain (physical pixel) size, which differs on HiDPI/fractional-scale
		// displays. (NDC is viewport-relative, so window units are correct here; the
		// record functions separately scale the photo/game viewports to pixels.)
		float win_w = static_cast<float>(gui.window_w());
		float win_h = static_cast<float>(gui.window_h());
		auto to_ndc = [&](float px, float py) -> Vec2 {
			// Window pixels use top-left origin (y down). Vulkan NDC y=-1 maps
			// to the framebuffer top, so py=0 must yield -1 (NOT 1.0f - ...,
			// which rendered the whole GUI upside down).
			return {(px / win_w) * 2.0f - 1.0f, (py / win_h) * 2.0f - 1.0f};
		};
		(void) rounding;
		Vec2 p0 = to_ndc(x, y);
		Vec2 p1 = to_ndc(x + w, y);
		Vec2 p2 = to_ndc(x + w, y + h);
		Vec2 p3 = to_ndc(x, y + h);
		Vec4 col = {((rgba >> 16) & 0xFF) / 255.0f, ((rgba >> 8) & 0xFF) / 255.0f,
					(rgba & 0xFF) / 255.0f, ((rgba >> 24) & 0xFF) / 255.0f};
		// Reorder to match shader's RGBA order? Our Vec4 is x,y,z,w as r,g,b,a.
		u32 base = static_cast<u32>(verts.size());
		verts.push_back({p0, {col.x, col.y, col.z, col.w}});
		verts.push_back({p1, {col.x, col.y, col.z, col.w}});
		verts.push_back({p2, {col.x, col.y, col.z, col.w}});
		verts.push_back({p3, {col.x, col.y, col.z, col.w}});
		indices.push_back(base + 0);
		indices.push_back(base + 1);
		indices.push_back(base + 2);
		indices.push_back(base + 2);
		indices.push_back(base + 3);
		indices.push_back(base + 0);
	};

	auto push_circle = [&](float cx, float cy, float r, u32 rgba) {
		// Window coordinates, like push_rect above (NOT swapchain pixels).
		float win_w = static_cast<float>(gui.window_w());
		float win_h = static_cast<float>(gui.window_h());
		Vec4 col = {((rgba >> 16) & 0xFF) / 255.0f, ((rgba >> 8) & 0xFF) / 255.0f,
					(rgba & 0xFF) / 255.0f, ((rgba >> 24) & 0xFF) / 255.0f};
		const int segs = 16;
		u32 center_idx = static_cast<u32>(verts.size());
		Vec2 center = {(cx / win_w) * 2.0f - 1.0f, (cy / win_h) * 2.0f - 1.0f};
		verts.push_back({center, {col.x, col.y, col.z, col.w}});
		for (int i = 0; i < segs; i++) {
			float a = (i / static_cast<float>(segs)) * 2.0f * 3.1415926f;
			float x = cx + std::cos(a) * r;
			float y = cy + std::sin(a) * r;
			Vec2 p = {(x / win_w) * 2.0f - 1.0f, (y / win_h) * 2.0f - 1.0f};
			verts.push_back({p, {col.x, col.y, col.z, col.w}});
		}
		for (int i = 0; i < segs; i++) {
			u32 i0 = center_idx;
			u32 i1 = center_idx + 1 + static_cast<u32>(i);
			u32 i2 = center_idx + 1 + static_cast<u32>((i + 1) % segs);
			indices.push_back(i0);
			indices.push_back(i1);
			indices.push_back(i2);
		}
	};
	auto push_text = [&](float x, float y, const std::string &str, float scale, u32 rgba) {
		float cursor_x = x;
		for (char ch : str) {
			if (ch < 32 || ch > 126) {
				cursor_x += 8 * scale;
				continue;
			}
			const uint8_t *glyph = font::kFont[static_cast<int>(ch)];
			for (int row = 0; row < 8; row++) {
				uint8_t bits = glyph[row];
				// Font is LSB-first (bit 0 = leftmost pixel), row 0 = top row.
				for (int col = 0; col < 8; col++) {
					if (bits & (1 << col)) {
						push_rect(cursor_x + col * scale, y + row * scale, scale, scale, rgba, 0);
					}
				}
			}
			cursor_x += 8 * scale;
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
	// Letterbox: fill the glass around the fitted game image with black so
	// the picture sits edge-to-edge in the bezel (a 3:2 GBA frame leaves
	// tall bars in the ~1.06 glass; GB/GBC thin ones). Drawn first so all
	// later overlay sits above; the bars are clamped to the glass minus the
	// image, so they can never cover game pixels.
	if (game_rect != nullptr) {
		// Each bar overlaps the image by 1 window px: the game quad goes
		// through viewport/scissor integer truncation while these rects use
		// exact float NDC, so hairline photo slivers showed at the right and
		// bottom edges. A 1px black inner border is invisible and seals all
		// four seams at any window/HiDPI scale.
		const auto glass = gui.bezel_rect();
		const float gx0 = glass.x, gy0 = glass.y, gx1 = glass.x + glass.w,
					gy1 = glass.y + glass.h;
		const float ix0 = game_rect->x, iy0 = game_rect->y, ix1 = game_rect->x + game_rect->w,
					iy1 = game_rect->y + game_rect->h;
		push_rect(gx0, gy0, glass.w, std::max(0.0f, iy0 - gy0 + 1.0f), 0xFF000000, 0);
		push_rect(gx0, iy1 - 1.0f, glass.w, std::max(0.0f, gy1 - iy1 + 1.0f), 0xFF000000, 0);
		push_rect(gx0, gy0, std::max(0.0f, ix0 - gx0 + 1.0f), glass.h, 0xFF000000, 0);
		push_rect(ix1 - 1.0f, gy0, std::max(0.0f, gx1 - ix1 + 1.0f), glass.h, 0xFF000000, 0);
	}
	// GBA shoulder pills, always visible (menu included) so L/R stay
	// discoverable and remappable everywhere, like the settings rows.
	// They sit on the top body corners, clear of the menu grid and game.
	{
		for (const auto &s : gui.shoulders()) {
			if (s.r.w <= 0)
				continue;
			const bool pressed = gui.is_gba_pressed(s.key);
			push_rect(s.r.x, s.r.y, s.r.w, s.r.h,
					pressed ? 0xFF4F7FFF : 0xFF6E6E80, 4.0f);
			const float tscale = std::clamp(s.r.h * 0.36f / 8.0f, 1.25f, 2.5f);
			const float tw = 8.0f * tscale;
			push_text(s.r.x + (s.r.w - tw) * 0.5f, s.r.y + (s.r.h - 8.0f * tscale) * 0.5f,
					s.name, tscale, pressed ? 0xFF101018 : 0xFFFFFFFF);
		}
	}
	// Press highlights over the photo buttons.
	for (const auto &b : gui.buttons()) {
		if (b.w <= 0)
			continue;
		if (!gui.is_pressed(b.key))
			continue;
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
		// Menu chrome text scales with the window (reference: ~240px wide
		// screen at scale 1.0) so headers/footers stay legible instead of a
		// fixed 8px that is tiny on large windows and cramped on small ones.
		// The scale is additionally shrunk to fit so lines can never be cut
		// off or overlap the ROM grid: longest footer line fits the screen
		// width, the two-line footer fits its reserve (mirrors
		// menu_list_rect's footer_h), and the header fits the title reserve.
		float ui_scale = std::clamp(scr.w / 240.0f, 0.55f, 1.6f);
		ui_scale = std::min(ui_scale, scr.w / (28.0f * 8.0f)); // f2 width
		const float footer_h = std::max(26.0f, scr.h * 0.14f);
		ui_scale = std::min(ui_scale, (footer_h - 7.0f) / 16.0f); // 4+8s+3+8s block
		const float title_h = std::max(20.0f, scr.h * 0.10f);
		const float list_pad = scr.w * 0.045f; // mirrors menu_list_rect
		ui_scale = std::min(ui_scale, (title_h + list_pad - 6.0f) / 20.0f);
		ui_scale = std::max(ui_scale, 0.40f);
		const float ui_half = 4.0f * ui_scale; // half a char cell
		const auto &roms = gui.rom_list();
		const float cx = scr.x + scr.w * 0.5f;
		push_text(cx - 5 * ui_half, scr.y + 6, "GB4ME", ui_scale, 0xFFFFFFFF);
		const std::string sub = "SELECT ROM (" + std::to_string(roms.size()) + ")";
		push_text(cx - sub.size() * ui_half, scr.y + 6 + 12.0f * ui_scale, sub, ui_scale,
				  0xFFAAAAAA);
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
			if (i < 0 || i >= static_cast<int>(roms.size()))
				break;
			const int col = vi % cols;
			const int row = vi / cols;
			const float cx = list.x + col * (cw + gap);
			const float cy = list.y + row * ch;
			// Last row may be partial — skip empty cells.
			if (cx + cw > list.x + list.w + 1.0f)
				continue;
			const bool sel = (i == gui.menu_selected());
			// Cell background
			push_rect(cx, cy, cw, ch, sel ? 0xFF3A3A8F : 0xFF22252B, 3.0f);
			if (sel)
				push_rect(cx + 1, cy + 1, cw - 2, ch - 2, 0xFF2E3190, 3.0f);
			// System badge — top-right inside cell
			const auto &rom = roms[static_cast<size_t>(i)];
			const std::string badge = rom.gba ? "GBA" : (rom.cgb ? "CGB" : "DMG");
			const u32 bcol = rom.gba ? 0xFF4F7FFF : (rom.cgb ? 0xFF3FA06B : 0xFF5A5A5A);
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
			const u32 cart_top = sel ? 0xFF9A9A9A : 0xFF6E6E6E;
			const u32 label_bg = sel ? 0xFF1A1A1A : 0xFF252525;
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
			if (maxc_line < 4)
				maxc_line = 4;
			// Wrap into up to 2 lines
			auto trim = [](const std::string &s) -> std::string {
				size_t a = 0;
				while (a < s.size() && std::isspace(static_cast<unsigned char>(s[a])))
					a++;
				size_t b = s.size();
				while (b > a && std::isspace(static_cast<unsigned char>(s[b - 1])))
					b--;
				return s.substr(a, b - a);
			};
			std::vector<std::string> lines;
			std::string remaining = title;
			for (int line = 0; line < 2 && !remaining.empty(); line++) {
				if (remaining.size() <= maxc_line) {
					lines.push_back(remaining);
					break;
				}
				size_t cut = maxc_line;
				if (line == 1) {
					// Last line truncates with ...
					if (remaining.size() > maxc_line) {
						if (maxc_line > 3) {
							size_t sp = remaining.rfind(' ', maxc_line - 3);
							if (sp != std::string::npos && sp > maxc_line / 2)
								remaining = remaining.substr(0, sp) + "...";
							else
								remaining = remaining.substr(0, maxc_line - 3) + "...";
						} else
							remaining = remaining.substr(0, maxc_line);
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
				const std::string &line = lines[li];
				const float tw = line.size() * char_w;
				const float tx = cx + (cw - tw) * 0.5f;
				const float ty = title_y0 + li * 9.0f;
				if (ty + 8 > cy + ch - 4)
					break;
				push_text(tx, ty, line, tscale, sel ? 0xFFFFFFFF : 0xFFD8D8D8);
			}
		}
		if (roms.empty()) {
			const std::string e1 = "NO ROMS IN FOLDER";
			push_text(cx - e1.size() * ui_half, list.y + 10, e1, ui_scale, 0xFFAAAAAA);
			std::string folder = gui.settings() ? gui.settings()->rom_folder : ".";
			if (folder.size() > 24)
				folder = "..." + folder.substr(folder.size() - 21);
			push_text(cx - folder.size() * ui_half, list.y + 10 + 14.0f * ui_scale, folder,
					  ui_scale, 0xFF777777);
		}
		// scroll indicators — grid scrolls by row, show track + arrows
		if (first > 0)
			push_text(list.x + list.w - 12, list.y - 10, "^", ui_scale, 0xFFAAAAAA);
		if (first + vis < static_cast<int>(roms.size())) {
			push_text(list.x + list.w - 12, list.y + list.h + 2, "v", ui_scale, 0xFFAAAAAA);
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
		// footer hints — grid navigation (scaled with the window; anchored
		// to the bottom so the two lines never overlap each other).
		const std::string f1 = "ARROWS:MOVE ENTER:PLAY";
		const std::string f2 = "O:OPEN FILE S:SETTINGS R:RESCAN";
		const float f2_y = scr.y + scr.h - 4.0f - 8.0f * ui_scale;
		const float f1_y = f2_y - 8.0f * ui_scale - 3.0f;
		push_text(cx - f1.size() * ui_half, f1_y, f1, ui_scale, 0xFF888888);
		push_text(cx - f2.size() * ui_half, f2_y, f2, ui_scale, 0xFF888888);
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
		push_text(setr.x + setr.w * 0.5f - 4 * gs, setr.y + setr.h * 0.5f - 4 * gs, "*", gs,
				  0xFFFFFFFF);
	}
	// Settings panel
	if (gui.show_settings()) {
		// Darken background
		push_rect(0, 0, static_cast<float>(swapchain_extent.width),
				  static_cast<float>(swapchain_extent.height), 0xAA000000, 0);
		// ---- Settings panel ----
		// One accent (the console purple), a neutral ramp and a semantic
		// green, kept local so the page reads as a single design.
		constexpr u32 kPanelOuter = 0xFF2A2A33;
		constexpr u32 kPanelInner = 0xFF16161C;
		constexpr u32 kAccent = 0xFF6B3FA0;
		constexpr u32 kAccentDim = 0xFF4A3266;
		constexpr u32 kRowBg = 0xFF23232D;
		constexpr u32 kPillBg = 0xFF32323F;
		constexpr u32 kText = 0xFFECECF2;
		constexpr u32 kTextDim = 0xFF9A9AAC;
		constexpr u32 kTextMute = 0xFF71718A;
		constexpr u32 kOk = 0xFF4FB477;

		// The panel height depends on the active tab, so take the rect from
		// the console rather than recomputing it.
		const auto panel_rect = gui.settings_panel_rect();
		const float panel_w = panel_rect.w, panel_h = panel_rect.h;
		const float panel_x = panel_rect.x, panel_y = panel_rect.y;
		push_rect(panel_x, panel_y, panel_w, panel_h, kPanelOuter, 6.0f);
		push_rect(panel_x + 2, panel_y + 2, panel_w - 4, panel_h - 4, kPanelInner, 6.0f);

		const float tscale = body.w > 420.0f ? 1.0f : 0.6f;
		const float tchar = 8.0f * tscale;
		const float th = 8.0f * tscale;
		const auto fit = [&](const std::string &s, float w) {
			size_t maxc = w > tchar + 6.0f ? static_cast<size_t>((w - 6.0f) / tchar) : 1;
			if (s.size() <= maxc)
				return s;
			if (maxc <= 3)
				return s.substr(0, maxc);
			return std::string("...") + s.substr(s.size() - (maxc - 3));
		};
		const auto text_y = [&](const GUIConsole::Rect &r, float ts) {
			return r.y + (r.h - 8.0f * ts) * 0.5f;
		};

		// Title bar + close button.
		{
			const float bar_h = body.h * 0.042f;
			push_rect(panel_x + 4, panel_y + 5, panel_w - 8, bar_h, kAccent, 3.0f);
			push_text(panel_x + 10, panel_y + 7, "SETTINGS", 1.0f, 0xFFFFFFFF);
			const auto sr = gui.settings_close_rect();
			push_rect(sr.x, sr.y, sr.w, sr.h, 0xFF5A2430, 2.0f);
			push_text(sr.x + (sr.w - 8.0f) * 0.5f, sr.y + (sr.h - 8.0f) * 0.5f, "X", 1.0f,
					  0xFFE8A0B0);
		}

		// Tabs, with an underline for the active one.
		{
			auto draw_tab = [&](GUIConsole::SettingsTab t, const std::string &name) {
				const auto r = gui.settings_tab_rect(t);
				const bool active = gui.settings_tab() == t;
				push_rect(r.x, r.y, r.w, r.h, active ? kAccentDim : kRowBg, 2.0f);
				if (active)
					push_rect(r.x + 1, r.y + r.h - 2, r.w - 2, 2, kAccent, 0);
				float ts = 1.0f;
				while (ts > 0.5f && name.size() * 8.0f * ts > r.w - 6.0f)
					ts -= 0.1f;
				const float tw = static_cast<float>(name.size()) * 8.0f * ts;
				push_text(r.x + (r.w - tw) * 0.5f, r.y + (r.h - 8.0f * ts) * 0.5f, name, ts,
						  active ? 0xFFFFFFFF : kTextDim);
			};
			draw_tab(GUIConsole::SettingsTab::General, "GENERAL");
			draw_tab(GUIConsole::SettingsTab::Inputs, "INPUTS");
		}

		// Rows: a label field plus an inset value "pill", so the value reads
		// as a control rather than a second column of text.
		const auto rows = gui.settings_rows();
		if (gui.settings_tab() == GUIConsole::SettingsTab::General) {
			for (const auto &row : rows) {
				using Style = GUIConsole::RowStyle;
				if (row.style == Style::Header) {
					push_text(row.label_rect.x + 2, text_y(row.label_rect, tscale), row.label,
							  tscale, kTextMute);
					continue;
				}
				const bool is_action = row.style == Style::Action;
				const u32 row_bg =
					row.selected ? kAccentDim : (is_action ? 0xFF1E1E28 : kRowBg);
				push_rect(row.label_rect.x, row.label_rect.y, row.label_rect.w,
						  row.label_rect.h, row_bg, 2.0f);
				const GUIConsole::Rect v = row.value_rect;
				const GUIConsole::Rect vp{v.x + 2.0f, v.y + 2.0f, v.w - 4.0f, v.h - 4.0f};
				if (vp.w > 2.0f && vp.h > 2.0f)
					push_rect(vp.x, vp.y, vp.w, vp.h, row.selected ? kAccent : kPillBg, 2.0f);
				if (row.id == "volume") {
					const float vol = gui.settings() ? gui.settings()->volume : 1.0f;
					if (vol > 0.0f)
						push_rect(vp.x, vp.y, vp.w * vol, vp.h, kOk, 2.0f);
					push_rect(vp.x + vp.w * vol - 1.5f, vp.y, 3, vp.h, 0xFFFFFFFF, 0);
				}
				push_text(row.label_rect.x + 5, text_y(row.label_rect, tscale),
						  fit(row.label, row.label_rect.w), tscale,
						  row.selected ? 0xFFFFFFFF : kText);
				if (!row.value.empty()) {
					u32 vc = kTextDim;
					if (row.selected)
						vc = 0xFFFFFFFF;
					else if (row.unbound)
						vc = kTextMute;
					push_text(vp.x + 4, vp.y + (vp.h - th) * 0.5f, fit(row.value, vp.w), tscale,
							  vc);
				}
			}
		} else {
			// ---- Inputs page ----
			const auto &pad = gui.gbaPad();
			// Widget helper: a boxed control with an optional right-hand caret.
			const auto widget = [&](const GUIConsole::Rect &r, const std::string &label,
									u32 bg, u32 fg, bool caret, bool enabled) {
				push_rect(r.x, r.y, r.w, r.h, bg, 2.0f);
				if (!enabled)
					push_rect(r.x, r.y, r.w, r.h, 0x30000000, 0);
				const float pad_l = 4.0f;
				const float avail = r.w - pad_l * 2.0f - (caret ? 10.0f : 0.0f);
				push_text(r.x + pad_l, r.y + (r.h - th) * 0.5f, fit(label, avail), tscale, fg);
				if (caret) {
					// Small down triangle, so the field reads as a combo box.
					const float cx = r.x + r.w - 8.0f;
					const float cy = r.y + r.h * 0.5f;
					push_rect(cx - 4, cy - 1, 8, 2, fg, 0);
					push_rect(cx - 2, cy + 1, 4, 2, fg, 0);
				}
			};
			const auto dropdown = [&](const GUIConsole::Dropdown &dd, const std::string &value,
									 bool has_value) {
				widget(dd.closed, has_value ? value : "Keyboard only", dd.open ? kAccent : kPillBg,
					   dd.enabled ? (dd.open ? 0xFFFFFFFF : kText) : kTextMute, true,
					   dd.enabled);
			};

			// Profile row.
			push_text(pad.label_profile.x + 2, text_y(pad.label_profile, tscale), "Profile",
					  tscale, kTextDim);
			dropdown(gui.profile_box(),
					 gui.settings() ? gui.settings()->profileName(
										 gui.settings()->active_profile)
								   : std::string(),
					 true);
			widget(gui.btn_load(), "Load", kPillBg, kText, false, true);
			widget(gui.btn_save(), "Save", kPillBg, kText, false, true);

			// Device row.
			push_text(pad.label_device.x + 2, text_y(pad.label_device, tscale), "Device",
					  tscale, kTextDim);
			dropdown(gui.device_box(), gui.deviceName(), true);

			// Auto-map, only while a controller is the selected source.
			if (gui.automapVisible()) {
				const auto &b = gui.btn_automap();
				const std::string st = gui.controllerStatus();
				const bool done = (st == "mapped" || st == "defaults") &&
					gui.deviceIsController() == (st == "mapped");
				widget(b, done ? "Applied" : gui.autoMapLabel(),
					   done ? 0xFF24543A : kAccent, 0xFFFFFFFF, false, true);
			}

			// Open drop-down lists overlay everything drawn so far.
			const auto draw_list = [&](const GUIConsole::Dropdown &dd) {
				if (!dd.open)
					return;
				const bool profile_list = &dd == &gui.profile_box();
				for (size_t i = 0; i < dd.items.size(); i++) {
					const auto &r = dd.items[i];
					const bool sel = static_cast<int>(i) == dd.selected;
					const int idx = static_cast<int>(i);
					// Unavailable commands are dimmed so it is obvious they
					// need a user profile selected first.
					const bool avail =
						!profile_list || gui.profileMenuEnabled(idx);
					push_rect(r.x, r.y, r.w, r.h, sel ? kAccent : 0xFF262630, 2.0f);
					const std::string &t =
						i < dd.options.size() ? dd.options[i] : std::string();
					const u32 fg = !avail ? kTextMute : (sel ? 0xFFFFFFFF : kText);
					push_text(r.x + 4, r.y + (r.h - th) * 0.5f, fit(t, r.w - 8.0f), tscale,
							  fg);
				}
			};
			draw_list(gui.profile_box());
			draw_list(gui.device_box());

			// ---- GBA outline ----
			// Drawn only when no list is open: a drop-down overlays
			// everything, so a diagram behind it would just show through.
			if (!gui.anyDropdownOpen()) {
				const auto &g = pad;
				const float bw = g.body.w, bh = g.body.h;
				const auto PX = [&](float xmm) {
					return xmm / 144.5f * bw; // AGB-001 is 144.5 x 82 mm
				};
				const auto PY = [&](float ymm) {
					return ymm / 82.0f * bh;
				};

				// Shell: a bezel line over a darker face, plus the wider
				// lower "chin" the AGB has below the control row.
				push_rect(g.body.x, g.body.y, bw, bh, 0xFF474758, 3.0f);
				push_rect(g.body.x + PX(1.6f), g.body.y + PY(1.6f), bw - PX(3.2f),
						  bh - PY(3.2f), 0xFF1D1D26, 3.0f);

				// Cartridge slot along the top edge, and the power switch.
				push_rect(g.body.x + PX(44.0f), g.body.y + PY(0.8f), PX(30.0f), PY(2.6f),
						  0xFF111119, 1.0f);
				push_rect(g.body.x + PX(6.0f), g.body.y + PY(1.2f), PX(9.0f), PY(3.0f),
						  0xFF5A5A6C, 1.0f);

				// Screen: a recessed panel with a lit inner area.
				push_rect(g.screen.x, g.screen.y, g.screen.w, g.screen.h, 0xFF0E0E14, 2.0f);
				push_rect(g.screen.x + PX(1.4f), g.screen.y + PY(1.4f),
						  g.screen.w - PX(2.8f), g.screen.h - PY(2.8f), 0xFF1E2C3E, 2.0f);
				push_rect(g.screen.x + PX(1.4f), g.screen.y + PY(1.4f),
						  g.screen.w - PX(2.8f), PY(0.8f), 0xFF2C4058, 0);

				// A / B: rings, matching the moulded circles on the shell.
				const auto face = [&](const GUIConsole::Rect &r, u32 ring) {
					const float cxr = r.x + r.w * 0.5f, cyr = r.y + r.h * 0.5f;
					push_circle(cxr, cyr, r.w * 0.5f, ring);
					push_circle(cxr, cyr, r.w * 0.5f - PX(1.2f), 0xFF23232D);
				};
				face(g.btn_a, 0xFF55566A);
				face(g.btn_b, 0xFF55566A);

				// D-pad: a cross with a rounded hub.
				push_rect(g.dpad_h.x, g.dpad_h.y, g.dpad_h.w, g.dpad_h.h, 0xFF55566A, 2.0f);
				push_rect(g.dpad_v.x, g.dpad_v.y, g.dpad_v.w, g.dpad_v.h, 0xFF55566A, 2.0f);
				push_circle(g.dpad_h.x + g.dpad_h.w * 0.5f, g.dpad_h.y + g.dpad_h.h * 0.5f,
							g.dpad_h.h * 0.55f, 0xFF55566A);
				push_circle(g.dpad_h.x + g.dpad_h.w * 0.5f, g.dpad_h.y + g.dpad_h.h * 0.5f,
							g.dpad_h.h * 0.30f, 0xFF23232D);

				// START / SELECT: the two angled pills under the screen.
				push_rect(g.btn_select.x, g.btn_select.y, g.btn_select.w, g.btn_select.h,
						  0xFF55566A, 2.0f);
				push_rect(g.btn_start.x, g.btn_start.y, g.btn_start.w, g.btn_start.h,
						  0xFF55566A, 2.0f);
				// L / R shoulder tabs.
				push_rect(g.btn_l.x, g.btn_l.y, g.btn_l.w, g.btn_l.h, 0xFF55566A, 2.0f);
				push_rect(g.btn_r.x, g.btn_r.y, g.btn_r.w, g.btn_r.h, 0xFF55566A, 2.0f);

				// Speaker: six holes on a diagonal, as on the real shell.
				for (int i = 0; i < 6; i++) {
					const float hx = g.body.x + PX(113.0f + static_cast<float>(i) * 3.4f);
					const float hy = g.body.y + PY(52.0f + static_cast<float>(i) * 3.4f);
					push_circle(hx, hy, PX(0.9f), 0xFF111119);
				}

				// Letter labels.
				const auto tag = [&](const GUIConsole::Rect &r, const char *t) {
					const float tw = static_cast<float>(std::strlen(t)) * tchar;
					push_text(r.x + (r.w - tw) * 0.5f, r.y + (r.h - th) * 0.5f, t, tscale,
							  kText);
				};
				tag(g.btn_a, "A");
				tag(g.btn_b, "B");
				tag(g.btn_start, "START");
				tag(g.btn_select, "SELECT");
				tag(g.btn_l, "L");
				tag(g.btn_r, "R");
				// The screen gets a caption instead of a binding.
				{
					const float tw = 9.0f * tchar;
					push_text(g.screen.x + (g.screen.w - tw) * 0.5f,
							  g.screen.y + (g.screen.h - th) * 0.5f, "SCREEN", tscale, kTextMute);
				}

				// Highlight the armed button so the user can see what they are
				// about to rebind.
				if (gui.awaiting_input()) {
					const u32 hl = kAccent;
					const auto ring = [&](const GUIConsole::Rect &r, bool circle) {
						if (circle)
							push_circle(r.x + r.w * 0.5f, r.y + r.h * 0.5f, r.w * 0.5f + 2.0f,
									 hl);
						else
							push_rect(r.x - 2.0f, r.y - 2.0f, r.w + 4.0f, r.h + 4.0f, hl, 1.0f);
					};
					switch (gui.pending_pad()) {
					case gb::PadButton::A:
						ring(g.btn_a, true);
						break;
					case gb::PadButton::B:
						ring(g.btn_b, true);
						break;
					case gb::PadButton::Start:
						ring(g.btn_start, false);
						break;
					case gb::PadButton::Select:
						ring(g.btn_select, false);
						break;
					case gb::PadButton::L:
						ring(g.btn_l, false);
						break;
					case gb::PadButton::R:
						ring(g.btn_r, false);
						break;
					// Only the pressed ARM lights up, so the user can see
					// which direction they just bound.
					case gb::PadButton::Up:
						push_rect(g.dpad_v.x - 2.0f, g.dpad_v.y - 2.0f, g.dpad_v.w + 4.0f,
								  g.dpad_v.h * 0.5f + 2.0f, hl, 1.0f);
						break;
					case gb::PadButton::Down:
						push_rect(g.dpad_v.x - 2.0f,
								  g.dpad_v.y + g.dpad_v.h * 0.5f - 2.0f, g.dpad_v.w + 4.0f,
								  g.dpad_v.h * 0.5f + 2.0f, hl, 1.0f);
						break;
					case gb::PadButton::Left:
						push_rect(g.dpad_h.x - 2.0f, g.dpad_h.y - 2.0f,
								  g.dpad_h.w * 0.5f + 2.0f, g.dpad_h.h + 4.0f, hl, 1.0f);
						break;
					case gb::PadButton::Right:
						push_rect(g.dpad_h.x + g.dpad_h.w * 0.5f - 2.0f, g.dpad_h.y - 2.0f,
								  g.dpad_h.w * 0.5f + 2.0f, g.dpad_h.h + 4.0f, hl, 1.0f);
						break;
					default:
						break;
					}
				}
			}
		}
		// Footer hint, worded for the page you are on.
		{
			const bool inputs = gui.settings_tab() == GUIConsole::SettingsTab::Inputs;
			const char *hint = inputs
								   ? "CLICK A GBA BUTTON TO REBIND - ESC CLOSES"
								   : "CLICK A ROW - DRAG VOLUME - S/ESC CLOSES";
			const float fh = std::min(16.0f, panel_h * 0.09f);
			const float fy = panel_y + panel_h - fh - 4.0f;
			push_rect(panel_x + 6, fy, panel_w - 12, fh, 0xFF1B1B23, 2.0f);
			push_text(panel_x + 10, fy + (fh - 5.0f) * 0.5f, hint, 0.6f, kTextMute);
		}
	}
	// Profile-naming prompt. Mutually exclusive with the remap modal above, so
	// it can reuse the same box shape and centre on the body.
	if (gui.text_entry_active()) {
		push_rect(0, 0, static_cast<float>(swapchain_extent.width),
				  static_cast<float>(swapchain_extent.height), 0xAA000000, 0);
		const float modal_w = body.w * 0.6f, modal_h = body.h * 0.12f;
		const float modal_x = body.x + (body.w - modal_w) * 0.5f;
		const float modal_y = body.y + (body.h - modal_h) * 0.5f;
		push_rect(modal_x, modal_y, modal_w, modal_h, 0xFFFFFFFF, 6.0f);
		push_rect(modal_x + 2, modal_y + 2, modal_w - 4, modal_h - 4, 0xFF202020, 6.0f);
		const std::string line1 = gui.text_prompt();
		// A block cursor makes it obvious the field is editable.
		std::string typed = gui.text_value() + "_";
		const std::string hint = "Enter accepts  Esc cancels";
		const float cx = modal_x + modal_w * 0.5f;
		const float cy = modal_y + modal_h * 0.5f;
		push_text(cx - line1.size() * 4.0f, cy - 20, line1, 1.0f, 0xFFAAAAAA);
		push_text(cx - typed.size() * 4.0f, cy - 4, typed, 1.0f, 0xFFFFFFFF);
		push_text(cx - hint.size() * 4.0f, cy + 12, hint, 1.0f, 0xFFAAAAAA);
	}
	// Headless budget probe (no Vulkan upload): GB4ME_GUI_COUNT=1 prints the
	// overlay size so overflow can be diagnosed without a display.
#ifndef GB4ME_RELEASE
	if (std::getenv("GB4ME_GUI_COUNT") != nullptr) {
		std::fprintf(stderr, "gui-overlay verts=%zu indices=%zu\n", verts.size(), indices.size());
		return;
	}
#endif
	// Grow the buffers if this frame's overlay needs more room, then upload.
	if (!ensure_gui_buffers(static_cast<u32>(verts.size()), static_cast<u32>(indices.size())))
		return;
	void *vdst = nullptr;
	vkMapMemory(device, gui_vertex_buffer_memory, 0, verts.size() * sizeof(GuiVertex), 0, &vdst);
	std::memcpy(vdst, verts.data(), verts.size() * sizeof(GuiVertex));
	vkUnmapMemory(device, gui_vertex_buffer_memory);
	void *idst = nullptr;
	vkMapMemory(device, gui_index_buffer_memory, 0, indices.size() * sizeof(u32), 0, &idst);
	std::memcpy(idst, indices.data(), indices.size() * sizeof(u32));
	vkUnmapMemory(device, gui_index_buffer_memory);
	gui_index_count = static_cast<u32>(indices.size());
}

void VulkanRenderer::record_command_buffer_with_gui(VkCommandBuffer cmd, u32 image_index,
													const GUIConsole &gui,
													const GUIConsole::Rect &game_rect)
{
	VkCommandBufferBeginInfo begin{};
	begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
	vkBeginCommandBuffer(cmd, &begin);
	// Upload game texture as before
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
	vkCmdPipelineBarrier(
		cmd,
		first_upload ? VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT : VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
		VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &to_transfer);
	VkBufferImageCopy region{};
	region.imageExtent = {tex_w_, tex_h_, 1};
	region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
	region.imageSubresource.layerCount = 1;
	vkCmdCopyBufferToImage(cmd, texture_buffer, texture_image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
						   1, &region);
	VkImageMemoryBarrier to_shader = to_transfer;
	to_shader.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
	to_shader.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
	to_shader.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
	to_shader.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
	vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
						 0, 0, nullptr, 0, nullptr, 1, &to_shader);
	texture_layout_ = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
	VkClearValue clear{}; // Body color as clear? Use dark gray, but GBC body will be drawn as rect,
						  // so clear to black is fine.
	clear.color = {{0.08f, 0.08f, 0.08f, 1.0f}};
	VkRenderPassBeginInfo rp{};
	rp.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
	rp.renderPass = render_pass;
	rp.framebuffer = swapchain_framebuffers[image_index];
	rp.renderArea.extent = swapchain_extent;
	rp.clearValueCount = 1;
	rp.pClearValues = &clear;
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
		vp_body.x = bd.x * scale_x;
		vp_body.y = bd.y * scale_y;
		vp_body.width = bd.w * scale_x;
		vp_body.height = bd.h * scale_y;
		vp_body.minDepth = 0;
		vp_body.maxDepth = 1;
		vkCmdSetViewport(cmd, 0, 1, &vp_body);
		VkRect2D sc_body{};
		sc_body.offset = {static_cast<i32>(vp_body.x), static_cast<i32>(vp_body.y)};
		sc_body.extent = {static_cast<u32>(vp_body.width), static_cast<u32>(vp_body.height)};
		vkCmdSetScissor(cmd, 0, 1, &sc_body);
		push_neutral_video_constants(cmd);
		vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, graphics_pipeline);
		VkBuffer vbs[1] = {vertex_buffer};
		VkDeviceSize offs[1] = {0};
		vkCmdBindVertexBuffers(cmd, 0, 1, vbs, offs);
		vkCmdBindIndexBuffer(cmd, index_buffer, 0, VK_INDEX_TYPE_UINT16);
		vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_layout, 0, 1,
								&body_descriptor_sets[0], 0, nullptr);
		vkCmdDrawIndexed(cmd, 6, 1, 0, 0, 0);
	}
	// 2) Game texture inside the photo's LCD rect (caller passes the
	// aspect-correct rect: GB-shaped for GB/GBC, 3:2-fitted for GBA).
	const GUIConsole::Rect scr = game_rect;
	const PixelRect gp =
		fit_aspect(scr.x * scale_x, scr.y * scale_y, scr.w * scale_x, scr.h * scale_y);
	VkViewport vp_game{};
	vp_game.x = gp.x;
	vp_game.y = gp.y;
	vp_game.width = gp.w;
	vp_game.height = gp.h;
	vp_game.minDepth = 0;
	vp_game.maxDepth = 1;
	vkCmdSetViewport(cmd, 0, 1, &vp_game);
	VkRect2D sc_game{};
	sc_game.offset = {static_cast<i32>(std::floor(vp_game.x)),
					  static_cast<i32>(std::floor(vp_game.y))};
	sc_game.extent = {static_cast<u32>(std::ceil(vp_game.x + vp_game.width)) -
						  static_cast<u32>(std::floor(vp_game.x)),
					  static_cast<u32>(std::ceil(vp_game.y + vp_game.height)) -
						  static_cast<u32>(std::floor(vp_game.y))};
	vkCmdSetScissor(cmd, 0, 1, &sc_game);
	push_video_constants(cmd);
	vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, graphics_pipeline);
	VkBuffer vbs[1] = {vertex_buffer};
	VkDeviceSize offs[1] = {0};
	vkCmdBindVertexBuffers(cmd, 0, 1, vbs, offs);
	vkCmdBindIndexBuffer(cmd, index_buffer, 0, VK_INDEX_TYPE_UINT16);
	vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_layout, 0, 1,
							&descriptor_sets[current_frame], 0, nullptr);
	vkCmdDrawIndexed(cmd, 6, 1, 0, 0, 0);
	// 3) GUI overlay full-window: press highlights, gear, settings, modal.
	VkViewport vp_full{};
	vp_full.x = 0;
	vp_full.y = 0;
	vp_full.width = static_cast<float>(swapchain_extent.width);
	vp_full.height = static_cast<float>(swapchain_extent.height);
	vp_full.minDepth = 0;
	vp_full.maxDepth = 1;
	vkCmdSetViewport(cmd, 0, 1, &vp_full);
	VkRect2D sc_full{};
	sc_full.extent = swapchain_extent;
	vkCmdSetScissor(cmd, 0, 1, &sc_full);
	vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, gui_pipeline);
	VkBuffer gvbs[1] = {gui_vertex_buffer};
	VkDeviceSize goffs[1] = {0};
	vkCmdBindVertexBuffers(cmd, 0, 1, gvbs, goffs);
	vkCmdBindIndexBuffer(cmd, gui_index_buffer, 0, VK_INDEX_TYPE_UINT32);
	vkCmdDrawIndexed(cmd, gui_index_count, 1, 0, 0, 0);
	vkCmdEndRenderPass(cmd);
	vkEndCommandBuffer(cmd);
}

void VulkanRenderer::render_with_gui(const PPU::FrameData &frame, const GUIConsole &gui)
{
	// The game texture is shared with the GBA path (240x160). Without this,
	// a GB/GBC frame uploaded after any GBA frame lands in a 240-wide
	// staging image: row-pitch skew (diagonal distortion) plus a buffer
	// over-read past the 160x144 pixels. Every other upload path sizes the
	// texture first; this one must too.
	if (!ensure_texture(kTexWidth, kTexHeight))
		return;
	vkWaitForFences(device, 1, &in_flight_fences[current_frame], VK_TRUE,
					std::numeric_limits<u64>::max());
	u32 image_index = 0;
	VkResult acq = vkAcquireNextImageKHR(device, swapchain, std::numeric_limits<u64>::max(),
										 image_available_semaphores[current_frame], VK_NULL_HANDLE,
										 &image_index);
	if (acq == VK_ERROR_OUT_OF_DATE_KHR) {
		recreate_swapchain();
		return;
	}
	vkResetFences(device, 1, &in_flight_fences[current_frame]);
	void *dst = nullptr;
	vkMapMemory(device, texture_buffer_memory, 0, static_cast<VkDeviceSize>(tex_w_) * tex_h_ * 4, 0,
				&dst);
	std::memcpy(dst, frame.pixels.data(), static_cast<size_t>(tex_w_) * tex_h_ * 4);
	vkUnmapMemory(device, texture_buffer_memory);
	update_uniform_buffer(current_frame);
	const GUIConsole::Rect gb_rect = gui.screen_rect();
	update_gui_buffers(gui, &gb_rect);
	vkResetCommandBuffer(command_buffers[current_frame], 0);
	record_command_buffer_with_gui(command_buffers[current_frame], image_index, gui, gb_rect);
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
	VkResult res = vkQueuePresentKHR(present_queue, &present);
	if (res == VK_ERROR_OUT_OF_DATE_KHR || res == VK_SUBOPTIMAL_KHR)
		recreate_swapchain();
	current_frame = (current_frame + 1) % max_frames_in_flight;
}

void VulkanRenderer::render_gba_with_gui(const u32 *pixels, u32 w, u32 h, const GUIConsole &gui)
{
	if (pixels == nullptr || w == 0 || h == 0)
		return;
	if (!ensure_texture(w, h))
		return;
	vkWaitForFences(device, 1, &in_flight_fences[current_frame], VK_TRUE,
					std::numeric_limits<u64>::max());
	u32 image_index = 0;
	VkResult acq = vkAcquireNextImageKHR(device, swapchain, std::numeric_limits<u64>::max(),
										 image_available_semaphores[current_frame], VK_NULL_HANDLE,
										 &image_index);
	if (acq == VK_ERROR_OUT_OF_DATE_KHR) {
		recreate_swapchain();
		return;
	}
	vkResetFences(device, 1, &in_flight_fences[current_frame]);

	// GBA framebuffer is 0xAARRGGBB like the GB one: direct copy.
	void *dst = nullptr;
	vkMapMemory(device, texture_buffer_memory, 0, static_cast<VkDeviceSize>(tex_w_) * tex_h_ * 4, 0,
				&dst);
	std::memcpy(dst, pixels, static_cast<size_t>(tex_w_) * tex_h_ * 4);
	vkUnmapMemory(device, texture_buffer_memory);

	update_uniform_buffer(current_frame);
	const GUIConsole::Rect gba_rect = gui.gba_screen_rect();
	update_gui_buffers(gui, &gba_rect);
	vkResetCommandBuffer(command_buffers[current_frame], 0);
	record_command_buffer_with_gui(command_buffers[current_frame], image_index, gui, gba_rect);
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
	VkResult res = vkQueuePresentKHR(present_queue, &present);
	if (res == VK_ERROR_OUT_OF_DATE_KHR || res == VK_SUBOPTIMAL_KHR)
		recreate_swapchain();
	current_frame = (current_frame + 1) % max_frames_in_flight;
}

void VulkanRenderer::record_command_buffer_menu(VkCommandBuffer cmd, u32 image_index,
												const GUIConsole &gui)
{
	VkCommandBufferBeginInfo begin{};
	begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
	vkBeginCommandBuffer(cmd, &begin);
	VkClearValue clear{};
	clear.color = {{0.08f, 0.08f, 0.08f, 1.0f}};
	VkRenderPassBeginInfo rp{};
	rp.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
	rp.renderPass = render_pass;
	rp.framebuffer = swapchain_framebuffers[image_index];
	rp.renderArea.extent = swapchain_extent;
	rp.clearValueCount = 1;
	rp.pClearValues = &clear;
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
		vp_body.x = bd.x * scale_x;
		vp_body.y = bd.y * scale_y;
		vp_body.width = bd.w * scale_x;
		vp_body.height = bd.h * scale_y;
		vp_body.minDepth = 0;
		vp_body.maxDepth = 1;
		vkCmdSetViewport(cmd, 0, 1, &vp_body);
		VkRect2D sc_body{};
		sc_body.offset = {static_cast<i32>(vp_body.x), static_cast<i32>(vp_body.y)};
		sc_body.extent = {static_cast<u32>(vp_body.width), static_cast<u32>(vp_body.height)};
		vkCmdSetScissor(cmd, 0, 1, &sc_body);
		push_neutral_video_constants(cmd);
		vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, graphics_pipeline);
		VkBuffer vbs[1] = {vertex_buffer};
		VkDeviceSize offs[1] = {0};
		vkCmdBindVertexBuffers(cmd, 0, 1, vbs, offs);
		vkCmdBindIndexBuffer(cmd, index_buffer, 0, VK_INDEX_TYPE_UINT16);
		vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_layout, 0, 1,
								&body_descriptor_sets[0], 0, nullptr);
		vkCmdDrawIndexed(cmd, 6, 1, 0, 0, 0);
	}
	// 2) GUI overlay full-window: menu, buttons, settings, modal.
	VkViewport vp_full{};
	vp_full.x = 0;
	vp_full.y = 0;
	vp_full.width = static_cast<float>(swapchain_extent.width);
	vp_full.height = static_cast<float>(swapchain_extent.height);
	vp_full.minDepth = 0;
	vp_full.maxDepth = 1;
	vkCmdSetViewport(cmd, 0, 1, &vp_full);
	VkRect2D sc_full{};
	sc_full.extent = swapchain_extent;
	vkCmdSetScissor(cmd, 0, 1, &sc_full);
	vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, gui_pipeline);
	VkBuffer gvbs[1] = {gui_vertex_buffer};
	VkDeviceSize goffs[1] = {0};
	vkCmdBindVertexBuffers(cmd, 0, 1, gvbs, goffs);
	vkCmdBindIndexBuffer(cmd, gui_index_buffer, 0, VK_INDEX_TYPE_UINT32);
	vkCmdDrawIndexed(cmd, gui_index_count, 1, 0, 0, 0);
	vkCmdEndRenderPass(cmd);
	vkEndCommandBuffer(cmd);
}

void VulkanRenderer::render_menu(const GUIConsole &gui)
{
	vkWaitForFences(device, 1, &in_flight_fences[current_frame], VK_TRUE,
					std::numeric_limits<u64>::max());
	u32 image_index = 0;
	VkResult acq = vkAcquireNextImageKHR(device, swapchain, std::numeric_limits<u64>::max(),
										 image_available_semaphores[current_frame], VK_NULL_HANDLE,
										 &image_index);
	if (acq == VK_ERROR_OUT_OF_DATE_KHR) {
		recreate_swapchain();
		return;
	}
	vkResetFences(device, 1, &in_flight_fences[current_frame]);
	update_uniform_buffer(current_frame);
	update_gui_buffers(gui);
	vkResetCommandBuffer(command_buffers[current_frame], 0);
	record_command_buffer_menu(command_buffers[current_frame], image_index, gui);
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
	VkResult res = vkQueuePresentKHR(present_queue, &present);
	if (res == VK_ERROR_OUT_OF_DATE_KHR || res == VK_SUBOPTIMAL_KHR)
		recreate_swapchain();
	current_frame = (current_frame + 1) % max_frames_in_flight;
}

std::vector<char> VulkanRenderer::read_file(const std::string &filename)
{
	std::ifstream file(filename, std::ios::ate | std::ios::binary);
	if (!file)
		return {};
	const auto size = file.tellg();
	std::vector<char> bytes(static_cast<size_t>(size));
	file.seekg(0);
	file.read(bytes.data(), size);
	return bytes;
}

VkShaderModule VulkanRenderer::create_shader_module(VkDevice dev, const std::vector<char> &code)
{
	VkShaderModuleCreateInfo info{};
	info.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
	info.codeSize = code.size();
	info.pCode = reinterpret_cast<const u32 *>(code.data());
	VkShaderModule mod{};
	vkCreateShaderModule(dev, &info, nullptr, &mod);
	return mod;
}

bool VulkanRenderer::check_validation_layer_support()
{
	return false; // Validation layers off for now; enable with VK_LAYER_KHRONOS_validation.
}

VulkanRenderer::QueueFamilyIndices VulkanRenderer::find_queue_families(VkPhysicalDevice dev,
																	   VkSurfaceKHR surf)
{
	QueueFamilyIndices idx;
	u32 count = 0;
	vkGetPhysicalDeviceQueueFamilyProperties(dev, &count, nullptr);
	std::vector<VkQueueFamilyProperties> props(count);
	vkGetPhysicalDeviceQueueFamilyProperties(dev, &count, props.data());
	for (u32 i = 0; i < count; i++) {
		if (props[i].queueFlags & VK_QUEUE_GRAPHICS_BIT)
			idx.graphics_family = i;
		VkBool32 present = VK_FALSE;
		vkGetPhysicalDeviceSurfaceSupportKHR(dev, i, surf, &present);
		if (present)
			idx.present_family = i;
		if (idx.is_complete())
			break;
	}
	return idx;
}

VulkanRenderer::SwapChainSupportDetails
VulkanRenderer::query_swapchain_support(VkPhysicalDevice dev, VkSurfaceKHR surf)
{
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
		vkGetPhysicalDeviceSurfacePresentModesKHR(dev, surf, &count, details.present_modes.data());
	}
	return details;
}

VkSurfaceFormatKHR
VulkanRenderer::choose_swap_surface_format(const std::vector<VkSurfaceFormatKHR> &available)
{
	for (const auto &f : available) {
		if (f.format == VK_FORMAT_B8G8R8A8_SRGB &&
			f.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) {
			return f;
		}
	}
	return available[0];
}

VkPresentModeKHR
VulkanRenderer::choose_swap_present_mode(const std::vector<VkPresentModeKHR> &available)
{
	for (const auto &m : available) {
		if (m == VK_PRESENT_MODE_MAILBOX_KHR)
			return m;
	}
	return VK_PRESENT_MODE_FIFO_KHR;
}

VkExtent2D VulkanRenderer::choose_swap_extent(const VkSurfaceCapabilitiesKHR &caps, SDL_Window *win)
{
	if (caps.currentExtent.width != std::numeric_limits<u32>::max()) {
		return caps.currentExtent;
	}
	int w = 0, h = 0;
	SDL_GetWindowSizeInPixels(win, &w, &h);
	VkExtent2D extent{static_cast<u32>(w), static_cast<u32>(h)};
	extent.width = std::clamp(extent.width, caps.minImageExtent.width, caps.maxImageExtent.width);
	extent.height =
		std::clamp(extent.height, caps.minImageExtent.height, caps.maxImageExtent.height);
	return extent;
}

VKAPI_ATTR VkBool32 VKAPI_CALL VulkanRenderer::debug_callback(
	VkDebugUtilsMessageSeverityFlagBitsEXT /*severity*/, VkDebugUtilsMessageTypeFlagsEXT /*types*/,
	const VkDebugUtilsMessengerCallbackDataEXT * /*data*/, void * /*user*/)
{
	return VK_FALSE;
}

} // namespace gb
