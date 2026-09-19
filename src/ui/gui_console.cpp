#include "gb/gui_console.h"
#include <SDL3/SDL.h>
#include <algorithm>
#include <cmath>
#include <fstream>
#include <sstream>

namespace gb {

Settings::Settings() {
    // Default mappings mirror GameBoy::handle_events
    key_map[static_cast<int>(Joypad::Key::Right)] = SDL_SCANCODE_RIGHT;
    key_map[static_cast<int>(Joypad::Key::Left)] = SDL_SCANCODE_LEFT;
    key_map[static_cast<int>(Joypad::Key::Up)] = SDL_SCANCODE_UP;
    key_map[static_cast<int>(Joypad::Key::Down)] = SDL_SCANCODE_DOWN;
    key_map[static_cast<int>(Joypad::Key::A)] = SDL_SCANCODE_X;
    key_map[static_cast<int>(Joypad::Key::B)] = SDL_SCANCODE_Z;
    key_map[static_cast<int>(Joypad::Key::Start)] = SDL_SCANCODE_RETURN;
    key_map[static_cast<int>(Joypad::Key::Select)] = SDL_SCANCODE_RSHIFT;
}

SDL_Scancode Settings::scancode_for(Joypad::Key k) const {
    return key_map[static_cast<int>(k)];
}
void Settings::set_mapping(Joypad::Key k, SDL_Scancode code) {
    key_map[static_cast<int>(k)] = code;
}

std::string default_settings_path() {
    const char* home = SDL_getenv("HOME");
    std::string base = home ? std::string(home) + "/.config/GB4ME" : ".";
    return base + "/settings.cfg";
}

bool Settings::save(const std::string& path) const {
    std::string p = path.empty() ? default_settings_path() : path;
    // Ensure directory exists
    size_t slash = p.find_last_of('/');
    if (slash != std::string::npos) {
        std::string dir = p.substr(0, slash);
        SDL_CreateDirectory(dir.c_str());
    }
    std::ofstream f(p);
    if (!f) return false;
    f << "rom_folder=" << rom_folder << "\n";
    f << "volume=" << volume << "\n";
    f << "video_scale=" << video_scale << "\n";
    f << "palette=" << palette << "\n";
    f << "hardware_mode=" << hardware_mode << "\n";
    f << "console_color=" << static_cast<int>(console_color) << "\n";
    for (int i = 0; i < 8; i++) f << "key" << i << "=" << static_cast<int>(key_map[i]) << "\n";
    return true;
}
bool Settings::load(const std::string& path) {
    std::string p = path.empty() ? default_settings_path() : path;
    std::ifstream f(p);
    if (!f) return false;
    std::string line;
    while (std::getline(f, line)) {
        size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        std::string k = line.substr(0, eq);
        std::string v = line.substr(eq + 1);
        if (k == "rom_folder") rom_folder = v;
        else if (k == "volume") volume = std::stof(v);
        else if (k == "video_scale") video_scale = std::stoi(v);
        else if (k == "palette") palette = std::stoi(v);
        else if (k == "hardware_mode") hardware_mode = v;
        else if (k == "console_color") console_color = static_cast<ConsoleColor>(std::stoi(v));
        else if (k.rfind("key", 0) == 0) {
            int idx = std::stoi(k.substr(3));
            if (idx >= 0 && idx < 8) key_map[idx] = static_cast<SDL_Scancode>(std::stoi(v));
        }
    }
    return true;
}

// ---- GUIConsole ----

GUIConsole::GUIConsole() { update_layout(640, 576); }

u32 GUIConsole::color_for_console(ConsoleColor c) {
    switch (c) {
        case ConsoleColor::Purple: return 0xFF6B3FA0;
        case ConsoleColor::Green: return 0xFF3FA06B;
        case ConsoleColor::Blue: return 0xFF3F6BA0;
        case ConsoleColor::Yellow: return 0xFFA0A03F;
        case ConsoleColor::ClearPurple: return 0xFF8F6FC0;
        default: return 0xFF6B3FA0;
    }
}

void GUIConsole::update_layout(int window_w, int window_h) {
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
        return {body_x + fx * body_w, body_y + fy * body_h,
                fw * body_w, fh * body_h};
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
        screen_rect_ = {bezel_rect_.x + (gw - sw) * 0.5f,
                        bezel_rect_.y + (gh - sh) * 0.5f, sw, sh};
    }
    // D-pad enclosing area (cross + well): x 135-455, y 1440-1800.
    dpad_rect_ = F(0.09854f, 0.59259f, 0.23358f, 0.14815f);
    // Settings gear: top-right corner. OPEN button mirrors it top-left.
    // Large enough to tap comfortably (~0.075 body widths diameter).
    const float gear_s = body_w * 0.075f;
    settings_rect_ = {body_x + body_w * 0.875f, body_y + body_h * 0.012f,
                      gear_s, gear_s};
    // OPEN button (file dialog): top-left corner, mirroring the gear.
    open_rect_ = {body_x + body_w * 0.07f, body_y + body_h * 0.012f,
                  gear_s, gear_s};

    rebuild_buttons();
    rebuild_settings_cells();
}

void GUIConsole::rebuild_buttons() {
    buttons_.clear();
    const float bx = body_rect_.x, by = body_rect_.y;
    const float bw = body_rect_.w, bh = body_rect_.h;
    const auto add = [&](float fx, float fy, float fw, float fh,
                         const char* name, Joypad::Key key, bool circle) {
        buttons_.push_back({bx + fx * bw, by + fy * bh,
                            fw * bw, fh * bh, name, key, circle});
    };
    // D-pad cross arms, measured from the asset: vertical bar x 230-368
    // (top/bottom arms ~119-133 wide, spanning y 1445-1794), horizontal bar
    // y 1548-1694 (~136 tall, spanning x ~135-455). Arms stop 10px short of
    // the cross center (297, ~1620) so the dead zone is a tiny 10x10 square.
    // The old arms were ~28px too far left and ~76px too small.
    add(0.16788f, 0.59259f, 0.10073f, 0.07202f, "Up", Joypad::Key::Up, false);
    add(0.16788f, 0.66872f, 0.10073f, 0.07202f, "Down", Joypad::Key::Down, false);
    add(0.09854f, 0.63704f, 0.11460f, 0.06008f, "Left", Joypad::Key::Left, false);
    add(0.22044f, 0.63704f, 0.11168f, 0.06008f, "Right", Joypad::Key::Right, false);
    // A/B disks (circles; hit_test does a radius check). Measured dark
    // extents: A x 1081-1246 y 1496-1658 (center 1163, 1577, r ~80),
    // B x 840-1000 y 1572-1742 (center 920, 1657, r ~80). The old disks
    // were inset ~18-36px and ~44px too small in diameter.
    add(0.78905f, 0.61564f, 0.12117f, 0.06708f, "A", Joypad::Key::A, true);
    add(0.61314f, 0.64691f, 0.11679f, 0.06996f, "B", Joypad::Key::B, true);
    // SELECT/START pills: measured black-pill extents x 522-652 / 722-854,
    // y 1986-2044 (centers y ~2015). The old pills were narrower/shorter
    // and ~15px too high.
    add(0.38102f, 0.81728f, 0.09489f, 0.02387f, "Select", Joypad::Key::Select, false);
    add(0.52701f, 0.81728f, 0.09635f, 0.02387f, "Start", Joypad::Key::Start, false);
}

std::optional<Joypad::Key> GUIConsole::hit_test(int x, int y) const {
    for (const auto& b : buttons_) {
        if (b.w <= 0) continue;
        if (x >= b.x && x < b.x + b.w && y >= b.y && y < b.y + b.h) {
            // For circles, check the normalized radius (elliptical: the rect
            // can be a few px taller than wide once mapped to the window).
            if (b.is_circle) {
                float cx = b.x + b.w * 0.5f;
                float cy = b.y + b.h * 0.5f;
                float dx = (x - cx) / (b.w * 0.5f);
                float dy = (y - cy) / (b.h * 0.5f);
                if (dx * dx + dy * dy > 1.0f) continue;
            }
            return b.key;
        }
    }
    return std::nullopt;
}

bool GUIConsole::hit_settings(int x, int y) const {
    return x >= settings_rect_.x && x < settings_rect_.x + settings_rect_.w &&
           y >= settings_rect_.y && y < settings_rect_.y + settings_rect_.h;
}

bool GUIConsole::hit_open(int x, int y) const {
    return x >= open_rect_.x && x < open_rect_.x + open_rect_.w &&
           y >= open_rect_.y && y < open_rect_.y + open_rect_.h;
}

void GUIConsole::start_mapping(Joypad::Key k) {
    awaiting_input_ = true;
    pending_key_ = k;
}

bool GUIConsole::consume_mapping_key(SDL_Scancode sc) {
    if (!awaiting_input_ || !pending_key_ || !settings_) return false;
    settings_->set_mapping(*pending_key_, sc);
    awaiting_input_ = false;
    pending_key_ = std::nullopt;
    settings_->save("");
    return true;
}

void GUIConsole::cancel_mapping() {
    awaiting_input_ = false;
    pending_key_ = std::nullopt;
}

// ---- Settings panel (tabbed, clickable) ----

void GUIConsole::set_settings_tab(SettingsTab t) {
    settings_tab_ = t;
}

void GUIConsole::rebuild_settings_cells() {
    settings_general_cells_.clear();
    settings_inputs_cells_.clear();
    settings_general_labels_.clear();
    settings_inputs_labels_.clear();
    const float px = body_rect_.x, py = body_rect_.y;
    const float bw = body_rect_.w, bh = body_rect_.h;
    if (bw <= 0 || bh <= 0) return;
    const float panel_w = bw * 0.85f, panel_h = bh * 0.70f;
    const float panel_x = px + (bw - panel_w) * 0.5f;
    const float panel_y = py + (bh - panel_h) * 0.5f;
    settings_panel_ = {panel_x, panel_y, panel_w, panel_h};
    // Close box top-right of the panel.
    const float cs = bh * 0.032f;
    settings_close_ = {panel_x + panel_w - 8.0f - cs, panel_y + 8.0f, cs, cs};
    // Tabs under the title bar.
    const float tabs_y = panel_y + bh * 0.075f;
    const float tabs_h = bh * 0.042f;
    const float tab_w = panel_w * 0.30f;
    settings_tab_general_ = {panel_x + 6.0f, tabs_y, tab_w, tabs_h};
    settings_tab_inputs_ = {panel_x + 12.0f + tab_w, tabs_y, tab_w, tabs_h};
    // Rows: label + value cells. Same row_h for both tabs.
    const float row_h = bh * 0.05f;
    const float row_gap = 4.0f;
    float row_y = tabs_y + tabs_h + 8.0f;
    const float label_x = panel_x + 6.0f, label_w = panel_w * 0.42f;
    const float value_x = panel_x + panel_w * 0.45f, value_w = panel_w * 0.40f;
    for (int i = 0; i < 4; i++) {
        settings_general_labels_.push_back({label_x, row_y, label_w, row_h});
        settings_general_cells_.push_back({value_x, row_y, value_w, row_h});
        row_y += row_h + row_gap;
    }
    row_y = tabs_y + tabs_h + 8.0f;
    for (int i = 0; i < 8; i++) {
        settings_inputs_labels_.push_back({label_x, row_y, label_w, row_h});
        settings_inputs_cells_.push_back({value_x, row_y, value_w, row_h});
        row_y += row_h + row_gap;
    }
}

namespace {

const char* palette_name(int p) {
    switch (p) {
        case 1: return "Green";
        case 2: return "Sepia";
        case 3: return "Blue";
        case 4: return "Crimson";
        default: return "Gray";
    }
}

const char* joykey_name(Joypad::Key k) {
    switch (k) {
        case Joypad::Key::Right: return "Right";
        case Joypad::Key::Left: return "Left";
        case Joypad::Key::Up: return "Up";
        case Joypad::Key::Down: return "Down";
        case Joypad::Key::A: return "A";
        case Joypad::Key::B: return "B";
        case Joypad::Key::Start: return "Start";
        case Joypad::Key::Select: return "Select";
        default: return "?";
    }
}

std::string trunc_str(const std::string& s, size_t max_len) {
    if (s.size() <= max_len) return s;
    if (max_len <= 3) return s.substr(0, max_len);
    return "..." + s.substr(s.size() - (max_len - 3));
}

}  // namespace

std::vector<GUIConsole::SettingsRow> GUIConsole::settings_rows() const {
    std::vector<SettingsRow> rows;
    if (!settings_) return rows;
    if (settings_tab_ == SettingsTab::General) {
        static const char* ids[4] = {"folder", "volume", "palette", "hw"};
        static const char* labels[4] = {"ROM Folder", "Volume", "DMG Palette",
                                        "Hardware Mode"};
        for (int i = 0; i < 4; i++) {
            SettingsRow r;
            r.id = ids[i];
            r.label = labels[i];
            r.label_rect = settings_general_labels_[i];
            r.value_rect = settings_general_cells_[i];
            switch (i) {
                case 0: r.value = trunc_str(settings_->rom_folder, 20); break;
                case 1:
                    r.value = std::to_string(static_cast<int>(settings_->volume * 100)) + "%";
                    break;
                case 2: r.value = palette_name(settings_->palette); break;
                case 3: r.value = settings_->hardware_mode; break;
                default: break;
            }
            rows.push_back(r);
        }
    } else {
        for (int i = 0; i < 8; i++) {
            const auto key = static_cast<Joypad::Key>(i);
            SettingsRow r;
            r.id = "map" + std::to_string(i);
            r.label = joykey_name(key);
            r.label_rect = settings_inputs_labels_[i];
            r.value_rect = settings_inputs_cells_[i];
            if (awaiting_input_ && pending_key_ && *pending_key_ == key) {
                r.value = "...";
            } else {
                const char* name = SDL_GetScancodeName(settings_->key_map[i]);
                r.value = (name && name[0]) ? trunc_str(name, 14) : "?";
            }
            rows.push_back(r);
        }
    }
    return rows;
}

GUIConsole::SettingsHit GUIConsole::settings_hit_test(int x, int y) const {
    SettingsHit hit;
    if (!show_settings_) return hit;
    const auto in = [](const Rect& r, int px, int py) {
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
        static const SettingsAction acts[4] = {
            SettingsAction::RomFolder, SettingsAction::Volume,
            SettingsAction::Palette, SettingsAction::HwMode};
        for (int i = 0; i < 4; i++) {
            const bool in_label = in(settings_general_labels_[i], x, y);
            const bool in_cell = in(settings_general_cells_[i], x, y);
            if (!in_label && !in_cell) continue;
            hit.action = acts[i];
            if (acts[i] == SettingsAction::Volume) {
                // The slider only actuates on its bar; label clicks are ignored
                // so they can't mute by accident (ratio would clamp to 0).
                if (!in_cell) return SettingsHit{};
                const Rect& c = settings_general_cells_[i];
                hit.ratio = c.w > 0 ? std::clamp((x - c.x) / c.w, 0.0f, 1.0f) : 0.0f;
            }
            return hit;
        }
    } else {
        for (int i = 0; i < 8; i++) {
            if (in(settings_inputs_labels_[i], x, y) ||
                in(settings_inputs_cells_[i], x, y)) {
                hit.action = SettingsAction::MapKey;
                hit.index = i;
                return hit;
            }
        }
    }
    return hit;
}

GUIConsole::Rect GUIConsole::settings_volume_rect() const {
    if (settings_general_cells_.size() < 2) return {0, 0, 0, 0};
    return settings_general_cells_[1];
}

// ---- ROM menu ----

void GUIConsole::set_rom_list(std::vector<RomEntry> entries) {
    rom_list_ = std::move(entries);
    if (rom_list_.empty()) {
        menu_selected_ = 0;
    } else {
        menu_selected_ = std::clamp(menu_selected_, 0,
                                    static_cast<int>(rom_list_.size()) - 1);
    }
}

void GUIConsole::set_menu_selected(int i) {
    if (rom_list_.empty()) {
        menu_selected_ = 0;
        return;
    }
    menu_selected_ = std::clamp(i, 0, static_cast<int>(rom_list_.size()) - 1);
}

void GUIConsole::menu_move(int delta) {
    if (rom_list_.empty()) {
        menu_selected_ = 0;
        return;
    }
    set_menu_selected(menu_selected_ + delta);
}

GUIConsole::Rect GUIConsole::menu_list_rect() const {
    const Rect& s = screen_rect_;
    const float pad = s.w * 0.045f;
    const float title_h = std::max(20.0f, s.h * 0.10f);
    const float footer_h = std::max(26.0f, s.h * 0.14f);
    return {s.x + pad, s.y + pad + title_h, s.w - pad * 2.0f,
            s.h - pad * 2.0f - title_h - footer_h};
}

// Grid layout constants

int GUIConsole::menu_grid_cols() const {
    const Rect list = menu_list_rect();
    if (list.w <= 0) return 1;
    // Aim for ~110px min cell width on the base window; clamp 1..3 cols.
    int cols = static_cast<int>(list.w / 110.0f);
    cols = std::clamp(cols, 1, 3);
    // Avoid a third column that's cramped: require at least 105px per cell.
    if (cols == 3 && list.w / 3.0f < 105.0f) cols = 2;
    return cols;
}

float GUIConsole::menu_cell_w() const {
    const Rect list = menu_list_rect();
    const int cols = menu_grid_cols();
    if (cols <= 0) return list.w;
    const float gap = 8.0f;
    return (list.w - gap * (cols - 1)) / cols;
}

float GUIConsole::menu_cell_h() const {
    // Tall enough for a 32px cart + 2 text lines + badge/padding.
    // Compact so even a small GB screen shows 2 rows (≈4-6 items).
    const float cw = menu_cell_w();
    return std::clamp(cw * 0.42f + 22.0f, 64.0f, 78.0f);
}

int GUIConsole::menu_grid_rows() const {
    const int n = static_cast<int>(rom_list_.size());
    const int cols = menu_grid_cols();
    if (cols <= 0) return 0;
    return (n + cols - 1) / cols;
}

int GUIConsole::menu_grid_visible_rows() const {
    const Rect list = menu_list_rect();
    const float ch = menu_cell_h();
    if (ch <= 0 || list.h <= 0) return 1;
    return std::max(1, static_cast<int>(list.h / ch));
}

int GUIConsole::menu_grid_visible_count() const {
    return menu_grid_cols() * menu_grid_visible_rows();
}

bool GUIConsole::hit_screen(int x, int y) const {
    const Rect& s = screen_rect_;
    return x >= s.x && x < s.x + s.w && y >= s.y && y < s.y + s.h;
}

GUIConsole::Rect GUIConsole::menu_cell_rect(int index) const {
    const Rect list = menu_list_rect();
    const int cols = menu_grid_cols();
    const float cw = menu_cell_w();
    const float ch = menu_cell_h();
    const float gap = 8.0f;
    const int first = menu_first_visible();
    const int local = index - first;
    if (local < 0) return {0, 0, 0, 0};
    const int col = local % cols;
    const int row = local / cols;
    return {list.x + col * (cw + gap), list.y + row * ch, cw, ch};
}

float GUIConsole::menu_row_h() const {
    // Keep for tests/back-compat — now just the grid cell height.
    return menu_cell_h();
}

int GUIConsole::menu_visible_rows() const {
    return menu_grid_visible_rows();
}

int GUIConsole::menu_first_visible() const {
    const int n = static_cast<int>(rom_list_.size());
    const int cols = menu_grid_cols();
    if (n <= 0 || cols <= 0) return 0;
    const int rows = menu_grid_rows();
    const int vis_rows = menu_grid_visible_rows();
    const int sel_row = menu_selected_ / cols;
    int first_row = std::clamp(sel_row - vis_rows + 1, 0, std::max(0, rows - vis_rows));
    if (sel_row < first_row) first_row = sel_row;
    return first_row * cols;
}

std::optional<int> GUIConsole::menu_hit_test(int x, int y) const {
    if (!menu_active_ || rom_list_.empty()) return std::nullopt;
    const Rect list = menu_list_rect();
    if (x < list.x || x >= list.x + list.w || y < list.y ||
        y >= list.y + list.h) {
        return std::nullopt;
    }
    const int cols = menu_grid_cols();
    const float cw = menu_cell_w();
    const float ch = menu_cell_h();
    const float gap = 8.0f;
    const int col = static_cast<int>((x - list.x) / (cw + gap));
    const int row = static_cast<int>((y - list.y) / ch);
    if (col < 0 || col >= cols || row < 0) return std::nullopt;
    // Gap hit: to the right of a cell but before next column's start.
    const float cell_x = list.x + col * (cw + gap);
    if (x >= cell_x + cw) return std::nullopt;
    const int idx = menu_first_visible() + row * cols + col;
    if (idx < 0 || idx >= static_cast<int>(rom_list_.size())) return std::nullopt;
    // Last row may be partial — e.g. cols=3, n=7, first=0, idx=7 is second
    // column of row 2 but that row only has 1 item. Treat remainder as empty.
    const int row_start = (idx / cols) * cols;
    const int row_items = std::min(cols, static_cast<int>(rom_list_.size()) - row_start);
    if (col >= row_items) return std::nullopt;
    return idx;
}

} // namespace gb
