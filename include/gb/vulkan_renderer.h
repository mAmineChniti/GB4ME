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

	// Dear ImGui overlay (debug readout; future settings/debugger panels).
	// Best-effort: init may fail headless, every other call is a safe no-op
	// until it succeeds. The overlay window is display-only (NoInputs), so
	// it never captures game input.
	bool init_imgui();
	void set_imgui_visible(bool v);
	void toggle_imgui();
	bool imgui_visible() const
	{
		return imgui_visible_;
	}
	void imgui_event(const SDL_Event &ev);
	bool imgui_captures_keyboard() const;
	// Per-frame readout pushed by the frontend (1 frame of lag is fine).
	struct ImguiStats {
		u64 frame = 0;
		std::string rom;
		std::string core;
		float volume = 1.0f;
		int filter = 0;
		bool ff = false;
		// Emulation load: how much of the hardware clock the last second
		// actually achieved (100% = real time) and the same number in
		// emulated MHz, plus audio health.
		float emu_speed_pct = 0.0f;
		float emulated_mhz = 0.0f;
		u32 underruns = 0;
		// Audio-clock state: the pacer's target buffer depth in ms and the
		// drift correction it is currently applying (percent).
		float audio_target_ms = 0.0f;
		float resample_pct = 0.0f;
	};
	void set_imgui_stats(const ImguiStats &s);

	// Volume OSD: transient pill flashed by set_volume(), drawn above all
	// overlay UI while active. Pure clock logic (no upkeep, headless-safe).
	void flash_volume(float v)
	{
		osd_volume_ = v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v);
		osd_until_ = SDL_GetTicks() / 1000.0 + 1.5;
	}
	void flash_volume_for(float v, double seconds)
	{
		flash_volume(v);
		osd_until_ = SDL_GetTicks() / 1000.0 + seconds;
	}
	bool volume_osd_active() const
	{
		return SDL_GetTicks() / 1000.0 < osd_until_;
	}

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
	// Vector-font text vertex (atlas UV + tint). Text draws in its own pass
	// after the flat GUI pass; it is always UI foreground.
	struct TextVertex {
		Vec2 pos;
		Vec2 uv;
		Vec4 color;
	};
	// One baked glyph, in atlas pixels (see stb_truetype.h stbtt_bakedchar).
	// Layout keeps the historical monospace 8px grid: each glyph is centered
	// in its cell, so all existing text metrics stay valid.
	struct FontGlyph {
		float u0 = 0, v0 = 0, u1 = 0, v1 = 0;
		float xoff = 0, yoff = 0, w = 0, h = 0, advance = 0;
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
										const GUIConsole &gui, const GUIConsole::Rect &game_rect);
	void record_command_buffer_menu(VkCommandBuffer command_buffer, u32 image_index,
									const GUIConsole &gui);
	void update_uniform_buffer(u32 current_image);
	// When game_rect is provided (body + game mode, not menu/fullscreen),
	// the uncovered glass around the fitted image is filled with black
	// letterbox bars so the picture reads edge-to-edge in the bezel. The
	// bars never overlap *game_rect, so game pixels are always safe.
	void update_gui_buffers(const GUIConsole &gui, const GUIConsole::Rect *game_rect = nullptr);

	// Vector typeface (src/vulkan/vulkan_renderer_text.cpp): a baked atlas
	// (Cousine per build, else system DejaVu/Noto) rendered through its own
	// pipeline. Best-effort: when no TTF is found, text falls back to the
	// built-in 8x8 bitmap (per-pixel rects, as before).
	static constexpr int kFontFirst = 32;
	static constexpr int kFontCount = 96; // ASCII 32..126
	static constexpr int kFontPx = 28;    // bake height, atlas pixels
	bool font_ok_ = false;
	int font_atlas_px_ = 0;  // atlas is square, pixels
	float font_ascent_ = 0;  // bake-space px, from baseline up
	float font_descent_ = 0; // bake-space px, negative (below baseline)
	FontGlyph font_glyphs_[kFontCount]{};
	VkImage font_image_ = VK_NULL_HANDLE;
	VkDeviceMemory font_image_memory_ = VK_NULL_HANDLE;
	VkImageView font_image_view_ = VK_NULL_HANDLE;
	VkSampler font_sampler_ = VK_NULL_HANDLE;
	VkDescriptorPool font_descriptor_pool_ = VK_NULL_HANDLE;
	VkDescriptorSetLayout font_descriptor_set_layout_ = VK_NULL_HANDLE;
	std::vector<VkDescriptorSet> font_descriptor_sets_;
	VkPipelineLayout font_pipeline_layout_ = VK_NULL_HANDLE;
	VkPipeline font_pipeline_ = VK_NULL_HANDLE;
	VkBuffer font_vertex_buffer_ = VK_NULL_HANDLE;
	VkDeviceMemory font_vertex_buffer_memory_ = VK_NULL_HANDLE;
	VkBuffer font_index_buffer_ = VK_NULL_HANDLE;
	VkDeviceMemory font_index_buffer_memory_ = VK_NULL_HANDLE;
	u32 font_vert_cap_ = 0;
	u32 font_idx_cap_ = 0;
	u32 font_index_count_ = 0;
	bool init_font();
	void destroy_font();
	bool ensure_font_buffers(u32 verts, u32 indices);
	void build_text_buffers(const std::vector<TextVertex> &verts, const std::vector<u32> &indices);
	void draw_text(VkCommandBuffer command_buffer);
	// Emits one string into the text overlay (monospace 8px grid, like the
	// bitmap path). No-op unless a typeface baked successfully. Called by
	// update_gui_buffers; win_* map pixels to NDC exactly like push_rect.
	void emit_text(std::vector<TextVertex> &verts, std::vector<u32> &indices, float x, float y,
				   const std::string &str, float scale, u32 rgba, float win_w, float win_h);
	// Rendered width of a string at a scale (natural glyph advances; falls
	// back to the 8px grid when no typeface is baked, matching emit_text).
	float measure_text(const std::string &str, float scale) const;
	// Bake-space to rendered-pixel factor for a UI scale (cap-height fills
	// ~85% of the 8px cell). Shared by emit_text and measure_text so the
	// measured width always matches what is drawn.
	float text_advance_k(float scale) const;
	// Text origin placing one glyph's ink box centered in a rect (for icon
	// buttons like the gear/close glyphs, whose ink sits high in the em
	// box). Falls back to 8px-box centering without a baked typeface.
	void center_glyph(char ch, float scale, const GUIConsole::Rect &r, float &tx, float &ty);

	// ImGui overlay internals (src/vulkan/vulkan_renderer_imgui.cpp).
	bool imgui_ok_ = false;
	bool imgui_visible_ = false;
	ImguiStats imgui_stats_;
	double imgui_last_t_ = 0.0;
	double imgui_fps_ = 60.0;
	// Volume OSD state (clock-driven; see flash_volume above).
	float osd_volume_ = 1.0f;
	double osd_until_ = 0.0;
	void prepare_imgui(); // NewFrame + build + Render (call before recording).
	void draw_imgui(VkCommandBuffer command_buffer); // Record draw data (inside render pass).
	void shutdown_imgui();

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