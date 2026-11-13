// GB4ME Vulkan renderer — Dear ImGui overlay (debug readout; future panels).
// See include/gb/vulkan_renderer.h for the class interface.
//
// Best-effort: init may fail (headless/linked-minimal builds) and every other
// entry point no-ops until it succeeds, so the emulator never depends on it.
// The overlay window is display-only (NoInputs): it cannot capture game input.

#include "gb/vulkan_renderer.h"
#include "gba/cpu.h"

#include "backends/imgui_impl_sdl3.h"
#include "backends/imgui_impl_vulkan.h"
#include "imgui.h"
#include <cstdio>
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

void VulkanRenderer::apply_imgui_theme(bool light)
{
	if (!imgui_ok_)
		return;
	if (light)
		ImGui::StyleColorsLight();
	else
		ImGui::StyleColorsDark();
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

bool VulkanRenderer::imgui_captures_mouse() const
{
	return imgui_ok_ && imgui_visible_ && ImGui::GetIO().WantCaptureMouse;
}

void VulkanRenderer::set_imgui_stats(const ImguiStats &s)
{
	imgui_stats_ = s;
}

namespace
{

// Display name for a 0..3 Off/Light/Medium/Strong step (grid, ghost).
const char *step_name(int v)
{
	static const char *kSteps[4] = {"Off", "Light", "Medium", "Strong"};
	return (v >= 0 && v < 4) ? kSteps[v] : "-";
}

} // namespace

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
				 ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoFocusOnAppearing |
					 ImGuiWindowFlags_AlwaysAutoResize);
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
	ImGui::Separator();
	// CPU debugger: live registers for the running core plus the
	// newest-first trace-ring slice (hex; disassembly is a follow-up).
	if (imgui_stats_.gba_core) {
		ImGui::TextDisabled("CPU (ARM)");
		char rbuf[128];
		for (int r = 0; r < 16; r += 4) {
			std::snprintf(rbuf, sizeof(rbuf), "R%-2d:%08X R%-2d:%08X R%-2d:%08X R%-2d:%08X", r,
						  imgui_stats_.gba_regs[r], r + 1, imgui_stats_.gba_regs[r + 1], r + 2,
						  imgui_stats_.gba_regs[r + 2], r + 3, imgui_stats_.gba_regs[r + 3]);
			ImGui::TextUnformatted(rbuf);
		}
		std::snprintf(rbuf, sizeof(rbuf), "CPSR:%08X", imgui_stats_.gba_cpsr);
		ImGui::TextUnformatted(rbuf);
		ImGui::TextDisabled("Trace (newest first)");
		for (int i = 0; i < ImguiStats::kTraceLines; i++) {
			const auto &t = imgui_stats_.trace[i];
			if (t.addr == 0 && t.instr == 0)
				break;
			const std::string asm_text =
				t.thumb ? gba::disasmThumb(t.addr, static_cast<u16>(t.instr & 0xFFFFu))
						: gba::disasmArm(t.addr, t.instr);
			char rbuf[160];
			std::snprintf(rbuf, sizeof(rbuf), "%08X  %08X%c  %s", t.addr, t.instr,
						  t.thumb ? 'T' : ' ', asm_text.c_str());
			ImGui::TextUnformatted(rbuf);
		}
	} else if (!imgui_stats_.core.empty() && imgui_stats_.core != "menu") {
		ImGui::TextDisabled("CPU (LR35902)");
		char rbuf[96];
		std::snprintf(rbuf, sizeof(rbuf), "AF:%04X BC:%04X DE:%04X HL:%04X", imgui_stats_.gb_af,
					  imgui_stats_.gb_bc, imgui_stats_.gb_de, imgui_stats_.gb_hl);
		ImGui::TextUnformatted(rbuf);
		std::snprintf(rbuf, sizeof(rbuf), "SP:%04X PC:%04X", imgui_stats_.gb_sp,
					  imgui_stats_.gb_pc);
		ImGui::TextUnformatted(rbuf);
	}
	ImGui::Separator();
	ImGui::TextDisabled("Controls");
	if (ImGui::Button("Vol -"))
		imgui_actions_.push_back(ImguiAction::VolDown);
	ImGui::SameLine();
	if (ImGui::Button("Vol +"))
		imgui_actions_.push_back(ImguiAction::VolUp);
	const char *filter_name = "-";
	if (imgui_stats_.filter >= 0 && imgui_stats_.filter < static_cast<int>(VideoFilter::Count))
		filter_name = video_filter_name(static_cast<VideoFilter>(imgui_stats_.filter));
	if (ImGui::Button(filter_name))
		imgui_actions_.push_back(ImguiAction::FilterNext);
	char grid_label[32];
	std::snprintf(grid_label, sizeof(grid_label), "Grid: %s", step_name(imgui_stats_.grid));
	if (ImGui::Button(grid_label))
		imgui_actions_.push_back(ImguiAction::GridNext);
	char ghost_label[32];
	std::snprintf(ghost_label, sizeof(ghost_label), "Ghost: %s", step_name(imgui_stats_.ghost));
	if (ImGui::Button(ghost_label))
		imgui_actions_.push_back(ImguiAction::GhostNext);
	if (ImGui::Button(imgui_stats_.turbo ? "Turbo: ON" : "Turbo: OFF"))
		imgui_actions_.push_back(ImguiAction::TurboToggle);
	ImGui::SameLine();
	if (ImGui::Button(imgui_stats_.paused ? "Resume" : "Pause"))
		imgui_actions_.push_back(ImguiAction::PauseToggle);
	if (ImGui::Button("Settings..."))
		imgui_settings_open_ = !imgui_settings_open_;
	ImGui::TextDisabled("F3 toggles overlay");
	ImGui::End();
	if (imgui_settings_open_) {
		ImGui::SetNextWindowBgAlpha(0.85f);
		if (ImGui::Begin("GB4ME Settings", &imgui_settings_open_,
						 ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoFocusOnAppearing |
							 ImGuiWindowFlags_AlwaysAutoResize)) {
			ImGui::TextDisabled("Display");
			char pal_label[32];
			std::snprintf(pal_label, sizeof(pal_label), "Palette: %s",
						  palette_name(imgui_stats_.palette));
			if (ImGui::Button(pal_label))
				imgui_actions_.push_back(ImguiAction::PaletteNext);
			static const char *kModes[3] = {"Auto", "DMG", "CGB"};
			const char *mode_name = (imgui_stats_.hwmode >= 0 && imgui_stats_.hwmode < 3)
										? kModes[imgui_stats_.hwmode]
										: "-";
			char mode_label[32];
			std::snprintf(mode_label, sizeof(mode_label), "Hardware: %s", mode_name);
			if (ImGui::Button(mode_label))
				imgui_actions_.push_back(ImguiAction::HwModeNext);
			const char *console_name = "-";
			if (imgui_stats_.console >= 0 && imgui_stats_.console < GUIConsole::kConsoleColors)
				console_name =
					GUIConsole::console_color_name(static_cast<ConsoleColor>(imgui_stats_.console));
			char console_label[32];
			std::snprintf(console_label, sizeof(console_label), "Console: %s", console_name);
			if (ImGui::Button(console_label))
				imgui_actions_.push_back(ImguiAction::ConsoleNext);
			char skin_label[48];
			std::snprintf(skin_label, sizeof(skin_label), "Skin: %s",
						  imgui_stats_.skin.empty() ? "-" : imgui_stats_.skin.c_str());
			if (ImGui::Button(skin_label))
				imgui_actions_.push_back(ImguiAction::SkinNext);
			char theme_label[24];
			std::snprintf(theme_label, sizeof(theme_label), "Theme: %s",
						  imgui_stats_.theme == 1 ? "Light" : "Dark");
			if (ImGui::Button(theme_label))
				imgui_actions_.push_back(ImguiAction::ThemeNext);
			ImGui::TextDisabled("ROM folder");
			ImGui::TextWrapped("%s",
							   imgui_stats_.folder.empty() ? "." : imgui_stats_.folder.c_str());
			if (ImGui::Button("Browse..."))
				imgui_actions_.push_back(ImguiAction::FolderBrowse);
		}
		ImGui::End();
	}
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
