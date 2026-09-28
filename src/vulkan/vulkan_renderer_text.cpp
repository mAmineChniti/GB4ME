// GB4ME Vulkan renderer — vector typeface (baked atlas + text pipeline).
// See include/gb/vulkan_renderer.h for the class interface.
//
// Headless-testable split: font_detail::bake_font_file() is pure stb_truetype
// (no Vulkan) so the selftest exercises baking; only upload/pipeline need a
// device. When no TTF is found anywhere, every entry point degrades to the
// built-in 8x8 bitmap path in update_gui_buffers.

#include "gb/vulkan_renderer.h"
#include "vulkan/vulkan_internal.h"

#define STB_TRUETYPE_IMPLEMENTATION
#include "stb_truetype.h"

#include <SDL3/SDL_vulkan.h>
#include <algorithm>
#include <cstring>
#include <fstream>
#include <limits>
#include <string>
#include <utility>
#include <vector>

namespace gb
{
namespace font_detail
{

bool bake_font_file(const std::string &path, BakedFont &out)
{
	std::ifstream f(path, std::ios::binary | std::ios::ate);
	if (!f)
		return false;
	const auto size = f.tellg();
	if (size <= 0 || size > 16 * 1024 * 1024)
		return false;
	f.seekg(0, std::ios::beg);
	std::vector<u8> data(static_cast<size_t>(size));
	if (!f.read(reinterpret_cast<char *>(data.data()), size))
		return false;
	stbtt_fontinfo info;
	if (!stbtt_InitFont(&info, data.data(), 0))
		return false;
	int ascent_units = 0, descent_units = 0, gap = 0;
	stbtt_GetFontVMetrics(&info, &ascent_units, &descent_units, &gap);
	const float vscale = stbtt_ScaleForPixelHeight(&info, static_cast<float>(BakedFont::kPx));
	out.pixels.assign(static_cast<size_t>(BakedFont::kAtlas) * BakedFont::kAtlas, 0);
	// Signed-distance fields + shelf packing: resolution-independent coverage,
	// so tiny menu text stays crisp instead of blurring into its neighbours.
	// (Coverage-alpha baking needs ~match between bake and display size; SDF
	// does not.) Rects include the SDF padding border; advances are exact.
	int pen_x = 0, pen_y = 0, row_h = 0;
	for (int i = 0; i < BakedFont::kCount; i++) {
		const int ch = 32 + i;
		int w = 0, h = 0, xoff = 0, yoff = 0;
		// Empty glyphs (space) return NULL — a valid empty slot, not failure.
		unsigned char *bmp = stbtt_GetCodepointSDF(&info, vscale, ch, BakedFont::kPad, 128, 8.0f,
												   &w, &h, &xoff, &yoff);
		if (!bmp) {
			w = 0;
			h = 0;
		}
		BakedGlyph &g = out.glyphs[i];
		int advance = 0;
		stbtt_GetGlyphHMetrics(&info, stbtt_FindGlyphIndex(&info, ch), &advance, nullptr);
		g.advance = advance * vscale;
		g.xoff = static_cast<float>(xoff);
		g.yoff = static_cast<float>(yoff);
		g.w = 0;
		g.h = 0;
		if (w > 0 && h > 0) {
			if (pen_x + w > BakedFont::kAtlas) {
				pen_x = 0;
				pen_y += row_h;
				row_h = 0;
			}
			if (pen_y + h > BakedFont::kAtlas) {
				stbtt_FreeSDF(bmp, info.userdata);
				return false;
			}
			for (int row = 0; row < h; row++) {
				std::memcpy(
					&out.pixels[(static_cast<size_t>(pen_y + row) * BakedFont::kAtlas) + pen_x],
					bmp + static_cast<size_t>(row) * w, static_cast<size_t>(w));
			}
			const float inv = 1.0f / BakedFont::kAtlas;
			g.u0 = pen_x * inv;
			g.v0 = pen_y * inv;
			g.u1 = (pen_x + w) * inv;
			g.v1 = (pen_y + h) * inv;
			g.w = static_cast<float>(w);
			g.h = static_cast<float>(h);
			pen_x += w;
			if (h > row_h)
				row_h = h;
		}
		stbtt_FreeSDF(bmp, info.userdata);
	}
	// Sanity: 'A' must have inked extent beyond its padding border.
	const BakedGlyph &chk = out.glyphs['A' - 32];
	if ((chk.u1 - chk.u0) * BakedFont::kAtlas <= 2 * BakedFont::kPad)
		return false;
	out.ascent = ascent_units * vscale;
	out.descent = descent_units * vscale;
	return true;
}

} // namespace font_detail

bool VulkanRenderer::init_font()
{
	if (font_ok_)
		return true;
	// Cousine (monospace, Apache-2.0) staged by the build, then system
	// fallbacks (Debian/Ubuntu, Arch, Noto), then dev/asset layouts.
	const char *base = SDL_GetBasePath();
	const std::string base_dir = base ? base : "";
	const std::string candidates[] = {
		base_dir + "fonts/Cousine-Regular.ttf",
		base_dir + "../share/GB4ME/fonts/Cousine-Regular.ttf",
		"/usr/share/GB4ME/fonts/Cousine-Regular.ttf",
		"/usr/local/share/GB4ME/fonts/Cousine-Regular.ttf",
		"/usr/share/fonts/TTF/DejaVuSans.ttf",
		"/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
		"/usr/share/fonts/noto/NotoSans-Regular.ttf",
		"bin/fonts/Cousine-Regular.ttf",
		"assets/fonts/Cousine-Regular.ttf",
	};
	font_detail::BakedFont baked;
	bool baked_ok = false;
	for (const auto &p : candidates) {
		if (font_detail::bake_font_file(p, baked)) {
			baked_ok = true;
			break;
		}
	}
	if (!baked_ok)
		return false; // caller falls back to the 8x8 bitmap path

	// Upload the coverage atlas (R8, linear for smooth downscaling).
	const VkDeviceSize bytes =
		static_cast<VkDeviceSize>(font_detail::BakedFont::kAtlas) * font_detail::BakedFont::kAtlas;
	VkBuffer staging = VK_NULL_HANDLE;
	VkDeviceMemory staging_mem = VK_NULL_HANDLE;
	if (!vulkan_detail::create_buffer(
			physical_device, device, bytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
			VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, staging,
			staging_mem)) {
		return false;
	}
	void *dst = nullptr;
	vkMapMemory(device, staging_mem, 0, bytes, 0, &dst);
	std::memcpy(dst, baked.pixels.data(), static_cast<size_t>(bytes));
	vkUnmapMemory(device, staging_mem);
	VkImageCreateInfo info{};
	info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
	info.imageType = VK_IMAGE_TYPE_2D;
	info.format = VK_FORMAT_R8_UNORM;
	info.extent = {font_detail::BakedFont::kAtlas, font_detail::BakedFont::kAtlas, 1};
	info.mipLevels = 1;
	info.arrayLayers = 1;
	info.samples = VK_SAMPLE_COUNT_1_BIT;
	info.tiling = VK_IMAGE_TILING_OPTIMAL;
	info.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
	info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
	bool ok = vkCreateImage(device, &info, nullptr, &font_image_) == VK_SUCCESS;
	VkDeviceMemory mem = VK_NULL_HANDLE;
	if (ok) {
		VkMemoryRequirements req{};
		vkGetImageMemoryRequirements(device, font_image_, &req);
		const u32 idx = vulkan_detail::find_memory_type(physical_device, req.memoryTypeBits,
														VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
		VkMemoryAllocateInfo alloc{};
		alloc.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
		alloc.allocationSize = req.size;
		alloc.memoryTypeIndex = idx;
		ok = idx != std::numeric_limits<u32>::max() &&
			 vkAllocateMemory(device, &alloc, nullptr, &mem) == VK_SUCCESS;
	}
	if (ok)
		vkBindImageMemory(device, font_image_, mem, 0);
	VkCommandBuffer cmd = VK_NULL_HANDLE;
	if (ok) {
		cmd = vulkan_detail::begin_single_time(device, command_pool);
		VkImageMemoryBarrier barrier{};
		barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
		barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
		barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
		barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
		barrier.image = font_image_;
		barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
		barrier.subresourceRange.levelCount = 1;
		barrier.subresourceRange.layerCount = 1;
		vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
							 0, 0, nullptr, 0, nullptr, 1, &barrier);
		VkBufferImageCopy region{};
		region.imageExtent = {font_detail::BakedFont::kAtlas, font_detail::BakedFont::kAtlas, 1};
		region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
		region.imageSubresource.layerCount = 1;
		vkCmdCopyBufferToImage(cmd, staging, font_image_, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1,
							   &region);
		VkImageMemoryBarrier to_shader = barrier;
		to_shader.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
		to_shader.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
		to_shader.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
		to_shader.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
		vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
							 VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, nullptr, 0, nullptr, 1,
							 &to_shader);
		vulkan_detail::end_single_time(device, command_pool, graphics_queue, cmd);
	}
	vkDestroyBuffer(device, staging, nullptr);
	vkFreeMemory(device, staging_mem, nullptr);
	if (!ok) {
		if (font_image_ != VK_NULL_HANDLE) {
			vkDestroyImage(device, font_image_, nullptr);
			font_image_ = VK_NULL_HANDLE;
		}
		return false;
	}
	font_image_memory_ = mem;
	VkImageViewCreateInfo view{};
	view.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
	view.image = font_image_;
	view.viewType = VK_IMAGE_VIEW_TYPE_2D;
	view.format = VK_FORMAT_R8_UNORM;
	view.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
	view.subresourceRange.levelCount = 1;
	view.subresourceRange.layerCount = 1;
	ok = vkCreateImageView(device, &view, nullptr, &font_image_view_) == VK_SUCCESS;
	VkSamplerCreateInfo sampler{};
	sampler.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
	sampler.magFilter = VK_FILTER_LINEAR;
	sampler.minFilter = VK_FILTER_LINEAR;
	sampler.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
	sampler.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
	sampler.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
	if (ok)
		ok = vkCreateSampler(device, &sampler, nullptr, &font_sampler_) == VK_SUCCESS;
	// Own descriptor layout (sampler only) + pool + per-frame sets.
	VkDescriptorSetLayoutBinding binding{};
	binding.binding = 0;
	binding.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
	binding.descriptorCount = 1;
	binding.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
	VkDescriptorSetLayoutCreateInfo layout{};
	layout.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
	layout.bindingCount = 1;
	layout.pBindings = &binding;
	if (ok)
		ok = vkCreateDescriptorSetLayout(device, &layout, nullptr, &font_descriptor_set_layout_) ==
			 VK_SUCCESS;
	VkDescriptorPoolSize pool_size{};
	pool_size.type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
	pool_size.descriptorCount = max_frames_in_flight;
	VkDescriptorPoolCreateInfo pool{};
	pool.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
	pool.maxSets = max_frames_in_flight;
	pool.poolSizeCount = 1;
	pool.pPoolSizes = &pool_size;
	if (ok)
		ok = vkCreateDescriptorPool(device, &pool, nullptr, &font_descriptor_pool_) == VK_SUCCESS;
	if (ok) {
		std::vector<VkDescriptorSetLayout> layouts(max_frames_in_flight,
												   font_descriptor_set_layout_);
		VkDescriptorSetAllocateInfo alloc{};
		alloc.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
		alloc.descriptorPool = font_descriptor_pool_;
		alloc.descriptorSetCount = max_frames_in_flight;
		alloc.pSetLayouts = layouts.data();
		font_descriptor_sets_.resize(max_frames_in_flight);
		ok = vkAllocateDescriptorSets(device, &alloc, font_descriptor_sets_.data()) == VK_SUCCESS;
	}
	if (ok) {
		for (u32 i = 0; i < max_frames_in_flight; i++) {
			VkDescriptorImageInfo img{};
			img.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
			img.imageView = font_image_view_;
			img.sampler = font_sampler_;
			VkWriteDescriptorSet write{};
			write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
			write.dstSet = font_descriptor_sets_[i];
			write.dstBinding = 0;
			write.descriptorCount = 1;
			write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
			write.pImageInfo = &img;
			vkUpdateDescriptorSets(device, 1, &write, 0, nullptr);
		}
	}
	// Text pipeline: same render pass and blending as the GUI pipeline, but
	// the text vertex format (pos + uv + tint) and text shaders.
	if (ok) {
		const char *sdl_base = SDL_GetBasePath();
		const std::string base_dir = sdl_base ? sdl_base : "";
		const std::string bases[6] = {"shaders/",
									  "bin/shaders/",
									  base_dir + "shaders/",
									  base_dir + "../share/GB4ME/shaders/",
									  "/usr/share/GB4ME/shaders/",
									  "/usr/local/share/GB4ME/shaders/"};
		std::vector<char> vert, frag;
		for (const auto &b : bases) {
			vert = read_file(b + "text.vert.spv");
			frag = read_file(b + "text.frag.spv");
			if (!vert.empty() && !frag.empty())
				break;
		}
		ok = !vert.empty() && !frag.empty();
		if (ok) {
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
			bind.stride = sizeof(TextVertex);
			bind.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;
			VkVertexInputAttributeDescription attrs[3] = {};
			attrs[0].location = 0;
			attrs[0].format = VK_FORMAT_R32G32_SFLOAT;
			attrs[0].offset = 0;
			attrs[1].location = 1;
			attrs[1].format = VK_FORMAT_R32G32_SFLOAT;
			attrs[1].offset = sizeof(Vec2);
			attrs[2].location = 2;
			attrs[2].format = VK_FORMAT_R32G32B32A32_SFLOAT;
			attrs[2].offset = sizeof(Vec2) * 2;
			VkPipelineVertexInputStateCreateInfo vertex{};
			vertex.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
			vertex.vertexBindingDescriptionCount = 1;
			vertex.pVertexBindingDescriptions = &bind;
			vertex.vertexAttributeDescriptionCount = 3;
			vertex.pVertexAttributeDescriptions = attrs;
			VkPipelineInputAssemblyStateCreateInfo assembly{};
			assembly.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
			assembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
			VkPipelineViewportStateCreateInfo vp{};
			vp.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
			vp.viewportCount = 1;
			vp.scissorCount = 1;
			const VkDynamicState dyn_states[2] = {VK_DYNAMIC_STATE_VIEWPORT,
												  VK_DYNAMIC_STATE_SCISSOR};
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
			VkPipelineLayoutCreateInfo pl{};
			pl.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
			pl.setLayoutCount = 1;
			pl.pSetLayouts = &font_descriptor_set_layout_;
			ok = vkCreatePipelineLayout(device, &pl, nullptr, &font_pipeline_layout_) == VK_SUCCESS;
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
			pipe.layout = font_pipeline_layout_;
			pipe.renderPass = render_pass;
			if (ok)
				ok = vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &pipe, nullptr,
											   &font_pipeline_) == VK_SUCCESS;
			vkDestroyShaderModule(device, vert_mod, nullptr);
			vkDestroyShaderModule(device, frag_mod, nullptr);
		}
	}
	if (!ok) {
		destroy_font();
		return false;
	}
	font_atlas_px_ = font_detail::BakedFont::kAtlas;
	font_ascent_ = baked.ascent;
	font_descent_ = baked.descent;
	for (int i = 0; i < font_detail::BakedFont::kCount; i++) {
		const font_detail::BakedGlyph &b = baked.glyphs[i];
		FontGlyph &g = font_glyphs_[i];
		g.u0 = b.u0;
		g.v0 = b.v0;
		g.u1 = b.u1;
		g.v1 = b.v1;
		g.xoff = b.xoff;
		g.yoff = b.yoff;
		g.w = b.w;
		g.h = b.h;
		g.advance = b.advance;
	}
	font_ok_ = true;
	return true;
}

void VulkanRenderer::destroy_font()
{
	if (device == VK_NULL_HANDLE)
		return;
	if (font_vertex_buffer_ != VK_NULL_HANDLE)
		vkDestroyBuffer(device, font_vertex_buffer_, nullptr);
	if (font_vertex_buffer_memory_ != VK_NULL_HANDLE)
		vkFreeMemory(device, font_vertex_buffer_memory_, nullptr);
	if (font_index_buffer_ != VK_NULL_HANDLE)
		vkDestroyBuffer(device, font_index_buffer_, nullptr);
	if (font_index_buffer_memory_ != VK_NULL_HANDLE)
		vkFreeMemory(device, font_index_buffer_memory_, nullptr);
	font_vertex_buffer_ = VK_NULL_HANDLE;
	font_vertex_buffer_memory_ = VK_NULL_HANDLE;
	font_index_buffer_ = VK_NULL_HANDLE;
	font_index_buffer_memory_ = VK_NULL_HANDLE;
	font_vert_cap_ = 0;
	font_idx_cap_ = 0;
	font_index_count_ = 0;
	vkDestroyPipeline(device, font_pipeline_, nullptr);
	vkDestroyPipelineLayout(device, font_pipeline_layout_, nullptr);
	vkDestroyDescriptorPool(device, font_descriptor_pool_, nullptr);
	vkDestroyDescriptorSetLayout(device, font_descriptor_set_layout_, nullptr);
	vkDestroySampler(device, font_sampler_, nullptr);
	vkDestroyImageView(device, font_image_view_, nullptr);
	vkDestroyImage(device, font_image_, nullptr);
	vkFreeMemory(device, font_image_memory_, nullptr);
	font_pipeline_ = VK_NULL_HANDLE;
	font_pipeline_layout_ = VK_NULL_HANDLE;
	font_descriptor_pool_ = VK_NULL_HANDLE;
	font_descriptor_set_layout_ = VK_NULL_HANDLE;
	font_sampler_ = VK_NULL_HANDLE;
	font_image_view_ = VK_NULL_HANDLE;
	font_image_ = VK_NULL_HANDLE;
	font_image_memory_ = VK_NULL_HANDLE;
	font_descriptor_sets_.clear();
	font_ok_ = false;
}

bool VulkanRenderer::ensure_font_buffers(u32 verts, u32 indices)
{
	if (verts <= font_vert_cap_ && indices <= font_idx_cap_)
		return true;
	u32 nv = std::max(font_vert_cap_ * 2u, verts);
	u32 ni = std::max(font_idx_cap_ * 2u, indices);
	if (nv < 4096)
		nv = 4096;
	if (ni < 8192)
		ni = 8192;
	wait_idle();
	if (font_vertex_buffer_ != VK_NULL_HANDLE) {
		vkDestroyBuffer(device, font_vertex_buffer_, nullptr);
		vkFreeMemory(device, font_vertex_buffer_memory_, nullptr);
		vkDestroyBuffer(device, font_index_buffer_, nullptr);
		vkFreeMemory(device, font_index_buffer_memory_, nullptr);
		font_vertex_buffer_ = VK_NULL_HANDLE;
		font_vertex_buffer_memory_ = VK_NULL_HANDLE;
		font_index_buffer_ = VK_NULL_HANDLE;
		font_index_buffer_memory_ = VK_NULL_HANDLE;
	}
	font_vert_cap_ = nv;
	font_idx_cap_ = ni;
	const VkDeviceSize vsize = static_cast<VkDeviceSize>(nv) * sizeof(TextVertex);
	const VkDeviceSize isize = static_cast<VkDeviceSize>(ni) * sizeof(u32);
	if (!vulkan_detail::create_buffer(
			physical_device, device, vsize, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
			VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
			font_vertex_buffer_, font_vertex_buffer_memory_)) {
		return false;
	}
	if (!vulkan_detail::create_buffer(
			physical_device, device, isize, VK_BUFFER_USAGE_INDEX_BUFFER_BIT,
			VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
			font_index_buffer_, font_index_buffer_memory_)) {
		return false;
	}
	return true;
}

void VulkanRenderer::build_text_buffers(const std::vector<TextVertex> &verts,
										const std::vector<u32> &indices)
{
	font_index_count_ = 0;
	if (!font_ok_ || verts.empty() || indices.empty())
		return;
	if (!ensure_font_buffers(static_cast<u32>(verts.size()), static_cast<u32>(indices.size())))
		return;
	void *vdst = nullptr;
	vkMapMemory(device, font_vertex_buffer_memory_, 0, verts.size() * sizeof(TextVertex), 0, &vdst);
	std::memcpy(vdst, verts.data(), verts.size() * sizeof(TextVertex));
	vkUnmapMemory(device, font_vertex_buffer_memory_);
	void *idst = nullptr;
	vkMapMemory(device, font_index_buffer_memory_, 0, indices.size() * sizeof(u32), 0, &idst);
	std::memcpy(idst, indices.data(), indices.size() * sizeof(u32));
	vkUnmapMemory(device, font_index_buffer_memory_);
	font_index_count_ = static_cast<u32>(indices.size());
}

void VulkanRenderer::draw_text(VkCommandBuffer command_buffer)
{
	if (!font_ok_ || font_index_count_ == 0)
		return;
	vkCmdBindPipeline(command_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS, font_pipeline_);
	VkBuffer vbs[1] = {font_vertex_buffer_};
	VkDeviceSize offs[1] = {0};
	vkCmdBindVertexBuffers(command_buffer, 0, 1, vbs, offs);
	vkCmdBindIndexBuffer(command_buffer, font_index_buffer_, 0, VK_INDEX_TYPE_UINT32);
	vkCmdBindDescriptorSets(command_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS, font_pipeline_layout_,
							0, 1, &font_descriptor_sets_[current_frame], 0, nullptr);
	vkCmdDrawIndexed(command_buffer, font_index_count_, 1, 0, 0, 0);
}

void VulkanRenderer::emit_text(std::vector<TextVertex> &verts, std::vector<u32> &indices, float x,
							   float y, const std::string &str, float scale, u32 rgba, float win_w,
							   float win_h)
{
	if (!font_ok_ || win_w <= 0 || win_h <= 0)
		return;
	const float cell = 8.0f * scale;
	const float k = text_advance_k(scale);
	const float asc = font_ascent_ * k;
	const float desc = -font_descent_ * k;
	const float baseline = y + (cell - (asc + desc)) * 0.5f + asc;
	const Vec4 col = {((rgba >> 16) & 0xFF) / 255.0f, ((rgba >> 8) & 0xFF) / 255.0f,
					  (rgba & 0xFF) / 255.0f, ((rgba >> 24) & 0xFF) / 255.0f};
	auto to_ndc = [&](float px, float py) -> Vec2 {
		return {(px / win_w) * 2.0f - 1.0f, (py / win_h) * 2.0f - 1.0f};
	};
	float cx = x;
	// unsigned char: `char` is signed here, so a byte >= 128 is negative and
	// would slip past the range check into a negative glyph index.
	for (const char raw : str) {
		const unsigned char ch = static_cast<unsigned char>(raw);
		if (ch < kFontFirst || ch >= kFontFirst + kFontCount) {
			cx += cell;
			continue;
		}
		const FontGlyph &g = font_glyphs_[ch - kFontFirst];
		const float adv = g.advance * k;
		if (g.w > 0 && g.h > 0) {
			// Natural advances (not grid cells): the pen follows the typeface,
			// and centering call sites measure the same way via measure_text().
			const float gx = cx + g.xoff * k;
			const float gy = baseline + g.yoff * k;
			const float gw = g.w * k;
			const float gh = g.h * k;
			const u32 base = static_cast<u32>(verts.size());
			const Vec4 c = {col.x, col.y, col.z, col.w};
			verts.push_back({to_ndc(gx, gy), {g.u0, g.v0}, c});
			verts.push_back({to_ndc(gx + gw, gy), {g.u1, g.v0}, c});
			verts.push_back({to_ndc(gx + gw, gy + gh), {g.u1, g.v1}, c});
			verts.push_back({to_ndc(gx, gy + gh), {g.u0, g.v1}, c});
			indices.push_back(base + 0);
			indices.push_back(base + 1);
			indices.push_back(base + 2);
			indices.push_back(base + 2);
			indices.push_back(base + 3);
			indices.push_back(base + 0);
		}
		cx += adv;
	}
}

float VulkanRenderer::text_advance_k(float scale) const
{
	// SDF rects carry a padding border; the cap reference is the ink height.
	const float cap_h = font_glyphs_['A' - kFontFirst].h - 2 * font_detail::BakedFont::kPad;
	const float cap_ref = cap_h > 0.0f ? cap_h : static_cast<float>(kFontPx) * 0.7f;
	return 8.0f * scale * 0.85f / cap_ref;
}

float VulkanRenderer::measure_text(const std::string &str, float scale) const
{
	if (!font_ok_)
		return str.size() * 8.0f * scale;
	const float k = text_advance_k(scale);
	float w = 0;
	for (const char raw : str) {
		const unsigned char ch = static_cast<unsigned char>(raw);
		if (ch < kFontFirst || ch >= kFontFirst + kFontCount)
			w += 8.0f * scale;
		else
			w += font_glyphs_[ch - kFontFirst].advance * k;
	}
	return w;
}

void VulkanRenderer::center_glyph(char ch, float scale, const GUIConsole::Rect &r, float &tx,
								  float &ty)
{
	const float cell = 8.0f * scale;
	const unsigned char uch = static_cast<unsigned char>(ch); // see draw_text
	if (!font_ok_ || uch < kFontFirst || uch >= kFontFirst + kFontCount) {
		tx = r.x + (r.w - cell) * 0.5f;
		ty = r.y + (r.h - cell) * 0.5f;
		return;
	}
	const float k = text_advance_k(scale);
	const FontGlyph &g = font_glyphs_[uch - kFontFirst];
	const float asc = font_ascent_ * k;
	const float desc = -font_descent_ * k;
	// Ink left edge is pen + xoff*k; center the ink box (not the em box,
	// whose descender gap would push descender-less glyphs like '*' up).
	tx = r.x + (r.w - g.w * k) * 0.5f - g.xoff * k;
	// Inverts emit_text's baseline placement so the ink centers vertically.
	ty = r.y + (r.h - g.h * k) * 0.5f - g.yoff * k - asc - (cell - (asc + desc)) * 0.5f;
}

} // namespace gb
