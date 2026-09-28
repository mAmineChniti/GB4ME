// GB4ME Vulkan renderer — Dear ImGui overlay (debug readout; future panels).
// See include/gb/vulkan_renderer.h for the class interface.
//
// Best-effort: init may fail (headless/linked-minimal builds) and every other
// entry point no-ops until it succeeds, so the emulator never depends on it.
// The overlay window is display-only (NoInputs): it cannot capture game input.

#include "gb/vulkan_renderer.h"

#include "backends/imgui_impl_sdl3.h"
#include "backends/imgui_impl_vulkan.h"
#include "imgui.h"
#include <cstdlib>

namespace gb
{

bool VulkanRenderer::init_imgui()
{
	if (imgui_ok_)
		return true;
	if (window == nullptr || device == VK_NULL_HANDLE || render_pass == VK_NULL_HANDLE)
		return false;
	IMGUI_CHECKVERSION();
	ImGui::CreateContext();
	ImGuiIO &io = ImGui::GetIO();
	io.IniFilename = nullptr; // No imgui.ini next to ROMs; layout resets each run.
	io.LogFilename = nullptr;
	ImGui::StyleColorsDark();
	if (!ImGui_ImplSDL3_InitForVulkan(window)) {
		ImGui::DestroyContext();
		return false;
	}
	ImGui_ImplVulkan_InitInfo info{};
	info.ApiVersion = VK_API_VERSION_1_0; // Matches create_instance().
	info.Instance = instance;
	info.PhysicalDevice = physical_device;
	info.Device = device;
	info.QueueFamily = find_queue_families(physical_device, surface).graphics_family.value();
	info.Queue = graphics_queue;
	info.DescriptorPoolSize = 64; // Backend-owned pool (fonts + future AddTexture use).
	info.MinImageCount = 2;
	info.ImageCount = 3; // Covers MAILBOX triple-buffering.
	info.PipelineInfoMain.RenderPass = render_pass;
	info.PipelineInfoMain.Subpass = 0;
	if (!ImGui_ImplVulkan_Init(&info)) {
		ImGui_ImplSDL3_Shutdown();
		ImGui::DestroyContext();
		return false;
	}
	// Font atlas uploads itself on the first RenderDrawData (1.92 texture
	// system); no manual CreateFontsTexture step needed.
	imgui_ok_ = true;
#ifndef GB4ME_RELEASE
	if (std::getenv("GB4ME_DEBUG_OVERLAY") != nullptr)
		imgui_visible_ = true;
#else
	(void) 0;
#endif
	return true;
}

void VulkanRenderer::shutdown_imgui()
{
	if (!imgui_ok_)
		return;
	ImGui_ImplVulkan_Shutdown();
	ImGui_ImplSDL3_Shutdown();
	ImGui::DestroyContext();
	imgui_ok_ = false;
	imgui_visible_ = false;
}

void VulkanRenderer::set_imgui_visible(bool v)
{
	imgui_visible_ = v;
}

void VulkanRenderer::toggle_imgui()
{
	imgui_visible_ = !imgui_visible_;
}

void VulkanRenderer::imgui_event(const SDL_Event &ev)
{
	if (imgui_ok_)
		ImGui_ImplSDL3_ProcessEvent(&ev);
}

bool VulkanRenderer::imgui_captures_keyboard() const
{
	return imgui_ok_ && imgui_visible_ && ImGui::GetIO().WantCaptureKeyboard;
}

void VulkanRenderer::set_imgui_stats(const ImguiStats &s)
{
	imgui_stats_ = s;
}

void VulkanRenderer::prepare_imgui()
{
	if (!imgui_ok_ || !imgui_visible_)
		return;
	if (swapchain_extent.width == 0 || swapchain_extent.height == 0)
		return;
	// Renderer-side FPS (frame pacing lives in the frontend loop).
	const double now = SDL_GetTicks() / 1000.0;
	if (imgui_last_t_ > 0.0) {
		const double dt = now - imgui_last_t_;
		if (dt > 0.0)
			imgui_fps_ += (1.0 / dt - imgui_fps_) * 0.05;
	}
	imgui_last_t_ = now;
	ImGui_ImplVulkan_NewFrame();
	ImGui_ImplSDL3_NewFrame();
	ImGui::NewFrame();
	ImGui::SetNextWindowPos(ImVec2(12, 12), ImGuiCond_FirstUseEver);
	ImGui::SetNextWindowBgAlpha(0.7f);
	ImGui::Begin("GB4ME Debug", nullptr,
				 ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoInputs |
					 ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_AlwaysAutoResize);
	ImGui::Text("FPS: %.1f", imgui_fps_);
	ImGui::Text("Frame: %llu", static_cast<unsigned long long>(imgui_stats_.frame));
	ImGui::Text("ROM: %s", imgui_stats_.rom.empty() ? "(menu)" : imgui_stats_.rom.c_str());
	ImGui::Text("Core: %s", imgui_stats_.core.empty() ? "-" : imgui_stats_.core.c_str());
	ImGui::Text("Volume: %d%%", static_cast<int>(imgui_stats_.volume * 100.0f + 0.5f));
	ImGui::Text("Filter: %d  Tex: %ux%u", imgui_stats_.filter, tex_w_, tex_h_);
	if (imgui_stats_.emu_speed_pct > 0.0f) {
		ImGui::Text("Emu: %.1f%% of hardware (%.1f MHz)", imgui_stats_.emu_speed_pct,
					imgui_stats_.emulated_mhz);
		// >100% only happens in fast-forward; the audio line then explains
		// itself. The resample percent is the audio-clock pacer's drift
		// correction: it sits near 0 in steady state and moves when the
		// device clock and the emulated clock disagree.
		ImGui::Text("Audio: %u underrun%s, %.1fms target, drift %+.2f%%", imgui_stats_.underruns,
					imgui_stats_.underruns == 1 ? "" : "s", imgui_stats_.audio_target_ms,
					imgui_stats_.resample_pct);
	}
	if (imgui_stats_.ff)
		ImGui::Text(">> FAST-FORWARD");
	ImGui::TextDisabled("F3 toggles overlay");
	ImGui::End();
	ImGui::Render();
}

void VulkanRenderer::draw_imgui(VkCommandBuffer command_buffer)
{
	if (!imgui_ok_ || !imgui_visible_)
		return;
	ImDrawData *draw_data = ImGui::GetDrawData();
	if (draw_data != nullptr && draw_data->TotalVtxCount > 0)
		ImGui_ImplVulkan_RenderDrawData(draw_data, command_buffer);
}

} // namespace gb
