#include "gb/gui_console.h"
#include <SDL3/SDL.h>
#include <algorithm>
#include <cmath>
#include <cmath>
#include <fstream>
#include <sstream>

namespace gb
{

std::array<SDL_Scancode, 10> Settings::defaultKeyMap()
{
	// Default mappings mirror GameBoy::handle_events (shared table, GBA key
	// order). R is on D: S toggles the settings panel globally and can never
	// reach a game.
	std::array<SDL_Scancode, 10> m{};
	m[static_cast<int>(gba::GbaKeypad::Key::Right)] = SDL_SCANCODE_RIGHT;
	m[static_cast<int>(gba::GbaKeypad::Key::Left)] = SDL_SCANCODE_LEFT;
	m[static_cast<int>(gba::GbaKeypad::Key::Up)] = SDL_SCANCODE_UP;
	m[static_cast<int>(gba::GbaKeypad::Key::Down)] = SDL_SCANCODE_DOWN;
	m[static_cast<int>(gba::GbaKeypad::Key::A)] = SDL_SCANCODE_X;
	m[static_cast<int>(gba::GbaKeypad::Key::B)] = SDL_SCANCODE_Z;
	m[static_cast<int>(gba::GbaKeypad::Key::Start)] = SDL_SCANCODE_RETURN;
	m[static_cast<int>(gba::GbaKeypad::Key::Select)] = SDL_SCANCODE_RSHIFT;
	m[static_cast<int>(gba::GbaKeypad::Key::R)] = SDL_SCANCODE_D;
	m[static_cast<int>(gba::GbaKeypad::Key::L)] = SDL_SCANCODE_A;
	return m;
}

void Settings::applyDefaultKeyMap()
{
	key_map = defaultKeyMap();
	// One source per button, so a keyboard layout replaces any pad bindings.
	for (int i = 0; i < static_cast<int>(kPadButtonCount); i++) {
		pad_map[static_cast<size_t>(i)] = Binding{};
		pad_map_alt[static_cast<size_t>(i)] = Binding{};
	}
}

Settings::Settings()
{
	key_map = defaultKeyMap();
	// profiles[0] is the built-in Default: it mirrors the tables above and can
	// never be deleted, so the original mapping stays recoverable.
	InputProfile def;
	def.name = "Default";
	def.key_map = key_map;
	profiles.push_back(def);
}

SDL_Scancode Settings::scancode_for(Joypad::Key k) const
{
	return key_map[shared_key_index(k)];
}
void Settings::set_mapping(Joypad::Key k, SDL_Scancode code)
{
	key_map[shared_key_index(k)] = code;
}

std::string Settings::profileName(int idx) const
{
	if (idx < 0 || idx >= profileCount())
		return std::string();
	return profiles[static_cast<size_t>(idx)].name;
}

void Settings::syncActiveProfile()
{
	// The live tables are the editable copy of whichever profile is selected,
	// so commit them back before anything replaces or persists them.
	if (active_profile < 0 || active_profile >= profileCount())
		return;
	InputProfile &p = profiles[static_cast<size_t>(active_profile)];
	p.key_map = key_map;
	p.pad_map = pad_map;
	p.pad_map_alt = pad_map_alt;
}

bool Settings::loadProfile(int idx)
{
	if (idx < 0 || idx >= profileCount())
		return false;
	const InputProfile &p = profiles[static_cast<size_t>(idx)];
	key_map = p.key_map;
	pad_map = p.pad_map;
	pad_map_alt = p.pad_map_alt;
	active_profile = idx;
	return true;
}

Binding Settings::currentBinding(int i) const
{
	if (i < 0 || i >= static_cast<int>(kPadButtonCount))
		return Binding{};
	if (key_map[static_cast<size_t>(i)] != SDL_SCANCODE_UNKNOWN)
		return Binding::makeKey(key_map[static_cast<size_t>(i)]);
	if (pad_map[static_cast<size_t>(i)].bound())
		return pad_map[static_cast<size_t>(i)];
	if (pad_map_alt[static_cast<size_t>(i)].bound())
		return pad_map_alt[static_cast<size_t>(i)];
	return Binding{};
}

void Settings::setBinding(int i, const Binding &b)
{
	if (i < 0 || i >= static_cast<int>(kPadButtonCount))
		return;
	// Exactly one source per button: assigning a key clears any controller
	// bindings, and assigning a controller button clears the key.
	if (b.kind == BindKind::Key) {
		key_map[static_cast<size_t>(i)] = static_cast<SDL_Scancode>(b.code);
		pad_map[static_cast<size_t>(i)] = Binding{};
		pad_map_alt[static_cast<size_t>(i)] = Binding{};
	} else {
		key_map[static_cast<size_t>(i)] = SDL_SCANCODE_UNKNOWN;
		pad_map[static_cast<size_t>(i)] = b;
		pad_map_alt[static_cast<size_t>(i)] = Binding{};
	}
}

bool Settings::activateProfile(int idx)
{
	if (idx < 0 || idx >= profileCount())
		return false;
	// Commit any pending edits to the profile we are leaving before its
	// stored mapping is replaced on screen.
	syncActiveProfile();
	return loadProfile(idx);
}

int Settings::addProfile(const std::string &name)
{
	if (profileCount() >= kMaxProfiles)
		return -1;
	InputProfile p;
	p.key_map = key_map; // snapshot whatever is live right now
	p.pad_map = pad_map;
	p.pad_map_alt = pad_map_alt;
	std::string n = name;
	n.erase(std::remove(n.begin(), n.end(), '\n'), n.end());
	if (n.empty())
		n = "Profile " + std::to_string(profileCount() + 1);
	n = n.substr(0, 24);
	// Keep names unique so a list of profiles is never ambiguous.
	const std::string base = n;
	for (int suffix = 2;; ++suffix) {
		bool clash = false;
		for (int i = 1; i < profileCount(); ++i) {
			if (profiles[static_cast<size_t>(i)].name == n) {
				clash = true;
				break;
			}
		}
		if (!clash)
			break;
		n = base + " (" + std::to_string(suffix) + ")";
	}
	p.name = n;
	profiles.push_back(p);
	return activateProfile(static_cast<int>(profiles.size()) - 1) ? active_profile : -1;
}

bool Settings::renameProfile(int idx, const std::string &name)
{
	if (idx <= 0 || idx >= profileCount())
		return false; // Default keeps its name
	std::string n = name;
	n.erase(std::remove(n.begin(), n.end(), '\n'), n.end());
	if (n.empty())
		return false;
	profiles[static_cast<size_t>(idx)].name = n.substr(0, 24);
	return true;
}

bool Settings::deleteProfile(int idx)
{
	if (idx <= 0 || idx >= profileCount())
		return false; // Default is permanent
	profiles.erase(profiles.begin() + idx);
	// Profiles after the removed one shift down by one.
	// Commit first, so edits to the profile being deleted are simply dropped
	// with it rather than leaking into whichever profile becomes active.
	syncActiveProfile();
	if (active_profile == idx)
		active_profile = 0;
	else if (active_profile > idx)
		--active_profile;
	// Load directly rather than through activateProfile(): the live tables
	// still hold the DELETED profile, and re-syncing here would commit them
	// into whichever profile is taking over.
	return loadProfile(active_profile);
}

std::string default_settings_path()
{
	const char *home = SDL_getenv("HOME");
	std::string base = home ? std::string(home) + "/.config/GB4ME" : ".";
	return base + "/settings.cfg";
}

bool Settings::save(const std::string &path)
{
	// Persist any remapping done since the last switch into the active
	// profile, so a restart comes back on the mapping the user is looking at.
	syncActiveProfile();
	std::string p = path.empty() ? default_settings_path() : path;
	// Ensure directory exists
	size_t slash = p.find_last_of('/');
	if (slash != std::string::npos) {
		std::string dir = p.substr(0, slash);
		SDL_CreateDirectory(dir.c_str());
	}
	std::ofstream f(p);
	if (!f)
		return false;
	f << "rom_folder=" << rom_folder << "\n";
	f << "volume=" << volume << "\n";
	f << "video_scale=" << video_scale << "\n";
	f << "video_filter=" << video_filter << "\n";
	f << "lcd_grid=" << lcd_grid << "\n";
	f << "palette=" << palette << "\n";
	f << "hardware_mode=" << hardware_mode << "\n";
	f << "console_color=" << static_cast<int>(console_color) << "\n";
	for (int i = 0; i < 10; i++)
		f << "key" << i << "=" << static_cast<int>(key_map[i]) << "\n";
	for (int i = 0; i < static_cast<int>(kPadButtonCount); i++)
		f << "pad" << i << "=" << pad_map[i].packed() << "\n";
	for (int i = 0; i < static_cast<int>(kPadButtonCount); i++)
		f << "alt" << i << "=" << pad_map_alt[i].packed() << "\n";
	// Every profile, keyed prof<N>... where N is 1-based so it cannot collide
	// with the bare key/pad/alt lines above. The active profile is also
	// mirrored into those bare lines, so an older build still reads the live
	// mapping even though it does not understand profiles.
	f << "active_profile=" << active_profile << "\n";
	f << "profile_count=" << profileCount() << "\n";
	// N runs 1..profileCount() so the LAST profile is written too.
	for (int p = 1; p <= profileCount(); p++) {
		// Tag profN is 1-based and NAMES profiles[N-1], so prof1 is Default.
		const InputProfile &pr = profiles[static_cast<size_t>(p - 1)];
		const std::string tag = "prof" + std::to_string(p) + "_";
		f << tag << "name=" << pr.name << "\n";
		for (int i = 0; i < 10; i++)
			f << tag << "key" << i << "=" << static_cast<int>(pr.key_map[i]) << "\n";
		for (int i = 0; i < static_cast<int>(kPadButtonCount); i++)
			f << tag << "pad" << i << "=" << pr.pad_map[i].packed() << "\n";
		for (int i = 0; i < static_cast<int>(kPadButtonCount); i++)
			f << tag << "alt" << i << "=" << pr.pad_map_alt[i].packed() << "\n";
	}
	return true;
}
namespace
{

// Non-throwing integer parse for the settings file (a hand-edited or
// corrupt line must never take down startup). Returns -1 on garbage.
int safe_index(const std::string &s)
{
	if (s.empty() || s.size() > 9)
		return -1;
	size_t i = 0;
	if (s[0] == '+' || s[0] == '-')
		i = 1;
	if (i >= s.size())
		return -1;
	int v = 0;
	for (; i < s.size(); i++) {
		if (s[i] < '0' || s[i] > '9')
			return -1;
		v = v * 10 + (s[i] - '0');
		if (v > 1000000)
			return -1;
	}
	return s[0] == '-' ? -v : v;
}

	// Non-throwing float parse for volume (accepts "1", "0.8", ".5").
	bool safe_float(const std::string &s, float &out)
	{
		if (s.empty() || s.size() > 16)
			return false;
		size_t i = 0;
		bool neg = false;
		if (s[0] == '+' || s[0] == '-') {
			neg = (s[0] == '-');
			i = 1;
		}
		if (i >= s.size())
			return false;
		double whole = 0, frac = 0, div = 1;
		bool dot = false, any = false;
		for (; i < s.size(); i++) {
			if (s[i] == '.' && !dot) {
				dot = true;
				continue;
			}
			if (s[i] < '0' || s[i] > '9')
				return false;
			any = true;
			if (!dot)
				whole = whole * 10 + (s[i] - '0');
			else {
				frac = frac * 10 + (s[i] - '0');
				div *= 10;
			}
		}
		if (!any)
			return false;
		out = static_cast<float>((neg ? -1 : 1) * (whole + frac / div));
		return true;
	}

} // namespace

bool Settings::load(const std::string &path)
{
	std::string p = path.empty() ? default_settings_path() : path;
	std::ifstream f(p);
	if (!f)
		return false;
	// Staging for the unified table (see migration note below). Staged
	// from the current table so missing lines keep existing values.
	std::array<SDL_Scancode, 10> legacy_key = key_map;
	std::array<SDL_Scancode, 10> legacy_gkey = key_map;
	bool saw_new = false, saw_gkey = false;
	// Profiles are staged separately and applied last, because the bare
	// key/pad/alt lines below still describe the ACTIVE profile and must win
	// when the file has no profile section at all.
	std::vector<InputProfile> loaded;
	int loaded_active = 0;
	std::string line;
	while (std::getline(f, line)) {
		size_t eq = line.find('=');
		if (eq == std::string::npos)
			continue;
		std::string k = line.substr(0, eq);
		std::string v = line.substr(eq + 1);
		if (k == "rom_folder")
			rom_folder = v;
		else if (k == "volume") {
			float f = 0;
			if (safe_float(v, f) && f >= 0.0f && f <= 1.0f)
				volume = f;
		} else if (k == "video_scale") {
			const int s = safe_index(v);
			if (s >= 1 && s <= 6)
				video_scale = s;
		} else if (k == "video_filter") {
			const int v2 = safe_index(v);
			if (v2 >= 0 && v2 < static_cast<int>(VideoFilter::Count))
				video_filter = v2;
		} else if (k == "lcd_grid") {
			const int v2 = safe_index(v);
			if (v2 >= 0 && v2 <= kLcdGridMax)
				lcd_grid = v2;
		} else if (k == "palette") {
			const int p = safe_index(v);
			if (p >= 0 && p <= 4)
				palette = p;
		} else if (k == "hardware_mode") {
			if (v == "Auto" || v == "DMG" || v == "CGB")
				hardware_mode = v;
		} else if (k == "console_color") {
			const int c = safe_index(v);
			if (c >= 0 && c <= static_cast<int>(ConsoleColor::ClearPurple))
				console_color = static_cast<ConsoleColor>(c);
		}
		else if (k.rfind("gkey", 0) == 0) {
			const int idx = safe_index(k.substr(4));
			const int code = safe_index(v);
			if (idx >= 0 && idx < 10 && code >= 0)
				legacy_gkey[idx] = static_cast<SDL_Scancode>(code);
			saw_gkey = true;
		} else if (k.rfind("pad", 0) == 0 || k.rfind("alt", 0) == 0) {
			// padN / altN: packed controller bindings (kind<<16 | code).
			const bool alt = k.rfind("alt", 0) == 0;
			const int idx = safe_index(k.substr(3));
			const int packed = std::atoi(v.c_str());
			if (idx >= 0 && idx < static_cast<int>(kPadButtonCount) && packed >= 0) {
				const Binding b = Binding::unpack(packed);
				if (alt)
					pad_map_alt[static_cast<size_t>(idx)] = b;
				else
					pad_map[static_cast<size_t>(idx)] = b;
			}
		} else if (k.rfind("key", 0) == 0) {
			const int idx = safe_index(k.substr(3));
			const int code = safe_index(v);
			if (idx >= 0 && idx < 10 && code >= 0) {
				legacy_key[idx] = static_cast<SDL_Scancode>(code);
				if (idx >= 8)
					saw_new = true; // old files never wrote bare key8/key9
			}
		} else if (k.rfind("prof", 0) == 0) {
			// profN_name / profN_keyM / profN_padM / profN_altM, N 1-based.
			const size_t us = k.find('_');
			if (us == std::string::npos)
				continue;
			const int p = safe_index(k.substr(4, us - 4));
			if (p < 1 || p > Settings::kMaxProfiles)
				continue;
			if (static_cast<int>(loaded.size()) < p)
				loaded.resize(static_cast<size_t>(p));
			InputProfile &pr = loaded[static_cast<size_t>(p - 1)];
			const std::string fld = k.substr(us + 1);
			if (fld == "name") {
				pr.name = v;
			} else if (fld.rfind("key", 0) == 0) {
				const int idx = safe_index(fld.substr(3));
				const int code = safe_index(v);
				if (idx >= 0 && idx < 10 && code >= 0)
					pr.key_map[idx] = static_cast<SDL_Scancode>(code);
			} else if (fld.rfind("pad", 0) == 0 || fld.rfind("alt", 0) == 0) {
				const bool alt = fld.rfind("alt", 0) == 0;
				const int idx = safe_index(fld.substr(3));
				const int packed = std::atoi(v.c_str());
				if (idx >= 0 && idx < static_cast<int>(kPadButtonCount) && packed >= 0) {
					const Binding b = Binding::unpack(packed);
					if (alt)
						pr.pad_map_alt[idx] = b;
					else
						pr.pad_map[idx] = b;
				}
			}
		} else if (k == "active_profile") {
			loaded_active = safe_index(v);
		}
	}
	// Unified table, GBA key order. Old files stored GB order (key0-7) plus
	// a GBA table (gkey0-9); migrate either into the shared table once.
	if (saw_new) {
		for (int i = 0; i < 10; i++)
			key_map[i] = legacy_key[i];
	} else if (saw_gkey) {
		for (int i = 0; i < 10; i++)
			key_map[i] = legacy_gkey[i];
	} else {
		// GB order: Right,Left,Up,Down,A,B,Select,Start.
		static const int gb_to_shared[8] = {4, 5, 6, 7, 0, 1, 2, 3};
		for (int i = 0; i < 8; i++)
			key_map[gb_to_shared[i]] = legacy_key[i];
	}
	// Adopt any profiles the file carried. Default is persisted as prof1_*, so
	// when a profile section exists it must be taken verbatim: the live tables
	// read above belong to the ACTIVE profile, not to Default, and reusing
	// them would overwrite Default with another profile's mapping. Only a
	// file with no profile section (pre-profiles builds) seeds Default from
	// the live tables.
	InputProfile def;
	def.name = "Default";
	def.key_map = key_map;
	def.pad_map = pad_map;
	def.pad_map_alt = pad_map_alt;
	profiles.clear();
	profiles.push_back(loaded.empty() ? def : loaded[0]);
	if (loaded.empty())
		profiles[0].name = "Default";
	for (size_t i = 1; i < loaded.size(); ++i) {
		if (loaded[i].name.empty())
			loaded[i].name = "Profile " + std::to_string(static_cast<int>(i) + 1);
		profiles.push_back(loaded[i]);
	}
	if (profileCount() > kMaxProfiles)
		profiles.resize(static_cast<size_t>(kMaxProfiles));
	// Load the active profile DIRECTLY: the live tables read above already
	// belong to it, so going through activateProfile() would first sync them
	// into Default and clobber the profile we just adopted from the file.
	loadProfile(loaded_active >= 0 && loaded_active < profileCount() ? loaded_active : 0);
	return true;
}

// ---- GUIConsole ----

GUIConsole::GUIConsole()
{
	update_layout(640, 576);
}

u32 GUIConsole::color_for_console(ConsoleColor c)
{
	switch (c) {
	case ConsoleColor::Purple:
		return 0xFF6B3FA0;
	case ConsoleColor::Green:
		return 0xFF3FA06B;
	case ConsoleColor::Blue:
		return 0xFF3F6BA0;
	case ConsoleColor::Yellow:
		return 0xFFA0A03F;
	case ConsoleColor::ClearPurple:
		return 0xFF8F6FC0;
	default:
		return 0xFF6B3FA0;
	}
}

void GUIConsole::update_layout(int window_w, int window_h)
{
	window_w_ = window_w;
	window_h_ = window_h;
	// Body aspect 75:133 (W:H =0.5639). Fit centered with letterbox.
	// Matches assets/gbc_body.png exactly (1370x2430 crop of the Evan-Amos
	// public-domain GBC photo), so fractional photo coordinates map 1:1.
	const float body_aspect = 75.0f / 133.0f;
	const float win_aspect = static_cast<float>(window_w) / window_h;
	float body_w, body_h;
	if (win_aspect > body_aspect) {
		body_h = window_h * 0.96f;
		body_w = body_h * body_aspect;
	} else {
		body_w = window_w * 0.96f;
		body_h = body_w / body_aspect;
	}
	float body_x = (window_w - body_w) * 0.5f;
	float body_y = (window_h - body_h) * 0.5f;
	body_rect_ = {body_x, body_y, body_w, body_h};

	// Feature rects as fractions of the body rect, calibrated against the
	// photo asset (see tools notes: LCD glass, D-pad cross, A/B disks,
	// SELECT/START pills measured in asset pixels / 1370 x 2430).
	const auto F = [&](float fx, float fy, float fw, float fh) -> Rect {
		return {body_x + fx * body_w, body_y + fy * body_h, fw * body_w, fh * body_h};
	};
	// Game screen: the photo's LCD glass, measured from the asset pixels
	// (1370x2430). Black-bezel to gray-LCD transitions sit at x 292/1073 and
	// y 244/978 (stable across scanlines), i.e. a 782x735 glass rect.
	// The old rect (330-1036 x 264-902) was inset ~40px left/right and
	// ~20/76px top/bottom, leaving gray photo borders around the game.
	bezel_rect_ = F(0.21314f, 0.10041f, 0.57080f, 0.30247f);
	// Fit the 160x144 (10:9) game image inside the glass preserving aspect.
	// The glass (~1.064) is slightly taller than 10:9 (~1.111), so fit to
	// the width and letterbox top/bottom (~15 asset px each).
	{
		const float gw = bezel_rect_.w, gh = bezel_rect_.h;
		float sw = gw, sh = gw * (144.0f / 160.0f);
		if (sh > gh) {
			sh = gh;
			sw = gh * (160.0f / 144.0f);
		}
		screen_rect_ = {bezel_rect_.x + (gw - sw) * 0.5f, bezel_rect_.y + (gh - sh) * 0.5f, sw, sh};
	}
	// Fit the 240x160 (3:2) GBA image inside the same glass preserving
	// aspect (gba::GbaPpu::kWidth/kHeight). Drawing GBA frames into the
	// GB-shaped screen_rect_ above would stretch them horizontally.
	{
		const float gw = bezel_rect_.w, gh = bezel_rect_.h;
		float sw = gw, sh = gw * (160.0f / 240.0f);
		if (sh > gh) {
			sh = gh;
			sw = gh * (240.0f / 160.0f);
		}
		gba_screen_rect_ = {bezel_rect_.x + (gw - sw) * 0.5f, bezel_rect_.y + (gh - sh) * 0.5f,
							sw, sh};
	}
	// D-pad enclosing area (cross + well): x 130-465, y 1440-1800.
	dpad_rect_ = F(0.09489f, 0.59259f, 0.24453f, 0.14815f);
	// Screen bezel corners: OPEN top-left of the glass, gear top-right.
	// Both sit on the near-black bezel beside the glass (grey circle +
	// white glyph read well there). Hit-testing uses these same rects.
	settings_rect_ = {body_x + body_w * 0.797f, body_y + body_h * 0.100f, body_w * 0.080f,
		body_h * 0.046f};
	// OPEN button (file dialog): top-left bezel corner, mirroring the gear.
	open_rect_ = {body_x + body_w * 0.123f, body_y + body_h * 0.100f, body_w * 0.080f,
		body_h * 0.046f};

	rebuild_buttons();
	rebuild_settings_cells();
}

void GUIConsole::rebuild_buttons()
{
	buttons_.clear();
	const float bx = body_rect_.x, by = body_rect_.y;
	const float bw = body_rect_.w, bh = body_rect_.h;
	const auto add = [&](float fx, float fy, float fw, float fh, const char *name, Joypad::Key key,
						 bool circle) {
		buttons_.push_back({bx + fx * bw, by + fy * bh, fw * bw, fh * bh, name, key, circle});
	};
	// D-pad cross arms, re-measured from the asset pixels: vertical bar
	// x 244-365, horizontal bar y 1555-1694 x 130-460, cross center
	// ~(307, 1625). Arms stop 10px short of the center so the dead zone is
	// a tiny 10x10 square.
	add(0.17810f, 0.59259f, 0.08832f, 0.07202f, "Up", Joypad::Key::Up, false);
	add(0.17810f, 0.66872f, 0.08832f, 0.07202f, "Down", Joypad::Key::Down, false);
	add(0.09854f, 0.64000f, 0.11460f, 0.05700f, "Left", Joypad::Key::Left, false);
	add(0.22044f, 0.64000f, 0.11533f, 0.05700f, "Right", Joypad::Key::Right, false);
	// A/B disks (circles; hit_test does a radius check). Measured dark
	// extents: A x 1092-1236 y 1500-1652 (center ~1164, 1576, r ~76),
	// B x 848-992 y 1576-1732 (center ~920, 1654, r ~75).
	add(0.79416f, 0.61687f, 0.11095f, 0.06337f, "A", Joypad::Key::A, true);
	add(0.61971f, 0.64856f, 0.10511f, 0.06420f, "B", Joypad::Key::B, true);
	// SELECT/START pills: measured extents x 495-655 / 685-845,
	// y 1980-2040 (centers y ~2010).
	add(0.36131f, 0.81481f, 0.11679f, 0.02469f, "Select", Joypad::Key::Select, false);
	add(0.50000f, 0.81481f, 0.11679f, 0.02469f, "Start", Joypad::Key::Start, false);
	// GBA shoulders: the GBC photo has no L/R controls, so they live as
	// overlay pills on the body's top corners, like real shoulder buttons.
	// That corner plastic is light lavender, so the pills use a bright fill
	// with white labels to stay readable. Clicking a pill presses it
	// in-game; remapping happens through the settings Inputs tab (GBA
	// rows), same click-to-remap flow as the rest.
	shoulders_.clear();
	shoulders_.push_back({{bx + 0.020f * bw, by + 0.012f * bh, 0.130f * bw, 0.055f * bh},
			"L",
			gba::GbaKeypad::Key::L});
	shoulders_.push_back({{bx + 0.850f * bw, by + 0.012f * bh, 0.130f * bw, 0.055f * bh},
			"R",
			gba::GbaKeypad::Key::R});
}

std::optional<Joypad::Key> GUIConsole::hit_test(int x, int y) const
{
	for (const auto &b : buttons_) {
		if (b.w <= 0)
			continue;
		if (x >= b.x && x < b.x + b.w && y >= b.y && y < b.y + b.h) {
			// For circles, check the normalized radius (elliptical: the rect
			// can be a few px taller than wide once mapped to the window).
			if (b.is_circle) {
				float cx = b.x + b.w * 0.5f;
				float cy = b.y + b.h * 0.5f;
				float dx = (x - cx) / (b.w * 0.5f);
				float dy = (y - cy) / (b.h * 0.5f);
				if (dx * dx + dy * dy > 1.0f)
					continue;
			}
			return b.key;
		}
	}
	return std::nullopt;
}

std::optional<gba::GbaKeypad::Key> GUIConsole::hit_shoulder(int x, int y) const
{
	for (const auto &s : shoulders_) {
		if (s.r.w <= 0)
			continue;
		if (x >= s.r.x && x < s.r.x + s.r.w && y >= s.r.y && y < s.r.y + s.r.h)
			return s.key;
	}
	return std::nullopt;
}

bool GUIConsole::hit_settings(int x, int y) const
{
	return x >= settings_rect_.x && x < settings_rect_.x + settings_rect_.w &&
		   y >= settings_rect_.y && y < settings_rect_.y + settings_rect_.h;
}

bool GUIConsole::hit_open(int x, int y) const
{
	return x >= open_rect_.x && x < open_rect_.x + open_rect_.w && y >= open_rect_.y &&
		   y < open_rect_.y + open_rect_.h;
}

void GUIConsole::start_binding(PadButton b)
{
	awaiting_input_ = true;
	pending_pad_ = b;
}

// Accepts a key OR a controller button/axis: the button being remapped gets
// whatever the user pressed, and the other source is cleared.
bool GUIConsole::consume_binding(const Binding &b)
{
	if (!awaiting_input_ || !settings_ || !b.bound())
		return false;
	settings_->setBinding(static_cast<int>(pending_pad_), b);
	awaiting_input_ = false;
	settings_->save("");
	return true;
}

void GUIConsole::cancel_mapping()
{
	awaiting_input_ = false;
}

namespace
{

std::string trunc_str(const std::string &s, size_t max_len)
{
	if (s.size() <= max_len)
		return s;
	if (max_len <= 3)
		return s.substr(0, max_len);
	return "..." + s.substr(s.size() - (max_len - 3));
}

const char *palette_name(int p)
{
	switch (p) {
	case 0:
		return "Original";
	case 1:
		return "Pocket";
	case 2:
		return "Light";
	case 3:
		return "Blue";
	case 4:
		return "Custom";
	default:
		return "Original";
	}
}

} // namespace

// ---- Settings page ----

void GUIConsole::refreshControllers(const InputManager &input)
{
	if (controller_status_ == "mapped" || controller_status_ == "failed" ||
		controller_status_ == "defaults")
		return; // keep the result visible until the next auto-map attempt
	const int n = input.controllerCount();
	controller_status_ = (n == 0) ? "none" : (n == 1 ? "1 pad" : std::to_string(n) + " pads");
}

bool GUIConsole::applyAutoMap(InputManager &input)
{
	if (input.controllerCount() == 0) {
		controller_status_ = "none";
		return false;
	}
	if (!input.autoMap()) {
		controller_status_ = "failed";
		return false;
	}
	// autoMap fills the controller tables; mirror them into the settings so
	// the live mapping, the profile and the input manager all agree.
	for (unsigned i = 0; i < kPadButtonCount; ++i) {
		settings_->pad_map[i] = input.padBindings()[i];
		settings_->pad_map_alt[i] = input.padBinding2(static_cast<PadButton>(i));
		// A controller layout replaces the keyboard one: one source per button.
		settings_->key_map[i] = SDL_SCANCODE_UNKNOWN;
	}
	settings_->save("");
	controller_status_ = "mapped";
	return true;
}

bool GUIConsole::applyKeyboardDefaults()
{
	if (settings_ == nullptr)
		return false;
	settings_->applyDefaultKeyMap();
	settings_->save("");
	controller_status_ = "defaults";
	return true;
}

void GUIConsole::start_text_entry(TextPurpose p, const std::string &prompt,
								  const std::string &initial, int target)
{
	text_active_ = true;
	text_purpose_ = p;
	text_target_ = target;
	text_prompt_ = prompt;
	text_text_ = initial.substr(0, kMaxTextLen);
}

void GUIConsole::text_input(const std::string &utf8)
{
	if (!text_active_)
		return;
	// The overlay font is ASCII-only, so drop everything else (including
	// multi-byte UTF-8) rather than drawing replacement glyphs.
	for (const unsigned char ch : utf8) {
		if (text_text_.size() >= kMaxTextLen)
			break;
		if (ch >= 0x20 && ch < 0x7F)
			text_text_.push_back(static_cast<char>(ch));
	}
}

void GUIConsole::text_backspace()
{
	if (text_active_ && !text_text_.empty())
		text_text_.pop_back();
}

bool GUIConsole::text_accept()
{
	if (!text_active_)
		return false;
	text_active_ = false;
	return !text_text_.empty();
}

void GUIConsole::text_cancel()
{
	text_active_ = false;
	text_text_.clear();
}

std::vector<GUIConsole::SettingsRow> GUIConsole::settings_rows() const
{
	std::vector<SettingsRow> rows;
	if (!settings_)
		return rows;
	if (settings_tab_ == SettingsTab::General) {
		static const char *ids[kGeneralRows] = {"folder", "volume", "palette",
		                                       "hw",     "filter",  "lcdgrid"};
		static const char *labels[kGeneralRows] = {
			"ROM Folder", "Volume", "DMG Palette", "Hardware Mode", "Filter", "LCD Grid"};
		for (int i = 0; i < kGeneralRows; i++) {
			SettingsRow r;
			r.id = ids[i];
			r.label = labels[i];
			r.label_rect = settings_general_labels_[i];
			r.value_rect = settings_general_cells_[i];
			r.style = RowStyle::Value;
			switch (i) {
			case 0:
				r.value = trunc_str(settings_->rom_folder, 20);
				break;
			case 1:
				r.value = std::to_string(static_cast<int>(settings_->volume * 100)) + "%";
				break;
			case 2:
				r.value = palette_name(settings_->palette);
				break;
			case 3:
				r.value = settings_->hardware_mode;
				break;
			case 4:
				r.value = video_filter_name(static_cast<VideoFilter>(settings_->video_filter));
				break;
			case 5: {
				static const char *kGridNames[Settings::kLcdGridMax + 1] = {
					"Off", "Light", "Medium", "Strong"};
				r.value = kGridNames[settings_->lcd_grid < 0
									 ? 0
									 : (settings_->lcd_grid > Settings::kLcdGridMax
											? Settings::kLcdGridMax
											: settings_->lcd_grid)];
				break;
			}
			default:
				break;
			}
			rows.push_back(r);
		}
		return rows;
	}

	// The Inputs page is NOT a row list: it is a GBA diagram plus a handful of
	// widgets, all drawn by the renderer from their own rects. Returning no
	// rows here stops the generic row loop from drawing anything.
	return rows;
}

GUIConsole::SettingsHit GUIConsole::settings_hit_test(int x, int y) const
{
	SettingsHit hit;
	if (!show_settings_ || !settings_)
		return hit;
	const auto in = [](const Rect &r, int px, int py) {
		return px >= r.x && px < r.x + r.w && py >= r.y && py < r.y + r.h;
	};
	if (in(settings_close_, x, y)) {
		hit.action = SettingsAction::Close;
		return hit;
	}
	if (in(settings_tab_general_, x, y)) {
		hit.action = SettingsAction::TabGeneral;
		return hit;
	}
	if (in(settings_tab_inputs_, x, y)) {
		hit.action = SettingsAction::TabInputs;
		return hit;
	}
	if (settings_tab_ == SettingsTab::General) {
		static const SettingsAction acts[kGeneralRows] = {
			SettingsAction::RomFolder, SettingsAction::Volume,  SettingsAction::Palette,
			SettingsAction::HwMode,    SettingsAction::Filter,  SettingsAction::LcdGrid};
		for (int i = 0; i < kGeneralRows; i++) {
			if (!in(settings_general_labels_[i], x, y) &&
				!in(settings_general_cells_[i], x, y))
				continue;
			hit.action = acts[i];
			if (acts[i] == SettingsAction::Volume) {
				const Rect &v = settings_volume_rect();
				hit.ratio = v.w > 0.0f ? static_cast<float>((x - v.x) / v.w) : 0.0f;
				hit.ratio = std::clamp(hit.ratio, 0.0f, 1.0f);
			}
			return hit;
		}
		return hit;
	}
	// Inputs page: dropdowns first (an open list overlays everything else),
	// then Load/Save/Auto-map, then the GBA diagram.
	if (profile_dd_.open) {
		if (in(profile_dd_.closed, x, y)) {
			hit.action = SettingsAction::ProfileBox;
			return hit;
		}
		for (size_t i = 0; i < profile_dd_.items.size(); i++) {
			if (in(profile_dd_.items[i], x, y)) {
				hit.action = SettingsAction::ProfileItem;
				hit.index = static_cast<int>(i);
				return hit;
			}
		}
		// Clicking anywhere else falls through: the dispatcher sees a
		// non-dropdown action and closes the list.
		return hit;
	}
	if (device_dd_.open) {
		if (in(device_dd_.closed, x, y)) {
			hit.action = SettingsAction::DeviceBox;
			return hit;
		}
		for (size_t i = 0; i < device_dd_.items.size(); i++) {
			if (in(device_dd_.items[i], x, y)) {
				hit.action = SettingsAction::DeviceItem;
				hit.index = static_cast<int>(i);
				return hit;
			}
		}
		return hit;
	}
	if (in(profile_dd_.closed, x, y) || in(gba_pad_.label_profile, x, y)) {
		hit.action = SettingsAction::ProfileBox;
		return hit;
	}
	if (in(device_dd_.closed, x, y) || in(gba_pad_.label_device, x, y)) {
		hit.action = SettingsAction::DeviceBox;
		return hit;
	}
	if (in(btn_load_, x, y)) {
		hit.action = SettingsAction::ProfileLoad;
		return hit;
	}
	if (in(btn_save_, x, y)) {
		hit.action = SettingsAction::ProfileSave;
		return hit;
	}
	if (automapVisible() && in(btn_automap_, x, y)) {
		hit.action = SettingsAction::AutoMap;
		return hit;
	}
	if (auto b = hit_gba_pad(x, y)) {
		hit.action = SettingsAction::PadButton;
		hit.index = static_cast<int>(*b);
		return hit;
	}
	return hit;
}

std::string GUIConsole::deviceName() const
{
	if (device_dd_.selected < 0 ||
		static_cast<size_t>(device_dd_.selected) >= device_dd_.options.size())
		return "Keyboard";
	return device_dd_.options[static_cast<size_t>(device_dd_.selected)];
}

int GUIConsole::profileMenuSize() const
{
	// options ALREADY contains the trailing New / Rename / Delete entries, so
	// the menu size is just the option count. Adding the specials again put
	// the command boundary out of reach and the commands silently did nothing.
	return static_cast<int>(profile_dd_.options.size());
}

int GUIConsole::profileCommandIndex() const
{
	// Index of "+ New profile...", i.e. where the profile list ends.
	return static_cast<int>(profile_dd_.options.size()) - kProfileMenuSpecials;
}

bool GUIConsole::profileMenuEnabled(int i) const
{
	const int nprof = profileCommandIndex();
	if (i < nprof || i == nprof)
		return true; // any profile, or "New..."
	return settings_ != nullptr && settings_->active_profile > 0;
}

void GUIConsole::setDeviceOptions(const std::vector<std::string> &names, int selected)
{
	// Entry 0 is always the keyboard, then one entry per connected pad.
	device_dd_.options.clear();
	device_dd_.options.push_back("Keyboard");
	for (const std::string &n : names)
		device_dd_.options.push_back(n);
	// Locked to the keyboard when no pad is present, so it is obvious why the
	// control will not open rather than silently ignoring clicks.
	device_dd_.enabled = !names.empty();
	if (selected < 0 || selected > static_cast<int>(device_dd_.options.size()))
		selected = 0;
	if (selected > 0 && !device_dd_.enabled)
		selected = 0;
	device_dd_.selected = selected;
	relayout_dropdown(device_dd_);
}

void GUIConsole::relayout_dropdown(Dropdown &dd)
{
	dd.items.clear();
	if (dd.options.empty())
		return;
	const float item_h = dd.closed.h;
	const float gap = 2.0f;
	const float total = static_cast<float>(dd.options.size()) * item_h;
	const float limit = settings_panel_.y + settings_panel_.h - 2.0f;
	float top = dd.closed.y + dd.closed.h;
	// Flip above the box when opening downward would leave the panel.
	if (top + total > limit && dd.closed.y - total > settings_panel_.y)
		top = dd.closed.y - total;
	dd.flipped = top < dd.closed.y;
	for (size_t i = 0; i < dd.options.size(); i++) {
		dd.items.push_back({dd.closed.x, top + static_cast<float>(i) * item_h, dd.closed.w,
							item_h});
	}
	(void) gap;
}

std::optional<PadButton> GUIConsole::hit_gba_pad(int x, int y) const
{
	if (settings_tab_ != SettingsTab::Inputs || !show_settings_)
		return std::nullopt;
	const auto in = [&](const Rect &r) {
		return x >= r.x && x < r.x + r.w && y >= r.y && y < r.y + r.h;
	};
	// Arms overlap slightly; test the small inner buttons first so a click
	// near the edge of a face button does not fall through to the body.
	if (in(gba_pad_.btn_a))
		return PadButton::A;
	if (in(gba_pad_.btn_b))
		return PadButton::B;
	if (in(gba_pad_.btn_start))
		return PadButton::Start;
	if (in(gba_pad_.btn_select))
		return PadButton::Select;
	if (in(gba_pad_.btn_l))
		return PadButton::L;
	if (in(gba_pad_.btn_r))
		return PadButton::R;
	// The D-pad cross resolves to whichever arm the point is in.
	{
		const Rect &h = gba_pad_.dpad_h;
		const Rect &v = gba_pad_.dpad_v;
		// Centre of a rect is x + w/2. Writing (x + w) / 2 yields the RIGHT
		// EDGE, which shifted the whole hit region off the cross — that is why
		// clicking the D-pad did nothing.
		const float ccx = h.x + h.w * 0.5f;
		const float ccy = v.y + v.h * 0.5f;
		const float half_h = h.w * 0.5f;
		const float half_v = v.h * 0.5f;
		if (x >= ccx - half_h && x <= ccx + half_h && y >= ccy - half_v && y <= ccy + half_v) {
			const float dx = (x - ccx) / half_h;
			const float dy = (y - ccy) / half_v;
			// Choose the dominant axis so the diagonal wedges still resolve.
			if (std::fabs(dy) >= std::fabs(dx))
				return dy < 0.0f ? PadButton::Up : PadButton::Down;
			return dx < 0.0f ? PadButton::Left : PadButton::Right;
		}
	}
	return std::nullopt;
}

void GUIConsole::set_settings_tab(SettingsTab t)
{
	if (settings_tab_ == t)
		return;
	settings_tab_ = t;
	// The two tabs carry different row counts, so the cells must be laid out
	// again for the new tab.
	rebuild_settings_cells();
}

void GUIConsole::rebuild_settings_cells()
{
	settings_general_cells_.clear();
	settings_inputs_cells_.clear();
	settings_general_labels_.clear();
	settings_inputs_labels_.clear();
	const float px = body_rect_.x, py = body_rect_.y;
	const float bw = body_rect_.w, bh = body_rect_.h;
	if (bw <= 0 || bh <= 0)
		return;
	// The panel is anchored BELOW the LCD glass rather than centred on the
	// body: the point of the Filter / LCD Grid rows is to watch the effect on
	// the running game, so the screen must stay uncovered.
	const float glass_bottom = bezel_rect_.y + bezel_rect_.h;
	const float band_top = std::max(py + bh * 0.42f, glass_bottom + bh * 0.015f);
	const float band_h = std::max(bh * 0.30f, (py + bh) - band_top);
	const float panel_w = bw * 0.85f;
	const float want_h = bh * (settings_tab_ == SettingsTab::General ? 0.46f : 0.54f);
	// Never taller than the band, never taller than the body.
	const float panel_h = std::min(std::min(want_h, band_h), bh);
	const float panel_x = px + (bw - panel_w) * 0.5f;
	const float panel_y = band_top + (band_h - panel_h) * 0.5f;
	settings_panel_ = {panel_x, panel_y, panel_w, panel_h};

	// Close box, top-right of the title bar.
	const float cs = std::min(bh * 0.032f, 18.0f);
	settings_close_ = {panel_x + panel_w - 8.0f - cs, panel_y + 7.0f, cs, cs};

	// Tabs under the title bar. Two tabs, split evenly with a small gutter.
	const float tabs_y = panel_y + bh * 0.075f;
	const float tabs_h = bh * 0.042f;
	const float tab_gap = 3.0f;
	const float tab_w = (panel_w - 12.0f - tab_gap) * 0.5f;
	settings_tab_general_ = {panel_x + 6.0f, tabs_y, tab_w, tabs_h};
	settings_tab_inputs_ = {panel_x + 9.0f + tab_w, tabs_y, tab_w, tabs_h};

	// Row geometry, sized to the active tab's row count.
	const float row_gap = 4.0f;
	const float rows_top = tabs_y + tabs_h + 8.0f;
	const float rows_avail = (panel_y + panel_h - 22.0f) - rows_top; // footer inset
	// Keep rows legible: if a row would shrink under the font height, reclaim
	// the inter-row gap first and only then accept a shorter row.
	constexpr float kMinRowH = 9.0f;
	const auto fit_rows = [&](int rows, float &out_h, float &out_gap) {
		out_h = std::min(bh * 0.045f, (rows_avail - (rows - 1) * row_gap) / rows);
		out_gap = row_gap;
		if (out_h < kMinRowH && rows > 1) {
			out_h = std::min(kMinRowH, rows_avail / static_cast<float>(rows));
			out_gap = std::max(
				0.0f, (rows_avail - out_h * static_cast<float>(rows)) /
						  static_cast<float>(rows - 1));
		}
	};
	const float label_x = panel_x + 6.0f, label_w = panel_w * 0.44f;
	const float value_x = panel_x + panel_w * 0.52f, value_w = panel_w * 0.42f;

	float gen_h, gen_gap;
	fit_rows(kGeneralRows, gen_h, gen_gap);
	float row_y = rows_top;
	for (int i = 0; i < kGeneralRows; i++) {
		settings_general_labels_.push_back({label_x, row_y, label_w, gen_h});
		settings_general_cells_.push_back({value_x, row_y, value_w, gen_h});
		row_y += gen_h + gen_gap;
	}
	// The Inputs page does NOT use the row list: it is a diagram plus a few
	// widgets, laid out explicitly below.
	profile_dd_.items.clear();
	device_dd_.items.clear();
	settings_inputs_labels_.clear();
	settings_inputs_cells_.clear();

	const float in_x = panel_x + 6.0f;
	const float in_w = panel_w - 12.0f;
	// Row rhythm for the widget block at the top.
	const float wr_h = std::max(14.0f, std::min(bh * 0.032f, 22.0f));
	const float wr_gap = 4.0f;
	const float lbl_w = std::max(34.0f, in_w * 0.24f);
	// Profile row: [label][dropdown .......] [Load] [Save]
	const float btn_w = std::max(30.0f, in_w * 0.17f);
	float wy = rows_top;
	gba_pad_.label_profile = {in_x, wy, lbl_w, wr_h};
	profile_dd_.closed = {in_x + lbl_w, wy, in_w - lbl_w - btn_w * 2 - wr_gap * 2, wr_h};
	btn_load_ = {in_x + in_w - btn_w * 2 - wr_gap, wy, btn_w, wr_h};
	btn_save_ = {in_x + in_w - btn_w, wy, btn_w, wr_h};
	wy += wr_h + wr_gap;
	// Device row: [label][dropdown .......]
	gba_pad_.label_device = {in_x, wy, lbl_w, wr_h};
	device_dd_.closed = {in_x + lbl_w, wy, in_w - lbl_w, wr_h};
	wy += wr_h + wr_gap;

	// Profile menu: every profile, then New / Rename / Delete.
	profile_dd_.options.clear();
	if (settings_ != nullptr) {
		for (int i = 0; i < settings_->profileCount(); i++)
			profile_dd_.options.push_back(settings_->profileName(i));
	}
	profile_dd_.options.push_back("+ New profile...");
	profile_dd_.options.push_back("Rename this...");
	profile_dd_.options.push_back("Delete this");
	if (settings_ != nullptr)
		profile_dd_.selected = settings_->active_profile;
	relayout_dropdown(profile_dd_);
	relayout_dropdown(device_dd_);

	// Auto-map button, only when a controller is the selected source.
	if (device_dd_.selected > 0) {
		btn_automap_ = {in_x + in_w * 0.5f - in_w * 0.22f, wy, in_w * 0.44f, wr_h + 2.0f};
		wy += wr_h + 2.0f + wr_gap;
	} else {
		btn_automap_ = {};
	}

	// GBA outline fills the rest of the panel, centred. Laid out in real
	// millimetres so the proportions are verifiable: the AGB-001 is
	// 144.5 x 82 mm with a 61.2 x 40.8 mm screen (Nintendo / GBATEK), and the
	// control positions follow GBATEK's layout sketch.
	constexpr f32 kBodyWmm = 144.5f;
	constexpr f32 kBodyHmm = 82.0f;
	const float pad_top = wy + 2.0f;
	const float pad_bot = panel_y + panel_h - 20.0f;
	const float avail_h = std::max(40.0f, pad_bot - pad_top);
	float body_h = avail_h;
	float body_w = body_h * (kBodyWmm / kBodyHmm);
	const float max_w = in_w;
	if (body_w > max_w) {
		body_w = max_w;
		body_h = body_w * (kBodyHmm / kBodyWmm);
	}
	const float bx = in_x + (in_w - body_w) * 0.5f;
	const float by = pad_top + (avail_h - body_h) * 0.5f;
	gba_pad_.body = {bx, by, body_w, body_h};

	// Millimetre -> body rect, so every number below is a real dimension.
	const auto MM = [&](float x, float y, float w, float h) {
		return Rect{bx + x / kBodyWmm * body_w, by + y / kBodyHmm * body_h, w / kBodyWmm * body_w,
					h / kBodyHmm * body_h};
	};
	const auto MMPt = [&](float x, float y) {
		return Vec2{bx + x / kBodyWmm * body_w, by + y / kBodyHmm * body_h};
	};

	gba_pad_.screen = MM(30.0f, 12.0f, 61.2f, 40.8f);
	// D-pad: 15 mm across, centred at (20, 50) mm on the far left.
	{
		const auto c = MMPt(20.0f, 50.0f);
		const float rx = 7.5f / kBodyWmm * body_w;
		const float ry = 7.5f / kBodyHmm * body_h;
		const float tx = 2.4f / kBodyWmm * body_w;
		const float ty = 2.4f / kBodyHmm * body_h;
		gba_pad_.dpad_h = {c.x - rx, c.y - ty, rx * 2.0f, ty * 2.0f};
		gba_pad_.dpad_v = {c.x - tx, c.y - ry, tx * 2.0f, ry * 2.0f};
	}
	// Face buttons: A upper-right of B, both right of the screen.
	gba_pad_.btn_a = MM(103.0f, 27.0f, 12.0f, 12.0f);
	gba_pad_.btn_b = MM(89.0f, 38.0f, 12.0f, 12.0f);
	// START / SELECT sit below the screen (SELECT left, START right).
	gba_pad_.btn_select = MM(44.0f, 58.0f, 14.0f, 5.0f);
	gba_pad_.btn_start = MM(61.0f, 58.0f, 14.0f, 5.0f);
	// Shoulders are tabs on the top edge.
	gba_pad_.btn_l = MM(14.0f, 0.0f, 22.0f, 9.0f);
	gba_pad_.btn_r = MM(108.0f, 0.0f, 22.0f, 9.0f);
}

GUIConsole::Rect GUIConsole::settings_volume_rect() const
{
	if (settings_general_cells_.size() < 2)
		return {0, 0, 0, 0};
	return settings_general_cells_[1];
}

// ---- ROM menu ----

void GUIConsole::set_rom_list(std::vector<RomEntry> entries)
{
	rom_list_ = std::move(entries);
	if (rom_list_.empty()) {
		menu_selected_ = 0;
	} else {
		menu_selected_ = std::clamp(menu_selected_, 0, static_cast<int>(rom_list_.size()) - 1);
	}
}

void GUIConsole::set_menu_selected(int i)
{
	if (rom_list_.empty()) {
		menu_selected_ = 0;
		return;
	}
	menu_selected_ = std::clamp(i, 0, static_cast<int>(rom_list_.size()) - 1);
}

void GUIConsole::menu_move(int delta)
{
	if (rom_list_.empty()) {
		menu_selected_ = 0;
		return;
	}
	set_menu_selected(menu_selected_ + delta);
}

GUIConsole::Rect GUIConsole::menu_list_rect() const
{
	const Rect &s = screen_rect_;
	const float pad = s.w * 0.045f;
	const float title_h = std::max(20.0f, s.h * 0.10f);
	const float footer_h = std::max(26.0f, s.h * 0.14f);
	return {s.x + pad, s.y + pad + title_h, s.w - pad * 2.0f,
			s.h - pad * 2.0f - title_h - footer_h};
}

// Grid layout constants

int GUIConsole::menu_grid_cols() const
{
	const Rect list = menu_list_rect();
	if (list.w <= 0)
		return 1;
	// Aim for ~110px min cell width on the base window; clamp 1..3 cols.
	int cols = static_cast<int>(list.w / 110.0f);
	cols = std::clamp(cols, 1, 3);
	// Avoid a third column that's cramped: require at least 105px per cell.
	if (cols == 3 && list.w / 3.0f < 105.0f)
		cols = 2;
	return cols;
}

float GUIConsole::menu_cell_w() const
{
	const Rect list = menu_list_rect();
	const int cols = menu_grid_cols();
	if (cols <= 0)
		return list.w;
	const float gap = 8.0f;
	return (list.w - gap * (cols - 1)) / cols;
}

float GUIConsole::menu_cell_h() const
{
	// Tall enough for a 32px cart + 2 text lines + badge/padding.
	// Compact so even a small GB screen shows 2 rows (≈4-6 items).
	const float cw = menu_cell_w();
	const float ch = std::clamp(cw * 0.42f + 22.0f, 64.0f, 78.0f);
	// Never taller than the list itself: on tiny windows the 64px minimum
	// would spill cells over the footer hints (garbled overlap). Hit-testing
	// uses these same metrics, so it stays consistent.
	const float list_h = menu_list_rect().h;
	if (list_h > 0)
		return std::min(ch, list_h);
	return ch;
}

int GUIConsole::menu_grid_rows() const
{
	const int n = static_cast<int>(rom_list_.size());
	const int cols = menu_grid_cols();
	if (cols <= 0)
		return 0;
	return (n + cols - 1) / cols;
}

int GUIConsole::menu_grid_visible_rows() const
{
	const Rect list = menu_list_rect();
	const float ch = menu_cell_h();
	if (ch <= 0 || list.h <= 0)
		return 1;
	return std::max(1, static_cast<int>(list.h / ch));
}

int GUIConsole::menu_grid_visible_count() const
{
	return menu_grid_cols() * menu_grid_visible_rows();
}

bool GUIConsole::hit_screen(int x, int y) const
{
	return hit_screen(x, y, screen_rect_);
}

bool GUIConsole::hit_screen(int x, int y, const Rect &r) const
{
	const Rect &s = r;
	return x >= s.x && x < s.x + s.w && y >= s.y && y < s.y + s.h;
}

GUIConsole::Rect GUIConsole::menu_cell_rect(int index) const
{
	const Rect list = menu_list_rect();
	const int cols = menu_grid_cols();
	const float cw = menu_cell_w();
	const float ch = menu_cell_h();
	const float gap = 8.0f;
	const int first = menu_first_visible();
	const int local = index - first;
	if (local < 0)
		return {0, 0, 0, 0};
	const int col = local % cols;
	const int row = local / cols;
	return {list.x + col * (cw + gap), list.y + row * ch, cw, ch};
}

float GUIConsole::menu_row_h() const
{
	// Keep for tests/back-compat — now just the grid cell height.
	return menu_cell_h();
}

int GUIConsole::menu_visible_rows() const
{
	return menu_grid_visible_rows();
}

int GUIConsole::menu_first_visible() const
{
	const int n = static_cast<int>(rom_list_.size());
	const int cols = menu_grid_cols();
	if (n <= 0 || cols <= 0)
		return 0;
	const int rows = menu_grid_rows();
	const int vis_rows = menu_grid_visible_rows();
	const int sel_row = menu_selected_ / cols;
	int first_row = std::clamp(sel_row - vis_rows + 1, 0, std::max(0, rows - vis_rows));
	if (sel_row < first_row)
		first_row = sel_row;
	return first_row * cols;
}

std::optional<int> GUIConsole::menu_hit_test(int x, int y) const
{
	if (!menu_active_ || rom_list_.empty())
		return std::nullopt;
	const Rect list = menu_list_rect();
	if (x < list.x || x >= list.x + list.w || y < list.y || y >= list.y + list.h) {
		return std::nullopt;
	}
	const int cols = menu_grid_cols();
	const float cw = menu_cell_w();
	const float ch = menu_cell_h();
	const float gap = 8.0f;
	const int col = static_cast<int>((x - list.x) / (cw + gap));
	const int row = static_cast<int>((y - list.y) / ch);
	if (col < 0 || col >= cols || row < 0)
		return std::nullopt;
	// Gap hit: to the right of a cell but before next column's start.
	const float cell_x = list.x + col * (cw + gap);
	if (x >= cell_x + cw)
		return std::nullopt;
	const int idx = menu_first_visible() + row * cols + col;
	if (idx < 0 || idx >= static_cast<int>(rom_list_.size()))
		return std::nullopt;
	// Last row may be partial — e.g. cols=3, n=7, first=0, idx=7 is second
	// column of row 2 but that row only has 1 item. Treat remainder as empty.
	const int row_start = (idx / cols) * cols;
	const int row_items = std::min(cols, static_cast<int>(rom_list_.size()) - row_start);
	if (col >= row_items)
		return std::nullopt;
	return idx;
}

} // namespace gb
