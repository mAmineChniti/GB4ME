#pragma once

#include "apu.h"
#include "audio_sync.h"
#include "cartridge.h"
#include "cheats.h"
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
#include <deque>
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

	// Savestate slots in the UI: 8 files beside the ROM (`<rom>.gb4s0` ..
	// `<rom>.gb4s7`, thumbnails at `<rom>.gb4sN.ppm`). The path helper is
	// static and headless-testable; out-of-range slots yield "".
	static constexpr int kStateSlots = 8;
	static std::string state_slot_path(const std::string &rom_path, int slot);
	// Rewind ring on top of the slots: snapshots every N frames, capped so
	// memory stays bounded (GB ~80KB, GBA ~500KB per entry). Hold U to step
	// back one entry per frame; cleared on load/eject.
	static constexpr int kRewindInterval = 30;
	static constexpr size_t kRewindMax = 120;
	// Full-machine file states (`GB4S` v2). GB/CGB only: returns false with
	// no ROM loaded, in GBA mode (the GBA core has its own file format,
	// reached through save_state_slot/load_state_slot), or on file IO
	// failure. Headless-safe (pure machine + file IO), unlike load_rom.
	bool saveState(const std::string &path) const;
	bool loadState(const std::string &path);
	// Headless test seam (used by --selftest): loads ROM bytes straight into
	// the live core and steps whole frames, with none of load_rom()'s
	// window, audio-device, config-file, thumbnail or GUI side effects. The
	// header CGB mode and DMG palette 0 are forced (never the user config),
	// so tests are hermetic. Lets the selftest cover the REAL saveState /
	// loadState file shell end to end instead of only the device layer.
	// `tag` names the session for sidecar-derived paths; keep it under /tmp
	// in tests. GBA bytes route to the GBA core like load_rom's probe does.
	bool load_test_bytes(const std::vector<u8> &data, const std::string &tag);
	// Steps whole frames without rendering, audio, pacing or autosaves.
	// Bounded, so a wedged core cannot hang the suite.
	void test_step_frames(int frames);
	// Headless seam for the selftest: marks the debug overlay visible,
	// pushes the stats and returns a copy (no window needed).
	VulkanRenderer::ImguiStats debug_stats_for_test();

  private:
	void main_loop();
	void handle_events();
	void handle_menu_key(SDL_Scancode sc, bool pressed);
	void handle_settings_shortcut(SDL_Scancode sc);
	void dispatch_settings_hit(const GUIConsole::SettingsHit &hit);
	void set_volume(float v); // clamp 0..1 + apply to APU and GBA audio (no save)

	// `GB4S` savestates for the GB/GBC core (the GBA core has its own).
	// These work on the machine even with no window/audio device, which is
	// what makes them testable headlessly.
	// In-memory snapshot (same bytes as the file shell) for the rewind ring.
	bool saveMemory(std::vector<u8> &out) const;
	bool loadMemory(const std::vector<u8> &data);
	// Parse a state into the live components without rollback; the public
	// loadMemory() wraps this so a corrupt file leaves the machine untouched.
	bool loadMemoryInPlace(const std::vector<u8> &data);
	std::deque<std::vector<u8>> rewind_buf_;
	bool rw_held_ = false;
	void rewind_push();
	bool rewind_step();
	void rewind_clear()
	{
		rewind_buf_.clear();
	}
	void rescan_roms();
	void open_file_dialog();
	void open_folder_dialog();
	void consume_pending_dialogs();
	static void open_file_callback(void *userdata, const char *const *files, int filter);
	static void open_folder_callback(void *userdata, const char *const *files, int filter);
	void step_frame();
	void step_gba_frame(); // Run the GBA core to the next frame (bounded).
	void update_timers(u32 cycles);
	// CPU per-M-cycle hook: advances the timer/PPU/APU by one M-cycle (see
	// CPU::set_cycle_hook). The CPU already drops every other M-cycle in
	// CGB double speed, so this is unconditional.
	static void on_cpu_mcycle(void *ctx);
	void check_interrupts();

	Cartridge cartridge;
	CPU cpu;
	MMU mmu;
	PPU ppu;
	APU apu;
	Timer timer;
	Joypad joypad;
	Cheats cheats_;
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
	// Fast-forward (hold Tab) + turbo toggle (T): skip frame pacing.
	// Env GB4ME_FASTFORWARD forces it on (headless verification).
	bool ff_held_ = false;
	bool ff_toggle_ = false;
	bool ff_env_ = false;
	// WarioWare Twisted tilt keys (COMMA = left, PERIOD = right): fixed
	// outside the remappable pad map; the GPIO ADC samples them, and the
	// cartridge ignores them without a gyro sensor.
	bool tilt_left_held_ = false;
	bool tilt_right_held_ = false;
	void apply_tilt();
	void clear_tilt()
	{
		tilt_left_held_ = false;
		tilt_right_held_ = false;
		apply_tilt();
	}
	bool fast_forward() const
	{
		return ff_held_ || ff_toggle_ || ff_env_;
	}
	u64 frame_count = 0;
	// Savestate slot selected with 1-8 while a ROM runs (F5 saves, F8
	// loads). The menu keeps 1-8 for recent-ROM quick-launch, so the slot
	// keys only apply in-game.
	int state_slot_ = 0;
	// Route through whichever core is running (GB/CGB state vs GBA state)
	// and flash the outcome on the status OSD. False when no ROM is loaded.
	bool save_state_slot();
	bool load_state_slot();
	// A PPM screenshot of the current frame next to the state file, so the
	// menu/ImGui work can show thumbnails later. Best-effort: a missing
	// thumbnail never fails the save.
	void write_slot_thumbnail(const std::string &state_path);
	// Auto-thumbnail beside the ROM (`<rom>.thumb.ppm`) on eject, so the menu
	// always has something without the user providing images. Best-effort.
	void write_rom_thumbnail(const std::string &rom_path);
	// Audio-clock pacing (include/gb/audio_sync.h) plus the per-frame
	// watermark that turns the cores' cumulative frame counters into a
	// per-frame delta.
	AudioSync audio_sync_{};
	u64 audio_produced_ = 0;
	u32 produced_audio();
	int queued_audio() const;
	// Rumble passthrough: the motor bit comes from the cartridge (MBC5
	// bit 3), the effect is played on the first open gamepad. Best-effort -
	// a pad with no rumble support just stays silent.
	SDL_Gamepad *rumble_pad_ = nullptr;
	bool rumble_on_ = false;
	void open_rumble();
	void update_rumble();
	void stop_rumble();
	// Emulated-cycle accounting for the debug overlay: T-cycles actually
	// executed and the wall-clock second they were measured over.
	u64 emu_tcycles_ = 0;
	double emu_window_t_ = 0.0;
	f64 last_time = 0.0;
	u32 target_frame_time_ms = 16;
	std::string last_rom_path_;

	// File/folder dialog results arrive in SDL callbacks (possibly off the
	// main thread). The callback only writes into a process-lifetime queue;
	// this id says which request this instance is waiting for, so a callback
	// that outlives a destroyed GameBoy can never touch freed memory.
	u64 dialog_request_ = 0;
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
	// The same push, plus volume and the DMG palette, into both cores - the
	// per-game overlay changes those too, and they are read once at load.
	void push_display_settings();
	void apply_game_override_to_systems();
	// Drops the `<rom>.cfg` overlay when the user opens the settings panel,
	// so panel edits are global instead of being overwritten on next load.
	void leave_game_override();
	// Names of the connected controllers, for the Inputs device dropdown.
	std::vector<std::string> controller_names_;
	// Rebuilds that list from the currently connected pads.
	void refresh_controller_names();
	// Applies an accepted profile name from the text-entry prompt.
	void apply_profile_name();
	// Persists the running game's battery save (GB SRAM or GBA flash/EEPROM)
	// next to last_rom_path_. No-op when nothing is loaded.
	void flush_active_save();
	// Pushes the debug readout into the ImGui overlay (no-op when hidden).
	void push_imgui_stats();
	// Applies button presses queued by the interactive overlay (volume,
	// filter/grid/ghost cycle, turbo, pause) through the same paths as the
	// keyboard shortcuts. Drained once per frame; empty when hidden.
	void drain_imgui();
	// Env-gated event tracer (GB4ME_INPUT_TRACE, Phase 7 diagnostics like
	// the IRQ/DMA/IO traces): logs mouse/key dispositions so dead input
	// can be told apart from missed hit-tests.
	bool input_trace_ = false;

	std::atomic<bool> request_stop{false};
};

} // namespace gb