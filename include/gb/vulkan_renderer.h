#pragma once

#include "gui_console.h"
#include "ppu.h"
#include "types.h"
#include <SDL3/SDL.h>
#include <array>
#include <optional>
#include <string>
#include <vector>
#include <vulkan/vulkan.h>

namespace gb
{

class VulkanRenderer
{
  public:
	struct QueueFamilyIndices {
		std::optional<u32> graphics_family;
		std::optional<u32> present_family;

		bool is_complete() const
		{
			return graphics_family.has_value() && present_family.has_value();
		}
	};

	struct SwapChainSupportDetails {
		VkSurfaceCapabilitiesKHR capabilities;
		std::vector<VkSurfaceFormatKHR> formats;
		std::vector<VkPresentModeKHR> present_modes;
	};

	VulkanRenderer();
	~VulkanRenderer();

	bool initialize(SDL_Window *window);
	void shutdown();
	void render(const PPU::FrameData &frame);
	// Upscaling filter applied when the frame is stretched to the window.
	void setVideoFilter(VideoFilter f)
	{
		video_filter_ = f;
	}
	void setLcdGrid(f32 depth)
	{
		lcd_grid_ = depth < 0.0f ? 0.0f : (depth > 1.0f ? 1.0f : depth);
	}
	// GBA present path: raw RGBA8 pixels of any size (240x160). The game
	// texture is recreated when the size changes (rare: core switches).
	void render_gba(const u32 *pixels, u32 w, u32 h);
	void render_gba_with_gui(const u32 *pixels, u32 w, u32 h, const GUIConsole &gui);
	void render_with_gui(const PPU::FrameData &frame, const GUIConsole &gui);
	void render_menu(const GUIConsole &gui);
	void resize(u32 width, u32 height);
	void wait_idle();

	u32 get_framebuffer_width() const
	{
		return swapchain_extent.width;
	}
	u32 get_framebuffer_height() const
	{
		return swapchain_extent.height;
	}

  private:
	SDL_Window *window = nullptr;
	VkInstance instance = VK_NULL_HANDLE;
	VkSurfaceKHR surface = VK_NULL_HANDLE;
	VkPhysicalDevice physical_device = VK_NULL_HANDLE;
	VkDevice device = VK_NULL_HANDLE;
	VkQueue graphics_queue = VK_NULL_HANDLE;
	VkQueue present_queue = VK_NULL_HANDLE;
	VkSwapchainKHR swapchain = VK_NULL_HANDLE;
	std::vector<VkImage> swapchain_images;
	VkFormat swapchain_image_format = VK_FORMAT_B8G8R8A8_SRGB;
	VkExtent2D swapchain_extent{};
	std::vector<VkImageView> swapchain_image_views;
	VkRenderPass render_pass = VK_NULL_HANDLE;
	VkPipelineLayout pipeline_layout = VK_NULL_HANDLE;
	VkPipeline graphics_pipeline = VK_NULL_HANDLE;
	VkPipelineLayout gui_pipeline_layout = VK_NULL_HANDLE;
	VkPipeline gui_pipeline = VK_NULL_HANDLE;
	std::vector<VkFramebuffer> swapchain_framebuffers;
	VkCommandPool command_pool = VK_NULL_HANDLE;
	std::vector<VkCommandBuffer> command_buffers;
	std::vector<VkSemaphore> image_available_semaphores;
	std::vector<VkSemaphore> render_finished_semaphores;
	std::vector<VkFence> in_flight_fences;
	u32 current_frame = 0;
	u32 max_frames_in_flight = 2;

	VkBuffer vertex_buffer = VK_NULL_HANDLE;
	VkDeviceMemory vertex_buffer_memory = VK_NULL_HANDLE;
	VkBuffer index_buffer = VK_NULL_HANDLE;
	VkDeviceMemory index_buffer_memory = VK_NULL_HANDLE;
	// GUI colored rect pipeline (body, buttons, bezel). Buffers grow on
	// demand (long ROM lists = more text); indices are 32-bit so large
	// menus can never wrap around and drop later draws (gear/footer).
	VkBuffer gui_vertex_buffer = VK_NULL_HANDLE;
	VkDeviceMemory gui_vertex_buffer_memory = VK_NULL_HANDLE;
	VkBuffer gui_index_buffer = VK_NULL_HANDLE;
	VkDeviceMemory gui_index_buffer_memory = VK_NULL_HANDLE;
	u32 gui_index_count = 0;
	u32 gui_vert_cap_ = 0;
	u32 gui_idx_cap_ = 0;

	VkBuffer texture_buffer = VK_NULL_HANDLE;
	VkDeviceMemory texture_buffer_memory = VK_NULL_HANDLE;
	VkImage texture_image = VK_NULL_HANDLE;
	VkDeviceMemory texture_image_memory = VK_NULL_HANDLE;
	VkImageView texture_image_view = VK_NULL_HANDLE;
	VkSampler texture_sampler = VK_NULL_HANDLE;
	// Upscaling state, pushed to the fragment shader for every frame draw.
	// The shader does all filtering manually on top of the NEAREST sampler, so
	// this is the only per-draw state the filter needs.
	struct VideoPush {
		u32 mode;  // VideoFilter
		f32 grid;  // LCD grid border depth, 0 = off
		u32 flags; // reserved, must be 0
		u32 pad;
	};
	VideoFilter video_filter_ = VideoFilter::Nearest;
	f32 lcd_grid_ = 0.0f;
	// Letterboxes a draw rect to the source aspect ratio (the layout the
	// renderer has always used; not an integer/whole-multiple scale).
	struct PixelRect {
		f32 x, y, w, h;
	};
	PixelRect fit_aspect(f32 x, f32 y, f32 w, f32 h) const;
	// Current game-texture dimensions. Recreated on demand when the core
	// changes frame size (160x144 GB vs 240x160 GBA).
	u32 tex_w_ = 0;
	u32 tex_h_ = 0;
	// Current layout of texture_image. First upload transitions from
	// UNDEFINED; every later frame must transition from SHADER_READ_ONLY
	// (re-using UNDEFINED discards the image and shows garbage on drivers
	// that take the layout at face value).
	VkImageLayout texture_layout_ = VK_IMAGE_LAYOUT_UNDEFINED;
	// GBC body photo texture (assets/gbc_body.png)
	VkImage body_image = VK_NULL_HANDLE;
	VkDeviceMemory body_image_memory = VK_NULL_HANDLE;
	VkImageView body_image_view = VK_NULL_HANDLE;
	VkSampler body_sampler = VK_NULL_HANDLE;
	// True when the photo shell texture loaded (best-effort).
	bool body_photo_ok_ = false;
	VkDescriptorSetLayout body_descriptor_set_layout = VK_NULL_HANDLE;
	std::vector<VkDescriptorSet> body_descriptor_sets;
	VkDescriptorPool body_descriptor_pool = VK_NULL_HANDLE;
	VkDescriptorSetLayout descriptor_set_layout = VK_NULL_HANDLE;
	VkDescriptorPool descriptor_pool = VK_NULL_HANDLE;
	std::vector<VkDescriptorSet> descriptor_sets;

	struct Vertex {
		Vec2 pos;
		Vec2 tex_coord;
	};
	struct GuiVertex {
		Vec2 pos;
		Vec4 color;
	};

	struct UniformBufferObject {
		Mat4 mvp;
	};

	std::vector<VkBuffer> uniform_buffers;
	std::vector<VkDeviceMemory> uniform_buffers_memory;
	std::vector<void *> uniform_buffers_mapped;

	bool create_instance();
	bool create_surface();
	bool pick_physical_device();
	bool create_logical_device();
	bool create_swapchain();
	bool create_image_views();
	bool create_render_pass();
	bool create_descriptor_set_layout();
	bool create_graphics_pipeline();
	bool create_gui_pipeline();
	bool create_framebuffers();
	bool create_command_pool();
	bool create_command_buffers();
	bool create_sync_objects();
	bool create_vertex_buffer();
	bool create_index_buffer();
	bool create_gui_buffers();
	bool create_texture(u32 w, u32 h);
	void destroy_texture();
	bool ensure_texture(u32 w, u32 h);
	void update_texture_descriptors();
	void present_frame(u32 image_index);
	void push_video_constants(VkCommandBuffer cmd);
	void push_neutral_video_constants(VkCommandBuffer cmd);
	void push_video_state(VkCommandBuffer cmd, VideoFilter f, f32 grid);
	bool create_body_texture();
	bool create_descriptor_pool();
	bool create_body_photo_descriptors();
	bool create_descriptor_sets();
	bool create_uniform_buffers();

	void recreate_swapchain();
	bool ensure_gui_buffers(u32 verts, u32 indices);
	void destroy_gui_buffers();
	void record_command_buffer(VkCommandBuffer command_buffer, u32 image_index);
	void record_command_buffer_with_gui(VkCommandBuffer command_buffer, u32 image_index,
										const GUIConsole &gui,
										const GUIConsole::Rect &game_rect);
	void record_command_buffer_menu(VkCommandBuffer command_buffer, u32 image_index,
									const GUIConsole &gui);
	void update_uniform_buffer(u32 current_image);
	// When game_rect is provided (body + game mode, not menu/fullscreen),
	// the uncovered glass around the fitted image is filled with black
	// letterbox bars so the picture reads edge-to-edge in the bezel. The
	// bars never overlap *game_rect, so game pixels are always safe.
	void update_gui_buffers(const GUIConsole &gui, const GUIConsole::Rect *game_rect = nullptr);

	static std::vector<char> read_file(const std::string &filename);
	static VkShaderModule create_shader_module(VkDevice device, const std::vector<char> &code);
	static bool check_validation_layer_support();
	static QueueFamilyIndices find_queue_families(VkPhysicalDevice device, VkSurfaceKHR surface);
	static SwapChainSupportDetails query_swapchain_support(VkPhysicalDevice device,
														   VkSurfaceKHR surface);
	static VkSurfaceFormatKHR
	choose_swap_surface_format(const std::vector<VkSurfaceFormatKHR> &available_formats);
	static VkPresentModeKHR
	choose_swap_present_mode(const std::vector<VkPresentModeKHR> &available_present_modes);
	static VkExtent2D choose_swap_extent(const VkSurfaceCapabilitiesKHR &capabilities,
										 SDL_Window *window);

	static VKAPI_ATTR VkBool32 VKAPI_CALL
	debug_callback(VkDebugUtilsMessageSeverityFlagBitsEXT message_severity,
				   VkDebugUtilsMessageTypeFlagsEXT message_types,
				   const VkDebugUtilsMessengerCallbackDataEXT *callback_data, void *user_data);
};

} // namespace gb