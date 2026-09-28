// Shared Vulkan helpers (texture size constants, buffer/single-time utilities).
// Internal to src/vulkan/*; the renderer class lives in gb/vulkan_renderer.h.
#pragma once

#include "gb/types.h"
#include <limits>
#include <string>
#include <vector>
#include <vulkan/vulkan.h>

namespace gb
{
namespace vulkan_detail
{

constexpr u32 kTexWidth = SCREEN_WIDTH;
constexpr u32 kTexHeight = SCREEN_HEIGHT;
constexpr VkFormat kTexFormat = VK_FORMAT_B8G8R8A8_SRGB;

inline u32 find_memory_type(VkPhysicalDevice phys, u32 filter, VkMemoryPropertyFlags props)
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

inline bool create_buffer(VkPhysicalDevice phys, VkDevice dev, VkDeviceSize size,
						  VkBufferUsageFlags usage, VkMemoryPropertyFlags props, VkBuffer &buf,
						  VkDeviceMemory &mem)
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

inline VkCommandBuffer begin_single_time(VkDevice dev, VkCommandPool pool)
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

inline void end_single_time(VkDevice dev, VkCommandPool pool, VkQueue queue, VkCommandBuffer cmd)
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

} // namespace vulkan_detail

// Vector-font baking (stb_truetype, implemented in vulkan_renderer_text.cpp).
// stb types never leak: the baked glyph mirrors FontGlyph (vulkan_renderer.h)
// so the headless selftest can exercise baking without a Vulkan device.
namespace font_detail
{

struct BakedGlyph {
	float u0 = 0, v0 = 0, u1 = 0, v1 = 0;
	float xoff = 0, yoff = 0, w = 0, h = 0, advance = 0;
};

struct BakedFont {
	static constexpr int kCount = 96; // ASCII 32..126
	static constexpr int kPx = 32;    // SDF reference height, font pixels
	static constexpr int kPad = 5;    // SDF padding border, font pixels
	static constexpr int kAtlas = 512;
	std::vector<u8> pixels; // kAtlas*kAtlas signed-distance coverage
	float ascent = 0;       // bake-space px above baseline
	float descent = 0;      // bake-space px, negative (below baseline)
	BakedGlyph glyphs[kCount]{};
};

// Bakes `path` (TTF) into coverage pixels + metrics. False when the file is
// missing, unparsable, or the atlas overflows — callers fall back gracefully.
bool bake_font_file(const std::string &path, BakedFont &out);

} // namespace font_detail
} // namespace gb
