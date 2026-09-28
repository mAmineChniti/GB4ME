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
	// Build vertex/index data for GUI rects (body, bezel, buttons, etc.)
	// Indices are 32-bit: long ROM lists generate tens of thousands of
	// vertices, and 16-bit indices would wrap and silently drop the tail
	// of the overlay (footer text, gear/open buttons).
	std::vector<GuiVertex> verts;
	std::vector<u32> indices;
	std::vector<TextVertex> text_verts;
	std::vector<u32> text_indices;
	auto push_rect = [&](float x, float y, float w, float h, u32 rgba, float rounding) {
		// Rounded rectangle: for now, just axis-aligned rect (rounding ignored, but keep param for
		// future). Convert to NDC. Layout rects live in WINDOW coordinates (same space as
		// mouse events; see handle_events), so NDC must use the window size — NOT the
		// swapchain (physical pixel) size, which differs on HiDPI/fractional-scale
		// displays. (NDC is viewport-relative, so window units are correct here; the
		// record functions separately scale the photo/game viewports to pixels.)
		float win_w = static_cast<float>(gui.window_w());
		float win_h = static_cast<float>(gui.window_h());
		auto to_ndc = [&](float px, float py) -> Vec2 {
			// Window pixels use top-left origin (y down). Vulkan NDC y=-1 maps
			// to the framebuffer top, so py=0 must yield -1 (NOT 1.0f - ...,
			// which rendered the whole GUI upside down).
			return {(px / win_w) * 2.0f - 1.0f, (py / win_h) * 2.0f - 1.0f};
		};
		(void) rounding;
		Vec2 p0 = to_ndc(x, y);
		Vec2 p1 = to_ndc(x + w, y);
		Vec2 p2 = to_ndc(x + w, y + h);
		Vec2 p3 = to_ndc(x, y + h);
		Vec4 col = {((rgba >> 16) & 0xFF) / 255.0f, ((rgba >> 8) & 0xFF) / 255.0f,
					(rgba & 0xFF) / 255.0f, ((rgba >> 24) & 0xFF) / 255.0f};
		// Reorder to match shader's RGBA order? Our Vec4 is x,y,z,w as r,g,b,a.
		u32 base = static_cast<u32>(verts.size());
		verts.push_back({p0, {col.x, col.y, col.z, col.w}});
		verts.push_back({p1, {col.x, col.y, col.z, col.w}});
		verts.push_back({p2, {col.x, col.y, col.z, col.w}});
		verts.push_back({p3, {col.x, col.y, col.z, col.w}});
		indices.push_back(base + 0);
		indices.push_back(base + 1);
		indices.push_back(base + 2);
		indices.push_back(base + 2);
		indices.push_back(base + 3);
		indices.push_back(base + 0);
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
	if (gui.menu_active()) {
		auto scr = gui.screen_rect();
		push_rect(scr.x, scr.y, scr.w, scr.h, 0xFF101418, 0);
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
		const std::string f2 = "O:OPEN FILE S:SETTINGS R:RESCAN";
		ui_scale = std::min(ui_scale, scr.w / ((f2.size() + 1.0f) * 8.0f)); // f2 width
		const float footer_h = std::max(26.0f, scr.h * 0.14f);
		ui_scale = std::min(ui_scale, (footer_h - 7.0f) / 16.0f); // 4+8s+3+8s block
		const float title_h = std::max(20.0f, scr.h * 0.10f);
		const float list_pad = scr.w * 0.045f; // mirrors menu_list_rect
		ui_scale = std::min(ui_scale, (title_h + list_pad - 6.0f) / 20.0f);
		ui_scale = std::max(ui_scale, 0.50f);
		const auto &roms = gui.rom_list();
		const float cx = scr.x + scr.w * 0.5f;
		push_text(cx - measure_text("GB4ME", ui_scale) * 0.5f, scr.y + 6, "GB4ME", ui_scale,
				  0xFFFFFFFF);
		const std::string base_sub = "SELECT ROM (" + std::to_string(roms.size()) + ")";
		const std::string sub =
			n_recent > 0 ? base_sub + " - 1-" + std::to_string(n_recent) + " PLAYS RECENT"
						 : base_sub;
		push_text(cx - measure_text(sub, ui_scale) * 0.5f, scr.y + 6 + 12.0f * ui_scale, sub,
				  ui_scale, 0xFFAAAAAA);
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
			if (i < 0 || i >= static_cast<int>(roms.size()))
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
			push_rect(cx, cy, cw, ch, sel ? 0xFF3A3A8F : 0xFF22252B, 3.0f);
			if (sel)
				push_rect(cx + 1, cy + 1, cw - 2, ch - 2, 0xFF2E3190, 3.0f);
			// System badge — top-right inside cell
			const auto &rom = roms[static_cast<size_t>(i)];
			const std::string badge = rom.gba ? "GBA" : (rom.cgb ? "CGB" : "DMG");
			const u32 bcol = rom.gba ? 0xFF4F7FFF : (rom.cgb ? 0xFF3FA06B : 0xFF5A5A5A);
			const float bx = cx + cw - badge_w - 4;
			const float by = cy + 4;
			push_rect(bx - 2, by, badge_w, badge_h, bcol, 2.0f);
			push_text(bx, by + 1, badge, 1.0f, 0xFFFFFFFF);
			// Large cartridge icon — centered top of cell
			const float icon_w = std::clamp(cw * 0.38f, 28.0f, 42.0f);
			const float icon_h = icon_w * 0.92f;
			const float ix = cx + (cw - icon_w) * 0.5f;
			const float iy = cy + 8;
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
			// Title — centered below icon, wrapped to 2 lines max
			std::string title = roms[static_cast<size_t>(i)].title;
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
				push_text(tx, ty, shown, tscale, sel ? 0xFFFFFFFF : 0xFFD8D8D8);
			}
		}
		if (roms.empty()) {
			const std::string e1 = "NO ROMS IN FOLDER";
			push_text(cx - measure_text(e1, ui_scale) * 0.5f, list.y + 10, e1, ui_scale,
					  0xFFAAAAAA);
			std::string folder = gui.settings() ? gui.settings()->rom_folder : ".";
			if (folder.size() > 24)
				folder = "..." + folder.substr(folder.size() - 21);
			push_text(cx - measure_text(folder, ui_scale) * 0.5f, list.y + 10 + 14.0f * ui_scale,
					  folder, ui_scale, 0xFF999999);
		}
		// scroll indicators — grid scrolls by row, show track + arrows
		if (first > 0)
			push_text(list.x + list.w - 12, list.y - 10, "^", ui_scale, 0xFFAAAAAA);
		if (first + vis < static_cast<int>(roms.size())) {
			push_text(list.x + list.w - 12, list.y + list.h + 2, "v", ui_scale, 0xFFAAAAAA);
		}
		// vertical scrollbar for large libraries (50+ games)
		if (static_cast<int>(roms.size()) > vis) {
			const float track_x = list.x + list.w - 4;
			const float track_y = list.y;
			const float track_h = list.h;
			push_rect(track_x, track_y, 3.0f, track_h, 0xFF1E2025, 1.0f);
			const float frac_vis = static_cast<float>(vis) / roms.size();
			const float frac_first = static_cast<float>(first) / roms.size();
			const float thumb_h = std::clamp(track_h * frac_vis, 12.0f, track_h);
			const float thumb_y = track_y + frac_first * track_h;
			const float clamped_thumb_y = std::min(thumb_y, track_y + track_h - thumb_h);
			push_rect(track_x, clamped_thumb_y, 3.0f, thumb_h, 0xFF6B6B8A, 1.0f);
		}
		// footer hints — grid navigation (scaled with the window; anchored
		// to the bottom so the two lines never overlap each other).
		const std::string f1 = "ARROWS:MOVE ENTER:PLAY";
		const float f2_y = scr.y + scr.h - 4.0f - 8.0f * ui_scale;
		const float f1_y = f2_y - 8.0f * ui_scale - 3.0f;
		push_text(cx - measure_text(f1, ui_scale) * 0.5f, f1_y, f1, ui_scale, 0xFFAAAAAA);
		push_text(cx - measure_text(f2, ui_scale) * 0.5f, f2_y, f2, ui_scale, 0xFFAAAAAA);
	}
	// OPEN button (file dialog, top-left) + settings gear (top-right).
	// The photo has neither; both stay clickable (also O / S keys).
	{
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
	{
		auto setr = gui.settings_button_rect();
		push_circle(setr.x + setr.w * 0.5f, setr.y + setr.h * 0.5f, setr.w * 0.5f, 0xFF5A5A5A);
		// Glyph sized to the button with its ink centered (an em-box center
		// would sit high: '*' has no descender).
		const float gs = setr.w / 14.0f;
		float tx = 0, ty = 0;
		center_glyph('*', gs, setr, tx, ty);
		push_text(tx, ty, "*", gs, 0xFFFFFFFF);
	}
	// Settings panel
	if (gui.show_settings()) {
		// Darken background
		push_rect(0, 0, static_cast<float>(swapchain_extent.width),
				  static_cast<float>(swapchain_extent.height), 0xAA000000, 0);
		// ---- Settings panel ----
		// One accent (the console purple), a neutral ramp and a semantic
		// green, kept local so the page reads as a single design.
		constexpr u32 kPanelOuter = 0xFF2A2A33;
		constexpr u32 kPanelInner = 0xFF16161C;
		constexpr u32 kAccent = 0xFF6B3FA0;
		constexpr u32 kAccentDim = 0xFF4A3266;
		constexpr u32 kRowBg = 0xFF23232D;
		constexpr u32 kPillBg = 0xFF32323F;
		constexpr u32 kText = 0xFFECECF2;
		// Values and navigation read at full brightness; only captions and
		// placeholders stay dimmed (still >= 3:1 on the panel fills).
		constexpr u32 kTextMute = 0xFF8E8EA3;
		constexpr u32 kOk = 0xFF4FB477;

		// The panel height depends on the active tab, so take the rect from
		// the console rather than recomputing it.
		const auto panel_rect = gui.settings_panel_rect();
		const float panel_w = panel_rect.w, panel_h = panel_rect.h;
		const float panel_x = panel_rect.x, panel_y = panel_rect.y;
		push_rect(panel_x, panel_y, panel_w, panel_h, kPanelOuter, 6.0f);
		push_rect(panel_x + 2, panel_y + 2, panel_w - 4, panel_h - 4, kPanelInner, 6.0f);

		const float tscale = body.w > 420.0f ? 1.0f : 0.6f;
		const float tchar = 8.0f * tscale;
		const float th = 8.0f * tscale;
		const auto fit = [&](const std::string &s, float w) {
			size_t maxc = w > tchar + 6.0f ? static_cast<size_t>((w - 6.0f) / tchar) : 1;
			if (s.size() <= maxc)
				return s;
			if (maxc <= 3)
				return s.substr(0, maxc);
			return std::string("...") + s.substr(s.size() - (maxc - 3));
		};
		const auto text_y = [&](const GUIConsole::Rect &r, float ts) {
			return r.y + (r.h - 8.0f * ts) * 0.5f;
		};

		// Title bar + close button.
		{
			const float bar_h = body.h * 0.042f;
			push_rect(panel_x + 4, panel_y + 5, panel_w - 8, bar_h, kAccent, 3.0f);
			push_text(panel_x + 10, panel_y + 7, "SETTINGS", 1.0f, 0xFFFFFFFF);
			const auto sr = gui.settings_close_rect();
			push_rect(sr.x, sr.y, sr.w, sr.h, 0xFF5A2430, 2.0f);
			float tx = 0, ty = 0;
			center_glyph('X', 1.0f, sr, tx, ty);
			push_text(tx, ty, "X", 1.0f, 0xFFE8A0B0);
		}

		// Tabs, with an underline for the active one.
		{
			auto draw_tab = [&](GUIConsole::SettingsTab t, const std::string &name) {
				const auto r = gui.settings_tab_rect(t);
				const bool active = gui.settings_tab() == t;
				push_rect(r.x, r.y, r.w, r.h, active ? kAccentDim : kRowBg, 2.0f);
				if (active)
					push_rect(r.x + 1, r.y + r.h - 2, r.w - 2, 2, kAccent, 0);
				float ts = 1.0f;
				while (ts > 0.5f && name.size() * 8.0f * ts > r.w - 6.0f)
					ts -= 0.1f;
				const float tw = measure_text(name, ts);
				push_text(r.x + (r.w - tw) * 0.5f, r.y + (r.h - 8.0f * ts) * 0.5f, name, ts,
						  active ? 0xFFFFFFFF : kText);
			};
			draw_tab(GUIConsole::SettingsTab::General, "GENERAL");
			draw_tab(GUIConsole::SettingsTab::Inputs, "INPUTS");
		}

		// Rows: a label field plus an inset value "pill", so the value reads
		// as a control rather than a second column of text.
		const auto rows = gui.settings_rows();
		if (gui.settings_tab() == GUIConsole::SettingsTab::General) {
			for (const auto &row : rows) {
				using Style = GUIConsole::RowStyle;
				if (row.style == Style::Header) {
					push_text(row.label_rect.x + 2, text_y(row.label_rect, tscale), row.label,
							  tscale, kTextMute);
					continue;
				}
				const bool is_action = row.style == Style::Action;
				const u32 row_bg = row.selected ? kAccentDim : (is_action ? 0xFF1E1E28 : kRowBg);
				push_rect(row.label_rect.x, row.label_rect.y, row.label_rect.w, row.label_rect.h,
						  row_bg, 2.0f);
				const GUIConsole::Rect v = row.value_rect;
				const GUIConsole::Rect vp{v.x + 2.0f, v.y + 2.0f, v.w - 4.0f, v.h - 4.0f};
				if (vp.w > 2.0f && vp.h > 2.0f)
					push_rect(vp.x, vp.y, vp.w, vp.h, row.selected ? kAccent : kPillBg, 2.0f);
				if (row.id == "volume") {
					const float vol = gui.settings() ? gui.settings()->volume : 1.0f;
					if (vol > 0.0f)
						push_rect(vp.x, vp.y, vp.w * vol, vp.h, kOk, 2.0f);
					push_rect(vp.x + vp.w * vol - 1.5f, vp.y, 3, vp.h, 0xFFFFFFFF, 0);
				}
				push_text(row.label_rect.x + 5, text_y(row.label_rect, tscale),
						  fit(row.label, row.label_rect.w), tscale,
						  row.selected ? 0xFFFFFFFF : kText);
				if (!row.value.empty()) {
					u32 vc = kText;
					if (row.selected)
						vc = 0xFFFFFFFF;
					else if (row.unbound)
						vc = kTextMute;
					push_text(vp.x + 4, vp.y + (vp.h - th) * 0.5f, fit(row.value, vp.w), tscale,
							  vc);
				}
			}
		} else {
			// ---- Inputs page ----
			const auto &pad = gui.gbaPad();
			// Widget helper: a boxed control with an optional right-hand caret.
			const auto widget = [&](const GUIConsole::Rect &r, const std::string &label, u32 bg,
									u32 fg, bool caret, bool enabled) {
				push_rect(r.x, r.y, r.w, r.h, bg, 2.0f);
				if (!enabled)
					push_rect(r.x, r.y, r.w, r.h, 0x30000000, 0);
				const float pad_l = 4.0f;
				const float avail = r.w - pad_l * 2.0f - (caret ? 10.0f : 0.0f);
				push_text(r.x + pad_l, r.y + (r.h - th) * 0.5f, fit(label, avail), tscale, fg);
				if (caret) {
					// Small down triangle, so the field reads as a combo box.
					const float cx = r.x + r.w - 8.0f;
					const float cy = r.y + r.h * 0.5f;
					push_rect(cx - 4, cy - 1, 8, 2, fg, 0);
					push_rect(cx - 2, cy + 1, 4, 2, fg, 0);
				}
			};
			const auto dropdown = [&](const GUIConsole::Dropdown &dd, const std::string &value,
									  bool has_value) {
				widget(dd.closed, has_value ? value : "Keyboard only", dd.open ? kAccent : kPillBg,
					   dd.enabled ? (dd.open ? 0xFFFFFFFF : kText) : kTextMute, true, dd.enabled);
			};

			// Profile row.
			push_text(pad.label_profile.x + 2, text_y(pad.label_profile, tscale), "Profile", tscale,
					  kText);
			dropdown(gui.profile_box(),
					 gui.settings() ? gui.settings()->profileName(gui.settings()->active_profile)
									: std::string(),
					 true);
			widget(gui.btn_load(), "Load", kPillBg, kText, false, true);
			widget(gui.btn_save(), "Save", kPillBg, kText, false, true);

			// Device row.
			push_text(pad.label_device.x + 2, text_y(pad.label_device, tscale), "Device", tscale,
					  kText);
			dropdown(gui.device_box(), gui.deviceName(), true);

			// Auto-map, only while a controller is the selected source.
			if (gui.automapVisible()) {
				const auto &b = gui.btn_automap();
				const std::string st = gui.controllerStatus();
				const bool done = (st == "mapped" || st == "defaults") &&
								  gui.deviceIsController() == (st == "mapped");
				widget(b, done ? "Applied" : gui.autoMapLabel(), done ? 0xFF24543A : kAccent,
					   0xFFFFFFFF, false, true);
			}

			// Open drop-down lists overlay everything drawn so far.
			const auto draw_list = [&](const GUIConsole::Dropdown &dd) {
				if (!dd.open)
					return;
				const bool profile_list = &dd == &gui.profile_box();
				for (size_t i = 0; i < dd.items.size(); i++) {
					const auto &r = dd.items[i];
					const bool sel = static_cast<int>(i) == dd.selected;
					const int idx = static_cast<int>(i);
					// Unavailable commands are dimmed so it is obvious they
					// need a user profile selected first.
					const bool avail = !profile_list || gui.profileMenuEnabled(idx);
					push_rect(r.x, r.y, r.w, r.h, sel ? kAccent : 0xFF262630, 2.0f);
					const std::string &t = i < dd.options.size() ? dd.options[i] : std::string();
					const u32 fg = !avail ? kTextMute : (sel ? 0xFFFFFFFF : kText);
					push_text(r.x + 4, r.y + (r.h - th) * 0.5f, fit(t, r.w - 8.0f), tscale, fg);
				}
			};
			draw_list(gui.profile_box());
			draw_list(gui.device_box());

			// ---- GBA outline ----
			// Drawn only when no list is open: a drop-down overlays
			// everything, so a diagram behind it would just show through.
			if (!gui.anyDropdownOpen()) {
				const auto &g = pad;
				const float bw = g.body.w, bh = g.body.h;
				const auto PX = [&](float xmm) {
					return xmm / 144.5f * bw; // AGB-001 is 144.5 x 82 mm
				};
				const auto PY = [&](float ymm) { return ymm / 82.0f * bh; };

				// Shell: a bezel line over a darker face, plus the wider
				// lower "chin" the AGB has below the control row.
				push_rect(g.body.x, g.body.y, bw, bh, 0xFF474758, 3.0f);
				push_rect(g.body.x + PX(1.6f), g.body.y + PY(1.6f), bw - PX(3.2f), bh - PY(3.2f),
						  0xFF1D1D26, 3.0f);

				// Cartridge slot along the top edge, and the power switch.
				push_rect(g.body.x + PX(44.0f), g.body.y + PY(0.8f), PX(30.0f), PY(2.6f),
						  0xFF111119, 1.0f);
				push_rect(g.body.x + PX(6.0f), g.body.y + PY(1.2f), PX(9.0f), PY(3.0f), 0xFF5A5A6C,
						  1.0f);

				// Screen: a recessed panel with a lit inner area.
				push_rect(g.screen.x, g.screen.y, g.screen.w, g.screen.h, 0xFF0E0E14, 2.0f);
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
				push_rect(g.dpad_h.x, g.dpad_h.y, g.dpad_h.w, g.dpad_h.h, 0xFF55566A, 2.0f);
				push_rect(g.dpad_v.x, g.dpad_v.y, g.dpad_v.w, g.dpad_v.h, 0xFF55566A, 2.0f);
				push_circle(g.dpad_h.x + g.dpad_h.w * 0.5f, g.dpad_h.y + g.dpad_h.h * 0.5f,
							g.dpad_h.h * 0.55f, 0xFF55566A);
				push_circle(g.dpad_h.x + g.dpad_h.w * 0.5f, g.dpad_h.y + g.dpad_h.h * 0.5f,
							g.dpad_h.h * 0.30f, 0xFF23232D);

				// START / SELECT: the two angled pills under the screen.
				push_rect(g.btn_select.x, g.btn_select.y, g.btn_select.w, g.btn_select.h,
						  0xFF55566A, 2.0f);
				push_rect(g.btn_start.x, g.btn_start.y, g.btn_start.w, g.btn_start.h, 0xFF55566A,
						  2.0f);
				// L / R shoulder tabs.
				push_rect(g.btn_l.x, g.btn_l.y, g.btn_l.w, g.btn_l.h, 0xFF55566A, 2.0f);
				push_rect(g.btn_r.x, g.btn_r.y, g.btn_r.w, g.btn_r.h, 0xFF55566A, 2.0f);

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
				tag(g.btn_a, "A");
				tag(g.btn_b, "B");
				tag(g.btn_start, "START");
				tag(g.btn_select, "SELECT");
				tag(g.btn_l, "L");
				tag(g.btn_r, "R");
				// The screen gets a caption instead of a binding.
				{
					const float tw = measure_text("SCREEN", tscale);
					push_text(g.screen.x + (g.screen.w - tw) * 0.5f,
							  g.screen.y + (g.screen.h - th) * 0.5f, "SCREEN", tscale, kTextMute);
				}

				// Highlight the armed button so the user can see what they are
				// about to rebind.
				if (gui.awaiting_input()) {
					const u32 hl = kAccent;
					const auto ring = [&](const GUIConsole::Rect &r, bool circle) {
						if (circle)
							push_circle(r.x + r.w * 0.5f, r.y + r.h * 0.5f, r.w * 0.5f + 2.0f, hl);
						else
							push_rect(r.x - 2.0f, r.y - 2.0f, r.w + 4.0f, r.h + 4.0f, hl, 1.0f);
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
					// Only the pressed ARM lights up, so the user can see
					// which direction they just bound.
					case gb::PadButton::Up:
						push_rect(g.dpad_v.x - 2.0f, g.dpad_v.y - 2.0f, g.dpad_v.w + 4.0f,
								  g.dpad_v.h * 0.5f + 2.0f, hl, 1.0f);
						break;
					case gb::PadButton::Down:
						push_rect(g.dpad_v.x - 2.0f, g.dpad_v.y + g.dpad_v.h * 0.5f - 2.0f,
								  g.dpad_v.w + 4.0f, g.dpad_v.h * 0.5f + 2.0f, hl, 1.0f);
						break;
					case gb::PadButton::Left:
						push_rect(g.dpad_h.x - 2.0f, g.dpad_h.y - 2.0f, g.dpad_h.w * 0.5f + 2.0f,
								  g.dpad_h.h + 4.0f, hl, 1.0f);
						break;
					case gb::PadButton::Right:
						push_rect(g.dpad_h.x + g.dpad_h.w * 0.5f - 2.0f, g.dpad_h.y - 2.0f,
								  g.dpad_h.w * 0.5f + 2.0f, g.dpad_h.h + 4.0f, hl, 1.0f);
						break;
					default:
						break;
					}
				}
			}
		}
		// Footer hint, worded for the page you are on.
		{
			const bool inputs = gui.settings_tab() == GUIConsole::SettingsTab::Inputs;
			const char *hint = inputs ? "CLICK A GBA BUTTON TO REBIND - ESC CLOSES"
									  : "CLICK A ROW - DRAG VOLUME - S/ESC CLOSES";
			const float fh = std::min(16.0f, panel_h * 0.09f);
			const float fy = panel_y + panel_h - fh - 4.0f;
			push_rect(panel_x + 6, fy, panel_w - 12, fh, 0xFF1B1B23, 2.0f);
			push_text(panel_x + 10, fy + (fh - 5.0f) * 0.5f, hint, 0.6f, kTextMute);
		}
	}
	// Profile-naming prompt. Mutually exclusive with the remap modal above, so
	// it can reuse the same box shape and centre on the body.
	if (gui.text_entry_active()) {
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
		std::fprintf(stderr, "gui-overlay verts=%zu indices=%zu\n", verts.size(), indices.size());
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
