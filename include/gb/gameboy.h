#pragma once

#include "apu.h"
#include "cartridge.h"
#include "cpu.h"
#include "gba/core.h"
#include "gui_console.h"
#include "joypad.h"
#include "mmu.h"
#include "ppu.h"
#include "timer.h"
#include "types.h"
#include "vulkan_renderer.h"
#include <SDL3/SDL.h>

#include "gb/input.h"
#include <atomic>
#include <mutex>
#include <string>
#include <vector>

namespace gb
{

class GameBoy
{
  public:
	GameBoy();
	~GameBoy();

	// Bring up the window + renderer with no ROM loaded (menu mode).
	bool initialize();
	// Load a ROM into the running emulator (menu -> game, or game -> game).
	// Returns false and keeps the current state on failure.
	bool load_rom(const std::string &rom_path);
	// Back to the ROM menu (saves battery RAM first).
	void eject_rom();
	// Scan a folder for playable ROMs (used by the menu + --list-roms).
	static std::vector<GUIConsole::RomEntry> scan_rom_folder(const std::string &folder);
	void run();
	void shutdown();

  private:
	void main_loop();
	void handle_events();
	void handle_menu_key(SDL_Scancode sc, bool pressed);
	void handle_settings_shortcut(SDL_Scancode sc);
	void dispatch_settings_hit(const GUIConsole::SettingsHit &hit);
	void set_volume(float v); // clamp 0..1 + apply to APU and GBA audio (no save)
	void rescan_roms();
	void open_file_dialog();
	void open_folder_dialog();
	void consume_pending_dialogs();
	static void open_file_callback(void *userdata, const char *const *files, int filter);
	static void open_folder_callback(void *userdata, const char *const *files, int filter);
	void step_frame();
	void step_gba_frame(); // Run the GBA core to the next frame (bounded).
	void update_timers(u32 cycles);
	void check_interrupts();

	Cartridge cartridge;
	CPU cpu;
	MMU mmu;
	PPU ppu;
	APU apu;
	Timer timer;
	Joypad joypad;
	// Unified keyboard + gamepad input (owns raw state for both).
	InputManager input_;
	gba::GameBoyAdvance gba_;
	bool gba_mode_ = false;
	VulkanRenderer renderer;
	Settings settings;
	GUIConsole gui_console;
	bool show_settings_panel = false;

	SDL_Window *window = nullptr;
	bool running = false;
	bool paused = false;
	bool rom_loaded_ = false;
	u64 frame_count = 0;
	f64 last_time = 0.0;
	u32 target_frame_time_ms = 16;
	std::string last_rom_path_;

	// File/folder dialog results arrive in SDL callbacks (possibly off the
	// main thread), so they are staged here and consumed in the main loop.
	std::mutex pending_mutex_;
	std::string pending_open_path_;
	bool has_pending_open_ = false;
	std::string pending_rom_folder_;
	bool has_pending_folder_ = false;
	// True while the user drags the volume slider (save on release).
	bool volume_drag_ = false;
	// Clears body/shoulder press highlights (mouse up, focus loss, ROM
	// switch) so a highlight can never stick.
	void release_mouse_buttons();
	// Pushes the settings tables into the input manager (after a remap or
	// auto-map) so gameplay follows the UI immediately.
	void sync_input_from_settings();
	// Push the upscaling settings (scale / filter / LCD grid) into the renderer.
	void apply_video_settings();
	// Names of the connected controllers, for the Inputs device dropdown.
	std::vector<std::string> controller_names_;
	// Rebuilds that list from the currently connected pads.
	void refresh_controller_names();
	// Applies an accepted profile name from the text-entry prompt.
	void apply_profile_name();
	// Persists the running game's battery save (GB SRAM or GBA flash/EEPROM)
	// next to last_rom_path_. No-op when nothing is loaded.
	void flush_active_save();
	// Env-gated event tracer (GB4ME_INPUT_TRACE, Phase 7 diagnostics like
	// the IRQ/DMA/IO traces): logs mouse/key dispositions so dead input
	// can be told apart from missed hit-tests.
	bool input_trace_ = false;

	std::atomic<bool> request_stop{false};
};

} // namespace gb