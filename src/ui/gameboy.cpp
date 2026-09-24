// GameBoy: owns all components, runs the frame loop, maps SDL input.
// https://github.com/aquova/gb-book (SDL frontend chapter)

#include "gb/gameboy.h"
#include "gba/cartridge.h"
#include "stb_image.h"
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <optional>

namespace gb
{

#ifdef GB4ME_RELEASE
// Release builds have no tracing at all, and the env var is not even read.
#define GB4ME_ITRACE(...) \
	do {                  \
	} while (0)
#else
#define GB4ME_ITRACE(...)                              \
	do {                                               \
		if (input_trace_)                              \
			std::fprintf(stderr, __VA_ARGS__);         \
	} while (0)
#endif

GameBoy::GameBoy()
{
	mmu.set_cartridge(&cartridge);
	mmu.set_ppu(&ppu);
	mmu.set_apu(&apu);
	mmu.set_timer(&timer);
	mmu.set_joypad(&joypad);
	cpu.set_mmu(&mmu);
	ppu.set_mmu(&mmu);
	apu.set_mmu(&mmu);
	timer.set_mmu(&mmu);
	joypad.set_mmu(&mmu);
	settings.load("");
	// The S key toggles the settings panel globally, so a GBA mapping that
	// uses it (the old R default) can never reach the game. Migrate it once
	// to a free key instead of leaving R permanently broken.
	if (settings.key_map[static_cast<int>(gba::GbaKeypad::Key::R)] == SDL_SCANCODE_S) {
		settings.key_map[static_cast<int>(gba::GbaKeypad::Key::R)] = SDL_SCANCODE_D;
		settings.save("");
	}
	gui_console.set_settings(&settings);
	gui_console.set_input(&input_);
	// Hand the loaded mappings to the input manager: the keyboard table is
	// the existing key_map, the controller tables are the new pad maps.
	for (unsigned i = 0; i < kPadButtonCount; ++i) {
		input_.setKeyBinding(static_cast<PadButton>(i),
		                    Binding::makeKey(settings.key_map[static_cast<size_t>(i)]));
		input_.setPadBinding(static_cast<PadButton>(i), settings.pad_map[i]);
		input_.setPadBinding2(static_cast<PadButton>(i), settings.pad_map_alt[i]);
	}
	refresh_controller_names();
	// Upscaling: apply the saved filter / LCD grid. Done after the renderer
	// exists so the first frame is already correct.
	renderer.setVideoFilter(static_cast<VideoFilter>(settings.video_filter));
	renderer.setLcdGrid(settings.lcdGridDepth());
#ifndef GB4ME_RELEASE
	input_trace_ = std::getenv("GB4ME_INPUT_TRACE") != nullptr;
#endif
}

void GameBoy::release_mouse_buttons()
{
	// Highlights only: gameplay keys belong to the keyboard and must never
	// be touched here (a mouse-up while a keyboard key is held must not
	// release it). Unconditional so a highlight can never stick.
	for (int i = 0; i < 8; i++)
		gui_console.set_button_pressed(static_cast<Joypad::Key>(i), false);
	for (int i = 0; i < 10; i++)
		gui_console.set_gba_pressed(static_cast<gba::GbaKeypad::Key>(i), false);
}

GameBoy::~GameBoy()
{
	shutdown();
}

bool GameBoy::initialize()
{
	// SDL_INIT_GAMEPAD implies joystick init; both are best-effort so the
	// emulator still runs with no controller support available.
#ifdef GB4ME_RELEASE
	// Release builds are completely silent. SDL's own logging goes to stderr,
	// so the output sink is replaced with a no-op before anything can
	// initialise. An explicit sink is used rather than a null callback, which
	// SDL documents as "install your own" rather than "disable"; this SDL3
	// also has no SDL_LOG_PRIORITY_NONE to fall back on.
	SDL_SetLogOutputFunction(+[](void *, int, SDL_LogPriority, const char *) {}, nullptr);
#endif
	if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO)) {
		SDL_Log("SDL_Init failed: %s", SDL_GetError());
		return false;
	}
	// Controllers are optional: if the subsystem is unavailable the
	// keyboard path still works unchanged.
	SDL_InitSubSystem(SDL_INIT_GAMEPAD);
	input_.openControllers();
	// Audio is best-effort: the emulator runs silent without a device.
	// Apply volume from settings
	apu.open_audio();
	apu.set_master_volume(settings.volume);
	// GBC aspect 75:133, window sized to fit body with margin. Use 480x852 (~0.56) close to
	// hardware.
	int win_w = 480, win_h = 852;
	// Allow video_scale setting to influence window size (1-6)
	win_w = 320 + settings.video_scale * 60;
	win_h = static_cast<int>(win_w / (75.0f / 133.0f));
	window = SDL_CreateWindow("GB4ME", win_w, win_h, SDL_WINDOW_VULKAN | SDL_WINDOW_RESIZABLE);
	if (!window) {
		SDL_Log("SDL_CreateWindow failed: %s", SDL_GetError());
		return false;
	}
	// Keep the window big enough that the OPEN/settings buttons, menu text
	// and game screen stay usable (they scale with the window; below this
	// they would shrink past legibility).
	SDL_SetWindowMinimumSize(window, 300, 420);
	gui_console.update_layout(win_w, win_h);
	SDL_SetWindowTitle(window, "GB4ME");
	// Window/taskbar icon (also used by the .desktop Icon=GB4ME). Best-effort.
	{
		const char *base = SDL_GetBasePath();
		std::string icon_candidates[5] = {std::string(base ? base : "") + "assets/GB4ME.png",
										  std::string(base ? base : "") +
											  "../share/icons/hicolor/512x512/apps/GB4ME.png",
										  "/usr/share/icons/hicolor/512x512/apps/GB4ME.png",
										  "assets/GB4ME.png", "assets/icon.png"};
		for (auto &p : icon_candidates) {
			int w, h, comp;
			unsigned char *pix = stbi_load(p.c_str(), &w, &h, &comp, 4);
			if (pix) {
				SDL_Surface *surf = SDL_CreateSurface(w, h, SDL_PIXELFORMAT_RGBA32);
				if (surf) {
					SDL_memcpy(surf->pixels, pix, (size_t) w * h * 4);
					SDL_SetWindowIcon(window, surf);
					SDL_DestroySurface(surf);
				}
				stbi_image_free(pix);
				break;
			}
		}
	}
	if (!renderer.initialize(window)) {
		SDL_Log("Vulkan renderer init failed");
		return false;
	}

	// Boot to the ROM menu; a ROM can be passed on the command line
	// (load_rom) or picked via the OPEN button / file dialog.
	gui_console.set_menu_active(true);
	rescan_roms();
	running = true;
	last_time = SDL_GetTicks() / 1000.0;
	return true;
}

void GameBoy::flush_active_save()
{
	if (!rom_loaded_ || last_rom_path_.empty())
		return;
	if (gba_mode_) {
		if (gba_.cartridge().hasSave())
			gba_.cartridge().saveToFile(last_rom_path_ + ".sav");
	} else if (cartridge.has_battery()) {
		cartridge.save_ram(last_rom_path_ + ".sav");
	}
}

bool GameBoy::load_rom(const std::string &rom_path)
{
	// Probe first so a bad file never tears down the running game.
	bool is_gb = false;
	{
		Cartridge probe;
		is_gb = probe.load(rom_path);
		if (!is_gb) {
			// Content-based GBA routing (extension is only a hint). Probe
			// with a throwaway cartridge so validation happens before the
			// running game's save is flushed or its core is replaced.
			gba::Cartridge gprobe;
			if (!gprobe.load(rom_path)) {
				SDL_Log("load_rom: not a supported ROM: %s", rom_path.c_str());
				return false;
			}
		}
	}
	// The new image is known good: persist the outgoing game's battery
	// save before its cartridge is replaced (GBA included - previously a
	// GBA->GBA switch dropped the first game's save entirely).
	flush_active_save();
	if (!is_gb) {
		if (!gba_.load(rom_path)) {
			SDL_Log("load_rom: GBA load failed: %s", rom_path.c_str());
			return false;
		}
		last_rom_path_ = rom_path;
		rom_loaded_ = true;
		gba_mode_ = true;
		gui_console.set_gba_mode(true);
		release_mouse_buttons();
		gui_console.cancel_mapping();
		gui_console.set_menu_active(false);
		show_settings_panel = false;
		gui_console.set_show_settings(false);
		paused = false;
		frame_count = 0;
		// GBA audio has its own stream (GB APU stays shut here).
		gba_.audio().open_audio();
		gba_.audio().set_master_volume(settings.volume);
		{
			const size_t slash = rom_path.find_last_of("/\\");
			const std::string title =
				"GB4ME - " +
				(slash == std::string::npos ? rom_path : rom_path.substr(slash + 1));
			SDL_SetWindowTitle(window, title.c_str());
		}
		return true;
	}
	// Load before flipping gba_mode_: if this fails the previously running
	// core stays selected, so save routing cannot disagree with what is
	// actually emulated.
	if (!cartridge.load(rom_path))
		return false; // probed OK above
	gba_mode_ = false;
	last_rom_path_ = rom_path;
	// Battery saves sit next to the ROM.
	cartridge.load_ram(rom_path + ".sav");

	// ---- CGB Mode Detection (AGENTS Part 1.1) ----
	// Header 0x0143 CGB flag: 0x80 compatible, 0xC0 CGB-only. Other = DMG.
	// GBC: this will branch on hardware_mode (CGB vs DMG rendering, banking, etc.)
	// Freebios/direct-boot: no boot ROM binary required (like SameBoy
	// GB_boot_rom_none) — init post-boot state directly per Pan Docs
	// Power_Up_Sequence. CGB needs 0x11/0x80 vs DMG 0x01/0xB0.
	HardwareMode mode = cartridge.hardware_mode();
	// Respect settings hardware_mode override
	if (settings.hardware_mode == "DMG")
		mode = HardwareMode::DMG;
	else if (settings.hardware_mode == "CGB")
		mode = HardwareMode::CGB_Only;
	// Auto otherwise uses cartridge header

	// Reset every component to post-boot state with the correct mode.
	// This is the direct-boot path — no boot ROM executed.
	cpu.reset(mode);
	mmu.reset();
	ppu.reset();
	apu.reset();
	timer.reset();
	joypad.reset();

	mmu.set_hardware_mode(mode);
	ppu.set_hardware_mode(mode);
	ppu.set_dmg_palette(settings.palette);
	gui_console.set_hardware_mode(mode);
	// CPU double_speed starts false regardless; KEY1 will toggle.
	cpu.double_speed = false;
	cpu.clock_speed_hz = kDefaultDmgClockHz;

	// Skip boot ROM: post-boot register state (see CPU/PPU/Timer::reset).
	mmu.boot_rom_enabled = false;
	for (int i = 0; i < 8; i++) {
		joypad.set_key(static_cast<Joypad::Key>(i), false);
		gui_console.set_button_pressed(static_cast<Joypad::Key>(i), false);
	}

	// Title shows the loaded ROM for easy identification.
	{
		const size_t slash = rom_path.find_last_of("/\\");
		const std::string title =
			"GB4ME - " + (slash == std::string::npos ? rom_path : rom_path.substr(slash + 1));
		SDL_SetWindowTitle(window, title.c_str());
	}
	rom_loaded_ = true;
	gui_console.set_gba_mode(false);
	release_mouse_buttons();
	gui_console.cancel_mapping();
	gui_console.set_menu_active(false);
	gui_console.set_fullscreen_game(false);
	show_settings_panel = false;
	gui_console.set_show_settings(false);
	paused = false;
	frame_count = 0;
	return true;
}

void GameBoy::eject_rom()
{
	if (!rom_loaded_)
		return;
	flush_active_save();
	gba_.audio().close_audio();
	rom_loaded_ = false;
	gba_mode_ = false;
	gui_console.set_gba_mode(false);
	release_mouse_buttons();
	gui_console.cancel_mapping();
	last_rom_path_.clear();
	paused = false;
	gui_console.set_fullscreen_game(false);
	SDL_SetWindowTitle(window, "GB4ME");
	gui_console.set_menu_active(true);
	rescan_roms();
}

void GameBoy::rescan_roms()
{
	auto entries = scan_rom_folder(settings.rom_folder);
	const int keep = gui_console.menu_selected();
	gui_console.set_rom_list(std::move(entries));
	gui_console.set_menu_selected(keep);
}

namespace
{

std::string trim_str(const std::string &s)
{
	size_t a = 0;
	while (a < s.size() && std::isspace(static_cast<unsigned char>(s[a])))
		a++;
	size_t b = s.size();
	while (b > a && std::isspace(static_cast<unsigned char>(s[b - 1])))
		b--;
	return s.substr(a, b - a);
}

std::string collapse_spaces(const std::string &s)
{
	std::string out;
	out.reserve(s.size());
	bool in_space = false;
	for (char c : s) {
		if (std::isspace(static_cast<unsigned char>(c))) {
			if (!in_space)
				out.push_back(' ');
			in_space = true;
		} else {
			out.push_back(c);
			in_space = false;
		}
	}
	return out;
}

std::string title_case_word(const std::string &w)
{
	if (w.empty())
		return w;
	std::string out = w;
	for (size_t i = 0; i < out.size(); i++) {
		unsigned char c = static_cast<unsigned char>(out[i]);
		if (i == 0)
			out[i] = static_cast<char>(std::toupper(c));
		else
			out[i] = static_cast<char>(std::tolower(c));
	}
	return out;
}

std::string title_case_phrase(const std::string &s)
{
	// Keep numbers and delimiters, Title-Case each word.
	std::string out;
	out.reserve(s.size());
	bool start = true;
	for (char ch : s) {
		unsigned char c = static_cast<unsigned char>(ch);
		if (ch == ' ' || ch == '-' || ch == '\'') {
			out.push_back(ch);
			start = true;
		} else {
			if (start)
				out.push_back(static_cast<char>(std::toupper(c)));
			else
				out.push_back(static_cast<char>(std::tolower(c)));
			start = false;
		}
	}
	return out;
}

// Dictionary for segmented GB header titles like "SUPERMARIOLAND3".
// Uppercase words as they appear concatenated in the header.
const char *kKnownWords[] = {
	"SUPER",  "MARIO",   "LAND",      "WARIO",   "POKEMON", "TETRIS",    "KIRBY",       "DREAM",
	"ZELDA",  "LINK",    "AWAKENING", "METROID", "DONKEY",  "KONG",      "MEGA",        "MAN",
	"BATTLE", "CHIP",    "CHALLENGE", "DR",      "BROS",    "BOMBERMAN", "CASTLEVANIA", "CONTRA",
	"FINAL",  "FANTASY", "LEGEND",    "WORLD",   "BATTLE",  "ARENA",     "GOLF",        "TENNIS",
	"RACING", "STARS",   "WARS",      "PINBALL", "PUZZLE",  "QUEST",     "ADVENTURE",   "COLOR",
	"DX",     "GB",      "C",         nullptr};

std::string segment_stuck_title(const std::string &upper)
{
	// upper is already uppercased, no spaces, may have digits.
	// DP to maximize (known-word chars + 2*matched_words) score.
	const int n = static_cast<int>(upper.size());
	std::vector<int> best_score(n + 1, -1000000);
	std::vector<int> best_next(n + 1, -1);
	std::vector<std::string> best_word(n + 1);
	best_score[n] = 0;
	for (int i = n - 1; i >= 0; i--) {
		// Option: take single char (fallback)
		best_score[i] = -1 + best_score[i + 1];
		best_next[i] = i + 1;
		best_word[i] = upper.substr(i, 1);
		// Try digits run as one token
		if (std::isdigit(static_cast<unsigned char>(upper[i]))) {
			int j = i;
			while (j < n && std::isdigit(static_cast<unsigned char>(upper[j])))
				j++;
			int sc = (j - i) + best_score[j];
			if (sc > best_score[i]) {
				best_score[i] = sc;
				best_next[i] = j;
				best_word[i] = upper.substr(i, j - i);
			}
		}
		// Try known words
		for (int k = 0; kKnownWords[k] != nullptr; k++) {
			std::string w = kKnownWords[k];
			int L = static_cast<int>(w.size());
			if (i + L <= n && upper.compare(i, L, w) == 0) {
				int sc = L * 2 + 3 + best_score[i + L];
				if (sc > best_score[i]) {
					best_score[i] = sc;
					best_next[i] = i + L;
					best_word[i] = w;
				}
			}
		}
	}
	// If we segmented mostly as single chars, the title is unknown —
	// just Title-Case it as one word instead of "T E T R I S".
	int single_chars = 0;
	int total = 0;
	for (int i = 0; i < n;) {
		int nxt = best_next[i];
		if (nxt == i + 1 && !std::isdigit(static_cast<unsigned char>(upper[i])))
			single_chars++;
		total++;
		i = nxt;
	}
	if (single_chars * 2 > total && total >= 4) {
		return title_case_word(upper);
	}
	std::string out;
	for (int i = 0; i < n;) {
		if (!out.empty())
			out.push_back(' ');
		out += title_case_word(best_word[i]);
		i = best_next[i];
	}
	out = collapse_spaces(out);
	return trim_str(out);
}

std::string pretty_from_header_raw(const std::string &raw)
{
	std::string t = trim_str(raw);
	if (t.empty())
		return {};
	// If it already has spaces, just Title-Case it.
	if (t.find(' ') != std::string::npos) {
		return title_case_phrase(collapse_spaces(t));
	}
	// No spaces — often stuck like SUPERMARIOLAND3. Try to segment.
	std::string upper;
	upper.reserve(t.size());
	for (char c : t)
		upper.push_back(static_cast<char>(std::toupper(static_cast<unsigned char>(c))));
	// If upper is already mixed case with no spaces, segment; otherwise Title-Case
	bool is_stuck_upper = true;
	for (char c : t) {
		if (std::islower(static_cast<unsigned char>(c))) {
			is_stuck_upper = false;
			break;
		}
	}
	if (is_stuck_upper || upper.find(' ') == std::string::npos) {
		std::string seg = segment_stuck_title(upper);
		if (!seg.empty() && seg.find(' ') != std::string::npos)
			return seg;
		return title_case_word(upper);
	}
	return title_case_phrase(t);
}

std::string pretty_from_stem(const std::string &stem_raw)
{
	std::string s = stem_raw;
	// Common filename noise: underscores -> spaces
	for (char &c : s)
		if (c == '_')
			c = ' ';
	s = collapse_spaces(trim_str(s));
	if (s.empty())
		return {};
	// If stem has no spaces and looks stuck upper (SUPERMARIOLAND3),
	// reuse the header segmenter for a nice spaced version.
	bool has_space = s.find(' ') != std::string::npos;
	bool has_dash = s.find('-') != std::string::npos;
	if (!has_space && !has_dash) {
		bool all_upper_or_digit = true;
		for (char c : s) {
			unsigned char uc = static_cast<unsigned char>(c);
			if (!(std::isupper(uc) || std::isdigit(uc))) {
				all_upper_or_digit = false;
				break;
			}
		}
		if (all_upper_or_digit && s.size() > 5) {
			std::string upper = s;
			for (char &c : upper)
				c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
			std::string seg = segment_stuck_title(upper);
			if (seg.find(' ') != std::string::npos)
				return seg;
			return title_case_word(s);
		}
	}
	// Keep the author's casing but normalize dash spacing: "A-B" -> "A - B"
	// and ensure " - " stays as separator.
	// Already Title-ish for No-Intro sets like "Wario Land - Super Mario Land 3 (World)"
	// so we just collapse and return as-is, but ensure words are Title-Cased if
	// the whole string is lower/upper.
	bool is_all_lower = true, is_all_upper = true;
	for (char c : s) {
		unsigned char uc = static_cast<unsigned char>(c);
		if (std::isalpha(uc)) {
			if (!std::islower(uc))
				is_all_lower = false;
			if (!std::isupper(uc))
				is_all_upper = false;
		}
	}
	if (is_all_lower || is_all_upper)
		return title_case_phrase(s);
	return s;
}

} // namespace

std::vector<GUIConsole::RomEntry> GameBoy::scan_rom_folder(const std::string &folder)
{
	std::vector<GUIConsole::RomEntry> entries;
	std::error_code ec;
	std::filesystem::directory_iterator it(folder, ec);
	if (!ec) {
		for (const auto &entry : it) {
			if (!entry.is_regular_file(ec) || ec)
				continue;
			std::string ext = entry.path().extension().string();
			std::transform(ext.begin(), ext.end(), ext.begin(),
						   [](unsigned char c) { return std::tolower(c); });
			if (ext != ".gb" && ext != ".gbc" && ext != ".gba")
				continue;
			if (ext == ".gba") {
				// GBA images validate against the GBA header (fixed byte +
				// complement check); the GB probe below would reject them.
				gba::Cartridge gba_probe;
				if (!gba_probe.load(entry.path().string()))
					continue;
				GUIConsole::RomEntry r;
				r.path = entry.path().string();
				// Just use the filename for the title
				r.title = entry.path().stem().string();
				r.cgb = false;
				r.gba = true;
				entries.push_back(std::move(r));
				continue;
			}
			Cartridge probe;
			if (!probe.load(entry.path().string()))
				continue;
			GUIConsole::RomEntry r;
			r.path = entry.path().string();
			// Header title: 16 bytes at 0x0134, but 0x0143 is the CGB flag
			// on CGB games, so stop the title there when it holds a flag.
			std::string raw(probe.header().title.data(), probe.header().title.size());
			const bool has_flag =
				probe.header().cgb_flag == 0x80 || probe.header().cgb_flag == 0xC0;
			std::string raw_title;
			for (size_t i = 0; i < raw.size(); i++) {
				if (has_flag && i == 15)
					break;
				const char c = raw[i];
				if (c == '\0')
					break;
				if (c < 0x20 || c > 0x7E)
					break;
				raw_title += c;
			}
			raw_title = trim_str(raw_title);
			std::string header_pretty = pretty_from_header_raw(raw_title);
			std::string stem_pretty = pretty_from_stem(entry.path().stem().string());
			// Prefer the filename-derived name: No-Intro sets already have
			// proper spacing/punctuation ("Wario Land - ..."), while the
			// header is often stuck ("SUPERMARIOLAND3"). Fall back to header
			// only when the stem is empty/generic or header is clearly better.
			std::string title;
			auto is_generic = [](const std::string &s) {
				std::string low = s;
				for (char &c : low)
					c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
				low = trim_str(low);
				return low == "game" || low == "rom" || low == "gb" || low.size() < 2;
			};
			if (!stem_pretty.empty() && !is_generic(stem_pretty)) {
				// If stem_pretty already has spaces/dashes it's almost always
				// the better display name. Use it.
				if (stem_pretty.find(' ') != std::string::npos ||
					stem_pretty.find('-') != std::string::npos) {
					title = stem_pretty;
				} else if (!header_pretty.empty() && header_pretty.find(' ') != std::string::npos) {
					title = header_pretty;
				} else {
					// Both are single-word: pick longer/more spaced.
					title =
						stem_pretty.size() >= header_pretty.size() ? stem_pretty : header_pretty;
					if (title.empty())
						title = stem_pretty;
				}
			} else {
				title = !header_pretty.empty() ? header_pretty : stem_pretty;
			}
			if (title.empty())
				title = entry.path().stem().string();
			r.title = title;
			r.cgb = is_cgb_mode(probe.hardware_mode());
			entries.push_back(std::move(r));
		}
		std::sort(entries.begin(), entries.end(),
				  [](const GUIConsole::RomEntry &a, const GUIConsole::RomEntry &b) {
					  std::string al = a.title, bl = b.title;
					  std::transform(al.begin(), al.end(), al.begin(),
									 [](unsigned char c) { return std::tolower(c); });
					  std::transform(bl.begin(), bl.end(), bl.begin(),
									 [](unsigned char c) { return std::tolower(c); });
					  return al < bl;
				  });
	}
	return entries;
}

void GameBoy::open_file_dialog()
{
	// Must outlive the async dialog (SDL requires filters valid till callback).
	// SDL pattern is a semicolon-separated list of *extensions* like
	// "gb;gbc" — not "*.gb;*.gbc". The latter fails portal validation with
	// "Invalid character in pattern (Only [a-zA-Z0-9_.-] allowed, or a
	// single *)". See SDL_dialog.h docs for SDL_DialogFileFilter.
	static const SDL_DialogFileFilter filters[] = {
		{"Game Boy ROMs", "gb;gbc;gba"},
		{"All files", "*"},
	};
	// log removed
	SDL_ShowOpenFileDialog(open_file_callback, this, window, filters, 2,
						   settings.rom_folder.c_str(), false);
}

void GameBoy::open_folder_dialog()
{
	SDL_ShowOpenFolderDialog(open_folder_callback, this, window, settings.rom_folder.c_str(),
							 false);
}

void GameBoy::open_file_callback(void *userdata, const char *const *files, int filter)
{
	(void) filter;
	if (!files) {
		// log removed
		return;
	}
	if (!files[0] || !files[0][0]) {
		// log removed
		return; // cancelled
	}
	// log removed
	auto *self = static_cast<GameBoy *>(userdata);
	std::lock_guard<std::mutex> lock(self->pending_mutex_);
	self->pending_open_path_ = files[0];
	self->has_pending_open_ = true;
}

void GameBoy::open_folder_callback(void *userdata, const char *const *files, int filter)
{
	(void) filter;
	if (!files || !files[0] || !files[0][0])
		return; // cancelled / error
	auto *self = static_cast<GameBoy *>(userdata);
	std::lock_guard<std::mutex> lock(self->pending_mutex_);
	self->pending_rom_folder_ = files[0];
	self->has_pending_folder_ = true;
}

void GameBoy::consume_pending_dialogs()
{
	std::string open_path, folder;
	bool do_open = false, do_folder = false;
	{
		std::lock_guard<std::mutex> lock(pending_mutex_);
		if (has_pending_open_) {
			open_path = std::move(pending_open_path_);
			has_pending_open_ = false;
			do_open = true;
		}
		if (has_pending_folder_) {
			folder = std::move(pending_rom_folder_);
			has_pending_folder_ = false;
			do_folder = true;
		}
	}
	if (do_folder && !folder.empty()) {
		settings.rom_folder = folder;
		settings.save("");
		rescan_roms();
		// log removed
	}
	if (do_open && !open_path.empty()) {
		if (!load_rom(open_path)) {
			// log removed
		}
	}
}

void GameBoy::set_volume(float v)
{
	settings.volume = std::clamp(v, 0.0f, 1.0f);
	apu.set_master_volume(settings.volume);
	// GBA has its own audio stream; the slider/wheel must reach it too.
	gba_.audio().set_master_volume(settings.volume);
}

void GameBoy::apply_profile_name()
{
	const std::string name = gui_console.text_value();
	switch (gui_console.text_purpose()) {
	case GUIConsole::TextPurpose::NewProfile:
		if (settings.addProfile(name) >= 0) {
			settings.save("");
			sync_input_from_settings();
		}
		break;
	case GUIConsole::TextPurpose::RenameProfile:
		if (settings.renameProfile(gui_console.text_target(), name))
			settings.save("");
		break;
	}
	// A new profile changes the Profiles tab row count, so the cells must be
	// laid out again.
	gui_console.rebuild_settings_cells();
}

// Push the upscaling settings into the renderer and persist them.
// Rebuild the Inputs device dropdown from the connected pads.
void GameBoy::refresh_controller_names()
{
	controller_names_.clear();
	for (unsigned i = 0; i < input_.controllerCount(); i++)
		controller_names_.push_back(input_.controllerName(i));
	gui_console.setDeviceOptions(controller_names_, gui_console.selectedDevice());
}

void GameBoy::apply_video_settings()
{
	renderer.setVideoFilter(static_cast<VideoFilter>(settings.video_filter));
	renderer.setLcdGrid(settings.lcdGridDepth());
	settings.save("");
}

void GameBoy::sync_input_from_settings()
{
	for (unsigned i = 0; i < kPadButtonCount; ++i) {
		input_.setKeyBinding(static_cast<PadButton>(i),
		                    Binding::makeKey(settings.key_map[i]));
		input_.setPadBinding(static_cast<PadButton>(i), settings.pad_map[i]);
		input_.setPadBinding2(static_cast<PadButton>(i), settings.pad_map_alt[i]);
	}
}

void GameBoy::dispatch_settings_hit(const GUIConsole::SettingsHit &hit)
{
	using Action = GUIConsole::SettingsAction;
	// Any panel action other than arming a remap dismisses the modal (the
	// click went somewhere else). Harmless when nothing is armed.
	if (hit.action != Action::PadButton)
		gui_console.cancel_mapping();
	switch (hit.action) {
	case Action::None:
		break;
	case Action::Close:
		show_settings_panel = false;
		gui_console.set_show_settings(false);
		gui_console.cancel_mapping();
		break;
	case Action::TabGeneral:
		gui_console.set_settings_tab(GUIConsole::SettingsTab::General);
		break;
	case Action::TabInputs:
		gui_console.set_settings_tab(GUIConsole::SettingsTab::Inputs);
		break;
	// Upscaling options: each click advances to the next value. They are all
	// optional, resample the image only, and default to the original behaviour
	// (Nearest / no grid).
	case Action::RomFolder:
		open_folder_dialog();
		break;
	case Action::Volume:
		set_volume(hit.ratio);
		settings.save("");
		volume_drag_ = true; // keep sliding until the button is released
		break;
	case Action::Palette:
		settings.palette = (settings.palette + 1) % 5;
		ppu.set_dmg_palette(settings.palette);
		settings.save("");
		break;
	case Action::HwMode:
		if (settings.hardware_mode == "Auto")
			settings.hardware_mode = "DMG";
		else if (settings.hardware_mode == "DMG")
			settings.hardware_mode = "CGB";
		else
			settings.hardware_mode = "Auto";
		settings.save("");
		break;
	// Upscaling options: each click advances to the next value. They are all
	// optional, resample the image only, and default to the original behaviour
	// (Nearest / no grid).
	case Action::Filter: {
		const int n = static_cast<int>(VideoFilter::Count);
		settings.video_filter = (settings.video_filter + 1) % n;
		apply_video_settings();
		break;
	}
	case Action::LcdGrid:
		settings.lcd_grid = (settings.lcd_grid + 1) % (Settings::kLcdGridMax + 1);
		apply_video_settings();
		break;
	case Action::ProfileBox:
		// The combo box toggles its list; the list carries New / Rename /
		// Delete as trailing entries.
		gui_console.profile_box().open = !gui_console.profile_box().open;
		if (gui_console.profile_box().open)
			gui_console.device_box().open = false;
		break;
	case Action::ProfileItem: {
		const auto &dd = gui_console.profile_box();
		const int total = gui_console.profileMenuSize();
		const int nprof = gui_console.profileCommandIndex();
		gui_console.closeDropdowns();
		if (hit.index < 0 || hit.index >= total || !gui_console.profileMenuEnabled(hit.index))
			break; // an unavailable command is a no-op, not a silent failure
		if (hit.index < nprof) {
			// A profile: activate it. Switching replaces the live mapping
			// wholesale, so the input manager must be re-seeded.
			if (settings.activateProfile(hit.index)) {
				settings.save("");
				sync_input_from_settings();
			}
		} else if (hit.index == nprof) {
			gui_console.start_text_entry(GUIConsole::TextPurpose::NewProfile, "New profile name",
										  std::string(), -1);
			SDL_StartTextInput(window);
		} else if (hit.index == nprof + 1) {
			// Default keeps its name, so there is nothing to rename.
			if (settings.active_profile > 0) {
				gui_console.start_text_entry(GUIConsole::TextPurpose::RenameProfile,
											 "Profile name",
											 settings.profileName(settings.active_profile),
											 settings.active_profile);
				SDL_StartTextInput(window);
			}
		} else if (settings.deleteProfile(settings.active_profile)) {
			settings.save("");
			sync_input_from_settings();
		}
		gui_console.rebuild_settings_cells();
		(void) dd;
		break;
	}
	case Action::ProfileLoad:
		// Re-apply the active profile, discarding live edits.
		if (settings.activateProfile(settings.active_profile)) {
			settings.save("");
			sync_input_from_settings();
		}
		break;
	case Action::ProfileSave:
		// Commit the live mapping into the active profile (syncActiveProfile
		// runs inside save()).
		settings.save("");
		sync_input_from_settings();
		break;
	case Action::DeviceBox:
		// Locked while keyboard-only, so the reason is obvious rather than
		// silently ignoring clicks.
		if (gui_console.device_box().enabled) {
			if (gui_console.device_box().open)
				gui_console.closeDropdowns();
			else
				gui_console.device_box().open = true;
		}
		break;
	case Action::DeviceItem:
		gui_console.setDeviceOptions(controller_names_, hit.index);
		gui_console.closeDropdowns();
		gui_console.rebuild_settings_cells();
		break;
	case Action::PadButton:
		// Clicking a button on the GBA diagram arms it. The capture then
		// accepts only the currently selected source, so picking "Controller"
		// in the device dropdown makes the diagram a controller-only map.
		if (hit.index >= 0 && hit.index < static_cast<int>(kPadButtonCount))
			gui_console.start_binding(static_cast<PadButton>(hit.index));
		break;
	case Action::AutoMap:
		// Context-sensitive: the button lays out a conventional pad mapping
		// when a controller is selected, and restores the factory keyboard
		// layout when the keyboard is.
		if (gui_console.deviceIsController() ? gui_console.applyAutoMap(input_)
										    : gui_console.applyKeyboardDefaults())
			sync_input_from_settings();
		break;
	default:
		// Any click that was not a widget dismisses an open drop-down list.
		gui_console.closeDropdowns();
		break;
	}
}

void GameBoy::handle_menu_key(SDL_Scancode sc, bool pressed)
{
	if (!pressed || show_settings_panel || gui_console.awaiting_input())
		return;
	const int cols = gui_console.menu_grid_cols();
	switch (sc) {
	case SDL_SCANCODE_UP:
		gui_console.menu_move(-cols);
		break;
	case SDL_SCANCODE_DOWN:
		gui_console.menu_move(cols);
		break;
	case SDL_SCANCODE_LEFT:
		gui_console.menu_move(-1);
		break;
	case SDL_SCANCODE_RIGHT:
		gui_console.menu_move(1);
		break;
	case SDL_SCANCODE_PAGEUP:
		gui_console.menu_move(-gui_console.menu_grid_visible_count());
		break;
	case SDL_SCANCODE_PAGEDOWN:
		gui_console.menu_move(gui_console.menu_grid_visible_count());
		break;
	case SDL_SCANCODE_RETURN:
	case SDL_SCANCODE_KP_ENTER:
	case SDL_SCANCODE_SPACE: {
		const auto &list = gui_console.rom_list();
		const int i = gui_console.menu_selected();
		if (i >= 0 && i < static_cast<int>(list.size())) {
			load_rom(list[static_cast<size_t>(i)].path);
		}
		break;
	}
	case SDL_SCANCODE_R:
		rescan_roms();
		break;
	default:
		break;
	}
}

void GameBoy::handle_settings_shortcut(SDL_Scancode sc)
{
	// Keyboard shortcuts for the settings panel (kept alongside clicking).
	if (sc == SDL_SCANCODE_H) {
		if (settings.hardware_mode == "Auto")
			settings.hardware_mode = "DMG";
		else if (settings.hardware_mode == "DMG")
			settings.hardware_mode = "CGB";
		else
			settings.hardware_mode = "Auto";
		settings.save("");
		// log removed
	} else if (sc == SDL_SCANCODE_C) {
		settings.palette = (settings.palette + 1) % 5;
		ppu.set_dmg_palette(settings.palette);
		settings.save("");
		// log removed
	} else if (sc == SDL_SCANCODE_O) {
		open_folder_dialog();
	} else if (sc == SDL_SCANCODE_LEFT) {
		set_volume(settings.volume - 0.05f);
		settings.save("");
	} else if (sc == SDL_SCANCODE_RIGHT) {
		set_volume(settings.volume + 0.05f);
		settings.save("");
	}
}

void GameBoy::run()
{
	while (running && !request_stop) {
		consume_pending_dialogs();
		handle_events();
		if (!running || request_stop)
			break;
		if (!rom_loaded_) {
			// Menu mode: no emulation, just draw the ROM list at ~60Hz.
			// SDL_DelayNS (not SDL_Delay): millisecond sleeps overshoot by
			// ~1ms per frame, which compounds into visible audio drift.
			const f64 frame_start = SDL_GetTicks() / 1000.0;
			renderer.render_menu(gui_console);
			const f64 elapsed = SDL_GetTicks() / 1000.0 - frame_start;
			const f64 target = 1.0 / 60.0;
			if (elapsed < target)
				SDL_DelayNS(static_cast<u64>((target - elapsed) * 1e9));
			last_time = SDL_GetTicks() / 1000.0;
			continue;
		}
		if (paused) {
			SDL_Delay(100);
			continue;
		}
		const f64 frame_start = SDL_GetTicks() / 1000.0;
		// Recompute the logical button aggregate (keyboard OR controller)
		// and push it into whichever emulated pad is active. Both devices
		// ignore no-op writes, so this is safe to do every frame.
		input_.update();
		input_.applyTo(joypad, gba_.keypad());
		if (gba_mode_) {
			// GBA now uses the same rendering mode as GB/GBC (GBC body + screen)
			step_gba_frame();
			if (gba_.ppu().frameReady()) {
				if (gui_console.fullscreen_game()) {
					// Full-window game: no CGB body, just the framebuffer fill
					renderer.render_gba(gba_.ppu().frame().pixels.data(), gba::GbaPpu::kWidth,
										gba::GbaPpu::kHeight);
				} else {
					// Use GUI-aware rendering (GBC body + bezel + screen) like GB/GBC
					renderer.render_gba_with_gui(gba_.ppu().frame().pixels.data(), gba::GbaPpu::kWidth,
												 gba::GbaPpu::kHeight, gui_console);
				}
				gba_.ppu().clearFrameReady();
				frame_count++;
				// Autosave battery-backed/Flash/EEPROM save hardware every ~10s.
				if (gba_.cartridge().hasSave() && !last_rom_path_.empty() &&
					frame_count % 600 == 0) {
					gba_.cartridge().saveToFile(last_rom_path_ + ".sav");
				}
			}
		} else {
			step_frame();
			gui_console.set_double_speed(cpu.double_speed);
			if (ppu.frame_ready()) {
				if (gui_console.fullscreen_game()) {
					// Full-window game: no CGB body, just the framebuffer fill
					renderer.render(ppu.frame());
				} else {
					// Use GUI-aware rendering (GBC body + bezel + screen)
					renderer.render_with_gui(ppu.frame(), gui_console);
				}
				ppu.clear_frame_ready();
				frame_count++;
				// Autosave battery RAM every ~10s so progress survives a crash
				// or kill (shutdown also saves). Cheap: a few KB write.
				if (cartridge.has_battery() && !last_rom_path_.empty() && frame_count % 600 == 0) {
					cartridge.save_ram(last_rom_path_ + ".sav");
				}
			}
		}
		// Pace to ~60Hz (59.7fps real hardware; 60 is close enough for now).
		// SDL_DelayNS: millisecond sleeps overshoot ~1ms/frame, compounding
		// into audio starvation crackle and a permanent ~6% slowdown.
		const f64 elapsed = SDL_GetTicks() / 1000.0 - frame_start;
		const f64 target = 1.0 / 60.0;
		if (elapsed < target)
			SDL_DelayNS(static_cast<u64>((target - elapsed) * 1e9));
		last_time = SDL_GetTicks() / 1000.0;
	}
}

void GameBoy::shutdown()
{
	flush_active_save();
	settings.save("");
	apu.close_audio();
	gba_.audio().close_audio();
	renderer.shutdown();
	if (window) {
		SDL_DestroyWindow(window);
		window = nullptr;
	}
	SDL_Quit();
	running = false;
}

void GameBoy::handle_events()
{
	SDL_Event ev;
	while (SDL_PollEvent(&ev)) {
		if (ev.type == SDL_EVENT_QUIT) {
			running = false;
			return;
		}
		if (ev.type == SDL_EVENT_WINDOW_RESIZED || ev.type == SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED ||
			ev.type == SDL_EVENT_WINDOW_DISPLAY_CHANGED) {
			// Layout and hit-testing live in window coordinates (the same
			// space as mouse event x/y). Physical pixels differ on HiDPI,
			// so SDL_GetWindowSizeInPixels here would offset every button
			// and the screen rect; the renderer rescales to pixels itself.
			int w = 0, h = 0;
			SDL_GetWindowSize(window, &w, &h);
			if (w > 0 && h > 0)
				gui_console.update_layout(w, h);
			// Also notify renderer to recreate swapchain if needed
			// (resize() re-queries the drawable size itself; args unused).
			renderer.resize(static_cast<u32>(w), static_cast<u32>(h));
		}
		if (ev.type == SDL_EVENT_MOUSE_WHEEL) {
			const int steps = static_cast<int>(std::round(ev.wheel.y));
			if (steps != 0) {
				if (show_settings_panel) {
					// Scrolling over the volume slider nudges it.
					float mx = 0, my = 0;
					SDL_GetMouseState(&mx, &my);
					const auto vol = gui_console.settings_volume_rect();
					if (mx >= vol.x && mx < vol.x + vol.w && my >= vol.y && my < vol.y + vol.h) {
						set_volume(settings.volume + steps * 0.05f);
						settings.save("");
					}
				} else if (!rom_loaded_) {
					gui_console.menu_move(-steps * gui_console.menu_grid_cols());
				}
			}
		}
		// Dragging the volume slider: keep following the cursor until release.
		if (ev.type == SDL_EVENT_MOUSE_MOTION && volume_drag_ && show_settings_panel) {
			const auto vol = gui_console.settings_volume_rect();
			if (vol.w > 0) {
				set_volume((ev.motion.x - vol.x) / vol.w);
			}
		}
		if (ev.type == SDL_EVENT_MOUSE_BUTTON_DOWN && ev.button.button == SDL_BUTTON_LEFT) {
			int mx = static_cast<int>(ev.button.x);
			int my = static_cast<int>(ev.button.y);
			GB4ME_ITRACE("input: mousedown %d,%d clicks=%u win=%dx%d rom=%d gba=%d panel=%d "
						 "menu=%d full=%d awaiting=%d\n",
					mx, my, ev.button.clicks, gui_console.window_w(), gui_console.window_h(),
					(int) rom_loaded_, (int) gba_mode_, (int) show_settings_panel,
					(int) !rom_loaded_, (int) gui_console.fullscreen_game(),
					(int) gui_console.awaiting_input());
			// Full-window game fill toggle: double-click screen ↔ fill window
			// Only when a ROM is running and no modal is open.
			if (rom_loaded_ && !show_settings_panel && !gui_console.awaiting_input()) {
				const bool is_full = gui_console.fullscreen_game();
				// GBA frames use the 3:2-fitted rect, not the GB-shaped one.
				const bool on_screen =
					gba_mode_ ? gui_console.hit_screen(mx, my, gui_console.gba_screen_rect())
							  : gui_console.hit_screen(mx, my);
				if (!is_full && on_screen && ev.button.clicks == 2) {
					gui_console.set_fullscreen_game(true);
					// log removed
					continue;
				}
				if (is_full && ev.button.clicks == 2) {
					gui_console.set_fullscreen_game(false);
					// log removed
					continue;
				}
				if (is_full) {
					// Swallow single clicks while fullscreen so they don't hit buttons behind
					continue;
				}
			}
			// Resume choice swallows other clicks until answered; clicking
			// outside it cancels back to where you were.
			// Clickable settings panel has priority (it overlays everything).
			if (show_settings_panel) {
				auto shit = gui_console.settings_hit_test(mx, my);
				if (shit.action != GUIConsole::SettingsAction::None) {
					dispatch_settings_hit(shit);
					continue;
				}
				// Clicking outside the panel (and not on the gear or OPEN
				// button) dismisses it. Gear/OPEN clicks dismiss it too but
				// still fall through so they act on the same click.
				const auto panel = gui_console.settings_panel_rect();
				const bool in_panel = mx >= panel.x && mx < panel.x + panel.w && my >= panel.y &&
									  my < panel.y + panel.h;
				if (!in_panel) {
					show_settings_panel = false;
					gui_console.set_show_settings(false);
					gui_console.cancel_mapping();
					if (!gui_console.hit_settings(mx, my) && !gui_console.hit_open(mx, my)) {
						continue;
					}
				}
			}
			// OPEN button (file dialog) works in menu and in game.
			if (gui_console.hit_open(mx, my)) {
				open_file_dialog();
				continue;
			}
			// Settings gear toggles the panel.
			if (gui_console.hit_settings(mx, my)) {
				show_settings_panel = !show_settings_panel;
				gui_console.set_show_settings(show_settings_panel);
				// log removed
				continue;
			}
			if (!rom_loaded_) {
				// ROM list: click selects, clicking the selection launches.
				// Clicks that miss the list fall through to the body
				// controls, so remapping works from the menu too (no ROM
				// needed — the mapping table is global).
				bool rearmed = false;
				if (auto pick = gui_console.menu_hit_test(mx, my); pick) {
					GB4ME_ITRACE("input: menu pick %d\n", *pick);
					if (*pick == gui_console.menu_selected()) {
						const auto &list = gui_console.rom_list();
						if (*pick >= 0 && *pick < static_cast<int>(list.size())) {
							load_rom(list[static_cast<size_t>(*pick)].path);
						}
					} else {
						gui_console.set_menu_selected(*pick);
					}
				} else if (auto sh = gui_console.hit_shoulder(mx, my); sh) {
					GB4ME_ITRACE("input: menu shoulder %d -> arm GBA remap\n", (int) *sh);
					gui_console.set_gba_pressed(*sh, true);
					gui_console.start_binding(static_cast<PadButton>(*sh));
					rearmed = true;
				} else if (auto hit = gui_console.hit_test(mx, my); hit) {
					GB4ME_ITRACE("input: menu button %d -> arm remap\n", (int) *hit);
					gui_console.set_button_pressed(*hit, true);
					gui_console.start_binding(
						static_cast<PadButton>(Settings::shared_key_index(*hit)));
					rearmed = true;
				} else {
					GB4ME_ITRACE("input: menu click hit nothing\n");
				}
				if (!rearmed)
					gui_console.cancel_mapping();
				continue;
			}
			// Body controls use click-to-remap (photo buttons and GBA
			// shoulder pills alike): flash the press indicator while the
			// remap modal is armed, gameplay keys stay on the keyboard.
			// Anything else dismisses an armed remap modal (clicking away).
			// Shoulders arm GBA remap in any mode (global table).
			bool rearmed = false;
			if (auto sh = gui_console.hit_shoulder(mx, my); sh) {
				GB4ME_ITRACE("input: shoulder %d -> arm GBA remap\n", (int) *sh);
				gui_console.set_gba_pressed(*sh, true);
				gui_console.start_binding(static_cast<PadButton>(*sh));
				rearmed = true;
				continue;
			}
			if (auto hit = gui_console.hit_test(mx, my); hit) {
				GB4ME_ITRACE("input: button %d -> arm remap\n", (int) *hit);
				gui_console.set_button_pressed(*hit, true);
				gui_console.start_binding(
					static_cast<PadButton>(Settings::shared_key_index(*hit)));
				rearmed = true;
				continue;
			}
			GB4ME_ITRACE("input: mousedown hit nothing\n");
			if (!rearmed)
				gui_console.cancel_mapping();
		}
		if (ev.type == SDL_EVENT_MOUSE_BUTTON_UP && ev.button.button == SDL_BUTTON_LEFT) {
			// Release anything the mouse is holding (single pointer: one
			// release clears all, so drag-off releases can never stick).
			release_mouse_buttons();
			// End of a volume-slider drag: persist the final value.
			if (volume_drag_) {
				volume_drag_ = false;
				settings.save("");
				// log removed
			}
		}
		if (ev.type == SDL_EVENT_WINDOW_FOCUS_LOST) {
			// Missed key/mouse releases while unfocused must not stick.
			release_mouse_buttons();
			joypad.release_all();
			gba_.keypad().releaseAll();
			input_.releaseAll();
		}
		// Typed characters reach us as text events (layout/IME aware), which
		// is what the profile-naming prompt consumes.
		if (ev.type == SDL_EVENT_TEXT_INPUT) {
			if (gui_console.text_entry_active() && ev.text.text[0] != '\0')
				gui_console.text_input(ev.text.text);
			continue;
		}
		// Controller events: state tracking plus, when a controller remap is
		// armed, the binding to store. A captured controller input must not
		// also press the game.
		if (ev.type == SDL_EVENT_GAMEPAD_ADDED || ev.type == SDL_EVENT_GAMEPAD_REMOVED ||
		    ev.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN || ev.type == SDL_EVENT_GAMEPAD_BUTTON_UP ||
		    ev.type == SDL_EVENT_GAMEPAD_AXIS_MOTION) {
			input_.handleEvent(ev);
			if (ev.type == SDL_EVENT_GAMEPAD_ADDED ||
			    ev.type == SDL_EVENT_GAMEPAD_REMOVED) {
				gui_console.refreshControllers(input_);
				refresh_controller_names();
				gui_console.rebuild_settings_cells();
			}
			// The device dropdown decides what the armed button accepts, so
			// picking "Controller" makes the diagram a controller-only map.
			if (gui_console.awaiting_input() && !gui_console.captureWantsKey()) {
				Binding captured;
				bool have = false;
				if (ev.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN) {
					captured.kind = BindKind::Button;
					captured.code = static_cast<int>(ev.gbutton.button);
					have = true;
				} else if (ev.type == SDL_EVENT_GAMEPAD_AXIS_MOTION) {
					const int v = ev.gaxis.value;
					if (v > input_.axisThreshold()) {
						captured.kind = BindKind::AxisPos;
						captured.code = static_cast<int>(ev.gaxis.axis);
						have = true;
					} else if (v < -input_.axisThreshold()) {
						captured.kind = BindKind::AxisNeg;
						captured.code = static_cast<int>(ev.gaxis.axis);
						have = true;
					}
				}
				if (have && gui_console.consume_binding(captured)) {
					sync_input_from_settings();
					continue;
				}
			}
			continue;
		}
		if (ev.type == SDL_EVENT_KEY_DOWN || ev.type == SDL_EVENT_KEY_UP) {
			if (ev.key.repeat)
				continue;
			const bool pressed = ev.type == SDL_EVENT_KEY_DOWN;
			// Track the raw key state for the input manager, but only if the
			// frontend did not consume it (a UI shortcut must not also drive
			// the game). Escape is handled by the remap path below.
			input_.handleEvent(ev);
			// A key the frontend owns must never also drive the pad. Always
			// clear on release so suppression stays symmetric even if the
			// context changed between down and up (e.g. a remap completing).
			{
				const SDL_Scancode sc = ev.key.scancode;
				const bool reserved = sc == SDL_SCANCODE_S || sc == SDL_SCANCODE_O ||
				                      sc == SDL_SCANCODE_E || sc == SDL_SCANCODE_P ||
				                      sc == SDL_SCANCODE_F || sc == SDL_SCANCODE_F11 ||
				                      sc == SDL_SCANCODE_MINUS || sc == SDL_SCANCODE_EQUALS ||
				                      sc == SDL_SCANCODE_ESCAPE;
				const bool modal = !rom_loaded_ || show_settings_panel ||
				                   gui_console.awaiting_input() ||
				                   gui_console.text_entry_active();
				input_.setSuppressed(sc, pressed && (reserved || modal));
			}
			// Profile-naming prompt owns the keyboard outright: Enter accepts,
			// Backspace edits, Esc cancels, and nothing else leaks to the game.
			if (gui_console.text_entry_active()) {
				if (pressed) {
					if (ev.key.scancode == SDL_SCANCODE_RETURN ||
						ev.key.scancode == SDL_SCANCODE_KP_ENTER) {
						if (gui_console.text_accept())
							apply_profile_name();
						SDL_StopTextInput(window);
					} else if (ev.key.scancode == SDL_SCANCODE_ESCAPE) {
						gui_console.text_cancel();
						SDL_StopTextInput(window);
					} else if (ev.key.scancode == SDL_SCANCODE_BACKSPACE) {
						gui_console.text_backspace();
					}
				}
				continue;
			}
			// If awaiting input mapping, consume as mapping (non-blocking)
			if (gui_console.awaiting_input() && pressed) {
				if (ev.key.scancode == SDL_SCANCODE_ESCAPE) {
					GB4ME_ITRACE("input: remap cancelled\n");
					gui_console.cancel_mapping();
					// log removed
					continue;
				}
				if (gui_console.captureWantsKey() &&
					gui_console.consume_binding(Binding::makeKey(ev.key.scancode))) {
					GB4ME_ITRACE("input: remapped to scancode %d\n", (int) ev.key.scancode);
					sync_input_from_settings();
					continue; // do not also send as joypad press
				}
			}
			GB4ME_ITRACE("input: key %d %s\n", (int) ev.key.scancode,
					pressed ? "down" : "up");
			// Fullscreen toggle (Esc exits fullscreen first, F/F11 toggles).
			// Now works for both GB and GBA modes.
			if (pressed && (ev.key.scancode == SDL_SCANCODE_F || ev.key.scancode == SDL_SCANCODE_F11)) {
				if (rom_loaded_ && !show_settings_panel && !gui_console.awaiting_input()) {
					gui_console.toggle_fullscreen_game();
					// log removed
					continue;
				}
			}
			// Global shortcuts — Esc returns to menu, never quits app (use window X to quit)
			if (pressed && ev.key.scancode == SDL_SCANCODE_ESCAPE) {
				if (gui_console.fullscreen_game()) {
					gui_console.set_fullscreen_game(false);
					// log removed
					continue;
				}
				if (show_settings_panel) {
					show_settings_panel = false;
					gui_console.set_show_settings(false);
					gui_console.cancel_mapping();
				} else if (rom_loaded_) {
					eject_rom();
					// log removed
				} else {
					// In menu, Esc does nothing — window X still quits
				}
				continue;
			}
			// Settings panel now works for both GB and GBA modes.
			if (pressed && ev.key.scancode == SDL_SCANCODE_S) {
				show_settings_panel = !show_settings_panel;
				gui_console.set_show_settings(show_settings_panel);
				// log removed
				continue;
			}
			// OPEN file dialog (folder picker lives on the settings panel / O key there).
			if (pressed && ev.key.scancode == SDL_SCANCODE_O && !show_settings_panel) {
				open_file_dialog();
				continue;
			}
			// Eject back to the ROM menu while playing.
			if (pressed && ev.key.scancode == SDL_SCANCODE_E && rom_loaded_ &&
				!show_settings_panel) {
				eject_rom();
				continue;
			}
			if (pressed && ev.key.scancode == SDL_SCANCODE_P && rom_loaded_) {
				paused = !paused;
				continue;
			}
			// Volume +/- (non-blocking, does not pause emu)
			if (pressed &&
				(ev.key.scancode == SDL_SCANCODE_MINUS || ev.key.scancode == SDL_SCANCODE_EQUALS)) {
				if (ev.key.scancode == SDL_SCANCODE_MINUS)
					settings.volume = std::max(0.0f, settings.volume - 0.1f);
				else
					settings.volume = std::min(1.0f, settings.volume + 0.1f);
				apu.set_master_volume(settings.volume);
				gba_.audio().set_master_volume(settings.volume);
				settings.save("");
				// log removed
				continue;
			}
			// Settings panel keyboard shortcuts when open
			if (show_settings_panel && pressed) {
				handle_settings_shortcut(ev.key.scancode);
				continue;
			}
			// Menu navigation (no ROM loaded): arrows select, Enter plays.
			if (!rom_loaded_) {
				handle_menu_key(ev.key.scancode, pressed);
				continue;
			}
			// Don't block emu loop during modal; just don't send joypad presses while awaiting
			if (gui_console.awaiting_input())
				continue;

			// Gameplay key state is owned by the InputManager (keyboard AND
			// controller are OR'd together) and pushed into the emulated pads
			// once per frame in run(), so nothing is forwarded from here.
		}
	}
}

void GameBoy::step_frame()
{
	// One frame = 70224 T-cycles = 17556 M-cycles at normal speed.
	// In double-speed, CPU runs at 2x M-cycles per same real-time PPU frame
	// (recommended model: keep PPU/APU fixed, CPU variable per AGENTS.md).
	// CPU double means 35112 M per frame, PPU still 17556 M. We achieve this
	// by stepping CPU at double rate but feeding PPU/Timer/APU at half rate.
	constexpr u32 kMCyclesPerFrame = CYCLES_PER_FRAME / 4;
	const u32 cpu_target = cpu.double_speed ? kMCyclesPerFrame * 2 : kMCyclesPerFrame;
	u32 cpu_done = 0;
	u32 ppu_done = 0;
	while (cpu_done < cpu_target && ppu_done < kMCyclesPerFrame) {
		const u32 mc = cpu.step();
		cpu_done += mc;
		// PPU/Timer/APU stay at normal clock — in double mode they advance
		// at half the CPU rate (real-time fixed).
		u32 ppu_mc = mc;
		if (cpu.double_speed) {
			// Half, rounding up to avoid stalling on odd cycles (e.g. 3→2).
			ppu_mc = (mc + 1) / 2;
			if (ppu_mc == 0 && mc != 0)
				ppu_mc = 1;
		}
		if (ppu_done + ppu_mc > kMCyclesPerFrame)
			ppu_mc = kMCyclesPerFrame - ppu_done;
		update_timers(ppu_mc);
		ppu_done += ppu_mc;
		// If CPU already double but PPU still needs cycles due to rounding,
		// continue; if PPU done, still finish CPU tail.
		if (ppu_done >= kMCyclesPerFrame && cpu_done < cpu_target) {
			// Drain remaining CPU cycles without PPU (still tick timer/apu at half)
			continue;
		}
	}
	// Ensure any remaining PPU cycles (rounding) are flushed
	if (ppu_done < kMCyclesPerFrame) {
		update_timers(kMCyclesPerFrame - ppu_done);
	}
}

void GameBoy::step_gba_frame()
{
	// One GBA frame = 280896 ticks. Run until the PPU raises frame-ready,
	// bounded so a wedged core can't hang the frontend (~4 frames). The
	// bound must clear the worst legitimate frame (280896 + a max-size
	// immediate DMA ~131K stall cycles); truncating mid-frame would drop
	// PPU lines and timer/audio time.
	const u64 start = gba_.tick();
	while (!gba_.ppu().frameReady() && gba_.tick() - start < 1200000) {
		gba_.step(4096);
	}
}

void GameBoy::update_timers(u32 cycles)
{
	timer.step(cycles);
	ppu.step(cycles);
	apu.step(cycles);
}

void GameBoy::check_interrupts()
{
	// Serviced inside CPU::step() (see service_interrupts). Kept for the
	// future cycle-accurate interrupt-delay model.
}

} // namespace gb
