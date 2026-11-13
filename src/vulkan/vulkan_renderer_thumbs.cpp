// GB4ME Vulkan renderer — menu tile thumbnails (boxart / auto-shots).
// An LRU of decoded tile images, drawn as textured quads over the menu cart
// icon. Best-effort throughout: a missing/unreadable/oversize file simply
// falls back to the drawn cart, and nothing here runs without a device.

#include "gb/vulkan_renderer.h"
#include "vulkan/vulkan_internal.h"

#include "stb_image.h"
#include <SDL3/SDL_vulkan.h>
#include <chrono>
#include <cstring>
#include <filesystem>

namespace gb
{
using namespace vulkan_detail;

namespace
{

u64 file_mtime_ns(const std::string &path, bool &ok)
{
	std::error_code ec;
	const auto t = std::filesystem::last_write_time(path, ec);
	if (ec) {
		ok = false;
		return 0;
	}
	ok = true;
	return static_cast<u64>(
		std::chrono::duration_cast<std::chrono::nanoseconds>(t.time_since_epoch()).count());
}

} // namespace

const VulkanRenderer::ThumbTex *VulkanRenderer::find_thumb(const std::string &path) const
{
	for (const auto &t : thumb_cache_) {
		if (t.path == path)
			return &t;
	}
	return nullptr;
}

bool VulkanRenderer::ensure_thumb_pool()
{
	if (thumb_descriptor_pool_ != VK_NULL_HANDLE)
		return true;
	if (thumb_sampler_ == VK_NULL_HANDLE) {
		VkSamplerCreateInfo sampler{};
		sampler.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
		sampler.magFilter = VK_FILTER_LINEAR;
		sampler.minFilter = VK_FILTER_LINEAR;
		sampler.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
		sampler.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
		sampler.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
		if (vkCreateSampler(device, &sampler, nullptr, &thumb_sampler_) != VK_SUCCESS)
			return false;
	}
	VkDescriptorPoolSize sizes[2] = {};
	sizes[0].type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
	sizes[0].descriptorCount = static_cast<u32>(kThumbCacheMax);
	sizes[1].type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
	sizes[1].descriptorCount = static_cast<u32>(kThumbCacheMax);
	VkDescriptorPoolCreateInfo info{};
	info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
	info.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
	info.maxSets = static_cast<u32>(kThumbCacheMax);
	info.poolSizeCount = 2;
	info.pPoolSizes = sizes;
	return vkCreateDescriptorPool(device, &info, nullptr, &thumb_descriptor_pool_) == VK_SUCCESS;
}

void VulkanRenderer::evict_thumb(size_t index)
{
	if (index >= thumb_cache_.size())
		return;
	ThumbTex &t = thumb_cache_[index];
	if (t.set != VK_NULL_HANDLE && thumb_descriptor_pool_ != VK_NULL_HANDLE)
		vkFreeDescriptorSets(device, thumb_descriptor_pool_, 1, &t.set);
	if (t.view != VK_NULL_HANDLE)
		vkDestroyImageView(device, t.view, nullptr);
	if (t.image != VK_NULL_HANDLE)
		vkDestroyImage(device, t.image, nullptr);
	if (t.memory != VK_NULL_HANDLE)
		vkFreeMemory(device, t.memory, nullptr);
	thumb_cache_.erase(thumb_cache_.begin() + static_cast<ptrdiff_t>(index));
}

bool VulkanRenderer::load_thumb(const std::string &path, u64 mtime_ns)
{
	if (!ensure_thumb_pool() || uniform_buffers.empty())
		return false;
	int w = 0, h = 0, comp = 0;
	unsigned char *pixels = stbi_load(path.c_str(), &w, &h, &comp, 4);
	if (pixels == nullptr)
		return false;
	bool dims_ok = w > 0 && h > 0 && static_cast<u32>(w) <= kThumbMaxDim &&
				   static_cast<u32>(h) <= kThumbMaxDim;
	if (!dims_ok) {
		stbi_image_free(pixels);
		return false;
	}
	wait_idle();
	const VkDeviceSize size = static_cast<VkDeviceSize>(w) * h * 4;
	VkBuffer staging = VK_NULL_HANDLE;
	VkDeviceMemory staging_mem = VK_NULL_HANDLE;
	if (!create_buffer(physical_device, device, size, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
					   VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
					   staging, staging_mem)) {
		stbi_image_free(pixels);
		return false;
	}
	void *dst = nullptr;
	vkMapMemory(device, staging_mem, 0, size, 0, &dst);
	std::memcpy(dst, pixels, static_cast<size_t>(size));
	vkUnmapMemory(device, staging_mem);
	stbi_image_free(pixels);

	VkImage image = VK_NULL_HANDLE;
	VkImageCreateInfo info{};
	info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
	info.imageType = VK_IMAGE_TYPE_2D;
	info.format = VK_FORMAT_R8G8B8A8_SRGB;
	info.extent = {static_cast<u32>(w), static_cast<u32>(h), 1};
	info.mipLevels = 1;
	info.arrayLayers = 1;
	info.samples = VK_SAMPLE_COUNT_1_BIT;
	info.tiling = VK_IMAGE_TILING_OPTIMAL;
	info.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
	info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
	if (vkCreateImage(device, &info, nullptr, &image) != VK_SUCCESS) {
		vkDestroyBuffer(device, staging, nullptr);
		vkFreeMemory(device, staging_mem, nullptr);
		return false;
	}
	VkMemoryRequirements req{};
	vkGetImageMemoryRequirements(device, image, &req);
	const u32 mem_idx =
		find_memory_type(physical_device, req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
	if (mem_idx == std::numeric_limits<u32>::max()) {
		vkDestroyImage(device, image, nullptr);
		vkDestroyBuffer(device, staging, nullptr);
		vkFreeMemory(device, staging_mem, nullptr);
		return false;
	}
	VkDeviceMemory memory = VK_NULL_HANDLE;
	VkMemoryAllocateInfo alloc{};
	alloc.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
	alloc.allocationSize = req.size;
	alloc.memoryTypeIndex = mem_idx;
	if (vkAllocateMemory(device, &alloc, nullptr, &memory) != VK_SUCCESS) {
		vkDestroyImage(device, image, nullptr);
		vkDestroyBuffer(device, staging, nullptr);
		vkFreeMemory(device, staging_mem, nullptr);
		return false;
	}
	vkBindImageMemory(device, image, memory, 0);
	VkCommandBuffer cmd = begin_single_time(device, command_pool);
	VkImageMemoryBarrier barrier{};
	barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
	barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
	barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
	barrier.srcAccessMask = 0;
	barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
	barrier.image = image;
	barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
	barrier.subresourceRange.levelCount = 1;
	barrier.subresourceRange.layerCount = 1;
	vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0,
						 0, nullptr, 0, nullptr, 1, &barrier);
	VkBufferImageCopy region{};
	region.imageExtent = {static_cast<u32>(w), static_cast<u32>(h), 1};
	region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
	region.imageSubresource.layerCount = 1;
	vkCmdCopyBufferToImage(cmd, staging, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
	VkImageMemoryBarrier to_shader = barrier;
	to_shader.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
	to_shader.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
	to_shader.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
	to_shader.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
	vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
						 0, 0, nullptr, 0, nullptr, 1, &to_shader);
	end_single_time(device, command_pool, graphics_queue, cmd);
	vkDestroyBuffer(device, staging, nullptr);
	vkFreeMemory(device, staging_mem, nullptr);

	VkImageView view = VK_NULL_HANDLE;
	VkImageViewCreateInfo view_info{};
	view_info.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
	view_info.image = image;
	view_info.viewType = VK_IMAGE_VIEW_TYPE_2D;
	view_info.format = VK_FORMAT_R8G8B8A8_SRGB;
	view_info.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
	view_info.subresourceRange.levelCount = 1;
	view_info.subresourceRange.layerCount = 1;
	if (vkCreateImageView(device, &view_info, nullptr, &view) != VK_SUCCESS) {
		vkDestroyImage(device, image, nullptr);
		vkFreeMemory(device, memory, nullptr);
		return false;
	}
	// Make room first: the pool slot freed by eviction is reused below.
	while (thumb_cache_.size() >= kThumbCacheMax) {
		size_t lru = 0;
		for (size_t i = 1; i < thumb_cache_.size(); i++) {
			if (thumb_cache_[i].last_frame < thumb_cache_[lru].last_frame)
				lru = i;
		}
		evict_thumb(lru);
	}
	VkDescriptorSet set = VK_NULL_HANDLE;
	VkDescriptorSetAllocateInfo set_alloc{};
	set_alloc.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
	set_alloc.descriptorPool = thumb_descriptor_pool_;
	set_alloc.descriptorSetCount = 1;
	set_alloc.pSetLayouts = &descriptor_set_layout;
	if (vkAllocateDescriptorSets(device, &set_alloc, &set) != VK_SUCCESS) {
		vkDestroyImageView(device, view, nullptr);
		vkDestroyImage(device, image, nullptr);
		vkFreeMemory(device, memory, nullptr);
		return false;
	}
	VkDescriptorBufferInfo buf{};
	buf.buffer = uniform_buffers[0];
	buf.range = sizeof(UniformBufferObject);
	VkDescriptorImageInfo img{};
	img.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
	img.imageView = view;
	img.sampler = thumb_sampler_;
	VkWriteDescriptorSet writes[2] = {};
	writes[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
	writes[0].dstSet = set;
	writes[0].dstBinding = 0;
	writes[0].descriptorCount = 1;
	writes[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
	writes[0].pBufferInfo = &buf;
	writes[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
	writes[1].dstSet = set;
	writes[1].dstBinding = 1;
	writes[1].descriptorCount = 1;
	writes[1].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
	writes[1].pImageInfo = &img;
	vkUpdateDescriptorSets(device, 2, writes, 0, nullptr);

	ThumbTex t;
	t.path = path;
	t.mtime_ns = mtime_ns;
	t.w = static_cast<u32>(w);
	t.h = static_cast<u32>(h);
	t.image = image;
	t.memory = memory;
	t.view = view;
	t.set = set;
	t.last_frame = thumb_frame_;
	thumb_cache_.push_back(t);
	thumbs_ready_.insert(path);
	return true;
}

void VulkanRenderer::resolve_thumbs(const GUIConsole &gui)
{
	thumb_frame_++;
	thumbs_ready_.clear();
	if (device == VK_NULL_HANDLE)
		return;
	const auto &roms = gui.rom_list();
	const auto &filt = gui.filtered_indices();
	const int first = gui.menu_first_visible();
	const int vis = gui.menu_grid_visible_count();
	for (int vi = 0; vi < vis; vi++) {
		const int i = first + vi;
		if (i < 0 || i >= static_cast<int>(filt.size()))
			break;
		const int ri = filt[static_cast<size_t>(i)];
		if (ri < 0 || ri >= static_cast<int>(roms.size()))
			continue;
		const std::string &path = roms[static_cast<size_t>(ri)].thumb_path;
		if (path.empty())
			continue;
		bool ok = false;
		const u64 mtime = file_mtime_ns(path, ok);
		if (!ok)
			continue;
		bool hit = false;
		for (auto &t : thumb_cache_) {
			if (t.path == path) {
				hit = true;
				if (t.mtime_ns != mtime) {
					// Rewritten on disk (new shot): reload in place.
					const size_t idx = static_cast<size_t>(&t - thumb_cache_.data());
					evict_thumb(idx);
					load_thumb(path, mtime);
				} else {
					t.last_frame = thumb_frame_;
					thumbs_ready_.insert(path);
				}
				break;
			}
		}
		if (!hit)
			load_thumb(path, mtime); // miss inserts + marks ready; failure = cart fallback
	}
}

void VulkanRenderer::destroy_thumbs()
{
	if (device == VK_NULL_HANDLE)
		return;
	while (!thumb_cache_.empty())
		evict_thumb(0);
	if (thumb_sampler_ != VK_NULL_HANDLE) {
		vkDestroySampler(device, thumb_sampler_, nullptr);
		thumb_sampler_ = VK_NULL_HANDLE;
	}
	if (thumb_descriptor_pool_ != VK_NULL_HANDLE) {
		vkDestroyDescriptorPool(device, thumb_descriptor_pool_, nullptr);
		thumb_descriptor_pool_ = VK_NULL_HANDLE;
	}
	thumbs_ready_.clear();
	thumb_draws_.clear();
}

} // namespace gb
