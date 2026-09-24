#pragma once

#include "gba/state.h"
#include <array>

namespace gba
{

class GbaBus;
class GbaIrq;
class GbaDma;

// GBA PPU (Phase 6): scanline-timed video, modes 0-5, sprites, windows,
// blending, mosaic. Owns palette/VRAM/OAM (gb::PPU precedent); the bus
// delegates those regions + LCD registers here.
// Timing: 1232 cycles/line (1008 draw + 224 blank), 228 lines, 280896/frame.
// https://problemkaputt.de/gbatek.htm#gbalcdvideocontroller
class GbaPpu
{
  public:
	static constexpr unsigned kWidth = 240;
	static constexpr unsigned kHeight = 160;
	static constexpr unsigned kLines = 228;
	static constexpr unsigned kDotsPerLine = 1232;
	static constexpr unsigned kDrawEnd = 1008; // HBlank starts (mGBA-tested split).
	static constexpr unsigned kVBlankLine = 160;

	static constexpr u32 kPalSize = 0x400;
	static constexpr u32 kVramSize = 0x18000;
	static constexpr u32 kOamSize = 0x400;

	// LCD I/O block owned here (routed by the bus).
	static constexpr u32 kIoBase = 0x04000000;
	static constexpr u32 kIoEnd = 0x04000056;

	struct FrameData {
		std::array<u32, kWidth * kHeight> pixels{};
	};

	GbaPpu();

	void reset();
	void setIrq(GbaIrq *irq)
	{
		irq_ = irq;
	}
	void setDma(GbaDma *dma)
	{
		dma_ = dma;
	}

	// Save state serialization.
	void save(StateBuffer &out) const;
	void load(const StateBuffer &in);

	// Bus-facing video memory (mirrors preserved; GBA write rules).
	// 8-bit PAL writes replicate the byte to both halves; 8-bit VRAM
	// writes replicate except in OBJ VRAM (ignored there); 8-bit OAM
	// writes are ignored. 16/32-bit accesses are native.
	u8 readPal(u32 off) const;
	void writePal(u32 off, u8 value);
	void writePal16(u32 off, u16 value);
	u8 readVram(u32 off) const;
	void writeVram(u32 off, u8 value);
	void writeVram16(u32 off, u16 value);
	u8 readOam(u32 off) const;
	// Per-frame OAM snapshot the renderer reads. Hardware cannot reach OAM
	// during active display (only in HBlank with DISPCNT bit 5, or VBlank -
	// GBATEK "VRAM, OAM, and Palette RAM Access"), so a mid-frame OAM write
	// (OAM DMA landing between scanlines) must not change the frame already
	// being drawn; that was the Golden Sun sprite flicker.
	u8 readOamFrame(u32 off) const
	{
		return oam_frame_[off & (kOamSize - 1)];
	}
	void writeOam(u32 off, u8 value);
	void writeOam16(u32 off, u16 value);
	// Bus-facing LCD registers (full 16-bit semantics).
	u16 readIo(u32 addr) const;
	void writeIo(u32 addr, u16 value);

	// Advance the dot clock; fires HBlank/VBlank/VCount events (IRQ + DMA).
	void step(u32 cycles);

	const FrameData &frame() const
	{
		return frame_;
	}
	// Diagnostic view of the WRITE-ONLY BG registers. Reading them through
	// the bus returns open bus (0xFFFF), so a debugger has to ask the PPU
	// directly to see the scroll offsets and affine parameters the renderer
	// is actually using.
	u16 scrollH(unsigned b) const
	{
		return bghofs_[b & 3u];
	}
	u16 scrollV(unsigned b) const
	{
		return bgvofs_[b & 3u];
	}
	u16 affineParam(unsigned set, unsigned index) const
	{
		switch (index) {
		case 0:
			return bgpa_[set & 1u];
		case 1:
			return bgpb_[set & 1u];
		case 2:
			return bgpc_[set & 1u];
		default:
			return bgpd_[set & 1u];
		}
	}
	i32 affineX(unsigned set) const
	{
		return bgx_[set & 1u];
	}
	i32 affineY(unsigned set) const
	{
		return bgy_[set & 1u];
	}
	// Raw BG/OBJ VRAM backing for the scanline renderer's inner loops. The
	// caller is responsible for masking offsets (<= 0x1FFFF); this avoids
	// an out-of-line call per pixel.
	const u8 *vramData() const
	{
		return vram_.data();
	}
	bool frameReady() const
	{
		return frame_ready_;
	}
	void clearFrameReady()
	{
		frame_ready_ = false;
	}
	unsigned line() const
	{
		return line_;
	}

  private:
	// Upper-32K VRAM mirror (GBATEK): 0x06018000-0x0601FFFF mirrors
	// 0x06010000-0x06017FFF.
	static u32 mirrorVram(u32 off);
	friend void renderScanline(GbaPpu &ppu, unsigned y);

	GbaIrq *irq_ = nullptr;
	GbaDma *dma_ = nullptr;

	std::array<u8, kPalSize> pal_{};
	std::array<u8, kVramSize> vram_{};
	std::array<u8, kOamSize> oam_{};
	std::array<u8, kOamSize> oam_frame_{};

	// LCD registers.
	u16 dispcnt_ = 0;
	u16 dispstat_ = 0; // Bits 0-2 read-only flags; 3-5 IRQ enables; 8-15 LYC.
	u16 greenswap_ = 0;
	u16 bgcnt_[4]{};
	u16 bghofs_[4]{};
	u16 bgvofs_[4]{};
	u16 bgpa_[2]{}, bgpb_[2]{}, bgpc_[2]{}, bgpd_[2]{};
	// Affine reference points: latch = write-only IO register,
	// internal = accumulator that advances each scanline (reloaded from latch at VBlank).
	i32 bgx_latch_[2]{}, bgy_latch_[2]{}; // Written by BGxX/BGxY IO writes.
	i32 bgx_[2]{}, bgy_[2]{}; // Internal 28-bit reference points (reloaded at VBlank).
	u16 win0h_ = 0, win1h_ = 0, win0v_ = 0, win1v_ = 0;
	u16 winin_ = 0, winout_ = 0;
	u16 mosaic_ = 0;
	u16 bldcnt_ = 0, bldalpha_ = 0, bldy_ = 0;

	unsigned line_ = 0;   // VCOUNT 0-227.
	u32 line_cycles_ = 0; // Dots into the current line (0-1231).
	bool hblank_ = false;
	FrameData frame_{};
	bool frame_ready_ = false;

	// Event helpers.
	void enterHBlank();
	void endLine();
	void updateVCountFlag(bool raise_irq);
	static u32 rgb555(u16 c);
	u16 palEntry(unsigned i) const;
};

// Scanline renderer (ppu_render.cpp).
void renderScanline(GbaPpu &ppu, unsigned y);

} // namespace gba
