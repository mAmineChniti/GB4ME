#include "gb/gui_console.h"
#include <SDL3/SDL.h>
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
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
	// A per-game overlay is live, so the in-memory values are partly the
	// game's. Write the file with the overlay lifted, then put it back -
	// otherwise a save from inside a game (load_rom() does one for the
	// recent-ROMs list) would quietly promote that game's palette/scale to
	// the global default for every other game.
	const bool had_override = game.active;
	clear_game_override();
	std::string p = path.empty() ? default_settings_path() : path;
	// Ensure directory exists
	size_t slash = p.find_last_of('/');
	if (slash != std::string::npos) {
		std::string dir = p.substr(0, slash);
		SDL_CreateDirectory(dir.c_str());
	}
	std::ofstream f(p);
	if (!f) {
		// Failed write: the game must keep its own values, not fall back to
		// the globals it was only borrowing.
		if (had_override)
			load_game_override(game.rom);
		return false;
	}
	// Schema header. A comment line keeps the file self-describing, and the
	// machine-readable `schema=` line is what load() reads.
	f << "# GB4ME settings (key=value, one per line)\n";
	f << "schema=" << kSchemaVersion << "\n";
	f << "rom_folder=" << rom_folder << "\n";
	f << "volume=" << volume << "\n";
	f << "video_scale=" << video_scale << "\n";
	f << "video_filter=" << video_filter << "\n";
	f << "lcd_grid=" << lcd_grid << "\n";
	f << "ghosting=" << ghosting << "\n";
	f << "palette=" << palette << "\n";
	f << "hardware_mode=" << hardware_mode << "\n";
	f << "console_color=" << static_cast<int>(console_color) << "\n";
	f << "body_skin=" << body_skin << "\n";
	f << "theme=" << theme << "\n";
	for (size_t i = 0; i < recent_roms.size() && i < Settings::kMaxRecent; i++)
		f << "recent" << i << "=" << recent_roms[i] << "\n";
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
	// Put the game's values back on top, so the running game keeps them.
	if (had_override)
		load_game_override(game.rom);
	return true;
}

void Settings::push_recent(const std::string &path)
{
	if (path.empty())
		return;
	auto it = std::find(recent_roms.begin(), recent_roms.end(), path);
	if (it != recent_roms.end())
		recent_roms.erase(it);
	recent_roms.insert(recent_roms.begin(), path);
	if (recent_roms.size() > kMaxRecent)
		recent_roms.resize(kMaxRecent);
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
	// Schema: an absent `schema=` line means the original v1 file. A file
	// from a NEWER build still loads best-effort (every key is optional and
	// unknown ones are only reported), so a downgrade keeps the settings it
	// does understand.
	int schema = 1;
	unknown_keys_.clear();
	present_keys_.clear();
	// Recents are staged by rank and compacted after the loop, so sparse or
	// out-of-order recentN lines still load most-recent-first.
	std::array<std::string, Settings::kMaxRecent> staged_recent{};
	std::string line;
	while (std::getline(f, line)) {
		// '#' and ';' start a comment (the schema header uses one), and a
		// line without '=' is not a setting. Skipping both keeps comment
		// text out of the unknown-key report.
		const size_t first = line.find_first_not_of(" \t\r");
		if (first == std::string::npos || line[first] == '#' || line[first] == ';')
			continue;
		size_t eq = line.find('=');
		if (eq == std::string::npos)
			continue;
		std::string k = line.substr(0, eq);
		std::string v = line.substr(eq + 1);
		present_keys_.push_back(k);
		if (k == "schema") {
			const int parsed = safe_index(v);
			if (parsed > 0)
				schema = parsed;
			continue;
		}
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
		} else if (k == "ghosting") {
			const int v2 = safe_index(v);
			if (v2 >= 0 && v2 <= kGhostMax)
				ghosting = v2;
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
		} else if (k == "body_skin") {
			if (!v.empty())
				body_skin = v; // unknown values fall back at resolve time
		} else if (k == "theme") {
			const int t = safe_index(v);
			if (t == 0 || t == 1)
				theme = t;
		} else if (k.rfind("recent", 0) == 0) {
			// recentN=path, N = recency rank. Staged and compacted below so
			// out-of-order or sparse lines still load most-recent-first.
			const int idx = safe_index(k.substr(6));
			if (idx >= 0 && idx < static_cast<int>(Settings::kMaxRecent) && !v.empty() &&
				v.size() <= 1024)
				staged_recent[static_cast<size_t>(idx)] = v;
		} else if (k.rfind("gkey", 0) == 0) {
			const int idx = safe_index(k.substr(4));
			const int code = safe_index(v);
			if (idx >= 0 && idx < 10 && code >= 0)
				legacy_gkey[idx] = static_cast<SDL_Scancode>(code);
			saw_gkey = true;
		} else if (k.rfind("pad", 0) == 0 || k.rfind("alt", 0) == 0) {
			// padN / altN: packed controller bindings (kind<<16 | code).
			const bool alt = k.rfind("alt", 0) == 0;
			const int idx = safe_index(k.substr(3));
			const int packed = safe_index(v);
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
				const int packed = safe_index(v);
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
		} else if (k.rfind("recent", 0) != 0 && !k.empty()) {
			// A key this build does not know: keep it for the report, but
			// never let it break the load (a file written by a newer build
			// must still apply the settings we do understand).
			unknown_keys_.push_back(k);
		}
	}
	loaded_schema_ = schema;
#ifndef GB4ME_RELEASE
	if (schema > kSchemaVersion) {
		std::printf("settings: file schema %d is newer than this build (%d); unknown "
					"keys kept out of the way\n",
					schema, kSchemaVersion);
	}
	for (const std::string &k : unknown_keys_) {
		std::printf("settings: ignoring unknown key '%s'\n", k.c_str());
	}
#endif
	recent_roms.clear();
	for (const auto &r : staged_recent) {
		if (!r.empty())
			recent_roms.push_back(r);
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

bool Settings::has_key(const std::string &key) const
{
	return std::find(present_keys_.begin(), present_keys_.end(), key) != present_keys_.end();
}

std::string Settings::game_override_path(const std::string &rom_path)
{
	if (rom_path.empty())
		return std::string();
	const size_t dot = rom_path.find_last_of('.');
	const size_t slash = rom_path.find_last_of('/');
	// Only an extension in the final path segment is replaced, so a dot in
	// a directory name ("Mr. B's ROMs/game.gb") is left alone.
	if (dot == std::string::npos || (slash != std::string::npos && dot < slash))
		return rom_path + ".cfg";
	return rom_path.substr(0, dot) + ".cfg";
}

bool Settings::load_game_override(const std::string &rom_path)
{
	// Drop any previous overlay first: the globals restored here are the ones
	// this file will then displace.
	clear_game_override();
	game.rom = rom_path;
	if (rom_path.empty())
		return false;
	// Same parser as the global config - a per-game file is a settings file
	// with a different name, so a key added to Settings::save() works here
	// for free.
	Settings over;
	if (!over.load(game_override_path(rom_path)))
		return false;
	// Only the display keys, and only the ones the file actually has. Input
	// is deliberately not here (see GameOverride).
	const auto take_int = [&over](const char *key, int &live, int from, int &saved, bool &flag) {
		if (!over.has_key(key))
			return;
		saved = live;
		flag = true;
		live = from;
	};
	take_int("video_scale", video_scale, over.video_scale, game.video_scale, game.has_video_scale);
	take_int("video_filter", video_filter, over.video_filter, game.video_filter,
			 game.has_video_filter);
	take_int("lcd_grid", lcd_grid, over.lcd_grid, game.lcd_grid, game.has_lcd_grid);
	take_int("ghosting", ghosting, over.ghosting, game.ghosting, game.has_ghosting);
	take_int("palette", palette, over.palette, game.palette, game.has_palette);
	if (over.has_key("console_color")) {
		game.console_color = static_cast<int>(console_color);
		game.has_console_color = true;
		console_color = over.console_color;
	}
	if (over.has_key("hardware_mode")) {
		game.hardware_mode = hardware_mode;
		game.has_hardware_mode = true;
		hardware_mode = over.hardware_mode;
	}
	if (over.has_key("volume")) {
		game.volume = volume;
		game.has_volume = true;
		volume = std::clamp(over.volume, 0.0f, 1.0f);
	}
	// A file with only unrecognised keys is not an overlay.
	game.active = game.has_volume || game.has_video_scale || game.has_video_filter ||
				  game.has_lcd_grid || game.has_ghosting || game.has_palette ||
				  game.has_console_color || game.has_hardware_mode;
	return game.active;
}

void Settings::clear_game_override()
{
	if (!game.active)
		return;
	// Only the keys the file set: an overlay that sets one key must not
	// reset every other display setting to a default.
	if (game.has_volume)
		volume = game.volume;
	if (game.has_video_scale)
		video_scale = game.video_scale;
	if (game.has_video_filter)
		video_filter = game.video_filter;
	if (game.has_lcd_grid)
		lcd_grid = game.lcd_grid;
	if (game.has_ghosting)
		ghosting = game.ghosting;
	if (game.has_palette)
		palette = game.palette;
	if (game.has_console_color)
		console_color = static_cast<ConsoleColor>(game.console_color);
	if (game.has_hardware_mode)
		hardware_mode = game.hardware_mode;
	const std::string rom = game.rom;
	game = GameOverride{};
	game.rom = rom;
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

u32 GUIConsole::color_for_console_dim(ConsoleColor c)
{
	switch (c) {
	case ConsoleColor::Purple:
		return 0xFF4A3266;
	case ConsoleColor::Green:
		return 0xFF2E6B4A;
	case ConsoleColor::Blue:
		return 0xFF2E4A6B;
	case ConsoleColor::Yellow:
		return 0xFF6B6B2E;
	case ConsoleColor::ClearPurple:
		return 0xFF5F4A86;
	default:
		return 0xFF4A3266;
	}
}

const char *GUIConsole::console_color_name(ConsoleColor c)
{
	switch (c) {
	case ConsoleColor::Purple:
		return "Purple";
	case ConsoleColor::Green:
		return "Green";
	case ConsoleColor::Blue:
		return "Blue";
	case ConsoleColor::Yellow:
		return "Yellow";
	case ConsoleColor::ClearPurple:
		return "Clear";
	default:
		return "Purple";
	}
}

// The bundled skins. GBC geometry is the long-measured set from
// update_layout()/rebuild_buttons (asset pixels / 1370 x 2430);
// button order is Up Down Left Right A B Select Start.
static const BodySkin kBodySkinTable[kBodySkins] = {{
	"gbc",
	"Game Boy Color",
	"gbc_body.png",
	75.0f / 133.0f,
	{0.21314f, 0.10041f, 0.57080f, 0.30247f}, // bezel (LCD glass)
	{0.09489f, 0.59259f, 0.24453f, 0.14815f}, // dpad enclosing area
	{0.797f, 0.100f, 0.080f, 0.046f},         // settings corner
	{0.123f, 0.100f, 0.080f, 0.046f},         // open corner
	{
		{0.17810f, 0.59259f, 0.08832f, 0.07202f}, // Up
		{0.17810f, 0.66872f, 0.08832f, 0.07202f}, // Down
		{0.09854f, 0.64000f, 0.11460f, 0.05700f}, // Left
		{0.22044f, 0.64000f, 0.11533f, 0.05700f}, // Right
		{0.79416f, 0.61687f, 0.11095f, 0.06337f}, // A
		{0.61971f, 0.64856f, 0.10511f, 0.06420f}, // B
		{0.36131f, 0.81481f, 0.11679f, 0.02469f}, // Select
		{0.50000f, 0.81481f, 0.11679f, 0.02469f}, // Start
	},
	{false, false, false, false, true, true, false, false},
	{
		{0.020f, 0.012f, 0.130f, 0.055f}, // L (overlay pill)
		{0.850f, 0.012f, 0.130f, 0.055f}, // R (overlay pill)
	},
}};

const BodySkin *body_skin_by_id(const char *id)
{
	if (id == nullptr)
		return nullptr;
	for (int i = 0; i < kBodySkins; i++) {
		if (std::strcmp(kBodySkinTable[i].id, id) == 0)
			return &kBodySkinTable[i];
	}
	return nullptr;
}

const BodySkin *body_skin_at(int index)
{
	if (index < 0 || index >= kBodySkins)
		return nullptr;
	return &kBodySkinTable[index];
}

const BodySkin *resolve_body_skin(const char *setting, bool gba_mode)
{
	(void) gba_mode; // second photo (gba) not bundled yet; Auto is GBC.
	if (setting != nullptr && std::strcmp(setting, "Auto") != 0) {
		if (const BodySkin *named = body_skin_by_id(setting))
			return named;
	}
	return &kBodySkinTable[0]; // GBC default + unknown fallback.
}

std::string GUIConsole::body_skin_value() const
{
	const std::string setting = settings_ ? settings_->body_skin : "Auto";
	const BodySkin *skin = active_skin();
	if (setting == "Auto")
		return std::string("Auto (") + skin->name + ")";
	return std::string(skin->name);
}

static const UiTheme kUiThemeDark = {
	0xFF2A2A33, // panel_outer (legacy)
	0xFF16161C, // panel_inner (legacy)
	0xFF23232D, // row_bg (legacy)
	0xFF32323F, // pill_bg (legacy)
	0xFFECECF2, // text (legacy)
	0xFF8E8EA3, // text_mute (contrast-audit floor)
	0xFF4FB477, // ok (legacy green)
	0xFF101418, // menu_bg (game-screen menu ground)
	0xFF22252B, // menu_row
	0xFF3A3A8F, // menu_row_sel
	0xFF2E3190, // menu_row_sel_edge
	0xFFFFFFFF, // menu_text
	0xFFD8D8D8, // menu_text_dim
	0xFFAAAAAA, // menu_hint
};
static const UiTheme kUiThemeLight = {
	0xFFF0F0F4, // panel_outer
	0xFFFFFFFF, // panel_inner
	0xFFE8E8EE, // row_bg
	0xFFDCDCE4, // pill_bg
	0xFF16161C, // text
	0xFF5A5A66, // text_mute
	0xFF2E7D4F, // ok (darkened for contrast on light fills)
	0xFFE9E9EE, // menu_bg
	0xFFDCDCE2, // menu_row
	0xFFB9B9F2, // menu_row_sel
	0xFF4A4A9A, // menu_row_sel_edge
	0xFF14141A, // menu_text
	0xFF3A3A44, // menu_text_dim
	0xFF55555F, // menu_hint
};

const UiTheme &ui_theme_dark()
{
	return kUiThemeDark;
}

const UiTheme &ui_theme_light()
{
	return kUiThemeLight;
}

void GUIConsole::update_layout(int window_w, int window_h)
{
	window_w_ = window_w;
	window_h_ = window_h;
	// Body aspect from the active photo skin (GBC default). Fractional
	// photo coordinates map 1:1 only against the skin they were measured
	// from, so every rect below comes from the same skin entry.
	const BodySkin *skin = active_skin();
	const float body_aspect = skin->aspect;
	const float win_aspect = static_cast<float>(window_w) / window_h;
	float body_w = 0, body_h = 0;
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
	// active skin's photo asset (the GBC numbers below live in the skin
	// table now; LCD glass, D-pad cross, A/B disks, SELECT/START pills were
	// measured in asset pixels / 1370 x 2430).
	const auto F = [&](float fx, float fy, float fw, float fh) -> Rect {
		return {body_x + fx * body_w, body_y + fy * body_h, fw * body_w, fh * body_h};
	};
	const auto SF = [&](BodySkinRect r) -> Rect { return F(r.x, r.y, r.w, r.h); };
	// Game screen: the photo's LCD glass. The GBC glass (black-bezel to
	// gray-LCD transitions at x 292/1073, y 244/978) is a 782x735 rect;
	// the old hardcoded rect was inset ~40px left/right, leaving gray
	// photo borders around the game.
	bezel_rect_ = SF(skin->bezel);
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
		gba_screen_rect_ = {bezel_rect_.x + (gw - sw) * 0.5f, bezel_rect_.y + (gh - sh) * 0.5f, sw,
							sh};
	}
	// D-pad enclosing area (cross + well), from the active skin.
	dpad_rect_ = SF(skin->dpad);
	// Screen bezel corners: OPEN top-left of the glass, gear top-right.
	// Both sit on the near-black bezel beside the glass (grey circle +
	// white glyph read well there). Hit-testing uses these same rects.
	settings_rect_ = SF(skin->settings);
	// OPEN button (file dialog): top-left bezel corner, mirroring the gear.
	open_rect_ = SF(skin->open);

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
	// Pad buttons from the active skin's measured geometry (GBC: D-pad
	// cross arms re-measured from the asset pixels, A/B disks, SELECT/START
	// pills). Order matches BodySkin::buttons: Up Down Left Right A B
	// Select Start.
	const BodySkin *skin = active_skin();
	static const char *kNames[8] = {"Up", "Down", "Left", "Right", "A", "B", "Select", "Start"};
	static const Joypad::Key kKeys[8] = {Joypad::Key::Up,     Joypad::Key::Down, Joypad::Key::Left,
										 Joypad::Key::Right,  Joypad::Key::A,    Joypad::Key::B,
										 Joypad::Key::Select, Joypad::Key::Start};
	for (int i = 0; i < 8; i++) {
		const BodySkinRect r = skin->buttons[i];
		add(r.x, r.y, r.w, r.h, kNames[i], kKeys[i], skin->button_circle[i]);
	}
	// GBA shoulders: the GBC photo has no L/R controls, so they live as
	// overlay pills on the body's top corners, like real shoulder buttons.
	// That corner plastic is light lavender, so the pills use a bright fill
	// with white labels to stay readable. Clicking a pill presses it
	// in-game; remapping happens through the settings Inputs tab (GBA
	// rows), same click-to-remap flow as the rest.
	shoulders_.clear();
	const char *kShoulderNames[2] = {"L", "R"};
	const gba::GbaKeypad::Key kShoulderKeys[2] = {gba::GbaKeypad::Key::L, gba::GbaKeypad::Key::R};
	for (int i = 0; i < 2; i++) {
		const BodySkinRect r = skin->shoulders[i];
		shoulders_.push_back({{bx + r.x * bw, by + r.y * bh, r.w * bw, r.h * bh},
							  kShoulderNames[i],
							  kShoulderKeys[i]});
	}
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

} // namespace

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

// ---- Settings page ----

namespace
{
// The General page is data-driven: one descriptor list drives layout,
// labels, live values and click actions, so adding a row cannot leave a
// stale hand-maintained index somewhere else.
enum class GeneralGroup : int { Display = 0, Sound, Interface, System, Library, Count };

struct GeneralSpec {
	const char *id;
	const char *label;
	const char *description;
	GeneralGroup group;
};

constexpr GeneralSpec kGeneralSpecs[] = {
	{"scale", "Scale", "Resize the window (1x-6x)", GeneralGroup::Display},
	{"filter", "Filter", "Pixel upscaling quality", GeneralGroup::Display},
	{"lcdgrid", "LCD Grid", "Simulated pixel gaps", GeneralGroup::Display},
	{"ghost", "Ghosting", "Frame persistence trail", GeneralGroup::Display},
	{"palette", "Palette", "Green-screen colour tint", GeneralGroup::Display},
	{"volume", "Volume", "Master output level", GeneralGroup::Sound},
	{"theme", "Theme", "Panel and menu colours", GeneralGroup::Interface},
	{"console", "Accent", "Highlight colour", GeneralGroup::Interface},
	{"skin", "Skin", "Shell photo (next launch)", GeneralGroup::Interface},
	{"hw", "Hardware", "DMG / CGB / Auto detection", GeneralGroup::System},
	{"folder", "Folder", "Library scan location", GeneralGroup::Library},
};

constexpr int kGeneralSpecCount = static_cast<int>(std::size(kGeneralSpecs));

struct GeneralEntry {
	bool header = false;
	int spec = -1;
	GeneralGroup group = GeneralGroup::Display;
};

std::vector<GeneralEntry> general_entries()
{
	std::vector<GeneralEntry> entries;
	entries.reserve(kGeneralSpecCount + static_cast<int>(GeneralGroup::Count));
	for (int g = 0; g < static_cast<int>(GeneralGroup::Count); g++) {
		entries.push_back({true, -1, static_cast<GeneralGroup>(g)});
		for (int i = 0; i < kGeneralSpecCount; i++) {
			if (static_cast<int>(kGeneralSpecs[i].group) == g)
				entries.push_back({false, i, static_cast<GeneralGroup>(g)});
		}
	}
	return entries;
}

const char *general_group_name(GeneralGroup g)
{
	switch (g) {
	case GeneralGroup::Display:
		return "DISPLAY";
	case GeneralGroup::Sound:
		return "SOUND";
	case GeneralGroup::Interface:
		return "INTERFACE";
	case GeneralGroup::System:
		return "SYSTEM";
	case GeneralGroup::Library:
		return "LIBRARY";
	default:
		return "";
	}
}

GUIConsole::SettingsAction action_for_row(const std::string &id)
{
	using Action = GUIConsole::SettingsAction;
	if (id == "folder")
		return Action::RomFolder;
	if (id == "volume")
		return Action::Volume;
	if (id == "scale")
		return Action::WindowScale;
	if (id == "palette")
		return Action::Palette;
	if (id == "hw")
		return Action::HwMode;
	if (id == "filter")
		return Action::Filter;
	if (id == "lcdgrid")
		return Action::LcdGrid;
	if (id == "ghost")
		return Action::Ghosting;
	if (id == "console")
		return Action::Console;
	if (id == "skin")
		return Action::BodySkin;
	if (id == "theme")
		return Action::Theme;
	return Action::None;
}
} // namespace

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
		const std::vector<GeneralEntry> entries = general_entries();
		for (size_t i = 0; i < entries.size(); i++) {
			const GeneralEntry &entry = entries[i];
			SettingsRow r;
			if (i < settings_general_labels_.size())
				r.label_rect = settings_general_labels_[i];
			if (i < settings_general_cells_.size())
				r.value_rect = settings_general_cells_[i];
			if (entry.header) {
				r.id = std::string("header:") + general_group_name(entry.group);
				r.label = general_group_name(entry.group);
				r.style = RowStyle::Header;
				rows.push_back(r);
				continue;
			}
			const GeneralSpec &spec = kGeneralSpecs[entry.spec];
			r.id = spec.id;
			r.label = spec.label;
			r.description = spec.description;
			r.style = RowStyle::Value;
			const std::string id = spec.id;
			if (id == "folder") {
				r.value = trunc_str(settings_->rom_folder, 20);
			} else if (id == "volume") {
				r.value = std::to_string(static_cast<int>(settings_->volume * 100)) + "%";
			} else if (id == "scale") {
				r.value = std::to_string(std::clamp(settings_->video_scale, 1, 6)) + "x";
			} else if (id == "palette") {
				r.value = palette_name(settings_->palette);
			} else if (id == "hw") {
				r.value = settings_->hardware_mode;
			} else if (id == "filter") {
				r.value = video_filter_name(static_cast<VideoFilter>(settings_->video_filter));
			} else if (id == "lcdgrid") {
				static const char *kGridNames[Settings::kLcdGridMax + 1] = {"Off", "Light",
																			"Medium", "Strong"};
				r.value = kGridNames[std::clamp(settings_->lcd_grid, 0, Settings::kLcdGridMax)];
			} else if (id == "ghost") {
				static const char *kGhostNames[Settings::kGhostMax + 1] = {"Off", "Light", "Medium",
																		   "Strong"};
				r.value = kGhostNames[std::clamp(settings_->ghosting, 0, Settings::kGhostMax)];
			} else if (id == "console") {
				const int idx = static_cast<int>(settings_->console_color);
				r.value = console_color_name(idx < 0 || idx >= kConsoleColors
												 ? ConsoleColor::Purple
												 : static_cast<ConsoleColor>(idx));
			} else if (id == "skin") {
				// Shows the resolved skin; Auto names its target so the
				// single bundled photo reads honestly.
				r.value = body_skin_value();
			} else if (id == "theme") {
				r.value = ui_theme_name(settings_->theme != 1 ? 0 : 1);
			}
			rows.push_back(r);
		}
		return rows;
	}

	// The Inputs page is NOT a row list: it is a GBA diagram, a clickable
	// binding list and a handful of widgets, all drawn by the renderer from
	// their own rects. Returning no rows here stops the generic row loop from
	// drawing anything.
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
		const std::vector<GeneralEntry> entries = general_entries();
		for (size_t i = 0; i < entries.size(); i++) {
			if (i >= settings_general_labels_.size() || i >= settings_general_cells_.size())
				break;
			if (entries[i].header)
				continue;
			const Rect &label = settings_general_labels_[i];
			const Rect &cell = settings_general_cells_[i];
			if (!in(label, x, y) && !in(cell, x, y))
				continue;
			const std::string id = kGeneralSpecs[entries[i].spec].id;
			hit.action = action_for_row(id);
			if (hit.action == SettingsAction::Volume) {
				hit.ratio = cell.w > 0.0f ? static_cast<float>((x - cell.x) / cell.w) : 0.0f;
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
	// Clickable list mirroring the GBA diagram: on a narrow panel the list is
	// the only remap surface, and on a wide one it is faster than tiny
	// shoulder tabs.
	for (int i = 0; i < kInputButtons; i++) {
		if (!in(binding_rects_[static_cast<size_t>(i)], x, y))
			continue;
		hit.action = SettingsAction::PadButton;
		hit.index = i;
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
		dd.items.push_back(
			{dd.closed.x, top + static_cast<float>(i) * item_h, dd.closed.w, item_h});
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
	binding_rects_.fill({0, 0, 0, 0});
	inputs_diagram_visible_ = false;
	const float py = body_rect_.y;
	const float bw = body_rect_.w, bh = body_rect_.h;
	if (bw <= 0 || bh <= 0)
		return;

	// The panel stays below the LCD glass (filter/grid changes should be
	// visible on the running game) but is allowed to grow wider than the
	// portrait body. A GBC shell is only 75 mm wide on screen; squeezing a
	// two-column settings page into that width made every control tiny.
	const float glass_bottom = bezel_rect_.y + bezel_rect_.h;
	const float panel_top = glass_bottom + bh * 0.018f;
	const float panel_bottom = std::min(py + bh, static_cast<float>(window_h_) - 8.0f);
	const float available_w = std::max(180.0f, static_cast<float>(window_w_) - 16.0f);
	const float wanted_w = std::max({bw * 0.92f, 420.0f, window_w_ * 0.62f});
	const float panel_w = std::min(wanted_w, std::min(620.0f, available_w));
	const float wanted_h = bh * 0.60f;
	const float panel_h = std::max(120.0f, std::min(wanted_h, panel_bottom - panel_top));
	const float panel_x = (window_w_ - panel_w) * 0.5f;
	settings_panel_ = {panel_x, panel_top, panel_w, panel_h};

	// Title bar, tabs, footer. Keeping these metrics here (and deriving them
	// from the panel in the renderer) stops the two from drifting apart on
	// resize.
	const float title_h = std::clamp(panel_h * 0.075f, 18.0f, 30.0f);
	const float bar_top = panel_top + 4.0f;
	const float close_s = std::clamp(title_h - 10.0f, 12.0f, 18.0f);
	settings_close_ = {panel_x + panel_w - 8.0f - close_s, bar_top + (title_h - close_s) * 0.5f,
					   close_s, close_s};
	const float tabs_y = bar_top + title_h + 6.0f;
	const float tabs_h = std::clamp(panel_h * 0.065f, 14.0f, 24.0f);
	const float tab_gap = 4.0f;
	const float tab_w = (panel_w - 20.0f - tab_gap) * 0.5f;
	settings_tab_general_ = {panel_x + 10.0f, tabs_y, tab_w, tabs_h};
	settings_tab_inputs_ = {panel_x + 10.0f + tab_w + tab_gap, tabs_y, tab_w, tabs_h};

	const float footer_h = std::clamp(panel_h * 0.06f, 12.0f, 20.0f);
	const float content_x = panel_x + 10.0f;
	const float content_w = panel_w - 20.0f;
	const float rows_top = tabs_y + tabs_h + 8.0f;
	const float rows_bottom = panel_top + panel_h - footer_h - 6.0f;
	const float content_h = std::max(20.0f, rows_bottom - rows_top);

	// ---- General: grouped, responsive row grid ----
	const std::vector<GeneralEntry> entries = general_entries();
	settings_general_labels_.assign(entries.size(), {0, 0, 0, 0});
	settings_general_cells_.assign(entries.size(), {0, 0, 0, 0});
	const bool two_columns = panel_w >= 360.0f;
	const auto column_of = [&](GeneralGroup g) {
		if (!two_columns)
			return 0;
		return static_cast<int>(g) <= static_cast<int>(GeneralGroup::Sound) ? 0 : 1;
	};
	int col_entries[2] = {0, 0};
	int col_specs[2] = {0, 0};
	int col_headers[2] = {0, 0};
	for (const GeneralEntry &e : entries) {
		const int c = column_of(e.group);
		col_entries[c]++;
		if (e.header)
			col_headers[c]++;
		else
			col_specs[c]++;
	}
	const float col_gap = std::max(10.0f, panel_w * 0.035f);
	const float col_w = two_columns ? (content_w - col_gap) * 0.5f : content_w;
	float row_gap = std::clamp(panel_h * 0.006f, 2.0f, 5.0f);
	float header_h = std::clamp(panel_h * 0.030f, 10.0f, 18.0f);
	float row_h = std::clamp(panel_h * 0.105f, 18.0f, 44.0f);
	const auto col_need = [&](int c) {
		return static_cast<float>(col_specs[c]) * row_h +
			   static_cast<float>(col_headers[c]) * header_h +
			   static_cast<float>(std::max(0, col_entries[c] - 1)) * row_gap;
	};
	const auto tallest = [&]() { return std::max(col_need(0), col_need(1)); };
	if (tallest() > content_h) {
		// First hide descriptions by capping rows at the two-line height,
		// then reclaim gaps and headers, and only then shrink the rows.
		row_h = std::min(row_h, 22.0f);
		row_gap = std::min(row_gap, 2.0f);
		if (tallest() > content_h) {
			header_h = std::min(header_h, 11.0f);
			row_gap = 0.0f;
		}
		if (tallest() > content_h) {
			float fit = row_h;
			for (int c = 0; c < 2; c++) {
				if (col_specs[c] <= 0)
					continue;
				const float room = content_h - static_cast<float>(col_headers[c]) * header_h;
				fit = std::min(fit, room / static_cast<float>(col_specs[c]));
			}
			row_h = std::clamp(fit, 9.0f, row_h);
		}
	}
	const float start_y = rows_top + std::max(0.0f, (content_h - tallest()) * 0.5f);
	const float col_x[2] = {content_x, content_x + col_w + col_gap};
	float col_y[2] = {start_y, start_y};
	for (size_t i = 0; i < entries.size(); i++) {
		const int c = column_of(entries[i].group);
		if (entries[i].header) {
			settings_general_labels_[i] = {col_x[c], col_y[c], col_w, header_h};
			col_y[c] += header_h + row_gap;
			continue;
		}
		// The label spans the column so the description can use the full
		// width; the value pill aligns with the first text line.
		const float line_h = std::min(row_h, std::max(14.0f, 8.0f * settings_text_scale() * 2.0f));
		settings_general_labels_[i] = {col_x[c], col_y[c], col_w, row_h};
		settings_general_cells_[i] = {col_x[c] + col_w * 0.53f, col_y[c] + 2.0f, col_w * 0.45f,
									  std::max(6.0f, std::min(line_h, row_h - 4.0f))};
		col_y[c] += row_h + row_gap;
	}

	// ---- Inputs: widgets on top, binding list + diagram below ----
	profile_dd_.items.clear();
	device_dd_.items.clear();
	const float wr_h = std::clamp(panel_h * 0.055f, 16.0f, 26.0f);
	const float wr_gap = std::clamp(panel_h * 0.008f, 3.0f, 5.0f);
	const float lbl_w = std::max(44.0f, content_w * 0.17f);
	const float btn_w = std::max(36.0f, content_w * 0.14f);
	float wy = rows_top;
	// Profile row: [label][dropdown .......] [Load] [Save]
	gba_pad_.label_profile = {content_x, wy, lbl_w, wr_h};
	profile_dd_.closed = {content_x + lbl_w, wy, content_w - lbl_w - btn_w * 2.0f - wr_gap * 2.0f,
						  wr_h};
	btn_load_ = {content_x + content_w - btn_w * 2.0f - wr_gap, wy, btn_w, wr_h};
	btn_save_ = {content_x + content_w - btn_w, wy, btn_w, wr_h};
	wy += wr_h + wr_gap;
	// Device row: [label][dropdown .......]
	gba_pad_.label_device = {content_x, wy, lbl_w, wr_h};
	device_dd_.closed = {content_x + lbl_w, wy, content_w - lbl_w, wr_h};
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

	// Auto-map button, only when a controller is the selected source.
	if (device_dd_.selected > 0) {
		btn_automap_ = {content_x + content_w * 0.5f - content_w * 0.22f, wy, content_w * 0.44f,
						wr_h + 2.0f};
		wy += wr_h + 2.0f + wr_gap;
	} else {
		btn_automap_ = {};
	}

	// Binding list. On a narrow panel the list gets two columns and the
	// diagram is dropped; on a wide panel the list sits left of the outline.
	const float list_top = wy + 18.0f; // room for the "BUTTON BINDINGS" caption
	const float list_h = std::max(20.0f, rows_bottom - list_top);
	const bool want_diagram = panel_w >= 380.0f && list_h >= 76.0f;
	inputs_diagram_visible_ = want_diagram;
	const float diagram_gap = want_diagram ? std::max(10.0f, content_w * 0.035f) : 0.0f;
	const float list_w = want_diagram ? content_w * 0.44f : content_w;
	const int list_cols = (!want_diagram && list_w >= 230.0f) ? 2 : 1;
	const int rows_per_col = (kInputBindings + list_cols - 1) / list_cols;
	const float list_gap = std::clamp(panel_h * 0.004f, 1.0f, 3.0f);
	float bind_h = (list_h - static_cast<float>(rows_per_col - 1) * list_gap) /
				   static_cast<float>(rows_per_col);
	bind_h = std::clamp(bind_h, 8.0f, 26.0f);
	const float bind_w =
		(list_w - static_cast<float>(list_cols - 1) * list_gap) / static_cast<float>(list_cols);
	const float used_h =
		static_cast<float>(rows_per_col) * bind_h + static_cast<float>(rows_per_col - 1) * list_gap;
	const float list_y = list_top + std::max(0.0f, (list_h - used_h) * 0.5f);
	for (int i = 0; i < kInputBindings; i++) {
		const int col = i / rows_per_col;
		const int row = i % rows_per_col;
		binding_rects_[static_cast<size_t>(i)] = {
			content_x + static_cast<float>(col) * (bind_w + list_gap),
			list_y + static_cast<float>(row) * (bind_h + list_gap), bind_w, bind_h};
	}

	// GBA outline, laid out in real millimetres so the proportions are
	// verifiable: the AGB-001 is 144.5 x 82 mm with a 61.2 x 40.8 mm screen
	// (Nintendo / GBATEK), and the control positions follow GBATEK's sketch.
	constexpr f32 kBodyWmm = 144.5f;
	constexpr f32 kBodyHmm = 82.0f;
	if (want_diagram) {
		const float gx = content_x + list_w + diagram_gap;
		const float gw = content_w - list_w - diagram_gap;
		const float gtop = list_top + 12.0f;
		const float gbottom = rows_bottom;
		float body_h = std::max(20.0f, gbottom - gtop);
		float body_w = body_h * (kBodyWmm / kBodyHmm);
		if (body_w > gw) {
			body_w = gw;
			body_h = body_w * (kBodyHmm / kBodyWmm);
		}
		const float bx = gx + (gw - body_w) * 0.5f;
		const float by = gtop + std::max(0.0f, (gbottom - gtop - body_h) * 0.5f);
		gba_pad_.body = {bx, by, body_w, body_h};

		// Millimetre -> body rect, so every number below is a real dimension.
		const auto MM = [&](float x, float y, float w, float h) {
			return Rect{bx + x / kBodyWmm * body_w, by + y / kBodyHmm * body_h,
						w / kBodyWmm * body_w, h / kBodyHmm * body_h};
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
	} else {
		gba_pad_.body = {0, 0, 0, 0};
		gba_pad_.screen = {0, 0, 0, 0};
		gba_pad_.dpad_h = {0, 0, 0, 0};
		gba_pad_.dpad_v = {0, 0, 0, 0};
		gba_pad_.btn_a = {0, 0, 0, 0};
		gba_pad_.btn_b = {0, 0, 0, 0};
		gba_pad_.btn_start = {0, 0, 0, 0};
		gba_pad_.btn_select = {0, 0, 0, 0};
		gba_pad_.btn_l = {0, 0, 0, 0};
		gba_pad_.btn_r = {0, 0, 0, 0};
	}

	relayout_dropdown(profile_dd_);
	relayout_dropdown(device_dd_);
}

std::vector<GUIConsole::BindingRow> GUIConsole::binding_rows() const
{
	std::vector<BindingRow> rows;
	if (settings_ == nullptr)
		return rows;
	rows.reserve(kInputBindings);
	for (int i = 0; i < kInputBindings; i++) {
		BindingRow r;
		r.rect = binding_rects_[static_cast<size_t>(i)];
		r.button = static_cast<PadButton>(i);
		r.label = pad_button_name(r.button);
		const Binding b = settings_->currentBinding(i);
		r.unbound = !b.bound();
		r.value = r.unbound ? "Unbound" : pad_label(b);
		r.selected = awaiting_input_ && pending_pad_ == r.button;
		rows.push_back(std::move(r));
	}
	return rows;
}

GUIConsole::Rect GUIConsole::settings_volume_rect() const
{
	const std::vector<GeneralEntry> entries = general_entries();
	for (size_t i = 0; i < entries.size() && i < settings_general_cells_.size(); i++) {
		if (!entries[i].header && std::string(kGeneralSpecs[entries[i].spec].id) == "volume")
			return settings_general_cells_[i];
	}
	return {0, 0, 0, 0};
}

float GUIConsole::settings_text_scale() const
{
	if (settings_panel_.w <= 0.0f)
		return 1.0f;
	// The panel always uses at least the atlas's native 8px cell - the old
	// 0.6 floor made the page unreadable on a default-size window - and grows
	// modestly on very wide ones. Layout and renderer share this value.
	return std::clamp(1.08f + (settings_panel_.w - 340.0f) * 0.0006f, 1.0f, 1.30f);
}

// ---- ROM menu ----

void GUIConsole::set_rom_list(std::vector<RomEntry> entries)
{
	rom_list_ = std::move(entries);
	rebuild_filter();
	if (filtered_.empty()) {
		menu_selected_ = 0;
	} else {
		menu_selected_ = std::clamp(menu_selected_, 0, static_cast<int>(filtered_.size()) - 1);
	}
	refresh_save_counts();
}

std::string GUIConsole::slot_path_for(const std::string &rom_path, int slot)
{
	if (rom_path.empty() || slot < 0 || slot >= 8)
		return "";
	return rom_path + ".gb4s" + static_cast<char>('0' + slot);
}

int GUIConsole::count_save_slots(const std::string &rom_path)
{
	if (rom_path.empty())
		return 0;
	int n = 0;
	for (int s = 0; s < 8; s++) {
		std::error_code ec;
		if (std::filesystem::exists(slot_path_for(rom_path, s), ec) && !ec)
			n++;
	}
	return n;
}

void GUIConsole::refresh_save_counts()
{
	for (auto &e : rom_list_) {
		e.save_slots = count_save_slots(e.path);
		e.thumb_path = thumb_path_for(e.path);
		e.has_thumb = !e.thumb_path.empty();
	}
}

std::string GUIConsole::thumb_path_for(const std::string &rom_path)
{
	if (rom_path.empty())
		return "";
	// User boxart wins over anything generated.
	{
		std::error_code ec;
		if (std::filesystem::exists(rom_path + ".png", ec) && !ec)
			return rom_path + ".png";
	}
	// Auto-shot written on eject, then slot shots from F5 saves.
	{
		std::error_code ec;
		if (std::filesystem::exists(rom_path + ".thumb.ppm", ec) && !ec)
			return rom_path + ".thumb.ppm";
	}
	for (int s = 0; s < 8; s++) {
		const std::string p = slot_path_for(rom_path, s) + ".ppm";
		std::error_code ec;
		if (std::filesystem::exists(p, ec) && !ec)
			return p;
	}
	return "";
}

void GUIConsole::rebuild_filter()
{
	filtered_.clear();
	if (!filter_active_ || menu_filter_.empty()) {
		filtered_.reserve(rom_list_.size());
		for (int i = 0; i < static_cast<int>(rom_list_.size()); i++)
			filtered_.push_back(i);
		return;
	}
	std::string needle = menu_filter_;
	std::transform(needle.begin(), needle.end(), needle.begin(),
				   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
	for (int i = 0; i < static_cast<int>(rom_list_.size()); i++) {
		std::string hay =
			rom_list_[static_cast<size_t>(i)].title + " " + rom_list_[static_cast<size_t>(i)].path;
		std::transform(hay.begin(), hay.end(), hay.begin(),
					   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
		if (hay.find(needle) != std::string::npos)
			filtered_.push_back(i);
	}
}

void GUIConsole::set_menu_filter(const std::string &f)
{
	menu_filter_ = f;
	filter_active_ = true;
	rebuild_filter();
	menu_selected_ = filtered_.empty()
						 ? 0
						 : std::clamp(menu_selected_, 0, static_cast<int>(filtered_.size()) - 1);
}

void GUIConsole::clear_menu_filter()
{
	menu_filter_.clear();
	filter_active_ = false;
	rebuild_filter();
	menu_selected_ = filtered_.empty()
						 ? 0
						 : std::clamp(menu_selected_, 0, static_cast<int>(filtered_.size()) - 1);
}

void GUIConsole::set_menu_filter_active(bool v)
{
	filter_active_ = v;
	if (!v)
		menu_filter_.clear();
	rebuild_filter();
	menu_selected_ = filtered_.empty()
						 ? 0
						 : std::clamp(menu_selected_, 0, static_cast<int>(filtered_.size()) - 1);
}

bool GUIConsole::menu_filter_type(const std::string &utf8)
{
	if (utf8.empty())
		return false;
	if (!filter_active_) {
		// Whatever physical key a layout uses for "/", SDL delivers "/".
		if (utf8 == "/") {
			set_menu_filter_active(true);
			return true;
		}
		return false;
	}
	// Space loads the selected ROM (scancode path below); never type it.
	if (utf8 == " ")
		return true;
	set_menu_filter(menu_filter_ + utf8);
	return true;
}

int GUIConsole::filtered_to_rom(int filtered_pos) const
{
	if (filtered_pos < 0 || filtered_pos >= static_cast<int>(filtered_.size()))
		return -1;
	return filtered_[static_cast<size_t>(filtered_pos)];
}

std::optional<GUIConsole::RomEntry> GUIConsole::selected_rom() const
{
	const int ri = filtered_to_rom(menu_selected_);
	if (ri < 0 || ri >= static_cast<int>(rom_list_.size()))
		return std::nullopt;
	return rom_list_[static_cast<size_t>(ri)];
}

void GUIConsole::set_menu_selected(int i)
{
	if (filtered_.empty()) {
		menu_selected_ = 0;
		return;
	}
	menu_selected_ = std::clamp(i, 0, static_cast<int>(filtered_.size()) - 1);
}

void GUIConsole::menu_move(int delta)
{
	if (filtered_.empty()) {
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
	const int n = static_cast<int>(filtered_.size());
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
	const int n = static_cast<int>(filtered_.size());
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
	if (!menu_active_ || filtered_.empty())
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
	if (idx < 0 || idx >= static_cast<int>(filtered_.size()))
		return std::nullopt;
	// Last row may be partial — e.g. cols=3, n=7, first=0, idx=7 is second
	// column of row 2 but that row only has 1 item. Treat remainder as empty.
	const int row_start = (idx / cols) * cols;
	const int row_items = std::min(cols, static_cast<int>(filtered_.size()) - row_start);
	if (col >= row_items)
		return std::nullopt;
	return idx;
}

} // namespace gb
