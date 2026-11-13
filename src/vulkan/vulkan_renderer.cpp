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

namespace gb
{

namespace
{

constexpr u32 kTexWidth = SCREEN_WIDTH;
constexpr u32 kTexHeight = SCREEN_HEIGHT;

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
	// Vector typeface is best-effort: without it all text falls back to the
	// built-in 8x8 bitmap, so failure never fails initialization.
	init_font();
	return true;
}

void VulkanRenderer::shutdown()
{
	if (device == VK_NULL_HANDLE)
		return;
	wait_idle();
	shutdown_imgui(); // ImGui owns a pipeline + descriptor pool on device.
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
	destroy_thumbs();
	destroy_gui_buffers();
	destroy_font();
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

// GUI overlay buffers live in vulkan_renderer_gui.cpp.

// Game + photo textures, descriptors and uniform buffers live in
// vulkan_renderer_textures.cpp.
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

// Frame recording + present paths live in vulkan_renderer_draw.cpp.
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
