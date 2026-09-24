// GBA scanline renderer: text/affine/bitmap BGs, sprites, windows, blending.
// Formats: GBATEK "LCD VRAM ..." + "LCD OBJ ..." chapters. Per-pixel
// priority: lower number wins; OBJ beats BG on ties; BG0 wins BG ties;
// lower OAM index wins OBJ ties. Mosaic sampling is screen-space
// (documented approximation of the hardware counters).

#include "gba/ppu.h"

namespace gba
{

namespace
{

// BG sizes in tiles: text {w,h} in screens, affine pixel dimensions.
constexpr unsigned kTextW[4] = {1, 2, 1, 2};
constexpr unsigned kTextH[4] = {1, 1, 2, 2};
constexpr unsigned kAffSize[4] = {128, 256, 512, 1024};

// OBJ shape/size -> (w, h).
constexpr unsigned kObjW[4][4] = {
	{8, 16, 32, 64},
	{16, 32, 32, 64},
	{8, 8, 16, 32},
	{0, 0, 0, 0},
};
constexpr unsigned kObjH[4][4] = {
	{8, 16, 32, 64},
	{8, 8, 16, 32},
	{16, 32, 32, 64},
	{0, 0, 0, 0},
};

struct LayerPx {
	u32 color = 0;
	u8 prio = 4;
	u8 kind = 5; // 0-3 BG, 4 OBJ, 5 backdrop.
	bool semi = false;
	bool present = false;
};

u16 vram16(const GbaPpu &p, u32 off)
{
	// Per-byte reads so the upper-32K mirror window composes correctly
	// across the boundary (readVram mirrors internally).
	return static_cast<u16>(p.readVram(off & 0x1FFFFu) | (p.readVram((off + 1) & 0x1FFFFu) << 8));
}

u16 oam16(const GbaPpu &p, u32 off)
{
	off &= GbaPpu::kOamSize - 1;
	return static_cast<u16>(p.readOam(off) | (p.readOam(off + 1) << 8));
}

} // namespace

void renderScanline(GbaPpu &ppu, unsigned y)
{
	const u16 dispcnt = ppu.dispcnt_;
	u32 *line = &ppu.frame_.pixels[y * GbaPpu::kWidth];
	const u8 *const vram = ppu.vramData();
	// Direct VRAM read that still applies the 96K/128K mirror. The backing
	// array is 0x18000 bytes, so masking with 0x1FFFF alone would read out
	// of bounds for offsets 0x18000-0x1FFFF (GbaPpu::mirrorVram folds those
	// back to 0x10000-0x17FFF).
	const auto vread = [vram](u32 off) -> u8 {
		off &= 0x1FFFFu;
		if (off >= GbaPpu::kVramSize)
			off = 0x10000u | (off & 0x7FFFu);
		return vram[off];
	};
	if ((dispcnt & 0x80u) != 0) { // Forced blank: white lines.
		for (unsigned x = 0; x < GbaPpu::kWidth; x++)
			line[x] = 0xFFFFFFFFu;
		return;
	}
	const unsigned mode = dispcnt & 7u;
	if (mode >= 6) { // Prohibited modes: backdrop only (documented).
		const u32 bd = GbaPpu::rgb555(ppu.palEntry(0));
		for (unsigned x = 0; x < GbaPpu::kWidth; x++)
			line[x] = bd;
		return;
	}

	// ---- BG layer buffers ----
	struct BgPx {
		u32 color = 0;
		bool present = false;
	};
	BgPx bg[4][GbaPpu::kWidth];
	u8 bg_prio[4] = {4, 4, 4, 4};
	const bool bg_enable[4] = {
		(dispcnt & 0x100u) != 0,
		(dispcnt & 0x200u) != 0,
		(dispcnt & 0x400u) != 0,
		(dispcnt & 0x800u) != 0,
	};
	const unsigned bg_mosaic_h = (ppu.mosaic_ & 0xFu) + 1;
	const unsigned bg_mosaic_v = ((ppu.mosaic_ >> 4) & 0xFu) + 1;

	auto renderText = [&](unsigned b) {
		const u16 cnt = ppu.bgcnt_[b];
		bg_prio[b] = cnt & 3u;
		const u32 char_base = ((cnt >> 2) & 3u) * 0x4000u;
		const bool mosaic_on = (cnt & 0x40u) != 0;
		const bool is8 = (cnt & 0x80u) != 0;
		const u32 map_base = ((cnt >> 8) & 0x1Fu) * 0x800u;
		const unsigned size = (cnt >> 14) & 3u;
		const unsigned my = mosaic_on ? (y - y % bg_mosaic_v) : y;
		const u32 vofs = ppu.bgvofs_[b];
		for (unsigned x = 0; x < GbaPpu::kWidth; x++) {
			const unsigned mx = mosaic_on ? (x - x % bg_mosaic_h) : x;
			const u32 tx = mx + ppu.bghofs_[b];
			const u32 ty = my + vofs;
			unsigned screen = 0;
			if (size == 1)
				screen = (tx >> 8) & 1;
			else if (size == 2)
				screen = ((ty >> 8) & 1) * 2;
			else if (size == 3)
				screen = (((ty >> 8) & 1) * 2) | ((tx >> 8) & 1);
			const u32 me = map_base + screen * 0x800u +
						   (((ty & 255u) / 8) * 32 + ((tx & 255u) / 8)) * 2;
			const u32 entry =
				static_cast<u16>(vread(me) | (vread(me + 1) << 8));
			const unsigned tile = entry & 0x3FFu;
			unsigned px = tx & 7u, row = ty & 7u;
			if ((entry & 0x0400u) != 0)
				px = 7 - px;
			if ((entry & 0x0800u) != 0)
				row = 7 - row;
			u32 color;
			if (!is8) {
				const u32 addr = char_base + tile * 32u + row * 4u + px / 2;
				if (addr >= 0x10000u)
					continue; // Outside BG VRAM; must not alias into OBJ VRAM.
				const u8 byte = vread(addr);
				color = (px & 1) != 0 ? (byte >> 4) : (byte & 0x0Fu);
				if (color == 0)
					continue;
				color = GbaPpu::rgb555(ppu.palEntry(((entry >> 12) & 0xFu) * 16 + color));
			} else {
				const u32 addr = char_base + tile * 64u + row * 8u + px;
				if (addr >= 0x10000u)
					continue; // Outside BG VRAM.
				color = vread(addr);
				if (color == 0)
					continue;
				color = GbaPpu::rgb555(ppu.palEntry(color));
			}
			bg[b][x].color = color;
			bg[b][x].present = true;
		}
	};

	auto renderAffine = [&](unsigned b) {
		// b = 2/3, param set i = b-2.
		const unsigned i = b - 2;
		const u16 cnt = ppu.bgcnt_[b];
		bg_prio[b] = cnt & 3u;
		const u32 char_base = ((cnt >> 2) & 3u) * 0x4000u;
		const bool mosaic_on = (cnt & 0x40u) != 0;
		const u32 map_base = ((cnt >> 8) & 0x1Fu) * 0x800u;
		const unsigned size = kAffSize[(cnt >> 14) & 3u];
		const bool wrap = (cnt & 0x2000u) != 0;
		const i32 pa = static_cast<i16>(ppu.bgpa_[i]);
		const i32 pb = static_cast<i16>(ppu.bgpb_[i]);
		const i32 pc = static_cast<i16>(ppu.bgpc_[i]);
		const i32 pd = static_cast<i16>(ppu.bgpd_[i]);
		const i32 cur_x = ppu.bgx_[i];
		const i32 cur_y = ppu.bgy_[i];
		// bgx_/bgy_ already carry the per-scanline PB/PD advance (the PPU
		// steps them after every visible line), so they are the source
		// coordinate of THIS scanline's x=0 pixel. The per-pixel transform
		// is therefore PA/PC only; adding PB*my/PD*my would double-count
		// the row term and scatter the layer.
		//
		// Vertical mosaic samples the texture as if the line were `my`, so
		// only the row DELTA from the real line is added back.
		const unsigned my = mosaic_on ? (y - y % bg_mosaic_v) : y;
		const i32 row_delta = static_cast<i32>(my) - static_cast<i32>(y);
		const i32 mx_off_x = pb * row_delta;
		const i32 mx_off_y = pd * row_delta;
		for (unsigned x = 0; x < GbaPpu::kWidth; x++) {
			const unsigned mx = mosaic_on ? (x - x % bg_mosaic_h) : x;
			const i32 sx = cur_x + pa * static_cast<i32>(mx) + mx_off_x;
			const i32 sy = cur_y + pc * static_cast<i32>(mx) + mx_off_y;
			const i32 tex = sx >> 8;
			const i32 tey = sy >> 8;
			i32 tx = tex, ty = tey;
			if (tx < 0 || ty < 0 || tx >= static_cast<i32>(size) || ty >= static_cast<i32>(size)) {
				if (!wrap)
					continue;
				tx &= size - 1;
				ty &= size - 1;
			}
			// Affine map entries are ONE BYTE each and cover an 8x8 pixel
			// block (GBATEK "Rotation/Scaling BG Screen (1 byte per
			// entry)"). The map is therefore indexed in TILES, not pixels:
			// entry = (ty/8) * (size/8) + (tx/8). Indexing it with a pixel
			// stride walked ~20x past the 2KB map into unrelated VRAM, which
			// is what scattered the Emerald title logo's pixels.
			const u32 map_off =
				map_base + (static_cast<u32>(ty) >> 3) * (size >> 3) + (static_cast<u32>(tx) >> 3);
			// BG map and BG character data live in the 64K BG half of VRAM;
			// anything past it is not BG data (mGBA renders it transparent)
			// rather than mirroring into OBJ VRAM.
			if (map_off >= 0x10000u)
				continue;
			const u32 tile = vread(map_off);
			const u32 addr = char_base + tile * 64u + static_cast<u32>(ty & 7) * 8u + (tx & 7);
			if (addr >= 0x10000u)
				continue;
			const u32 color = vread(addr);
			if (color == 0)
				continue;
			bg[b][x].color = GbaPpu::rgb555(ppu.palEntry(color));
			bg[b][x].present = true;
		}
	};

	auto renderBitmap = [&]() {
		bg_prio[2] = ppu.bgcnt_[2] & 3u;
		// Modes 3-5 are rotation/scaling layers: BG2X/BG2Y and BG2PA..PD map
		// screen coordinates into the framebuffer (GBATEK "Bitmap Mode
		// Distortions"). BG2PA..PD are parameter set 0.
		const i32 pa = static_cast<i16>(ppu.bgpa_[0]);
		const i32 pb = static_cast<i16>(ppu.bgpb_[0]);
		const i32 pc = static_cast<i16>(ppu.bgpc_[0]);
		const i32 pd = static_cast<i16>(ppu.bgpd_[0]);
		const i32 origin_x = ppu.bgx_[0];
		const i32 origin_y = ppu.bgy_[0];
		// Per-layer mosaic enable lives in BG2CNT bit 6, NOT the MOSAIC
		// register (whose bit 10 is the OBJ enable). Matches the text/affine
		// paths and mGBA, which reads the flag off the background.
		const bool mosaic_on = (ppu.bgcnt_[2] & 0x40u) != 0;
		// Screen pixel -> framebuffer pixel, with mosaic snapping. Anything
		// outside the framebuffer is transparent. As with affine BG, bgx_/
		// bgy_ already hold this scanline's origin, so the transform is
		// PA/PC per pixel and only the mosaic ROW DELTA uses PB/PD.
		const unsigned my = mosaic_on ? (y - y % bg_mosaic_v) : y;
		const i32 row_delta = static_cast<i32>(my) - static_cast<i32>(y);
		const i32 off_x = pb * row_delta;
		const i32 off_y = pd * row_delta;
		const auto sample = [&](unsigned x, unsigned &fx, unsigned &fy) {
			const unsigned mx = mosaic_on ? (x - x % bg_mosaic_h) : x;
			fx = static_cast<unsigned>((origin_x + pa * static_cast<i32>(mx) + off_x) >> 8);
			fy = static_cast<unsigned>((origin_y + pc * static_cast<i32>(mx) + off_y) >> 8);
		};
		unsigned height = GbaPpu::kHeight;
		if (mode == 3) {
			for (unsigned x = 0; x < GbaPpu::kWidth; x++) {
				unsigned fx = 0, fy = 0;
				sample(x, fx, fy);
				if (fx >= GbaPpu::kWidth || fy >= height)
					continue;
				const u16 c = vram16(ppu, (fy * GbaPpu::kWidth + fx) * 2);
				bg[2][x].color = GbaPpu::rgb555(c);
				bg[2][x].present = true;
			}
		} else if (mode == 4) {
			const u32 base = ((dispcnt >> 4) & 1u) != 0 ? 0xA000u : 0u;
			for (unsigned x = 0; x < GbaPpu::kWidth; x++) {
				unsigned fx = 0, fy = 0;
				sample(x, fx, fy);
				if (fx >= GbaPpu::kWidth || fy >= height)
					continue;
				const u32 idx = ppu.readVram((base + fy * GbaPpu::kWidth + fx) & 0x1FFFFu);
				// Mode 4 palette index 0 is transparent (GBATEK: "Color 0
				// is transparent"), so lower layers must still show through.
				if (idx == 0)
					continue;
				bg[2][x].color = GbaPpu::rgb555(ppu.palEntry(idx));
				bg[2][x].present = true;
			}
		} else { // Mode 5: 160x128, two frames, outside = transparent.
			height = 128;
			if (y >= 128)
				return;
			const u32 base = ((dispcnt >> 4) & 1u) != 0 ? 0xA000u : 0u;
			for (unsigned x = 0; x < 160; x++) {
				unsigned fx = 0, fy = 0;
				sample(x, fx, fy);
				if (fx >= 160 || fy >= height)
					continue;
				const u32 off = base + (fy * 160 + fx) * 2;
				bg[2][x].color = GbaPpu::rgb555(vram16(ppu, off));
				bg[2][x].present = true;
			}
		}
	};

	switch (mode) {
	case 0:
		for (unsigned b = 0; b < 4; b++) {
			if (bg_enable[b])
				renderText(b);
		}
		break;
	case 1:
		if (bg_enable[0])
			renderText(0);
		if (bg_enable[1])
			renderText(1);
		if (bg_enable[2])
			renderAffine(2);
		break;
	case 2:
		if (bg_enable[2])
			renderAffine(2);
		if (bg_enable[3])
			renderAffine(3);
		break;
	default:
		if (bg_enable[2])
			renderBitmap();
		break;
	}

	// ---- Sprites ----
	struct ObjPx {
		u32 color = 0;
		u8 prio = 4;
		u8 index = 0xFF;
		bool semi = false;
		bool present = false;
	};
	ObjPx obj[GbaPpu::kWidth];
	bool obj_win[GbaPpu::kWidth] = {false};
	const bool obj_enable = (dispcnt & 0x1000u) != 0;
	const bool map1d = (dispcnt & 0x40u) != 0;
	const unsigned obj_mosaic_h = ((ppu.mosaic_ >> 8) & 0xFu) + 1;
	const unsigned obj_mosaic_v = ((ppu.mosaic_ >> 12) & 0xFu) + 1;
	if (obj_enable) {
		// Per-line OBJ cycle budget (GBATEK "Maximum Number of Sprites per
		// Line"; mGBA CleanOAM + VBA-M lineOBJpix model it): 2 cycles per
		// OAM index gap between kept sprites, plus width-2 per normal
		// sprite or 8+2*width per affine sprite. Disabled and fully
		// offscreen sprites cost nothing; exhaustion drops the remaining
		// (lower-priority) sprites for this line.
		int budget = (dispcnt & 0x20u) != 0 ? 954 : 1210;
		int last = 0;
		for (unsigned i = 0; i < 128; i++) {
			const u16 a0 = oam16(ppu, i * 8);
			const u16 a1 = oam16(ppu, i * 8 + 2);
			const u16 a2 = oam16(ppu, i * 8 + 4);
			const bool rot = (a0 & 0x0100u) != 0;
			const bool dbl_or_hide = (a0 & 0x0200u) != 0;
			if (!rot && dbl_or_hide)
				continue; // Disabled.
			const unsigned obj_mode = (a0 >> 10) & 3u;
			if (obj_mode == 3)
				continue; // Prohibited.
			const unsigned shape = (a0 >> 14) & 3u;
			const unsigned size = (a1 >> 14) & 3u;
			if (shape == 3)
				continue;
			unsigned w = kObjW[shape][size], h = kObjH[shape][size];
			if (w == 0)
				continue;
			unsigned cols = w, rows = h;
			if (rot && dbl_or_hide) {
				cols *= 2;
				rows *= 2;
			}
			const unsigned obj_y = a0 & 0xFFu;
			// Fully below (in the VBlank zone without wrapping back) or
			// fully right/left: skipped with no cost (mGBA cache rules).
			// A NON-transformed sprite whose top is at/below the last visible
			// line is entirely off-screen and must be culled: mGBA only wraps
			// Y for transformed sprites (software-obj.c:222-225). Games park
			// unused OAM slots at Y=160..255, and wrapping those would draw
			// them over the top of the screen.
			if (!rot && obj_y >= GbaPpu::kHeight)
				continue;
			// X is 9-bit and mGBA sign-extends bit 8 (`X << 23 >> 23`,
			// software-obj.c:168-170), so 256..511 read as negative, i.e.
			// off-screen LEFT rather than right.
			const int sx9 = (a1 & 0x100) != 0 ? static_cast<int>(a1 & 0x1FFu) - 512
											 : static_cast<int>(a1 & 0x1FFu);
			if (obj_y >= GbaPpu::kHeight && obj_y + rows < GbaPpu::kLines)
				continue;
			if (sx9 >= static_cast<int>(GbaPpu::kWidth) &&
				sx9 + static_cast<int>(cols) < 512)
				continue;
			if (sx9 + static_cast<int>(cols) < 0)
				continue;
			budget -= 2 * (static_cast<int>(i) - last);
			last = static_cast<int>(i);
			if (budget <= 0)
				break;
			// Transformed sprites wrap vertically (mGBA adds 256 when the
			// line is above the sprite's origin); non-transformed ones are
			// simply outside [0,height) and are culled. An unconditional
			// `& 0xFF` here wrongly redrew parked Y>=160 sprites.
			int ry_s = static_cast<int>(y) - static_cast<int>(obj_y);
			if (rot) {
				if (ry_s < 0)
					ry_s += 256;
			} else if (ry_s < 0 || ry_s >= static_cast<int>(rows)) {
				continue;
			}
			const unsigned ry = static_cast<unsigned>(ry_s);
			if (ry >= rows)
				continue;
			unsigned tile = a2 & 0x3FFu;
			if (mode >= 3 && tile < 512)
				continue; // Bitmap-mode restriction.
			const bool is8 = (a0 & 0x2000u) != 0;
			// 256-color OAM numbers address 32-byte slots (GBATEK 1D/2D
			// examples step 04h->06h; mGBA uses tile*0x20). 2D ignores
			// bit 0 (mGBA align mask); 1D keeps it (adds half a tile).
			if (is8 && !map1d)
				tile &= ~1u;
			const unsigned prio = (a2 >> 10) & 3u;
			const unsigned pal = (a2 >> 12) & 0xFu;
			const bool mosaic_on = (a0 & 0x1000u) != 0;
			const unsigned obj_x = a1 & 0x1FFu;
			// Affine parameters.
			i32 pa = 0, pb = 0, pc = 0, pd = 0;
			if (rot) {
				const unsigned p = (a1 >> 9) & 0x1Fu;
				pa = static_cast<i16>(oam16(ppu, p * 32 + 6));
				pb = static_cast<i16>(oam16(ppu, p * 32 + 14));
				pc = static_cast<i16>(oam16(ppu, p * 32 + 22));
				pd = static_cast<i16>(oam16(ppu, p * 32 + 30));
			}
			const bool hflip = !rot && (a1 & 0x1000u) != 0;
			const bool vflip = !rot && (a1 & 0x2000u) != 0;
			// Charge this sprite's draw cost against the line budget
			// (normal: width-2, affine: 8+2*width); the current sprite
			// still draws on overdraft, later ones drop.
			// GBATEK "OBJ Character Processing": normal OBJ = n*1 cycles
			// (n = horizontal pixel size); rotation/scaling = 10 + n*2.
			// The old formula charged n-2 and 8+2n, under-charging every
			// sprite by 2 cycles and so fitting more OBJs per line than
			// hardware.
			budget -= rot ? static_cast<int>(10u + 2u * cols) : static_cast<int>(w);
			for (unsigned px = 0; px < cols; px++) {
				const unsigned sx = (obj_x + px) & 0x1FFu; // Wraps.
				if (sx >= GbaPpu::kWidth)
					continue;
				unsigned tx, ty;
				if (rot) {
					// Mosaic applies to transformed OBJs too (GBATEK: every
					// BG and OBJ layer). The phase is SCREEN-space: mGBA uses
					// mosaicY = y - y%V and outX%H (software-obj.c:16-28,
					// video-software.c:1027-1050), not the sprite-local
					// coordinate - so an unaligned sprite must still sample
					// the same texture cell across a block.
					const unsigned spx = mosaic_on ? (px - px % obj_mosaic_h) : px;
					const unsigned spy = mosaic_on ? (ry - ry % obj_mosaic_v) : ry;
					const i32 dx = static_cast<i32>(spx) - static_cast<i32>(cols / 2);
					const i32 dy = static_cast<i32>(spy) - static_cast<i32>(rows / 2);
					const i32 ox = ((pa * dx + pb * dy) >> 8) + static_cast<i32>(w / 2);
					const i32 oy = ((pc * dx + pd * dy) >> 8) + static_cast<i32>(h / 2);
					if (ox < 0 || oy < 0 || ox >= static_cast<i32>(w) ||
						oy >= static_cast<i32>(h)) {
						continue;
					}
					tx = static_cast<unsigned>(ox);
					ty = static_cast<unsigned>(oy);
				} else {
					tx = hflip ? (w - 1 - px) : px;
					// Vertical mosaic snaps by screen Y (VBA-M cites NBA/HW
					// research; mGBA snaps the scanline the same way), not
					// by sprite-local row. Horizontal stays screen-based.
					unsigned row = ry;
					if (mosaic_on) {
						const int snapped =
							static_cast<int>(ry) - static_cast<int>(y % obj_mosaic_v);
						row = static_cast<unsigned>(snapped < 0 ? 0 : snapped);
					}
					unsigned sry = vflip ? (h - 1 - row) : row;
					if (mosaic_on) {
						tx = tx - tx % obj_mosaic_h;
					}
					ty = sry;
				}
				u32 color;
				if (!is8) {
					u32 tile_no;
					if (map1d) {
						tile_no = tile + (ty / 8) * (w / 8) + tx / 8;
					} else {
						tile_no = tile + (ty / 8) * 32 + tx / 8;
					}
					const u32 addr = 0x10000u + tile_no * 32u + (ty % 8) * 4u + (tx % 8) / 2;
					const u8 byte = vread(addr);
					color = ((tx % 8) & 1) != 0 ? (byte >> 4) : (byte & 0x0Fu);
					if (color == 0) {
						if (obj_mode == 2) { /* transparent: no window bit */
						}
						continue;
					}
					if (obj_mode == 2) {
						obj_win[sx] = true;
						continue;
					}
					color = GbaPpu::rgb555(ppu.palEntry(0x100 + pal * 16 + color));
				} else {
					// 256-color tiles are 64 bytes but OAM numbers address
					// 32-byte slots (GBATEK steps 04h->06h; mGBA tile*0x20):
					// columns step one tile (+64), 1D rows step (w/8)
					// tiles, 2D rows step 16 tiles (1024 bytes).
					const u32 row_step = map1d ? (w / 8) : 16;
					const u32 addr = 0x10000u + tile * 32u +
						((ty / 8) * row_step + tx / 8) * 64u + (ty % 8) * 8u + (tx % 8);
					color = vread(addr);
					if (color == 0) {
						if (obj_mode == 2) { /* transparent: no window bit */
						}
						continue;
					}
					if (obj_mode == 2) {
						obj_win[sx] = true;
						continue;
					}
					color = GbaPpu::rgb555(ppu.palEntry(0x100 + color));
				}
				ObjPx &cur = obj[sx];
				if (!cur.present || prio < cur.prio || (prio == cur.prio && i < cur.index)) {
					cur.color = color;
					cur.prio = static_cast<u8>(prio);
					cur.index = static_cast<u8>(i);
					cur.semi = (obj_mode == 1);
					cur.present = true;
				}
			}
		}
	}

	// ---- Windows (WIN0 > WIN1 > OBJWIN > outside) ----
	// GBATEK garbage rules: X2>240 acts as X2=240 (likewise Y2>160);
	// X1>X2 acts as X2=240 (NOT empty), Y1>Y2 acts as Y2=160.
	const bool win0_on = (dispcnt & 0x2000u) != 0;
	const bool win1_on = (dispcnt & 0x4000u) != 0;
	const bool objwin_on = (dispcnt & 0x8000u) != 0;
	auto win0_has = [&](unsigned x) -> bool {
		unsigned x1 = (ppu.win0h_ >> 8) & 0xFFu, x2 = ppu.win0h_ & 0xFFu;
		unsigned y1 = (ppu.win0v_ >> 8) & 0xFFu, y2 = ppu.win0v_ & 0xFFu;
		if (x1 > x2)
			x2 = GbaPpu::kWidth;
		if (y1 > y2)
			y2 = GbaPpu::kHeight;
		if (x2 > GbaPpu::kWidth)
			x2 = GbaPpu::kWidth;
		if (y2 > GbaPpu::kHeight)
			y2 = GbaPpu::kHeight;
		return x >= x1 && x < x2 && y >= y1 && y < y2;
	};
	auto win1_has = [&](unsigned x) -> bool {
		unsigned x1 = (ppu.win1h_ >> 8) & 0xFFu, x2 = ppu.win1h_ & 0xFFu;
		unsigned y1 = (ppu.win1v_ >> 8) & 0xFFu, y2 = ppu.win1v_ & 0xFFu;
		if (x1 > x2)
			x2 = GbaPpu::kWidth;
		if (y1 > y2)
			y2 = GbaPpu::kHeight;
		if (x2 > GbaPpu::kWidth)
			x2 = GbaPpu::kWidth;
		if (y2 > GbaPpu::kHeight)
			y2 = GbaPpu::kHeight;
		return x >= x1 && x < x2 && y >= y1 && y < y2;
	};

	// ---- Compose + blend ----
	const u32 backdrop = GbaPpu::rgb555(ppu.palEntry(0));
	const unsigned effect = (ppu.bldcnt_ >> 6) & 3u;
	unsigned eva = ppu.bldalpha_ & 0x1Fu;
	unsigned evb = (ppu.bldalpha_ >> 8) & 0x1Fu;
	if (eva > 16)
		eva = 16;
	if (evb > 16)
		evb = 16;
	const unsigned evy_raw = ppu.bldy_ & 0x1Fu;
	const unsigned evy = evy_raw > 16 ? 16 : evy_raw;
	for (unsigned x = 0; x < GbaPpu::kWidth; x++) {
		// Region enables (WIN0 > WIN1 > OBJWIN > outside).
		u32 layer_en;
		bool blend_en;
		if (!win0_on && !win1_on && !objwin_on) {
			layer_en = (dispcnt >> 8) & 0x1Fu;
			blend_en = true;
		} else if (win0_on && win0_has(x)) {
			layer_en = ppu.winin_ & 0x1Fu;
			blend_en = (ppu.winin_ & 0x20u) != 0;
		} else if (win1_on && win1_has(x)) {
			layer_en = (ppu.winin_ >> 8) & 0x1Fu;
			blend_en = (ppu.winin_ & 0x2000u) != 0;
		} else if (objwin_on && obj_win[x]) {
			layer_en = (ppu.winout_ >> 8) & 0x1Fu;
			blend_en = (ppu.winout_ & 0x2000u) != 0;
		} else {
			layer_en = ppu.winout_ & 0x1Fu;
			blend_en = (ppu.winout_ & 0x20u) != 0;
		}
		// Ordered candidate stack (backdrop last).
		LayerPx stack[6];
		unsigned n = 0;
		if ((layer_en & 0x10u) != 0 && obj[x].present && obj_enable) {
			stack[n++] = {obj[x].color, obj[x].prio, 4, obj[x].semi, true};
		}
		for (unsigned b = 0; b < 4; b++) {
			if (((layer_en >> b) & 1u) == 0 || !bg[b][x].present)
				continue;
			if (!bg_enable[b])
				continue;
			// Insert sorted by (prio, kind): lower number wins; an OBJ
			// already in the stack (kind 4) stays ahead of any BG with
			// the same priority number (mGBA packs sprite index 0 below
			// every BG index, so OBJ wins all number ties); BG0 wins BG
			// ties (GBATEK "BG0 is having the highest").
			unsigned k = n;
			while (k > 0 && (bg_prio[b] < stack[k - 1].prio ||
							 (bg_prio[b] == stack[k - 1].prio && stack[k - 1].kind != 4 &&
								 b < stack[k - 1].kind))) {
				stack[k] = stack[k - 1];
				k--;
			}
			stack[k] = {bg[b][x].color, bg_prio[b], static_cast<u8>(b), false, true};
			n++;
		}
		stack[n++] = {backdrop, 4, 5, false, true};
		const LayerPx &top = stack[0];
		u32 out = top.color;
		const u32 tgt1 = (ppu.bldcnt_ & 0x3Fu) | (top.semi ? 0x10u : 0u);
		const bool is_first = ((tgt1 >> top.kind) & 1u) != 0;
		const unsigned mode_eff = top.semi ? 1u : effect;
		if (blend_en && is_first && mode_eff == 1) {
			// Alpha: mix with the next-lower pixel IFF it is a 2nd target
			// (GBATEK: a non-2nd-target pixel in between disables blending;
			// only the immediately lower pixel qualifies, never a deeper one).
			const u32 tgt2 = (ppu.bldcnt_ >> 8) & 0x3Fu;
			if (n > 1 && ((tgt2 >> stack[1].kind) & 1u) != 0) {
				const u32 c1 = top.color, c2 = stack[1].color;
				const unsigned r = (((c1 >> 16) & 0xFFu) * eva + ((c2 >> 16) & 0xFFu) * evb) / 16;
				const unsigned g = (((c1 >> 8) & 0xFFu) * eva + ((c2 >> 8) & 0xFFu) * evb) / 16;
				const unsigned b = ((c1 & 0xFFu) * eva + (c2 & 0xFFu) * evb) / 16;
				const unsigned rc = r > 255 ? 255 : r;
				const unsigned gc = g > 255 ? 255 : g;
				const unsigned bc = b > 255 ? 255 : b;
				out = 0xFF000000u | (rc << 16) | (gc << 8) | bc;
			}
		} else if (blend_en && is_first && (mode_eff == 2 || mode_eff == 3)) {
			auto adj = [&](unsigned v) -> unsigned {
				if (mode_eff == 2) {
					const unsigned r = v + ((255 - v) * evy) / 16;
					return r > 255 ? 255 : r;
				}
				const unsigned r = v - (v * evy) / 16;
				return r > 255 ? 0 : r;
			};
			const unsigned r = adj((top.color >> 16) & 0xFFu);
			const unsigned g = adj((top.color >> 8) & 0xFFu);
			const unsigned b = adj(top.color & 0xFFu);
			out = 0xFF000000u | (r << 16) | (g << 8) | b;
		}
		line[x] = out;
	}

	// Green swap (final output effect).
	if ((ppu.greenswap_ & 1u) != 0) {
		for (unsigned x = 0; x + 1 < GbaPpu::kWidth; x += 2) {
			const u32 a = line[x], b = line[x + 1];
			const u32 ga = (a >> 8) & 0xFFu, gb = (b >> 8) & 0xFFu;
			line[x] = (a & 0xFFFF00FFu) | (gb << 8);
			line[x + 1] = (b & 0xFFFF00FFu) | (ga << 8);
		}
	}
}

} // namespace gba
