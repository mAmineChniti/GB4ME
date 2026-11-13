// GB4ME Vulkan renderer — GUI overlay buffers (body, bezel, buttons, menu text).
// See include/gb/vulkan_renderer.h for the class interface.

#include "gb/vulkan_renderer.h"
#include "vulkan/vulkan_internal.h"

#include "gb/font.h"
#include "gb/gui_console.h"
#include <SDL3/SDL_vulkan.h>
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace gb
{
using namespace vulkan_detail;

bool VulkanRenderer::create_gui_buffers()
{
	// Generous initial sizes (typical menus use ~11k verts); growth in
	// ensure_gui_buffers() covers huge libraries.
	gui_vert_cap_ = 32768;
	gui_idx_cap_ = 65536;
	const VkDeviceSize vsize = static_cast<VkDeviceSize>(gui_vert_cap_) * sizeof(GuiVertex);
	const VkDeviceSize isize = static_cast<VkDeviceSize>(gui_idx_cap_) * sizeof(u32);
	if (!create_buffer(physical_device, device, vsize, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
					   VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
					   gui_vertex_buffer, gui_vertex_buffer_memory)) {
		return false;
	}
	if (!create_buffer(physical_device, device, isize, VK_BUFFER_USAGE_INDEX_BUFFER_BIT,
					   VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
					   gui_index_buffer, gui_index_buffer_memory)) {
		return false;
	}
	return true;
}

void VulkanRenderer::destroy_gui_buffers()
{
	if (gui_vertex_buffer != VK_NULL_HANDLE) {
		vkDestroyBuffer(device, gui_vertex_buffer, nullptr);
		gui_vertex_buffer = VK_NULL_HANDLE;
	}
	if (gui_vertex_buffer_memory != VK_NULL_HANDLE) {
		vkFreeMemory(device, gui_vertex_buffer_memory, nullptr);
		gui_vertex_buffer_memory = VK_NULL_HANDLE;
	}
	if (gui_index_buffer != VK_NULL_HANDLE) {
		vkDestroyBuffer(device, gui_index_buffer, nullptr);
		gui_index_buffer = VK_NULL_HANDLE;
	}
	if (gui_index_buffer_memory != VK_NULL_HANDLE) {
		vkFreeMemory(device, gui_index_buffer_memory, nullptr);
		gui_index_buffer_memory = VK_NULL_HANDLE;
	}
	gui_vert_cap_ = 0;
	gui_idx_cap_ = 0;
}

bool VulkanRenderer::ensure_gui_buffers(u32 verts, u32 indices)
{
	if (verts <= gui_vert_cap_ && indices <= gui_idx_cap_)
		return true;
	u32 nv = std::max(gui_vert_cap_ * 2u, verts);
	u32 ni = std::max(gui_idx_cap_ * 2u, indices);
	if (nv < 32768)
		nv = 32768;
	if (ni < 65536)
		ni = 65536;
	wait_idle();
	destroy_gui_buffers();
	gui_vert_cap_ = nv;
	gui_idx_cap_ = ni;
	const VkDeviceSize vsize = static_cast<VkDeviceSize>(nv) * sizeof(GuiVertex);
	const VkDeviceSize isize = static_cast<VkDeviceSize>(ni) * sizeof(u32);
	if (!create_buffer(physical_device, device, vsize, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
					   VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
					   gui_vertex_buffer, gui_vertex_buffer_memory)) {
		return false;
	}
	if (!create_buffer(physical_device, device, isize, VK_BUFFER_USAGE_INDEX_BUFFER_BIT,
					   VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
					   gui_index_buffer, gui_index_buffer_memory)) {
		return false;
	}
	return true;
}

void VulkanRenderer::update_gui_buffers(const GUIConsole &gui, const GUIConsole::Rect *game_rect)
{
	// Tile draws are rebuilt every frame from the visible ROMs (see menu grid
	// below); clear first so a shrinking list cannot leave stale draws behind.
	thumb_draws_.clear();
	// Build vertex/index data for GUI rects (body, bezel, buttons, etc.)
	// Indices are 32-bit: long ROM lists generate tens of thousands of
	// vertices, and 16-bit indices would wrap and silently drop the tail
	// of the overlay (footer text, gear/open buttons).
	std::vector<GuiVertex> verts;
	std::vector<u32> indices;
	std::vector<TextVertex> text_verts;
	std::vector<u32> text_indices;
	auto push_rect = [&](float x, float y, float w, float h, u32 rgba, float rounding) {
		// Rounded rectangles are emitted as a small triangle fan: four
		// quarter-circle arcs joined by straight edges. Layout rects live in
		// WINDOW coordinates (same space as mouse events; see handle_events),
		// so NDC must use the window size — NOT the swapchain (physical pixel)
		// size, which differs on HiDPI/fractional-scale displays.
		float win_w = static_cast<float>(gui.window_w());
		float win_h = static_cast<float>(gui.window_h());
		auto to_ndc = [&](float px, float py) -> Vec2 {
			// Window pixels use top-left origin (y down). Vulkan NDC y=-1 maps
			// to the framebuffer top, so py=0 must yield -1 (NOT 1.0f - ...,
			// which rendered the whole GUI upside down).
			return {(px / win_w) * 2.0f - 1.0f, (py / win_h) * 2.0f - 1.0f};
		};
		if (w <= 0.0f || h <= 0.0f)
			return;
		Vec4 col = {((rgba >> 16) & 0xFF) / 255.0f, ((rgba >> 8) & 0xFF) / 255.0f,
					(rgba & 0xFF) / 255.0f, ((rgba >> 24) & 0xFF) / 255.0f};
		const float r = std::min({rounding, w * 0.5f, h * 0.5f});
		const u32 base = static_cast<u32>(verts.size());
		verts.push_back({to_ndc(x + w * 0.5f, y + h * 0.5f), {col.x, col.y, col.z, col.w}});
		if (r < 0.5f) {
			verts.push_back({to_ndc(x, y), {col.x, col.y, col.z, col.w}});
			verts.push_back({to_ndc(x + w, y), {col.x, col.y, col.z, col.w}});
			verts.push_back({to_ndc(x + w, y + h), {col.x, col.y, col.z, col.w}});
			verts.push_back({to_ndc(x, y + h), {col.x, col.y, col.z, col.w}});
			for (u32 i = 0; i < 4; i++) {
				indices.push_back(base);
				indices.push_back(base + 1 + i);
				indices.push_back(base + 1 + ((i + 1) % 4));
			}
			return;
		}
		constexpr int kArcSegments = 4;
		const auto arc = [&](float cx, float cy, float a0, float a1) {
			for (int i = 0; i < kArcSegments; i++) {
				const float t = static_cast<float>(i) / static_cast<float>(kArcSegments);
				const float a = a0 + (a1 - a0) * t;
				verts.push_back({to_ndc(cx + std::cos(a) * r, cy + std::sin(a) * r),
								 {col.x, col.y, col.z, col.w}});
			}
		};
		arc(x + r, y + r, 3.14159265f, 4.71238898f);     // top-left
		arc(x + w - r, y + r, 4.71238898f, 6.28318531f); // top-right
		arc(x + w - r, y + h - r, 0.0f, 1.57079633f);    // bottom-right
		arc(x + r, y + h - r, 1.57079633f, 3.14159265f); // bottom-left
		const u32 count = static_cast<u32>(kArcSegments) * 4;
		for (u32 i = 0; i < count; i++) {
			indices.push_back(base);
			indices.push_back(base + 1 + i);
			indices.push_back(base + 1 + ((i + 1) % count));
		}
	};

	auto push_circle = [&](float cx, float cy, float r, u32 rgba) {
		// Window coordinates, like push_rect above (NOT swapchain pixels).
		float win_w = static_cast<float>(gui.window_w());
		float win_h = static_cast<float>(gui.window_h());
		Vec4 col = {((rgba >> 16) & 0xFF) / 255.0f, ((rgba >> 8) & 0xFF) / 255.0f,
					(rgba & 0xFF) / 255.0f, ((rgba >> 24) & 0xFF) / 255.0f};
		const int segs = 16;
		u32 center_idx = static_cast<u32>(verts.size());
		Vec2 center = {(cx / win_w) * 2.0f - 1.0f, (cy / win_h) * 2.0f - 1.0f};
		verts.push_back({center, {col.x, col.y, col.z, col.w}});
		for (int i = 0; i < segs; i++) {
			float a = (i / static_cast<float>(segs)) * 2.0f * 3.1415926f;
			float x = cx + std::cos(a) * r;
			float y = cy + std::sin(a) * r;
			Vec2 p = {(x / win_w) * 2.0f - 1.0f, (y / win_h) * 2.0f - 1.0f};
			verts.push_back({p, {col.x, col.y, col.z, col.w}});
		}
		for (int i = 0; i < segs; i++) {
			u32 i0 = center_idx;
			u32 i1 = center_idx + 1 + static_cast<u32>(i);
			u32 i2 = center_idx + 1 + static_cast<u32>((i + 1) % segs);
			indices.push_back(i0);
			indices.push_back(i1);
			indices.push_back(i2);
		}
	};
	auto push_text = [&](float x, float y, const std::string &str, float scale, u32 rgba) {
		// Vector typeface when a TTF baked successfully (same monospace 8px
		// grid, smooth outlines); otherwise the built-in 8x8 bitmap.
		if (font_ok_) {
			emit_text(text_verts, text_indices, x, y, str, scale, rgba,
					  static_cast<float>(gui.window_w()), static_cast<float>(gui.window_h()));
			return;
		}
		float cursor_x = x;
		for (char ch : str) {
			if (ch < 32 || ch > 126) {
				cursor_x += 8 * scale;
				continue;
			}
			const uint8_t *glyph = font::kFont[static_cast<int>(ch)];
			for (int row = 0; row < 8; row++) {
				uint8_t bits = glyph[row];
				// Font is LSB-first (bit 0 = leftmost pixel), row 0 = top row.
				for (int col = 0; col < 8; col++) {
					if (bits & (1 << col)) {
						push_rect(cursor_x + col * scale, y + row * scale, scale, scale, rgba, 0);
					}
				}
			}
			cursor_x += 8 * scale;
		}
	};
	// (procedural shell removed: photo body is drawn textured)
	// Fullscreen game: no console chrome at all (shoulders, highlights,
	// menu, open/gear, settings, modals). Only the transient OSD pills
	// below survive, so stale overlay text can never linger on the game.
	const bool chrome = !gui.fullscreen_game();
	// Photo shell: the GBC body photo (assets/gbc_body.png, Evan-Amos public
	// domain) is drawn as a textured quad in record_command_buffer_with_gui
	// before the game. All printed artwork (Nintendo / GAME BOY COLOR /
	// SELECT / START / POWER / speaker / LEDs / screws) comes from the photo,
	// so only interactive feedback is drawn here: button press highlights,
	// the settings gear, the settings panel, and the mapping modal.
	auto body = gui.body_rect();
	// Letterbox: fill the glass around the fitted game image with black so
	// the picture sits edge-to-edge in the bezel (a 3:2 GBA frame leaves
	// tall bars in the ~1.06 glass; GB/GBC thin ones). Drawn first so all
	// later overlay sits above; the bars are clamped to the glass minus the
	// image, so they can never cover game pixels.
	if (game_rect != nullptr) {
		// Each bar overlaps the image by 1 window px: the game quad goes
		// through viewport/scissor integer truncation while these rects use
		// exact float NDC, so hairline photo slivers showed at the right and
		// bottom edges. A 1px black inner border is invisible and seals all
		// four seams at any window/HiDPI scale.
		const auto glass = gui.bezel_rect();
		const float gx0 = glass.x, gy0 = glass.y, gx1 = glass.x + glass.w, gy1 = glass.y + glass.h;
		const float ix0 = game_rect->x, iy0 = game_rect->y, ix1 = game_rect->x + game_rect->w,
					iy1 = game_rect->y + game_rect->h;
		push_rect(gx0, gy0, glass.w, std::max(0.0f, iy0 - gy0 + 1.0f), 0xFF000000, 0);
		push_rect(gx0, iy1 - 1.0f, glass.w, std::max(0.0f, gy1 - iy1 + 1.0f), 0xFF000000, 0);
		push_rect(gx0, gy0, std::max(0.0f, ix0 - gx0 + 1.0f), glass.h, 0xFF000000, 0);
		push_rect(ix1 - 1.0f, gy0, std::max(0.0f, gx1 - ix1 + 1.0f), glass.h, 0xFF000000, 0);
	}
	// GBA shoulder pills, always visible (menu included) so L/R stay
	// discoverable and remappable everywhere, like the settings rows.
	// They sit on the top body corners, clear of the menu grid and game.
	{
		for (const auto &s : gui.shoulders()) {
			if (!chrome)
				break;
			if (s.r.w <= 0)
				continue;
			const bool pressed = gui.is_gba_pressed(s.key);
			push_rect(s.r.x, s.r.y, s.r.w, s.r.h, pressed ? 0xFF4F7FFF : 0xFF6E6E80, 4.0f);
			const float tscale = std::clamp(s.r.h * 0.36f / 8.0f, 1.25f, 2.5f);
			const float tw = measure_text(s.name, tscale);
			push_text(s.r.x + (s.r.w - tw) * 0.5f, s.r.y + (s.r.h - 8.0f * tscale) * 0.5f, s.name,
					  tscale, pressed ? 0xFF101018 : 0xFFFFFFFF);
		}
	}
	// Press highlights over the photo buttons.
	for (const auto &b : gui.buttons()) {
		if (!chrome)
			break;
		if (b.w <= 0)
			continue;
		if (!gui.is_pressed(b.key))
			continue;
		if (b.is_circle) {
			push_circle(b.x + b.w * 0.5f, b.y + b.h * 0.5f, b.w * 0.5f, 0x60FFFFFF);
		} else {
			push_rect(b.x, b.y, b.w, b.h, 0x50FFFFFF, 2.0f);
		}
	}
	// ROM menu on the game screen (no ROM loaded): ROM list from rom_folder.
	if (chrome && gui.menu_active()) {
		const gb::UiTheme &th = gui.ui_theme();
		auto scr = gui.screen_rect();
		push_rect(scr.x, scr.y, scr.w, scr.h, th.menu_bg, 0);
		// Menu chrome text scales with the window (reference: ~240px wide
		// screen at scale 1.0) so headers/footers stay legible instead of a
		// fixed 8px that is tiny on large windows and cramped on small ones.
		// The scale is additionally shrunk to fit so lines can never be cut
		// off or overlap the ROM grid: longest footer line fits the screen
		// width, the two-line footer fits its reserve (mirrors
		// menu_list_rect's footer_h), and the header fits the title reserve.
		float ui_scale = std::clamp(scr.w / 240.0f, 0.55f, 1.6f);
		// Footer hint grows a "1-8:RECENT" prefix when recents are pinned;
		// cap the scale from the footer line actually drawn.
		int n_recent = 0;
		for (const auto &r : gui.rom_list())
			n_recent += r.recent ? 1 : 0;
		// Footer hint keeps its classic two lines; the quick-launch hint
		// lives in the subtitle instead so a long footer never shrinks the
		// whole menu's text scale.
		const std::string f2 = "O:OPEN FILE S:SETTINGS R:RESCAN /:FILTER";
		ui_scale = std::min(ui_scale, scr.w / ((f2.size() + 1.0f) * 8.0f)); // f2 width
		const float footer_h = std::max(26.0f, scr.h * 0.14f);
		ui_scale = std::min(ui_scale, (footer_h - 7.0f) / 16.0f); // 4+8s+3+8s block
		const float title_h = std::max(20.0f, scr.h * 0.10f);
		const float list_pad = scr.w * 0.045f; // mirrors menu_list_rect
		ui_scale = std::min(ui_scale, (title_h + list_pad - 6.0f) / 20.0f);
		ui_scale = std::max(ui_scale, 0.50f);
		const auto &roms_full = gui.rom_list();
		const auto &filt = gui.filtered_indices();
		const float cx = scr.x + scr.w * 0.5f;
		push_text(cx - measure_text("GB4ME", ui_scale) * 0.5f, scr.y + 6, "GB4ME", ui_scale,
				  th.menu_text);
		std::string base_sub = "SELECT ROM (" + std::to_string(filt.size());
		if (gui.menu_filter_active() && !gui.menu_filter().empty())
			base_sub += "/" + gui.menu_filter();
		base_sub += ")";
		if (gui.menu_filter_active()) {
			base_sub += " FILTER:/... ESC:CLEAR";
		}
		const std::string sub =
			n_recent > 0 ? base_sub + " - 1-" + std::to_string(n_recent) + " PLAYS RECENT"
						 : base_sub;
		push_text(cx - measure_text(sub, ui_scale) * 0.5f, scr.y + 6 + 12.0f * ui_scale, sub,
				  ui_scale, th.menu_hint);
		const auto list = gui.menu_list_rect();
		const int cols = gui.menu_grid_cols();
		const float cw = gui.menu_cell_w();
		const float ch = gui.menu_cell_h();
		const int vis = gui.menu_grid_visible_count();
		const int first = gui.menu_first_visible();
		const float gap = 8.0f;
		const float badge_w = 3 * 8.0f + 6.0f;
		const float badge_h = 10.0f;
		for (int vi = 0; vi < vis; vi++) {
			const int i = first + vi;
			if (i < 0 || i >= static_cast<int>(filt.size()))
				break;
			const int col = vi % cols;
			const int row = vi / cols;
			const float cx = list.x + col * (cw + gap);
			const float cy = list.y + row * ch;
			// Last row may be partial — skip empty cells.
			if (cx + cw > list.x + list.w + 1.0f)
				continue;
			const bool sel = (i == gui.menu_selected());
			// Cell background
			push_rect(cx, cy, cw, ch, sel ? th.menu_row_sel : th.menu_row, 3.0f);
			if (sel)
				push_rect(cx + 1, cy + 1, cw - 2, ch - 2, th.menu_row_sel_edge, 3.0f);
			// System badge — top-right inside cell
			const auto &rom = roms_full[static_cast<size_t>(filt[static_cast<size_t>(i)])];
			const std::string badge = rom.gba ? "GBA" : (rom.cgb ? "CGB" : "DMG");
			const u32 bcol = rom.gba ? 0xFF4F7FFF : (rom.cgb ? 0xFF3FA06B : 0xFF5A5A5A);
			const float bx = cx + cw - badge_w - 4;
			const float by = cy + 4;
			push_rect(bx - 2, by, badge_w, badge_h, bcol, 2.0f);
			push_text(bx, by + 1, badge, 1.0f, 0xFFFFFFFF);
			// Save-slot badge — green S<n> when states exist, dim T when an
			// auto-thumbnail (<rom>.thumb.ppm, written on eject) exists.
			if (rom.save_slots > 0) {
				char sbuf[16];
				std::snprintf(sbuf, sizeof(sbuf), "S%d", rom.save_slots);
				push_text(cx + 4, by + 1, sbuf, 1.0f, th.ok);
			} else if (rom.has_thumb) {
				push_text(cx + 4, by + 1, "T", 1.0f, th.text_mute);
			}
			// Tile image (boxart / auto-shot / slot shot) over the cart icon
			// area, aspect-kept; the drawn cart below is the fallback when
			// there is no image or it failed to decode/upload.
			const ThumbTex *tile = nullptr;
			if (!rom.thumb_path.empty() && thumbs_ready_.count(rom.thumb_path) != 0) {
				const ThumbTex *t = find_thumb(rom.thumb_path);
				if (t != nullptr && t->w > 0 && t->h > 0 && t->set != VK_NULL_HANDLE)
					tile = t;
			}
			// Large cartridge icon — centered top of cell
			const float icon_w = std::clamp(cw * 0.38f, 28.0f, 42.0f);
			const float icon_h = icon_w * 0.92f;
			const float ix = cx + (cw - icon_w) * 0.5f;
			const float iy = cy + 8;
			if (tile != nullptr) {
				const PixelRectF fit = fit_aspect_rect(ix, iy, icon_w, icon_h, tile->w, tile->h);
				thumb_draws_.push_back({GUIConsole::Rect{fit.x, fit.y, fit.w, fit.h}, tile->set});
			} else {
				const u32 cart_body = sel ? 0xFFD6D6D6 : 0xFFB0B0B0;
				const u32 cart_top = sel ? 0xFF9A9A9A : 0xFF6E6E6E;
				const u32 label_bg = sel ? 0xFF1A1A1A : 0xFF252525;
				push_rect(ix, iy + 4, icon_w, icon_h, cart_body, 2.0f);
				push_rect(ix + icon_w * 0.22f, iy, icon_w * 0.56f, 5.0f, cart_top, 2.0f);
				// notch
				push_rect(ix + icon_w * 0.42f, iy + 5, icon_w * 0.16f, 2.0f, cart_top, 0);
				push_rect(ix + 3, iy + 10, icon_w - 6, icon_h - 12, label_bg, 1.0f);
				// GB emboss on label
				const float lbx = ix + (icon_w - 16) * 0.5f;
				push_rect(lbx, iy + 13, 16, 1, sel ? 0xFF4A4A4A : 0xFF3A3A3A, 0);
			}
			// Title — centered below icon, wrapped to 2 lines max
			std::string title = roms_full[static_cast<size_t>(filt[static_cast<size_t>(i)])].title;
			// Split " - " suffix (region) from stem to keep main title larger:
			// keep full title but allow wrapping; word-aware.
			const float text_max_w = cw - 10.0f;
			const float tscale = 1.0f;
			const float char_w = 8.0f * tscale;
			size_t maxc_line = text_max_w > 4 ? static_cast<size_t>((text_max_w) / char_w) : 1;
			if (maxc_line < 4)
				maxc_line = 4;
			// Wrap into up to 2 lines
			auto trim = [](const std::string &s) -> std::string {
				size_t a = 0;
				while (a < s.size() && std::isspace(static_cast<unsigned char>(s[a])))
					a++;
				size_t b = s.size();
				while (b > a && std::isspace(static_cast<unsigned char>(s[b - 1])))
					b--;
				return s.substr(a, b - a);
			};
			std::vector<std::string> lines;
			std::string remaining = title;
			for (int line = 0; line < 2 && !remaining.empty(); line++) {
				if (remaining.size() <= maxc_line) {
					lines.push_back(remaining);
					break;
				}
				size_t cut = maxc_line;
				if (line == 1) {
					// Last line truncates with ...
					if (remaining.size() > maxc_line) {
						if (maxc_line > 3) {
							size_t sp = remaining.rfind(' ', maxc_line - 3);
							if (sp != std::string::npos && sp > maxc_line / 2)
								remaining = remaining.substr(0, sp) + "...";
							else
								remaining = remaining.substr(0, maxc_line - 3) + "...";
						} else
							remaining = remaining.substr(0, maxc_line);
					}
					lines.push_back(remaining);
					break;
				}
				size_t sp = remaining.rfind(' ', cut);
				if (sp != std::string::npos && sp > maxc_line / 2) {
					lines.push_back(remaining.substr(0, sp));
					remaining = trim(remaining.substr(sp + 1));
				} else {
					size_t dash = remaining.rfind(" - ", cut);
					if (dash != std::string::npos && dash > maxc_line / 2) {
						lines.push_back(remaining.substr(0, dash));
						remaining = trim(remaining.substr(dash + 3));
					} else {
						lines.push_back(remaining.substr(0, cut));
						remaining = trim(remaining.substr(cut));
					}
				}
			}
			const float title_y0 = iy + icon_h + 8;
			for (size_t li = 0; li < lines.size(); li++) {
				// Pinned recents get a star on the first title line.
				const std::string shown = (li == 0 && rom.recent) ? "* " + lines[li] : lines[li];
				const float tw = measure_text(shown, tscale);
				const float tx = cx + (cw - tw) * 0.5f;
				const float ty = title_y0 + li * 9.0f;
				if (ty + 8 > cy + ch - 4)
					break;
				push_text(tx, ty, shown, tscale, sel ? th.menu_text : th.menu_text_dim);
			}
		}
		if (filt.empty()) {
			const std::string e1 =
				gui.menu_filter_active() ? "NO MATCHES FOR FILTER" : "NO ROMS IN FOLDER";
			push_text(cx - measure_text(e1, ui_scale) * 0.5f, list.y + 10, e1, ui_scale,
					  th.menu_hint);
			std::string folder = gui.settings() ? gui.settings()->rom_folder : ".";
			if (folder.size() > 24)
				folder = "..." + folder.substr(folder.size() - 21);
			push_text(cx - measure_text(folder, ui_scale) * 0.5f, list.y + 10 + 14.0f * ui_scale,
					  folder, ui_scale, th.menu_hint);
		}
		// scroll indicators — grid scrolls by row, show track + arrows
		if (first > 0)
			push_text(list.x + list.w - 12, list.y - 10, "^", ui_scale, th.menu_hint);
		if (first + vis < static_cast<int>(filt.size())) {
			push_text(list.x + list.w - 12, list.y + list.h + 2, "v", ui_scale, th.menu_hint);
		}
		// vertical scrollbar for large libraries (50+ games)
		if (static_cast<int>(filt.size()) > vis) {
			const float track_x = list.x + list.w - 4;
			const float track_y = list.y;
			const float track_h = list.h;
			push_rect(track_x, track_y, 3.0f, track_h, 0xFF1E2025, 1.0f);
			const float frac_vis = static_cast<float>(vis) / filt.size();
			const float frac_first = static_cast<float>(first) / filt.size();
			const float thumb_h = std::clamp(track_h * frac_vis, 12.0f, track_h);
			const float thumb_y = track_y + frac_first * track_h;
			const float clamped_thumb_y = std::min(thumb_y, track_y + track_h - thumb_h);
			push_rect(track_x, clamped_thumb_y, 3.0f, thumb_h, 0xFF6B6B8A, 1.0f);
		}
		// footer hints — grid navigation (scaled with the window; anchored
		// to the bottom so the two lines never overlap each other).
		const std::string f1 = "ARROWS:MOVE ENTER:PLAY T:TURBO";
		const float f2_y = scr.y + scr.h - 4.0f - 8.0f * ui_scale;
		const float f1_y = f2_y - 8.0f * ui_scale - 3.0f;
		push_text(cx - measure_text(f1, ui_scale) * 0.5f, f1_y, f1, ui_scale, th.menu_hint);
		push_text(cx - measure_text(f2, ui_scale) * 0.5f, f2_y, f2, ui_scale, th.menu_hint);
	}
	// OPEN button (file dialog, top-left) + settings gear (top-right).
	// The photo has neither; both stay clickable (also O / S keys).
	// Hidden in fullscreen (like all chrome).
	if (chrome) {
		auto openr = gui.open_button_rect();
		const float ocx = openr.x + openr.w * 0.5f, ocy = openr.y + openr.h * 0.5f;
		push_circle(ocx, ocy, openr.w * 0.5f, 0xFF5A5A5A);
		// folder glyph
		const float fs = openr.w / 30.0f;
		push_rect(ocx - 7 * fs, ocy - 5 * fs, 6 * fs, 3 * fs, 0xFFFFFFFF, 0);
		push_rect(ocx - 7 * fs, ocy - 3 * fs, 14 * fs, 8 * fs, 0xFFFFFFFF, 0);
		push_rect(ocx - 5 * fs, ocy - 1 * fs, 10 * fs, 2 * fs, 0xFF5A5A5A, 0);
	}
	// Settings gear (photo has none; kept clickable, also toggled by S key)
	// Hidden in fullscreen (like all chrome).
	if (chrome) {
		auto setr = gui.settings_button_rect();
		const float ccx = setr.x + setr.w * 0.5f, ccy = setr.y + setr.h * 0.5f;
		const float rr = setr.w * 0.5f;
		push_circle(ccx, ccy, rr, 0xFF5A5A5A);
		// Vector gear (replaces the '*' text hack): 8 teeth + ring + hub
		// hole, all white on the button disc. Sized like the old glyph
		// (~0.57 button widths across the teeth tips).
		constexpr u32 kGearBg = 0xFF5A5A5A;
		constexpr u32 kGearInk = 0xFFFFFFFF;
		const float tooth_r = rr * 0.44f, tooth_half = rr * 0.11f;
		for (int k = 0; k < 8; k++) {
			const float a = (k / 8.0f) * 2.0f * 3.1415926f;
			const float px = ccx + std::cos(a) * tooth_r, py = ccy + std::sin(a) * tooth_r;
			push_rect(px - tooth_half, py - tooth_half, tooth_half * 2.0f, tooth_half * 2.0f,
					  kGearInk, 0);
		}
		push_circle(ccx, ccy, rr * 0.46f, kGearInk);
		push_circle(ccx, ccy, rr * 0.30f, kGearBg);
		push_circle(ccx, ccy, rr * 0.14f, kGearInk);
	}
	// Settings panel
	if (chrome && gui.show_settings()) {
		// Dim the game underneath so the panel reads as a focused surface.
		push_rect(0, 0, static_cast<float>(swapchain_extent.width),
				  static_cast<float>(swapchain_extent.height), 0xB4000000, 0);

		const gb::UiTheme &thm = gui.ui_theme();
		const bool light = gui.settings() != nullptr && gui.settings()->theme == 1;
		const u32 kAccent = GUIConsole::color_for_console(gui.console_color());
		const u32 kAccentDim = GUIConsole::color_for_console_dim(gui.console_color());
		const u32 kPanelInner = thm.panel_inner;
		const u32 kPanelEdge = light ? 0xFFC4C4D2 : 0xFF3C3C4C;
		const u32 kRowBg = thm.row_bg;
		const u32 kPillBg = thm.pill_bg;
		const u32 kText = thm.text;
		const u32 kTextMute = thm.text_mute;
		const u32 kOk = thm.ok;
		const u32 kHover = light ? 0x16000000 : 0x1CFFFFFF;
		const u32 kFooterBg = light ? 0xFFE4E4EC : 0xFF1A1A22;

		const auto panel_rect = gui.settings_panel_rect();
		const float panel_w = panel_rect.w, panel_h = panel_rect.h;
		const float panel_x = panel_rect.x, panel_y = panel_rect.y;

		// Soft drop shadow, an edge, then the inner fill. Radii are real now
		// (push_rect builds rounded corners), which makes the panel feel like
		// a surface instead of a stack of flat boxes.
		push_rect(panel_x + 6, panel_y + 8, panel_w, panel_h, 0x50000000, 14.0f);
		push_rect(panel_x, panel_y, panel_w, panel_h, kPanelEdge, 12.0f);
		push_rect(panel_x + 2, panel_y + 2, panel_w - 4, panel_h - 4, kPanelInner, 10.0f);

		const float tscale = gui.settings_text_scale();
		const float th = 8.0f * tscale;
		const auto clip = [&](const std::string &s, float width, float scale) {
			const float cw = 8.0f * scale;
			if (width <= cw + 4.0f)
				return std::string();
			const size_t maxc = static_cast<size_t>((width - 4.0f) / cw);
			if (s.size() <= maxc)
				return s;
			if (maxc <= 3)
				return s.substr(0, maxc);
			return s.substr(0, maxc - 3) + "...";
		};
		float mouse_x = 0.0f, mouse_y = 0.0f;
		SDL_GetMouseState(&mouse_x, &mouse_y);
		const auto hovering = [&](const GUIConsole::Rect &r) {
			return mouse_x >= r.x && mouse_x < r.x + r.w && mouse_y >= r.y && mouse_y < r.y + r.h;
		};
		const auto text_y = [&](const GUIConsole::Rect &r, float ts) {
			return r.y + (r.h - 8.0f * ts) * 0.5f;
		};

		// ---- title bar ----
		const float title_h = std::clamp(panel_h * 0.075f, 18.0f, 30.0f);
		const float bar_top = panel_y + 4.0f;
		push_rect(panel_x + 4, bar_top, panel_w - 8, title_h, kAccent, 8.0f);
		push_rect(panel_x + 4, bar_top, panel_w - 8, std::max(2.0f, title_h * 0.16f), 0x22FFFFFF,
				  8.0f);
		const float title_scale = std::clamp(tscale * 1.18f, 1.0f, 1.35f);
		push_text(panel_x + 16, bar_top + (title_h - 8.0f * title_scale) * 0.5f, "SETTINGS",
				  title_scale, 0xFFFFFFFF);
		{
			const auto sr = gui.settings_close_rect();
			const bool hot = hovering(sr);
			push_rect(sr.x, sr.y, sr.w, sr.h, hot ? 0xFFD05064 : 0xA0302030, 5.0f);
			float tx = 0, ty = 0;
			center_glyph('X', 1.0f, sr, tx, ty);
			push_text(tx, ty, "X", 1.0f, hot ? 0xFFFFFFFF : 0xFFE8C0C8);
		}

		// ---- tabs ----
		{
			auto draw_tab = [&](GUIConsole::SettingsTab t, const std::string &name) {
				const auto r = gui.settings_tab_rect(t);
				const bool active = gui.settings_tab() == t;
				const bool hot = hovering(r);
				push_rect(r.x, r.y, r.w, r.h, active ? kAccent : kRowBg, r.h * 0.5f);
				if (!active && hot)
					push_rect(r.x, r.y, r.w, r.h, kHover, r.h * 0.5f);
				float ts = tscale;
				while (ts > 0.70f && name.size() * 8.0f * ts > r.w - 12.0f)
					ts -= 0.05f;
				const float tw = measure_text(name, ts);
				push_text(r.x + (r.w - tw) * 0.5f, r.y + (r.h - 8.0f * ts) * 0.5f, name, ts,
						  active ? 0xFFFFFFFF : kText);
			};
			draw_tab(GUIConsole::SettingsTab::General, "GENERAL");
			draw_tab(GUIConsole::SettingsTab::Inputs, "INPUTS");
		}

		// ---- General page ----
		const auto rows = gui.settings_rows();
		if (gui.settings_tab() == GUIConsole::SettingsTab::General) {
			for (const auto &row : rows) {
				using Style = GUIConsole::RowStyle;
				const GUIConsole::Rect &lr = row.label_rect;
				if (row.style == Style::Header) {
					const float hts = std::max(0.78f, tscale * 0.90f);
					const float hy = text_y(lr, hts);
					push_rect(lr.x + 1, hy + 1, 2.5f, 8.0f * hts - 2.0f, kAccent, 1.25f);
					const std::string shown = clip(row.label, lr.w - 20.0f, hts);
					push_text(lr.x + 9, hy, shown, hts, kTextMute);
					const float line_x = lr.x + 9.0f + measure_text(shown, hts) + 10.0f;
					if (line_x < lr.x + lr.w - 4.0f)
						push_rect(line_x, hy + 8.0f * hts * 0.5f, lr.x + lr.w - line_x, 1.0f,
								  kRowBg, 0);
					continue;
				}
				const bool hot = hovering(lr) || hovering(row.value_rect);
				push_rect(lr.x, lr.y, lr.w, lr.h, row.selected ? kAccentDim : kRowBg, 5.0f);
				if (hot)
					push_rect(lr.x, lr.y, lr.w, lr.h, kHover, 5.0f);
				if (row.selected)
					push_rect(lr.x, lr.y, 3.0f, lr.h, kAccent, 1.5f);

				const GUIConsole::Rect v = row.value_rect;
				if (v.w > 2.0f && v.h > 2.0f) {
					push_rect(v.x - 1.0f, v.y - 1.0f, v.w + 2.0f, v.h + 2.0f, kPanelEdge, 5.0f);
					push_rect(v.x, v.y, v.w, v.h, row.selected ? kAccent : kPillBg, 4.0f);
				}
				if (row.id == "volume") {
					const float vol = gui.settings() ? gui.settings()->volume : 1.0f;
					const float inner_w = std::max(0.0f, v.w - 4.0f);
					if (vol > 0.0f)
						push_rect(v.x + 2.0f, v.y + 2.0f, inner_w * vol, v.h - 4.0f, kOk, 3.0f);
					const float mark_x = v.x + 2.0f + inner_w * vol - 1.0f;
					push_rect(mark_x, v.y + 2.0f, 2.0f, v.h - 4.0f, 0xFFFFFFFF, 1.0f);
				}

				// A cramped row (very small windows) shrinks its own text instead
				// of drawing glyphs taller than the pill that contains them.
				const float title_ts =
					std::clamp(std::min(tscale, (lr.h - 4.0f) / 8.0f), 0.70f, tscale);
				const bool show_desc = !row.description.empty() && lr.h >= th * 2.65f;
				const float ty = show_desc ? lr.y + 4.0f : text_y(lr, title_ts);
				const float label_w = std::max(12.0f, v.x - lr.x - 12.0f);
				push_text(lr.x + 8.0f, ty, clip(row.label, label_w, title_ts), title_ts,
						  row.selected ? 0xFFFFFFFF : kText);
				if (show_desc) {
					const float dts = std::max(0.76f, tscale * 0.82f);
					push_text(lr.x + 8.0f, ty + th + 2.0f, clip(row.description, lr.w - 16.0f, dts),
							  dts, kTextMute);
				}
				if (!row.value.empty() && v.w > 4.0f) {
					const u32 vc = row.selected ? 0xFFFFFFFF : (row.unbound ? kTextMute : kText);
					float vts = std::clamp(std::min(tscale, (v.h - 2.0f) / 8.0f), 0.70f, tscale);
					const float avail = v.w - 8.0f;
					// Keep values legible: shrink a little for long labels, then
					// ellipsize instead of collapsing to an unreadable size.
					while (vts > 0.85f && measure_text(row.value, vts) > avail)
						vts -= 0.05f;
					const std::string shown = clip(row.value, avail, vts);
					const float vw = measure_text(shown, vts);
					push_text(v.x + std::max(2.0f, (v.w - vw) * 0.5f),
							  v.y + (v.h - 8.0f * vts) * 0.5f, shown, vts, vc);
				}
			}
		} else {
			// ---- Inputs page ----
			const auto &pad = gui.gbaPad();
			const auto widget = [&](const GUIConsole::Rect &r, const std::string &label, u32 bg,
									u32 fg, bool caret, bool enabled) {
				const bool hot = enabled && hovering(r);
				push_rect(r.x, r.y, r.w, r.h, bg, 5.0f);
				if (hot)
					push_rect(r.x, r.y, r.w, r.h, kHover, 5.0f);
				if (!enabled)
					push_rect(r.x, r.y, r.w, r.h, light ? 0x30FFFFFF : 0x30000000, 5.0f);
				const float pad_l = 6.0f;
				const float avail = r.w - pad_l * 2.0f - (caret ? 12.0f : 0.0f);
				push_text(r.x + pad_l, r.y + (r.h - th) * 0.5f, clip(label, avail, tscale), tscale,
						  fg);
				if (caret) {
					const float cx = r.x + r.w - 9.0f;
					const float cy = r.y + r.h * 0.5f;
					push_rect(cx - 4.0f, cy - 1.0f, 8.0f, 2.0f, fg, 1.0f);
					push_rect(cx - 2.0f, cy + 1.0f, 4.0f, 2.0f, fg, 1.0f);
				}
			};
			const auto dropdown = [&](const GUIConsole::Dropdown &dd, const std::string &value,
									  bool has_value) {
				widget(dd.closed, has_value ? value : "Keyboard only", dd.open ? kAccent : kPillBg,
					   dd.enabled ? (dd.open ? 0xFFFFFFFF : kText) : kTextMute, true, dd.enabled);
			};

			// Profile row.
			push_text(pad.label_profile.x, text_y(pad.label_profile, tscale * 0.9f), "Profile",
					  tscale * 0.9f, kTextMute);
			dropdown(gui.profile_box(),
					 gui.settings() ? gui.settings()->profileName(gui.settings()->active_profile)
									: std::string(),
					 true);
			widget(gui.btn_load(), "Load", kPillBg, kText, false, true);
			widget(gui.btn_save(), "Save", kPillBg, kText, false, true);

			// Device row.
			push_text(pad.label_device.x, text_y(pad.label_device, tscale * 0.9f), "Device",
					  tscale * 0.9f, kTextMute);
			dropdown(gui.device_box(), gui.deviceName(), true);

			if (gui.automapVisible()) {
				const auto &b = gui.btn_automap();
				const std::string st = gui.controllerStatus();
				const bool done = (st == "mapped" || st == "defaults") &&
								  gui.deviceIsController() == (st == "mapped");
				widget(b, done ? "Applied" : gui.autoMapLabel(), done ? 0xFF24543A : kAccent,
					   0xFFFFFFFF, false, true);
			}

			// Clickable binding list: the compact, unambiguous companion to
			// the GBA drawing (and the only remap surface on narrow panels).
			const auto binds = gui.binding_rows();
			if (!binds.empty()) {
				const float hts = std::max(0.78f, tscale * 0.88f);
				const float list_top = binds.front().rect.y;
				push_text(binds.front().rect.x + 2.0f, list_top - 13.0f, "BUTTON BINDINGS", hts,
						  kTextMute);
				for (const auto &b : binds) {
					const bool hot = hovering(b.rect);
					push_rect(b.rect.x, b.rect.y, b.rect.w, b.rect.h,
							  b.selected ? kAccentDim : kRowBg, 4.0f);
					if (hot)
						push_rect(b.rect.x, b.rect.y, b.rect.w, b.rect.h, kHover, 4.0f);
					if (b.selected)
						push_rect(b.rect.x, b.rect.y, 2.5f, b.rect.h, kAccent, 1.25f);
					const float rhs_w = std::max(34.0f, b.rect.w * 0.46f);
					const float rhs_x = b.rect.x + b.rect.w - rhs_w - 2.0f;
					push_text(b.rect.x + 6.0f, b.rect.y + (b.rect.h - th) * 0.5f,
							  clip(b.label, rhs_x - b.rect.x - 10.0f, tscale), tscale,
							  b.selected ? 0xFFFFFFFF : kText);
					push_rect(rhs_x, b.rect.y + 1.5f, rhs_w, b.rect.h - 3.0f,
							  b.selected ? kAccent : kPillBg, 3.0f);
					float vts = std::max(0.82f, tscale * 0.92f);
					while (vts > 0.80f && measure_text(b.value, vts) > rhs_w - 8.0f)
						vts -= 0.05f;
					const std::string bshown = clip(b.value, rhs_w - 8.0f, vts);
					const float vw = measure_text(bshown, vts);
					push_text(rhs_x + std::max(2.0f, (rhs_w - vw) * 0.5f),
							  b.rect.y + (b.rect.h - 8.0f * vts) * 0.5f, bshown, vts,
							  b.selected ? 0xFFFFFFFF : (b.unbound ? kTextMute : kText));
				}
			}

			// GBA outline.
			if (gui.inputs_diagram_visible() && !gui.anyDropdownOpen()) {
				const auto &g = pad;
				const float bw = g.body.w, bh = g.body.h;
				const auto PX = [&](float xmm) {
					return xmm / 144.5f * bw; // AGB-001 is 144.5 x 82 mm
				};
				const auto PY = [&](float ymm) { return ymm / 82.0f * bh; };
				push_text(g.body.x + 2.0f, g.body.y - 13.0f, "GBA CONTROLS",
						  std::max(0.78f, tscale * 0.88f), kTextMute);

				// Shell: a bezel line over a darker face, plus the wider
				// lower "chin" the AGB has below the control row.
				push_rect(g.body.x, g.body.y, bw, bh, 0xFF474758, 5.0f);
				push_rect(g.body.x + PX(1.6f), g.body.y + PY(1.6f), bw - PX(3.2f), bh - PY(3.2f),
						  0xFF1D1D26, 4.0f);

				// Cartridge slot along the top edge, and the power switch.
				push_rect(g.body.x + PX(44.0f), g.body.y + PY(0.8f), PX(30.0f), PY(2.6f),
						  0xFF111119, 1.0f);
				push_rect(g.body.x + PX(6.0f), g.body.y + PY(1.2f), PX(9.0f), PY(3.0f), 0xFF5A5A6C,
						  1.0f);

				// Screen: a recessed panel with a lit inner area.
				push_rect(g.screen.x, g.screen.y, g.screen.w, g.screen.h, 0xFF0E0E14, 3.0f);
				push_rect(g.screen.x + PX(1.4f), g.screen.y + PY(1.4f), g.screen.w - PX(2.8f),
						  g.screen.h - PY(2.8f), 0xFF1E2C3E, 2.0f);
				push_rect(g.screen.x + PX(1.4f), g.screen.y + PY(1.4f), g.screen.w - PX(2.8f),
						  PY(0.8f), 0xFF2C4058, 0);

				// A / B: rings, matching the moulded circles on the shell.
				const auto face = [&](const GUIConsole::Rect &r, u32 ring) {
					const float cxr = r.x + r.w * 0.5f, cyr = r.y + r.h * 0.5f;
					push_circle(cxr, cyr, r.w * 0.5f, ring);
					push_circle(cxr, cyr, r.w * 0.5f - PX(1.2f), 0xFF23232D);
				};
				face(g.btn_a, 0xFF55566A);
				face(g.btn_b, 0xFF55566A);

				// D-pad: a cross with a rounded hub.
				push_rect(g.dpad_h.x, g.dpad_h.y, g.dpad_h.w, g.dpad_h.h, 0xFF55566A, 3.0f);
				push_rect(g.dpad_v.x, g.dpad_v.y, g.dpad_v.w, g.dpad_v.h, 0xFF55566A, 3.0f);
				push_circle(g.dpad_h.x + g.dpad_h.w * 0.5f, g.dpad_h.y + g.dpad_h.h * 0.5f,
							g.dpad_h.h * 0.55f, 0xFF55566A);
				push_circle(g.dpad_h.x + g.dpad_h.w * 0.5f, g.dpad_h.y + g.dpad_h.h * 0.5f,
							g.dpad_h.h * 0.30f, 0xFF23232D);

				// START / SELECT: the two angled pills under the screen.
				push_rect(g.btn_select.x, g.btn_select.y, g.btn_select.w, g.btn_select.h,
						  0xFF55566A, 3.0f);
				push_rect(g.btn_start.x, g.btn_start.y, g.btn_start.w, g.btn_start.h, 0xFF55566A,
						  3.0f);
				// L / R shoulder tabs.
				push_rect(g.btn_l.x, g.btn_l.y, g.btn_l.w, g.btn_l.h, 0xFF55566A, 3.0f);
				push_rect(g.btn_r.x, g.btn_r.y, g.btn_r.w, g.btn_r.h, 0xFF55566A, 3.0f);

				// Speaker: six holes on a diagonal, as on the real shell.
				for (int i = 0; i < 6; i++) {
					const float hx = g.body.x + PX(113.0f + static_cast<float>(i) * 3.4f);
					const float hy = g.body.y + PY(52.0f + static_cast<float>(i) * 3.4f);
					push_circle(hx, hy, PX(0.9f), 0xFF111119);
				}

				// Letter labels.
				const auto tag = [&](const GUIConsole::Rect &r, const char *t) {
					const float tw = measure_text(t, tscale);
					push_text(r.x + (r.w - tw) * 0.5f, r.y + (r.h - th) * 0.5f, t, tscale, kText);
				};
				// SELECT/START are only a few millimetres wide here; their
				// names are already in the binding list, so drawing them on
				// the pills only produced overlapping text.
				tag(g.btn_a, "A");
				tag(g.btn_b, "B");
				tag(g.btn_l, "L");
				tag(g.btn_r, "R");
				{
					const float tw = measure_text("SCREEN", tscale);
					push_text(g.screen.x + (g.screen.w - tw) * 0.5f,
							  g.screen.y + (g.screen.h - th) * 0.5f, "SCREEN", tscale, kTextMute);
				}

				// Highlight the armed button.
				if (gui.awaiting_input()) {
					const u32 hl = kAccent;
					const auto ring = [&](const GUIConsole::Rect &r, bool circle) {
						if (circle)
							push_circle(r.x + r.w * 0.5f, r.y + r.h * 0.5f, r.w * 0.5f + 2.0f, hl);
						else
							push_rect(r.x - 2.0f, r.y - 2.0f, r.w + 4.0f, r.h + 4.0f, hl, 3.0f);
					};
					switch (gui.pending_pad()) {
					case gb::PadButton::A:
						ring(g.btn_a, true);
						break;
					case gb::PadButton::B:
						ring(g.btn_b, true);
						break;
					case gb::PadButton::Start:
						ring(g.btn_start, false);
						break;
					case gb::PadButton::Select:
						ring(g.btn_select, false);
						break;
					case gb::PadButton::L:
						ring(g.btn_l, false);
						break;
					case gb::PadButton::R:
						ring(g.btn_r, false);
						break;
					case gb::PadButton::Up:
						push_rect(g.dpad_v.x - 2.0f, g.dpad_v.y - 2.0f, g.dpad_v.w + 4.0f,
								  g.dpad_v.h * 0.5f + 2.0f, hl, 2.0f);
						break;
					case gb::PadButton::Down:
						push_rect(g.dpad_v.x - 2.0f, g.dpad_v.y + g.dpad_v.h * 0.5f - 2.0f,
								  g.dpad_v.w + 4.0f, g.dpad_v.h * 0.5f + 2.0f, hl, 2.0f);
						break;
					case gb::PadButton::Left:
						push_rect(g.dpad_h.x - 2.0f, g.dpad_h.y - 2.0f, g.dpad_h.w * 0.5f + 2.0f,
								  g.dpad_h.h + 4.0f, hl, 2.0f);
						break;
					case gb::PadButton::Right:
						push_rect(g.dpad_h.x + g.dpad_h.w * 0.5f - 2.0f, g.dpad_h.y - 2.0f,
								  g.dpad_h.w * 0.5f + 2.0f, g.dpad_h.h + 4.0f, hl, 2.0f);
						break;
					default:
						break;
					}
				}
			}

			// Drop-down lists overlay all page content.
			const auto draw_list = [&](const GUIConsole::Dropdown &dd) {
				if (!dd.open)
					return;
				const bool profile_list = &dd == &gui.profile_box();
				for (size_t i = 0; i < dd.items.size(); i++) {
					const auto &r = dd.items[i];
					const bool sel = static_cast<int>(i) == dd.selected;
					const int idx = static_cast<int>(i);
					const bool avail = !profile_list || gui.profileMenuEnabled(idx);
					const bool hot = hovering(r);
					push_rect(r.x, r.y, r.w, r.h, sel ? kAccent : kPillBg, 3.0f);
					if (hot && !sel)
						push_rect(r.x, r.y, r.w, r.h, kHover, 3.0f);
					const std::string &t = i < dd.options.size() ? dd.options[i] : std::string();
					const u32 fg = !avail ? kTextMute : (sel ? 0xFFFFFFFF : kText);
					push_text(r.x + 6, r.y + (r.h - th) * 0.5f, clip(t, r.w - 12.0f, tscale),
							  tscale, fg);
				}
			};
			draw_list(gui.profile_box());
			draw_list(gui.device_box());
		}

		// ---- footer hint ----
		{
			const bool inputs = gui.settings_tab() == GUIConsole::SettingsTab::Inputs;
			const char *full =
				inputs ? "CLICK A BINDING, THEN PRESS A KEY OR PAD BUTTON  -  ESC CLOSES"
					   : "CLICK A ROW TO CHANGE IT  -  DRAG VOLUME  -  S/ESC CLOSES";
			const char *medium = inputs ? "CLICK A BINDING, PRESS A KEY OR BUTTON - ESC CLOSES"
										: "CLICK A ROW TO CHANGE - ESC CLOSES";
			const char *brief = inputs ? "CLICK BINDING - ESC CLOSES" : "CLICK ROW - ESC CLOSES";
			const float footer_h = std::clamp(panel_h * 0.06f, 12.0f, 20.0f);
			const float fy = panel_y + panel_h - footer_h - 4.0f;
			push_rect(panel_x + 8, fy, panel_w - 16, footer_h, kFooterBg, 4.0f);
			const float fts = std::max(0.72f, tscale * 0.82f);
			const float avail = panel_w - 28.0f;
			// Prefer the complete hint, but step down instead of cutting the
			// closing "ESC CLOSES" off at narrow window sizes.
			const char *hint = full;
			if (measure_text(hint, fts) > avail)
				hint = medium;
			if (measure_text(hint, fts) > avail)
				hint = brief;
			push_text(panel_x + 14, fy + (footer_h - 8.0f * fts) * 0.5f, clip(hint, avail, fts),
					  fts, kTextMute);
		}
	}
	// Profile-naming prompt. Mutually exclusive with the remap modal above, so
	// it can reuse the same box shape and centre on the body.
	// Hidden in fullscreen (like all chrome).
	if (chrome && gui.text_entry_active()) {
		push_rect(0, 0, static_cast<float>(swapchain_extent.width),
				  static_cast<float>(swapchain_extent.height), 0xAA000000, 0);
		const float modal_w = body.w * 0.6f, modal_h = body.h * 0.12f;
		const float modal_x = body.x + (body.w - modal_w) * 0.5f;
		const float modal_y = body.y + (body.h - modal_h) * 0.5f;
		push_rect(modal_x, modal_y, modal_w, modal_h, 0xFFFFFFFF, 6.0f);
		push_rect(modal_x + 2, modal_y + 2, modal_w - 4, modal_h - 4, 0xFF202020, 6.0f);
		const std::string line1 = gui.text_prompt();
		// A block cursor makes it obvious the field is editable.
		std::string typed = gui.text_value() + "_";
		const std::string hint = "Enter accepts  Esc cancels";
		const float cx = modal_x + modal_w * 0.5f;
		const float cy = modal_y + modal_h * 0.5f;
		push_text(cx - measure_text(line1, 1.0f) * 0.5f, cy - 20, line1, 1.0f, 0xFFAAAAAA);
		push_text(cx - measure_text(typed, 1.0f) * 0.5f, cy - 4, typed, 1.0f, 0xFFFFFFFF);
		push_text(cx - measure_text(hint, 1.0f) * 0.5f, cy + 12, hint, 1.0f, 0xFFAAAAAA);
	}
	// Status OSD: one-line confirmation above the volume pill's spot (offset
	// up while the volume pill is also visible, so the two never overlap).
	if (status_osd_active()) {
		const float ww = static_cast<float>(gui.window_w());
		const float wh = static_cast<float>(gui.window_h());
		const float pw = measure_text(osd_text_.c_str(), 1.0f) + 32.0f, ph = 30.0f;
		const float px = (ww - pw) * 0.5f;
		float py = wh - ph - 16.0f;
		if (volume_osd_active())
			py -= 46.0f;
		push_rect(px, py, pw, ph, 0xCC101418, 6.0f);
		push_text(px + (pw - measure_text(osd_text_.c_str(), 1.0f)) * 0.5f, py + 5,
				  osd_text_.c_str(), 1.0f, 0xFFFFFFFF);
	}
	// Volume OSD: transient pill after -/=, above everything else (even the
	// settings panel, whose own slider already shows the live value).
	if (volume_osd_active()) {
		const float ww = static_cast<float>(gui.window_w());
		const float wh = static_cast<float>(gui.window_h());
		const float pw = 220.0f, ph = 38.0f;
		const float px = (ww - pw) * 0.5f, py = wh - ph - 16.0f;
		push_rect(px, py, pw, ph, 0xCC101418, 6.0f);
		char vbuf[32];
		std::snprintf(vbuf, sizeof(vbuf), "VOLUME %d%%",
					  static_cast<int>(osd_volume_ * 100.0f + 0.5f));
		push_text(px + (pw - measure_text(vbuf, 1.0f)) * 0.5f, py + 5, vbuf, 1.0f, 0xFFFFFFFF);
		const float bx = px + 12, bw = pw - 24, bh = 6.0f, by = py + ph - 13.0f;
		push_rect(bx, by, bw, bh, 0xFF32323F, 2.0f);
		if (osd_volume_ > 0.0f)
			push_rect(bx, by, bw * osd_volume_, bh, 0xFF4FB477, 2.0f);
	}
	// Headless budget probe (no Vulkan upload): GB4ME_GUI_COUNT=1 prints the
	// overlay size so overflow can be diagnosed without a display.
#ifndef GB4ME_RELEASE
	if (std::getenv("GB4ME_GUI_COUNT") != nullptr) {
		std::fprintf(stderr, "gui-overlay verts=%zu indices=%zu text_verts=%zu text_indices=%zu\n",
					 verts.size(), indices.size(), text_verts.size(), text_indices.size());
		return;
	}
#endif
	// Grow the buffers if this frame's overlay needs more room, then upload.
	if (!ensure_gui_buffers(static_cast<u32>(verts.size()), static_cast<u32>(indices.size())))
		return;
	void *vdst = nullptr;
	vkMapMemory(device, gui_vertex_buffer_memory, 0, verts.size() * sizeof(GuiVertex), 0, &vdst);
	std::memcpy(vdst, verts.data(), verts.size() * sizeof(GuiVertex));
	vkUnmapMemory(device, gui_vertex_buffer_memory);
	void *idst = nullptr;
	vkMapMemory(device, gui_index_buffer_memory, 0, indices.size() * sizeof(u32), 0, &idst);
	std::memcpy(idst, indices.data(), indices.size() * sizeof(u32));
	vkUnmapMemory(device, gui_index_buffer_memory);
	gui_index_count = static_cast<u32>(indices.size());
	build_text_buffers(text_verts, text_indices);
}

} // namespace gb
