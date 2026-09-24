#pragma once

#include "gba/keypad.h"
#include "joypad.h"
#include "types.h"
#include <SDL3/SDL.h>

#include "gb/input.h"
#include <array>
#include <optional>
#include <string>
#include <vector>

namespace gb
{

// Physical GBC dimensions: 75mm (W) ×133mm (H), screen 44×39mm
// A/B diagonal, D-Pad left, Start/Select below screen.
// https://en.wikipedia.org/wiki/Game_Boy_Color
// All layout computed preserving 75:133 aspect with letterboxing.

struct ButtonRegion {
	float x = 0, y = 0, w = 0, h = 0; // pixel coords in window
	std::string name;
	Joypad::Key key = Joypad::Key::A;
	bool is_circle = false; // A/B are circles
};



enum class ConsoleColor : u8 {
	Purple = 0,
	Green,
	Blue,
	Yellow,
	ClearPurple,
};

// A named, self-contained input mapping the user can switch between. The
// three tables mirror Settings' own key_map/pad_map* exactly, so activating a
// profile is a copy into those fields and every existing caller keeps working.
struct InputProfile {
	std::string name;
	std::array<SDL_Scancode, 10> key_map{};
	BindingTable pad_map{};
	BindingTable pad_map_alt{};
};

struct Settings {
	// Total profile cap, including the built-in Default at index 0. The
	// Inputs tab profile list, so this also bounds how many can be picked.
	static constexpr int kMaxProfiles = 16;
	std::string rom_folder = ".";
	// Unified input mapping, indexed by GbaKeypad::Key order
	// (A,B,Select,Start,Right,Left,Up,Down,R,L). The 8 common buttons are
	// shared by GB/GBC and GBA: mapped once, used by all systems. R/L are
	// GBA-only. Remapped through the settings Inputs tab.
	std::array<SDL_Scancode, 10> key_map{};
	// Controller half of the mapping: one Binding per logical button, in the
	// same order as key_map. Independent of the keyboard binding, so a
	// button can be driven by either or both. Serialised as pad0..pad9.
	BindingTable pad_map{};
	// Secondary controller source per button (the left stick mirroring the
	// d-pad after an auto-map). Serialised as alt0..alt9.
	BindingTable pad_map_alt{};
	// Input profiles, switchable from the Inputs tab. `profiles[0]` is always
	// the built-in "Default" and cannot be deleted, so the user's original
	// mapping always remains recoverable. `active_profile` indexes this vector;
	// whichever profile is active is mirrored into the key_map/pad_map* fields
	// above, which stay the single source of truth for gameplay.
	std::vector<InputProfile> profiles;
	int active_profile = 0;

	// Total profiles including Default.
	int profileCount() const
	{
		return static_cast<int>(profiles.size());
	}
	// The factory keyboard mapping. Shared by the constructor and the
	// "reset to defaults" auto-map so the two can never drift apart.
	static std::array<SDL_Scancode, 10> defaultKeyMap();
	// Restores that factory keyboard layout. One source per button, so any
	// controller bindings are cleared too.
	void applyDefaultKeyMap();

	// Snapshots the CURRENT mapping as a new named profile and activates it.
	// Returns the new index, or -1 when the cap is reached.
	int addProfile(const std::string &name);
	bool renameProfile(int idx, const std::string &name);
	// Deletes a user profile (idx > 0 only). Re-points active_profile and
	// re-activates so the live mapping never dangles.
	bool deleteProfile(int idx);
	// Copies a profile into the live mapping tables and marks it active.
	// Switching away first commits the live tables back into the profile that
	// was active, so remapping while a profile is selected updates it rather
	// than being silently discarded.
	bool activateProfile(int idx);
	// Writes the live mapping tables into the active profile. Called by
	// activateProfile() and save(), so edits always belong to the active
	// profile.
	void syncActiveProfile();
	// Copies a profile into the live tables WITHOUT first committing the live
	// tables back to the active profile. Used after a delete, where the live
	// tables belong to the profile that just disappeared.
	bool loadProfile(int idx);
	// Name of a profile, or an empty string for an out-of-range index.
	std::string profileName(int idx) const;
	// The single binding shown for a logical button, in the style of RPCS2 /
	// PCSX2 / Dolphin: one slot per ACTION, not one per device. A key wins if
	// both are somehow set, otherwise the controller binding, otherwise its
	// secondary source, otherwise unbound.
	Binding currentBinding(int i) const;
	// Assigns a binding to a logical button, clearing the other source so a
	// button is driven by exactly one thing at a time.
	void setBinding(int i, const Binding &b);

	// Shared-table index for a GB/GBC button.
	static int shared_key_index(Joypad::Key k)
	{
		switch (k) {
		case Joypad::Key::A:
			return 0;
		case Joypad::Key::B:
			return 1;
		case Joypad::Key::Select:
			return 2;
		case Joypad::Key::Start:
			return 3;
		case Joypad::Key::Right:
			return 4;
		case Joypad::Key::Left:
			return 5;
		case Joypad::Key::Up:
			return 6;
		default:
			return 7; // Down
		}
	}
	float volume = 1.0f;                // 0.0 -1.0
	int video_scale = 4;                // 1-6
	// Upscaling QUALITY options, all optional and all off by default so the
	// default look is unchanged. These resample the image; they do NOT change
	// the window or frame size. `video_filter` is a VideoFilter value
	// (Nearest / Bilinear / Smooth Bilinear / Scale2x); `lcd_grid` is a 0..3
	// step (Off / Light / Medium / Strong) that darkens the borders of each
	// source pixel for an LCD-panel look (SameBoy's MonoLCD grid).
	int video_filter = static_cast<int>(VideoFilter::Nearest);
	int lcd_grid = 0;
	// Highest lcd_grid step, and the depth each step applies to the border of
	// a source pixel (SameBoy's MonoLCD uses 0.25).
	static constexpr int kLcdGridMax = 3;
	static constexpr float kLcdGridDepth[kLcdGridMax + 1] = {0.0f, 0.12f, 0.22f, 0.34f};
	float lcdGridDepth() const
	{
		return kLcdGridDepth[lcd_grid < 0 ? 0
										  : (lcd_grid > kLcdGridMax ? kLcdGridMax : lcd_grid)];
	}
	int palette = 0;                    // DMG palette or CGB palette index
	std::string hardware_mode = "Auto"; // DMG / CGB / Auto
	ConsoleColor console_color = ConsoleColor::Purple;

	Settings();

	bool save(const std::string &path);
	bool load(const std::string &path);
	SDL_Scancode scancode_for(Joypad::Key k) const;
	void set_mapping(Joypad::Key k, SDL_Scancode code);
};

class GUIConsole
{
  public:
	struct Rect {
		float x, y, w, h;
	};
	// GBA shoulder buttons (L/R have no photo control on the GBC body, so
	// they live as overlay pills on the body's shoulders). Same
	// press/highlight/remap behaviour as the photo buttons, but bound to
	// GbaKeypad::Key.
	struct ShoulderRegion {
		Rect r{};
		std::string name;
		gba::GbaKeypad::Key key = gba::GbaKeypad::Key::L;
	};
	// ROM list entry for the on-screen menu (no ROM loaded).
	struct RomEntry {
		std::string title;
		std::string path;
		bool cgb = false;
		bool gba = false; // GBA image (routed to the GBA core, Phases 2+ execute)
	};

	enum class SettingsTab : u8 { General = 0, Inputs = 1 };

	enum class SettingsAction : u8 {
		None = 0,
		Close,
		TabGeneral,
		TabInputs,
		RomFolder,
		Volume,
		Palette,
		HwMode,
		Filter,       // cycle VideoFilter (Nearest / Bilinear / Smooth / Scale2x)
		LcdGrid,      // cycle the LCD grid strength (Off .. Strong)
		PadButton,    // index = logical button on the GBA diagram
		AutoMap,     // one-click layout: pad defaults, or keyboard defaults
		ProfileBox,   // open the profile dropdown (or the name prompt)
		ProfileItem,  // index = entry picked from the profile dropdown
		ProfileLoad,
		ProfileSave,
		DeviceBox,
		DeviceItem,   // index = entry picked from the device dropdown
	};

	struct SettingsHit {
		SettingsAction action = SettingsAction::None;
		// MapButton: logical PadButton index.
		int index = -1;
		float ratio = 0.0f;   // Volume: 0..1 position of the click
	};

	// Visual treatment for a settings row.
	enum class RowStyle : u8 {
		Value,  // label + value pill (Volume, Filter, ...)
		Button, // label + value pill that can be remapped
		Action, // a command row (Auto-map, New profile, ...)
		Header, // a section caption, no background
	};
	struct SettingsRow {
		std::string id; // "folder","volume",...,"bind<i>","automap","profsel"
		std::string label;
		std::string value;
		Rect label_rect{};
		Rect value_rect{};
		RowStyle style = RowStyle::Value;
		// Highlight the whole row: the active profile, or an armed remap.
		bool selected = false;
		// Dim the value: the button has no binding yet.
		bool unbound = false;
	};

	GUIConsole();

	void set_settings(Settings *s)
	{
		settings_ = s;
	}
	// The live input manager, used for controller labels and auto-map.
	void set_input(InputManager *in)
	{
		input_ = in;
	}
	// Performs the one-click default layout. The auto-map button is
	// context-sensitive: with a controller selected this writes a conventional
	// pad mapping (false when none is connected), and with the keyboard
	// selected it restores the factory keyboard layout.
	bool applyAutoMap(InputManager &input);
	bool applyKeyboardDefaults();
	// Label for that button, matching what the selected device will do.
	const char *autoMapLabel() const
	{
		return deviceIsController() ? "Auto-map to controller" : "Reset to defaults";
	}
	// Refresh the auto-map row's status text (pad count / last result).
	void refreshControllers(const InputManager &input);

	// Text-entry prompt, used to name a new or renamed input profile. The
	// console owns the buffer; GameBoy applies the accepted value.
	enum class TextPurpose : u8 { NewProfile, RenameProfile };
	bool text_entry_active() const
	{
		return text_active_;
	}
	TextPurpose text_purpose() const
	{
		return text_purpose_;
	}
	// Profile row the prompt was opened for (Rename), or -1 for New.
	int text_target() const
	{
		return text_target_;
	}
	const std::string &text_value() const
	{
		return text_text_;
	}
	const std::string &text_prompt() const
	{
		return text_prompt_;
	}
	// Arms the prompt. `initial` seeds the buffer for a rename.
	void start_text_entry(TextPurpose p, const std::string &prompt, const std::string &initial,
						  int target);
	void text_input(const std::string &utf8);
	void text_backspace();
	// Closes the prompt. Returns true when it was accepted with a non-empty
	// value; the caller then reads text_value() and applies it.
	bool text_accept();
	void text_cancel();
	void set_hardware_mode(HardwareMode m)
	{
		hardware_mode_ = m;
	}
	void set_double_speed(bool v)
	{
		double_speed_ = v;
	}

	// Layout update on window resize
	void update_layout(int window_w, int window_h);
	// Re-runs only the settings-cell layout, for when the row count changes at
	// runtime (adding, renaming or deleting an input profile).
	void rebuild_settings_cells();
	// Hit test: returns key if click inside a button region
	std::optional<Joypad::Key> hit_test(int x, int y) const;
	// GBA shoulder pills (L/R overlay zones flanking the screen).
	const std::vector<ShoulderRegion> &shoulders() const
	{
		return shoulders_;
	}
	std::optional<gba::GbaKeypad::Key> hit_shoulder(int x, int y) const;
	// Which system the Inputs tab and body buttons currently serve. Set by
	// the frontend on ROM load/eject; drives GBA rows + shoulder drawing.
	void set_gba_mode(bool v)
	{
		if (gba_mode_ == v)
			return;
		gba_mode_ = v;
		rebuild_settings_cells();
	}
	bool gba_mode() const
	{
		return gba_mode_;
	}
	// For settings gear hit
	bool hit_settings(int x, int y) const;
	// For the OPEN (file dialog) button on the console body
	bool hit_open(int x, int y) const;

	// ---- Inputs page ----
	// A dropdown widget: a closed box showing the current selection, and a
	// drop-down list of options when open. Option 0 is always the keyboard;
	// options 1..N are the connected controllers.
	struct Dropdown {
		Rect closed{};
		std::vector<Rect> items;
		std::vector<std::string> options;
		bool open = false;
		bool enabled = false;
		bool flipped = false; // list opened upward to stay on the panel
		int selected = 0;
	};
	void relayout_dropdown(Dropdown &dd);
	Dropdown &profile_box()
	{
		return profile_dd_;
	}
	const Dropdown &profile_box() const
	{
		return profile_dd_;
	}
	Dropdown &device_box()
	{
		return device_dd_;
	}
	const Dropdown &device_box() const
	{
		return device_dd_;
	}
	// Which input source the GBA diagram captures from: 0 = keyboard,
	// 1..N = the Nth connected controller.
	int selectedDevice() const
	{
		return device_dd_.selected;
	}
	bool deviceIsController() const
	{
		return device_dd_.selected > 0;
	}
	// True when the armed remap should accept a key / a pad button.
	bool captureWantsKey() const
	{
		return device_dd_.selected == 0;
	}
	// Display name of the current device selection.
	std::string deviceName() const;
	// Text shown on the auto-map button (pad count / last result).
	const std::string &controllerStatus() const
	{
		return controller_status_;
	}
	void closeDropdowns()
	{
		profile_dd_.open = false;
		device_dd_.open = false;
	}
	bool anyDropdownOpen() const
	{
		return profile_dd_.open || device_dd_.open;
	}
	const GUIConsole::Rect &btn_load() const
	{
		return btn_load_;
	}
	const GUIConsole::Rect &btn_save() const
	{
		return btn_save_;
	}
	const GUIConsole::Rect &btn_automap() const
	{
		return btn_automap_;
	}
	// Both devices have a one-click default, so the button is always shown.
	bool automapVisible() const
	{
		return true;
	}
	// Hit test the GBA diagram: which logical button (if any) was clicked.
	std::optional<PadButton> hit_gba_pad(int x, int y) const;
	// Refresh the device dropdown from the connected controllers. Call when
	// the pad set or the selection changes.
	void setDeviceOptions(const std::vector<std::string> &names, int selected);
	// Number of trailing entries in the profile menu that are commands rather
	// than profiles: "New...", "Rename..." and "Delete".
	static constexpr int kProfileMenuSpecials = 3;
	int profileMenuSize() const;
	// Index of the first command entry; entries below it are profiles.
	int profileCommandIndex() const;
	// True when a profile-menu entry can be acted on. Rename and Delete apply
	// to the ACTIVE profile, and the built-in Default can be neither renamed
	// nor deleted, so those two entries are unavailable until a user profile
	// is selected. Offering them anyway made them look broken.
	bool profileMenuEnabled(int i) const;
	// Layout of the GBA outline drawn on the Inputs page.
	struct GbaPad {
		Rect body{};
		Rect screen{};
		Rect dpad_h{}; // horizontal bar
		Rect dpad_v{}; // vertical bar
		Rect btn_a{};  // circles
		Rect btn_b{};
		Rect btn_start{};
		Rect btn_select{};
		Rect btn_l{};
		Rect btn_r{};
		Rect label_profile{};
		Rect label_device{};
	};
	const GbaPad &gbaPad() const
	{
		return gba_pad_;
	}

	// Input mapping modal
	bool awaiting_input() const
	{
		return awaiting_input_;
	}
	// The logical button currently armed for remapping.
	PadButton pending_pad() const
	{
		return pending_pad_;
	}
	void start_binding(PadButton b);
	// Applies a captured key or controller input to the armed logical button.
	// Returns true when the remap completed. Either source is accepted, which
	// is how RPCS2/PCSX2/Dolphin bind: one slot per action.
	bool consume_binding(const Binding &b);
	void cancel_mapping(); // abort modal without changing the mapping

	void set_show_settings(bool v)
	{
		show_settings_ = v;
	}
	bool show_settings() const
	{
		return show_settings_;
	}
	void set_button_pressed(Joypad::Key k, bool pressed)
	{
		pressed_[static_cast<int>(k)] = pressed;
	}
	bool is_pressed(Joypad::Key k) const
	{
		return pressed_[static_cast<int>(k)];
	}
	void set_gba_pressed(gba::GbaKeypad::Key k, bool pressed)
	{
		gba_pressed_[static_cast<int>(k)] = pressed;
	}
	bool is_gba_pressed(gba::GbaKeypad::Key k) const
	{
		return gba_pressed_[static_cast<int>(k)];
	}
	// Full-window game fill (click screen → fill, double-click → back to GBC body)
	void set_fullscreen_game(bool v)
	{
		fullscreen_game_ = v;
	}
	bool fullscreen_game() const
	{
		return fullscreen_game_;
	}
	void toggle_fullscreen_game()
	{
		fullscreen_game_ = !fullscreen_game_;
	}
	bool hit_screen(int x, int y) const;
	bool hit_screen(int x, int y, const Rect &r) const;

	// Accessors for renderer
	Rect body_rect() const
	{
		return body_rect_;
	}
	Rect bezel_rect() const
	{
		return bezel_rect_;
	}
	Rect screen_rect() const
	{
		return screen_rect_;
	}
	// GBA 240x160 image fitted into the same LCD glass (aspect-correct).
	// Used by the GBA render path so 3:2 frames are not stretched into the
	// GB-shaped screen_rect_.
	Rect gba_screen_rect() const
	{
		return gba_screen_rect_;
	}
	const std::vector<ButtonRegion> &buttons() const
	{
		return buttons_;
	}
	Rect settings_button_rect() const
	{
		return settings_rect_;
	}
	Rect open_button_rect() const
	{
		return open_rect_;
	}
	Rect dpad_rect() const
	{
		return dpad_rect_;
	} // enclosing D-Pad area for rendering
	// Settings panel geometry (for the renderer + click handling)
	Rect settings_panel_rect() const
	{
		return settings_panel_;
	}
	Rect settings_tab_rect(SettingsTab t) const
	{
		switch (t) {
		case SettingsTab::General:
			return settings_tab_general_;
		case SettingsTab::Inputs:
			return settings_tab_inputs_;
		}
		return settings_tab_general_;
	}
	Rect settings_close_rect() const
	{
		return settings_close_;
	}
	// Value cell of the volume slider (for drag handling).
	Rect settings_volume_rect() const;
	SettingsTab settings_tab() const
	{
		return settings_tab_;
	}
	void set_settings_tab(SettingsTab t);
	// Rows of the active tab with live label/value strings
	std::vector<SettingsRow> settings_rows() const;
	SettingsHit settings_hit_test(int x, int y) const;
	// ROM menu (shown on the game screen when no ROM is loaded)
	void set_menu_active(bool v)
	{
		if (menu_active_ == v)
			return;
		menu_active_ = v;
		rebuild_settings_cells();
	}
	bool menu_active() const
	{
		return menu_active_;
	}
	void set_rom_list(std::vector<RomEntry> entries);
	const std::vector<RomEntry> &rom_list() const
	{
		return rom_list_;
	}
	void set_menu_selected(int i);
	int menu_selected() const
	{
		return menu_selected_;
	}
	void menu_move(int delta); // clamp selection, used by keys/wheel
	Rect menu_list_rect() const;
	// Legacy list metrics (kept for tests, now backed by grid row height).
	float menu_row_h() const;
	int menu_visible_rows() const;
	int menu_first_visible() const;
	// Grid metrics — ROM menu now renders as a 2-3 column grid with large
	// cart icons. All layout is derived live from screen_rect so it scales
	// with the window and needs no rebuild on resize.
	int menu_grid_cols() const;
	float menu_cell_w() const;
	float menu_cell_h() const;
	int menu_grid_rows() const;
	int menu_grid_visible_rows() const;
	int menu_grid_visible_count() const;
	Rect menu_cell_rect(int index) const;
	// Entry index under the point, or nullopt (header/footer/empty area)
	std::optional<int> menu_hit_test(int x, int y) const;
	ConsoleColor console_color() const
	{
		return settings_ ? settings_->console_color : ConsoleColor::Purple;
	}
	HardwareMode hardware_mode() const
	{
		return hardware_mode_;
	}
	bool double_speed() const
	{
		return double_speed_;
	}
	Settings *settings()
	{
		return settings_;
	}
	const Settings *settings() const
	{
		return settings_;
	}

	// For window aspect handling
	int window_w() const
	{
		return window_w_;
	}
	int window_h() const
	{
		return window_h_;
	}

  private:
	Settings *settings_ = nullptr;
	InputManager *input_ = nullptr;
	HardwareMode hardware_mode_ = HardwareMode::DMG;
	bool double_speed_ = false;

	int window_w_ = 640, window_h_ = 576;
	Rect body_rect_{};
	Rect bezel_rect_{};
	Rect screen_rect_{};
	Rect gba_screen_rect_{};
	Rect dpad_rect_{};
	Rect settings_rect_{};
	Rect open_rect_{};
	std::vector<ButtonRegion> buttons_;
	std::vector<ShoulderRegion> shoulders_;
	bool gba_mode_ = false;

	bool awaiting_input_ = false;
	// Logical button the modal is remapping.
	PadButton pending_pad_ = PadButton::A;
	// Inputs page widgets.
	Dropdown profile_dd_;
	Dropdown device_dd_;
	GbaPad gba_pad_{};
	Rect btn_load_{};
	Rect btn_save_{};
	Rect btn_automap_{};
	// Text shown on the auto-map row (controller count / last result).
	std::string controller_status_;
	// Text-entry prompt state (profile naming).
	static constexpr size_t kMaxTextLen = 24;
	bool text_active_ = false;
	TextPurpose text_purpose_ = TextPurpose::NewProfile;
	int text_target_ = -1;
	std::string text_prompt_;
	std::string text_text_;
	bool show_settings_ = false;
	SettingsTab settings_tab_ = SettingsTab::General;
	Rect settings_panel_{};
	Rect settings_tab_general_{};
	Rect settings_tab_inputs_{};
	Rect settings_close_{};
	// Value-cell rects per row id, rebuilt in update_layout() for both tabs
	// (value strings are produced live in settings_rows()).
	std::vector<Rect> settings_general_cells_;
	std::vector<Rect> settings_inputs_cells_;
	std::vector<Rect> settings_general_labels_;
	std::vector<Rect> settings_inputs_labels_;

	bool menu_active_ = true;
	std::vector<RomEntry> rom_list_;
	int menu_selected_ = 0;
	bool fullscreen_game_ = false;
	std::array<bool, 8> pressed_{};
	std::array<bool, 10> gba_pressed_{};

	void rebuild_buttons();
	// Inputs tab rows: 10 keyboard bindings, 10 controller bindings for the
	// same logical buttons, then the one-click auto-map row. The 8 shared
	// slots drive GB/GBC and GBA alike; R/L are GBA-only.
	static constexpr int kGeneralRows = 6;
	static constexpr int kInputButtons = 10;
	// Inputs tab: profile selector, 10 button rows, then 4 action rows
	// (auto-map, new, rename, delete). Header rows share the geometry of a
	// following row but occupy no slot of their own.
	static constexpr int kInputActions = 4;
	int input_row_count() const
	{
		return 1 + kInputButtons + 1 + kInputActions; // profile + buttons + stick + actions
	}
	static u32 color_for_console(ConsoleColor c);
};

} // namespace gb
