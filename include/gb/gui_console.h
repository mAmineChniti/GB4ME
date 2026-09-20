#pragma once

#include "types.h"
#include "joypad.h"
#include <SDL3/SDL.h>
#include <array>
#include <optional>
#include <string>
#include <vector>

namespace gb {

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

struct Settings {
    std::string rom_folder = ".";
    // Input mapping: Joypad key -> SDL_Scancode
    std::array<SDL_Scancode, 8> key_map{};
    float volume = 1.0f; // 0.0 -1.0
    int video_scale = 4; // 1-6
    int palette = 0; // DMG palette or CGB palette index
    std::string hardware_mode = "Auto"; // DMG / CGB / Auto
    ConsoleColor console_color = ConsoleColor::Purple;

    Settings();

    bool save(const std::string& path) const;
    bool load(const std::string& path);
    SDL_Scancode scancode_for(Joypad::Key k) const;
    void set_mapping(Joypad::Key k, SDL_Scancode code);
};

class GUIConsole {
public:
    struct Rect { float x, y, w, h; };
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
        MapKey, // index = Joypad::Key being remapped
    };

    struct SettingsHit {
        SettingsAction action = SettingsAction::None;
        int index = -1; // MapKey: Joypad::Key value
        float ratio = 0.0f; // Volume: 0..1 position of the click
    };

    struct SettingsRow {
        std::string id; // "folder","volume","scale","palette","hw","color","map<i>"
        std::string label;
        std::string value;
        Rect label_rect{};
        Rect value_rect{};
    };

    GUIConsole();

    void set_settings(Settings* s) { settings_ = s; }
    void set_hardware_mode(HardwareMode m) { hardware_mode_ = m; }
    void set_double_speed(bool v) { double_speed_ = v; }

    // Layout update on window resize
    void update_layout(int window_w, int window_h);
    // Hit test: returns key if click inside a button region
    std::optional<Joypad::Key> hit_test(int x, int y) const;
    // For settings gear hit
    bool hit_settings(int x, int y) const;
    // For the OPEN (file dialog) button on the console body
    bool hit_open(int x, int y) const;

    // Input mapping modal
    bool awaiting_input() const { return awaiting_input_; }
    std::optional<Joypad::Key> pending_key() const { return pending_key_; }
    void start_mapping(Joypad::Key k);
    bool consume_mapping_key(SDL_Scancode sc);
    void cancel_mapping(); // abort modal without changing the mapping

    void set_show_settings(bool v) { show_settings_ = v; }
    bool show_settings() const { return show_settings_; }
    void set_button_pressed(Joypad::Key k, bool pressed) { pressed_[static_cast<int>(k)] = pressed; }
    bool is_pressed(Joypad::Key k) const { return pressed_[static_cast<int>(k)]; }
    // Full-window game fill (click screen → fill, double-click → back to GBC body)
    void set_fullscreen_game(bool v) { fullscreen_game_ = v; }
    bool fullscreen_game() const { return fullscreen_game_; }
    void toggle_fullscreen_game() { fullscreen_game_ = !fullscreen_game_; }
    bool hit_screen(int x, int y) const;

    // Accessors for renderer
    Rect body_rect() const { return body_rect_; }
    Rect bezel_rect() const { return bezel_rect_; }
    Rect screen_rect() const { return screen_rect_; }
    const std::vector<ButtonRegion>& buttons() const { return buttons_; }
    Rect settings_button_rect() const { return settings_rect_; }
    Rect open_button_rect() const { return open_rect_; }
    Rect dpad_rect() const { return dpad_rect_; } // enclosing D-Pad area for rendering
    // Settings panel geometry (for the renderer + click handling)
    Rect settings_panel_rect() const { return settings_panel_; }
    Rect settings_tab_rect(SettingsTab t) const {
        return t == SettingsTab::General ? settings_tab_general_ : settings_tab_inputs_;
    }
    Rect settings_close_rect() const { return settings_close_; }
    // Value cell of the volume slider (for drag handling).
    Rect settings_volume_rect() const;
    SettingsTab settings_tab() const { return settings_tab_; }
    void set_settings_tab(SettingsTab t);
    // Rows of the active tab with live label/value strings
    std::vector<SettingsRow> settings_rows() const;
    SettingsHit settings_hit_test(int x, int y) const;
    // ROM menu (shown on the game screen when no ROM is loaded)
    void set_menu_active(bool v) { menu_active_ = v; }
    bool menu_active() const { return menu_active_; }
    void set_rom_list(std::vector<RomEntry> entries);
    const std::vector<RomEntry>& rom_list() const { return rom_list_; }
    void set_menu_selected(int i);
    int menu_selected() const { return menu_selected_; }
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
    ConsoleColor console_color() const { return settings_ ? settings_->console_color : ConsoleColor::Purple; }
    HardwareMode hardware_mode() const { return hardware_mode_; }
    bool double_speed() const { return double_speed_; }
    Settings* settings() { return settings_; }
    const Settings* settings() const { return settings_; }

    // For window aspect handling
    int window_w() const { return window_w_; }
    int window_h() const { return window_h_; }

private:
    Settings* settings_ = nullptr;
    HardwareMode hardware_mode_ = HardwareMode::DMG;
    bool double_speed_ = false;

    int window_w_ = 640, window_h_ = 576;
    Rect body_rect_{};
    Rect bezel_rect_{};
    Rect screen_rect_{};
    Rect dpad_rect_{};
    Rect settings_rect_{};
    Rect open_rect_{};
    std::vector<ButtonRegion> buttons_;

    bool awaiting_input_ = false;
    std::optional<Joypad::Key> pending_key_;
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
    std::array<bool,8> pressed_{};

    void rebuild_buttons();
    void rebuild_settings_cells();
    static u32 color_for_console(ConsoleColor c);
};

} // namespace gb
