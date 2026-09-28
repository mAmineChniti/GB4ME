// GB4ME Vulkan renderer — game + body-photo textures, descriptors, uniform buffers.
// See include/gb/vulkan_renderer.h for the class interface.

#include "gb/vulkan_renderer.h"
#include "vulkan/vulkan_internal.h"

#include <SDL3/SDL_vulkan.h>
#include <cstring>
#include <string>
#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"

namespace gb
{
using namespace vulkan_detail;

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
	int tex_width = 0, tex_height = 0, tex_channels = 0;
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
	VkBuffer body_staging_buffer = VK_NULL_HANDLE;
	VkDeviceMemory body_staging_buffer_memory = VK_NULL_HANDLE;
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

} // namespace gb
