// GB4ME Vulkan renderer — pipelines, framebuffers, command pool/buffers, sync,
// static geometry. See include/gb/vulkan_renderer.h for the class interface.

#include "gb/vulkan_renderer.h"
#include "vulkan/vulkan_internal.h"

#include <SDL3/SDL_vulkan.h>
#include <cstddef>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

namespace gb
{
using namespace vulkan_detail;

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

} // namespace gb
