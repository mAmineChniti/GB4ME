// PPU: line-based DMG + CGB renderer.
// Timing: 456 dots/line (114 M-cycles); lines 0-143 visible, 144-153 VBlank.
// Modes: OAM scan (80 dots) -> Drawing (172) -> HBlank (rest).
// https://gbdev.io/pandocs/Rendering.html
// https://gbdev.io/pandocs/Tile_Data.html
// https://gbdev.io/pandocs/Interrupts.html (VBlank vector 0x40, LCD vector 0x48)
// https://gbdev.io/pandocs/CGB_Registers.html
// https://gbdev.io/pandocs/Palettes.html

#include "gb/ppu.h"
#include "gb/mmu.h"

namespace gb
{

namespace
{

constexpr u32 kDotsPerLine = 456;
constexpr u32 kOamDots = 80;
constexpr u32 kDrawDots = 80 + 172;
constexpr u8 kVBlankStart = 144;
constexpr u8 kLinesPerFrame = 154;

u32 rgb555_to_rgba_inner(u16 c)
{
	// 15-bit BGR555 to 32-bit RGBA8 (expand 5->8 bits)
	u8 r = (c & 0x1F) * 8;
	u8 g = ((c >> 5) & 0x1F) * 8;
	u8 b = ((c >> 10) & 0x1F) * 8;
	// Add lsb rounding for better accuracy
	r |= r >> 5;
	g |= g >> 5;
	b |= b >> 5;
	return 0xFF000000u | (static_cast<u32>(r) << 16) | (static_cast<u32>(g) << 8) | b;
}

} // namespace

PPU::PPU()
{
	reset();
}

void PPU::reset()
{
	for (auto &bank : vram_banks_)
		bank.fill(0);
	vram_bank_ = 0;
	oam_.fill(OAMEntry{});
	lcdc_ = 0x91;
	stat_ = 0x85;
	scy_ = 0;
	scx_ = 0;
	ly_ = 0;
	lyc_ = 0;
	dma_ = 0;
	bgp_ = 0xFC;
	obp0_ = 0xFF;
	obp1_ = 0xFF;
	wy_ = 0;
	wx_ = 0;
	current_mode_ = Mode::OAMScan;
	line_cycles_ = 0;
	window_line_ = 0;
	frame_buffer_.pixels.fill(dmg_shades_[0]);
	frame_ready_flag_ = false;
	hardware_mode_ = HardwareMode::DMG;
	// CGB palettes: init to white (0x7FFF) — boot ROM would fill them for DMG games.
	bgpi_ = 0;
	obpi_ = 0;
	bgpd_ram_.fill(0xFF);
	obpd_ram_.fill(0xFF);
	bg_palette_cache_.fill(0xFFFFFFFF);
	obj_palette_cache_.fill(0xFFFFFFFF);
	opri_ = 0;
	// Decode initial white palettes
	for (int i = 0; i < 64; i += 2) {
		u16 c = static_cast<u16>(bgpd_ram_[i] | ((bgpd_ram_[i + 1] & 0x7F) << 8));
		bg_palette_cache_[i / 2] = rgb555_to_rgba_inner(c);
		u16 oc = static_cast<u16>(obpd_ram_[i] | ((obpd_ram_[i + 1] & 0x7F) << 8));
		obj_palette_cache_[i / 2] = rgb555_to_rgba_inner(oc);
	}
}

// ---- VBK ----
void PPU::write_vbk(u8 value)
{
	if (!is_cgb_mode(hardware_mode_))
		return; // DMG: ignore
	vram_bank_ = value & 0x01;
}

u8 PPU::read_vram(u16 addr) const
{
	if (addr >= 0x2000)
		return 0xFF;
	// VRAM blocked during Drawing (mode 3) when LCD on — per Pan Docs
	// https://gbdev.io/pandocs/Accessing_VRAM_and_OAM.html
	if ((lcdc_ & 0x80) && current_mode_ == Mode::Drawing)
		return 0xFF;
	if (!is_cgb_mode(hardware_mode_)) {
		return vram_banks_[0][addr];
	}
	return vram_banks_[vram_bank_][addr];
}

void PPU::write_vram(u16 addr, u8 value)
{
	if (addr >= 0x2000)
		return;
	if ((lcdc_ & 0x80) && current_mode_ == Mode::Drawing)
		return;
	if (!is_cgb_mode(hardware_mode_)) {
		vram_banks_[0][addr] = value;
		return;
	}
	vram_banks_[vram_bank_][addr] = value;
}

void PPU::write_vram_dma(u16 addr, u8 value)
{
	if (addr >= 0x2000)
		return;
	if (!is_cgb_mode(hardware_mode_)) {
		vram_banks_[0][addr] = value;
		return;
	}
	// Respects VBK selection but bypasses Drawing block (HDMA/GDMA blind copy per Pan Docs).
	vram_banks_[vram_bank_][addr] = value;
}

// ---- DMG 4-shade palettes (user-selectable) ----
namespace
{
// white, light, dark, black
constexpr u32 kDmgPalettes[][4] = {
	{0xFFFFFFFF, 0xFFAAAAAA, 0xFF555555, 0xFF000000}, // 0 Gray
	{0xFF9BBC0F, 0xFF8BAC0F, 0xFF306230, 0xFF0F380F}, // 1 Green (orig Game Boy)
	{0xFFF8E8C8, 0xFFD8B878, 0xFF886838, 0xFF201808}, // 2 Sepia
	{0xFFE8F0F8, 0xFFA8C0D8, 0xFF587890, 0xFF102030}, // 3 Blue
	{0xFFF8E8E8, 0xFFD8A8A8, 0xFF984848, 0xFF300808}, // 4 Crimson
};
constexpr int kDmgPaletteCount = 5;
} // namespace

void PPU::set_dmg_palette(int idx)
{
	if (idx < 0 || idx >= kDmgPaletteCount)
		idx = 0;
	dmg_palette_ = idx;
	for (int i = 0; i < 4; i++)
		dmg_shades_[i] = kDmgPalettes[idx][i];
}

// ---- Palettes ----
void PPU::write_bgpi(u8 value)
{
	if (!is_cgb_mode(hardware_mode_))
		return;
	bgpi_ = value;
}
u8 PPU::read_bgpd() const
{
	if (!is_cgb_mode(hardware_mode_))
		return 0xFF;
	u8 idx = bgpi_ & 0x3F;
	return bgpd_ram_[idx];
}
void PPU::write_bgpd(u8 value)
{
	if (!is_cgb_mode(hardware_mode_))
		return;
	u8 idx = bgpi_ & 0x3F;
	bgpd_ram_[idx] = value;
	// Decode the color that this byte belongs to
	u8 color_idx = idx / 2;
	u16 raw =
		static_cast<u16>(bgpd_ram_[color_idx * 2] | ((bgpd_ram_[color_idx * 2 + 1] & 0x7F) << 8));
	bg_palette_cache_[color_idx] = rgb555_to_rgba_inner(raw);
	if (bgpi_ & 0x80) {
		bgpi_ = (bgpi_ & 0x80) | ((idx + 1) & 0x3F);
	}
}
void PPU::write_obpi(u8 value)
{
	if (!is_cgb_mode(hardware_mode_))
		return;
	obpi_ = value;
}
u8 PPU::read_obpd() const
{
	if (!is_cgb_mode(hardware_mode_))
		return 0xFF;
	u8 idx = obpi_ & 0x3F;
	return obpd_ram_[idx];
}
void PPU::write_obpd(u8 value)
{
	if (!is_cgb_mode(hardware_mode_))
		return;
	u8 idx = obpi_ & 0x3F;
	obpd_ram_[idx] = value;
	u8 color_idx = idx / 2;
	u16 raw =
		static_cast<u16>(obpd_ram_[color_idx * 2] | ((obpd_ram_[color_idx * 2 + 1] & 0x7F) << 8));
	obj_palette_cache_[color_idx] = rgb555_to_rgba_inner(raw);
	if (obpi_ & 0x80) {
		obpi_ = (obpi_ & 0x80) | ((idx + 1) & 0x3F);
	}
}
u32 PPU::bg_palette_color(u8 palette, u8 color) const
{
	u8 idx = (palette & 0x07) * 4 + (color & 0x03);
	return bg_palette_cache_[idx];
}
u32 PPU::obj_palette_color(u8 palette, u8 color) const
{
	u8 idx = (palette & 0x07) * 4 + (color & 0x03);
	return obj_palette_cache_[idx];
}
u32 PPU::rgb555_to_rgba(u16 c)
{
	return rgb555_to_rgba_inner(c);
}

u8 PPU::read_oam(u16 addr) const
{
	if (addr >= 0xA0)
		return 0xFF;
	// OAM blocked during OAMScan+Drawing when LCD on
	if ((lcdc_ & 0x80) && (current_mode_ == Mode::OAMScan || current_mode_ == Mode::Drawing))
		return 0xFF;
	const OAMEntry &e = oam_[addr / 4];
	switch (addr % 4) {
	case 0:
		return e.y;
	case 1:
		return e.x;
	case 2:
		return e.tile;
	default:
		return e.flags;
	}
}

void PPU::write_oam(u16 addr, u8 value)
{
	if (addr >= 0xA0)
		return;
	if ((lcdc_ & 0x80) && (current_mode_ == Mode::OAMScan || current_mode_ == Mode::Drawing))
		return;
	OAMEntry &e = oam_[addr / 4];
	switch (addr % 4) {
	case 0:
		e.y = value;
		break;
	case 1:
		e.x = value;
		break;
	case 2:
		e.tile = value;
		break;
	default:
		e.flags = value;
		break;
	}
}

void PPU::write_dma(u8 value)
{
	dma_ = value;
	dma_transfer(value);
}

void PPU::dma_transfer(u8 high)
{
	// 160 bytes from (high << 8) to OAM; runs outside normal timing.
	// https://gbdev.io/pandocs/OAM_DMA_Transfer.html
	// Directly writes to OAM array bypassing mode blocking — DMA has its own bus.
	if (!mmu_)
		return;
	const u16 src = make_u16(high, 0x00);
	for (u16 i = 0; i < 0xA0; i++) {
		u8 v = mmu_->read(src + i);
		// Bypass write_oam blocking
		OAMEntry &e = oam_[i / 4];
		switch (i % 4) {
		case 0:
			e.y = v;
			break;
		case 1:
			e.x = v;
			break;
		case 2:
			e.tile = v;
			break;
		default:
			e.flags = v;
			break;
		}
	}
}

void PPU::request_vblank_interrupt()
{
	if (mmu_)
		mmu_->request_interrupt(0);
}

void PPU::request_stat_interrupt()
{
	if (mmu_)
		mmu_->request_interrupt(1);
}

void PPU::step(u32 mcycles)
{
	// GBC: dot rate scales with double_speed; M-cycle counts are unchanged.
	if ((lcdc_ & 0x80) == 0) {
		// LCD off: LY stuck at 0, VRAM freely accessible.
		ly_ = 0;
		line_cycles_ = 0;
		current_mode_ = Mode::HBlank;
		window_line_ = 0;
		stat_ = (stat_ & 0xFC) | 0x00;
		stat_ &= ~0x04;
		return;
	}

	const bool was_hblank =
		(ly_ < kVBlankStart && line_cycles_ >= kDrawDots && line_cycles_ < kDotsPerLine);
	line_cycles_ += mcycles * 4;
	while (line_cycles_ >= kDotsPerLine) {
		line_cycles_ -= kDotsPerLine;
		if (ly_ < kVBlankStart)
			render_scanline(ly_);
		ly_++;
		if (ly_ == kVBlankStart) {
			current_mode_ = Mode::VBlank;
			frame_ready_flag_ = true;
			request_vblank_interrupt();
			if (stat_ & 0x10)
				request_stat_interrupt(); // Mode-1 STAT source.
		} else if (ly_ >= kLinesPerFrame) {
			ly_ = 0;
			window_line_ = 0;
		}
		// LYC coincidence update.
		if (ly_ == lyc_) {
			stat_ |= 0x04;
			if (stat_ & 0x40)
				request_stat_interrupt();
		} else {
			stat_ &= ~0x04;
		}
		// HBlank HDMA: one block per HBlank (lines 0-143)
		if (is_cgb_mode(hardware_mode_) && mmu_ && ly_ < kVBlankStart) {
			mmu_->hdma_step_hblank();
		}
	}

	// Mode + STAT for the current dot (visible lines only).
	if (ly_ < kVBlankStart) {
		Mode m = Mode::OAMScan;
		if (line_cycles_ >= kDrawDots)
			m = Mode::HBlank;
		else if (line_cycles_ >= kOamDots)
			m = Mode::Drawing;
		if (m != current_mode_) {
			current_mode_ = m;
			if (m == Mode::HBlank && (stat_ & 0x08))
				request_stat_interrupt();
			if (m == Mode::OAMScan && (stat_ & 0x20))
				request_stat_interrupt();
			// Trigger HDMA on entering HBlank if needed (alternative hook)
			// We already trigger per line above; also ensure first HBlank after drawing.
		}
		stat_ = (stat_ & 0xFC) | static_cast<u8>(current_mode_);
	} else {
		current_mode_ = Mode::VBlank;
		stat_ = (stat_ & 0xFC) | 0x01;
	}
	(void) was_hblank;
}

u8 PPU::color_id_to_shade(u8 palette, u8 color_id) const
{
	return (palette >> (color_id * 2)) & 0x03;
}

u32 PPU::shade_to_rgba(u8 shade) const
{
	return dmg_shades_[shade & 0x03];
}

void PPU::render_scanline(u8 y)
{
	if (!is_cgb_mode(hardware_mode_)) {
		// DMG path. LCDC bit 0 (BG/window enable) gates BOTH layers: with it
		// clear the window must not draw either (Pan Docs: bit 0 controls the
		// background AND window on DMG).
		if (lcdc_ & 0x01) {
			render_background(y);
			render_window(y);
		} else {
			for (u16 x = 0; x < SCREEN_WIDTH; x++) {
				frame_buffer_.pixels[y * SCREEN_WIDTH + x] = dmg_shades_[0];
			}
		}
		if (lcdc_ & 0x02)
			render_sprites(y);
	} else {
		// CGB path
		// In CGB mode, LCDC bit0 is master BG priority, not BG enable. BG always renders when LCD
		// is on. Priority handling is in sprite stage via LCDC bit0 + BG attr + OBJ attr.
		// https://gbdev.io/pandocs/Tile_Maps.html#bg-to-obj-priority-in-cgb-mode
		render_background_cgb(y);
		render_window_cgb(y);
		if (lcdc_ & 0x02)
			render_sprites_cgb(y);
	}
}

void PPU::render_background(u8 y)
{
	const u16 map_base = (lcdc_ & 0x08) ? 0x1C00 : 0x1800;
	const bool signed_tiles = (lcdc_ & 0x10) == 0;
	const u8 row_y = y + scy_;
	for (u16 x = 0; x < SCREEN_WIDTH; x++) {
		const u8 col_x = x + scx_;
		const u8 tile_num = vram_banks_[0][map_base + (row_y / 8) * 32 + (col_x / 8)];
		u16 tile_addr;
		if (signed_tiles) {
			const i16 s = static_cast<i8>(tile_num);
			tile_addr = static_cast<u16>(0x1000 + s * 16);
		} else {
			tile_addr = tile_num * 16;
		}
		const u8 bit = 7 - (col_x % 8);
		const u8 b0 = vram_banks_[0][tile_addr + (row_y % 8) * 2];
		const u8 b1 = vram_banks_[0][tile_addr + (row_y % 8) * 2 + 1];
		const u8 color_id = ((b0 >> bit) & 1) | (((b1 >> bit) & 1) << 1);
		// Record the palette INDEX, not the rendered shade: DMG OBJ priority
		// (bit 7 of the sprite flags) tests whether the BG colour is non-zero,
		// and a legal palette may map two indices to the same shade.
		cgb_bg_color_id_[y * SCREEN_WIDTH + x] = color_id;
		frame_buffer_.pixels[y * SCREEN_WIDTH + x] =
			shade_to_rgba(color_id_to_shade(bgp_, color_id));
	}
}

void PPU::render_background_cgb(u8 y)
{
	// CGB background with per-tile attributes from VRAM bank 1
	// https://gbdev.io/pandocs/Tile_Data.html#vram-tile-maps
	const u16 map_base = (lcdc_ & 0x08) ? 0x1C00 : 0x1800;
	const bool signed_tiles = (lcdc_ & 0x10) == 0;
	const u8 row_y = y + scy_;
	for (u16 x = 0; x < SCREEN_WIDTH; x++) {
		const u8 col_x = x + scx_;
		const u16 map_off = (row_y / 8) * 32 + (col_x / 8);
		const u8 tile_num = vram_banks_[0][map_base + map_off];
		const u8 attr = vram_banks_[1][map_base + map_off];
		// Decode attribute
		u8 palette = attr & 0x07;
		u8 bank = (attr >> 3) & 0x01;
		// Pan Docs: bit5 X-flip, bit6 Y-flip, bit7 BG-to-OAM priority
		// https://gbdev.io/pandocs/Tile_Data.html#vram-tile-maps
		bool xflip = (attr >> 5) & 0x01;
		bool y_flip = (attr >> 6) & 0x01;
		u8 row_in_tile = row_y % 8;
		if (y_flip)
			row_in_tile = 7 - row_in_tile;
		u16 tile_addr;
		if (signed_tiles) {
			const i16 s = static_cast<i8>(tile_num);
			tile_addr = static_cast<u16>(0x1000 + s * 16);
		} else {
			tile_addr = tile_num * 16;
		}
		u8 bit = 7 - (col_x % 8);
		if (xflip)
			bit = (col_x % 8);
		const u8 b0 = vram_banks_[bank][tile_addr + row_in_tile * 2];
		const u8 b1 = vram_banks_[bank][tile_addr + row_in_tile * 2 + 1];
		const u8 color_id = ((b0 >> bit) & 1) | (((b1 >> bit) & 1) << 1);
		u32 col = bg_palette_color(palette, color_id);
		frame_buffer_.pixels[y * SCREEN_WIDTH + x] = col;
		cgb_bg_color_id_[y * SCREEN_WIDTH + x] = color_id;
		cgb_bg_has_priority_[y * SCREEN_WIDTH + x] = (attr >> 7) & 0x01;
	}
}

void PPU::render_window(u8 y)
{
	if ((lcdc_ & 0x20) == 0)
		return;
	if (y < wy_)
		return;
	const int wx = static_cast<int>(wx_) - 7;
	if (wx >= SCREEN_WIDTH)
		return;
	const u16 map_base = (lcdc_ & 0x40) ? 0x1C00 : 0x1800;
	const bool signed_tiles = (lcdc_ & 0x10) == 0;
	const u8 row_y = window_line_;
	bool drew = false;
	for (u16 x = 0; x < SCREEN_WIDTH; x++) {
		if (static_cast<int>(x) < wx)
			continue;
		const u8 col_x = x - wx;
		const u8 tile_num = vram_banks_[0][map_base + (row_y / 8) * 32 + (col_x / 8)];
		u16 tile_addr;
		if (signed_tiles) {
			const i16 s = static_cast<i8>(tile_num);
			tile_addr = static_cast<u16>(0x1000 + s * 16);
		} else {
			tile_addr = tile_num * 16;
		}
		const u8 bit = 7 - (col_x % 8);
		const u8 b0 = vram_banks_[0][tile_addr + (row_y % 8) * 2];
		const u8 b1 = vram_banks_[0][tile_addr + (row_y % 8) * 2 + 1];
		const u8 color_id = ((b0 >> bit) & 1) | (((b1 >> bit) & 1) << 1);
		cgb_bg_color_id_[y * SCREEN_WIDTH + x] = color_id;
		frame_buffer_.pixels[y * SCREEN_WIDTH + x] =
			shade_to_rgba(color_id_to_shade(bgp_, color_id));
		drew = true;
	}
	if (drew)
		window_line_++;
}

void PPU::render_window_cgb(u8 y)
{
	if ((lcdc_ & 0x20) == 0)
		return;
	if (y < wy_)
		return;
	const int wx = static_cast<int>(wx_) - 7;
	if (wx >= SCREEN_WIDTH)
		return;
	const u16 map_base = (lcdc_ & 0x40) ? 0x1C00 : 0x1800;
	const bool signed_tiles = (lcdc_ & 0x10) == 0;
	const u8 row_y = window_line_;
	bool drew = false;
	for (u16 x = 0; x < SCREEN_WIDTH; x++) {
		if (static_cast<int>(x) < wx)
			continue;
		const u8 col_x = x - wx;
		const u16 map_off = (row_y / 8) * 32 + (col_x / 8);
		const u8 tile_num = vram_banks_[0][map_base + map_off];
		const u8 attr = vram_banks_[1][map_base + map_off];
		u8 palette = attr & 0x07;
		u8 bank = (attr >> 3) & 0x01;
		bool xflip = (attr >> 5) & 0x01;
		bool y_flip = (attr >> 6) & 0x01;
		u8 row_in_tile = row_y % 8;
		if (y_flip)
			row_in_tile = 7 - row_in_tile;
		u16 tile_addr;
		if (signed_tiles) {
			const i16 s = static_cast<i8>(tile_num);
			tile_addr = static_cast<u16>(0x1000 + s * 16);
		} else {
			tile_addr = tile_num * 16;
		}
		u8 bit = 7 - (col_x % 8);
		if (xflip)
			bit = (col_x % 8);
		const u8 b0 = vram_banks_[bank][tile_addr + row_in_tile * 2];
		const u8 b1 = vram_banks_[bank][tile_addr + row_in_tile * 2 + 1];
		const u8 color_id = ((b0 >> bit) & 1) | (((b1 >> bit) & 1) << 1);
		frame_buffer_.pixels[y * SCREEN_WIDTH + x] = bg_palette_color(palette, color_id);
		cgb_bg_color_id_[y * SCREEN_WIDTH + x] = color_id;
		cgb_bg_has_priority_[y * SCREEN_WIDTH + x] = (attr >> 7) & 0x01;
		drew = true;
	}
	if (drew)
		window_line_++;
}

void PPU::render_sprites(u8 y)
{
	const u8 height = (lcdc_ & 0x04) ? 16 : 8;
	// OAM scan: first 10 overlapping sprites in OAM order.
	int order[10];
	int count = 0;
	for (int i = 0; i < 40 && count < 10; i++) {
		const int sy = static_cast<int>(oam_[i].y) - 16;
		if (y >= sy && y < sy + height)
			order[count++] = i;
	}
	// Draw priority: lower X wins, OAM index breaks ties. Draw back-to-front
	// so the winner ends up on top (stable: keeps OAM order for equal X).
	// https://gbdev.io/pandocs/OAM.html
	for (int a = 0; a < count; a++) {
		for (int b = a + 1; b < count; b++) {
			if (static_cast<int>(oam_[order[b]].x) < static_cast<int>(oam_[order[a]].x)) {
				const int t = order[a];
				order[a] = order[b];
				order[b] = t;
			}
		}
	}
	for (int k = count - 1; k >= 0; k--) {
		const OAMEntry &s = oam_[order[k]];
		const int sy = static_cast<int>(s.y) - 16;
		const int sx = static_cast<int>(s.x) - 8;
		u8 line = y - sy;
		if (s.flags & 0x40)
			line = height - 1 - line; // Y-flip.
		u8 tile = s.tile;
		if (height == 16)
			tile = (tile & 0xFE) | ((line >= 8) ? 1 : 0);
		const u8 b0 = vram_banks_[0][tile * 16 + (line % 8) * 2];
		const u8 b1 = vram_banks_[0][tile * 16 + (line % 8) * 2 + 1];
		const u8 palette = (s.flags & 0x10) ? obp1_ : obp0_;
		for (int px = 0; px < 8; px++) {
			const int x = sx + px;
			if (x < 0 || x >= SCREEN_WIDTH)
				continue;
			u8 bit = 7 - px;
			if (s.flags & 0x20)
				bit = px; // X-flip.
			const u8 color_id = ((b0 >> bit) & 1) | (((b1 >> bit) & 1) << 1);
			if (color_id == 0)
				continue; // Transparent.
			// OBJ-to-BG priority: behind BG unless the BG palette INDEX is
			// non-zero. Comparing rendered RGB would misfire whenever a legal
			// BGP maps two indices to the same shade.
			if ((s.flags & 0x80) != 0) {
				if (cgb_bg_color_id_[y * SCREEN_WIDTH + x] != 0)
					continue;
			}
			frame_buffer_.pixels[y * SCREEN_WIDTH + x] =
				shade_to_rgba(color_id_to_shade(palette, color_id));
		}
	}
}

void PPU::render_sprites_cgb(u8 y)
{
	const u8 height = (lcdc_ & 0x04) ? 16 : 8;
	// CGB OPRI: 0 = OAM order priority, 1 = coordinate priority (like DMG)
	bool oam_priority = (opri_ == 0);
	int order[10];
	int count = 0;
	for (int i = 0; i < 40 && count < 10; i++) {
		const int sy = static_cast<int>(oam_[i].y) - 16;
		if (y >= sy && y < sy + height)
			order[count++] = i;
	}
	if (!oam_priority) {
		for (int a = 0; a < count; a++) {
			for (int b = a + 1; b < count; b++) {
				if (static_cast<int>(oam_[order[b]].x) < static_cast<int>(oam_[order[a]].x)) {
					const int t = order[a];
					order[a] = order[b];
					order[b] = t;
				}
			}
		}
	}
	// Draw back-to-front
	for (int k = count - 1; k >= 0; k--) {
		const OAMEntry &s = oam_[order[k]];
		const int sy = static_cast<int>(s.y) - 16;
		const int sx = static_cast<int>(s.x) - 8;
		u8 line = y - sy;
		if (s.flags & 0x40)
			line = height - 1 - line;
		u8 tile = s.tile;
		// CGB bank select
		u8 bank = (s.flags & 0x08) ? 1 : 0; // bit3
		u8 palette = s.flags & 0x07;        // bits 0-2
		if (height == 16)
			tile = (tile & 0xFE) | ((line >= 8) ? 1 : 0);
		const u8 b0 = vram_banks_[bank][tile * 16 + (line % 8) * 2];
		const u8 b1 = vram_banks_[bank][tile * 16 + (line % 8) * 2 + 1];
		for (int px = 0; px < 8; px++) {
			const int x = sx + px;
			if (x < 0 || x >= SCREEN_WIDTH)
				continue;
			u8 bit = 7 - px;
			if (s.flags & 0x20)
				bit = px;
			const u8 color_id = ((b0 >> bit) & 1) | (((b1 >> bit) & 1) << 1);
			if (color_id == 0)
				continue;
			// CGB sprite priority per Pan Docs:
			// - BG tile's attribute bit7 (BG-to-OAM) =1 => BG above OBJ if BG color !=0
			// - Else OBJ's bit7 =1 => OBJ behind BG if BG color !=0
			// - If LCDC bit0 (master priority) is clear, OBJ always above BG
			// https://gbdev.io/pandocs/Tile_Maps.html#bg-to-obj-priority-in-cgb-mode
			u8 bg_cid = cgb_bg_color_id_[y * SCREEN_WIDTH + x];
			u8 bg_prio = cgb_bg_has_priority_[y * SCREEN_WIDTH + x];
			bool master_prio = (lcdc_ & 0x01) != 0;
			if (bg_cid != 0 && master_prio) {
				if (bg_prio)
					continue; // BG priority overrides
				if (s.flags & 0x80)
					continue; // OBJ behind BG
			}
			frame_buffer_.pixels[y * SCREEN_WIDTH + x] = obj_palette_color(palette, color_id);
		}
	}
}

} // namespace gb
