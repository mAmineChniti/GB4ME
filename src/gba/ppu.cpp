// GBA PPU core: video memory, LCD registers, scanline timing, IRQ/DMA events.
// Rendering lives in ppu_render.cpp. Register map: GBATEK "GBA I/O Map".

#include "gba/ppu.h"

#include "gba/dma.h"
#include "gba/irq.h"

namespace gba
{

GbaPpu::GbaPpu()
{
	reset();
}

void GbaPpu::reset()
{
	pal_.fill(0);
	vram_.fill(0);
	oam_.fill(0);
	dispcnt_ = 0;
	dispstat_ = 0;
	greenswap_ = 0;
	for (int i = 0; i < 4; i++)
		bgcnt_[i] = bghofs_[i] = bgvofs_[i] = 0;
	for (int i = 0; i < 2; i++) {
		bgpa_[i] = bgpb_[i] = bgpc_[i] = bgpd_[i] = 0;
		bgx_latch_[i] = bgy_latch_[i] = 0;
		bgx_[i] = bgy_[i] = 0;
	}
	win0h_ = win1h_ = win0v_ = win1v_ = 0;
	winin_ = winout_ = 0;
	mosaic_ = 0;
	bldcnt_ = bldalpha_ = bldy_ = 0;
	line_ = 0;
	line_cycles_ = 0;
	hblank_ = false;
	frame_.pixels.fill(0xFF000000u);
	frame_ready_ = false;
}

// ---- Video memory (bus-facing) ----

u8 GbaPpu::readPal(u32 off) const
{
	off &= kPalSize - 1;
	return pal_[off];
}

void GbaPpu::writePal(u32 off, u8 value)
{
	// Byte writes replicate to both halves (hardware behavior).
	writePal16(off & ~1u, static_cast<u16>(value | (value << 8)));
}

void GbaPpu::writePal16(u32 off, u16 value)
{
	off &= kPalSize - 1;
	off &= ~1u;
	pal_[off] = static_cast<u8>(value & 0xFFu);
	pal_[off + 1] = static_cast<u8>(value >> 8);
}

u32 GbaPpu::mirrorVram(u32 off)
{
	off &= 0x1FFFFu;
	// 96K VRAM repeats in 128K steps; the upper 32K block mirrors
	// 0x06010000-0x06017FFF (GBATEK "most internal memory is mirrored").
	if (off >= kVramSize)
		off = 0x10000u | (off & 0x7FFFu);
	return off;
}

u8 GbaPpu::readVram(u32 off) const
{
	return vram_[mirrorVram(off)];
}

void GbaPpu::writeVram(u32 off, u8 value)
{
	off = mirrorVram(off);
	// Byte writes replicate except in OBJ VRAM (ignored there). OBJ VRAM
	// is 06010000+ in tile modes, 06014000+ in bitmap modes.
	const bool bitmap = (dispcnt_ & 7u) >= 3;
	const u32 obj_base = bitmap ? 0x14000u : 0x10000u;
	if (off >= obj_base)
		return;
	const u32 aligned = off & ~1u;
	vram_[aligned] = value;
	vram_[aligned + 1] = value;
}

void GbaPpu::writeVram16(u32 addr, u16 value)
{
	// 16-bit VRAM writes are native: no byte-replication and no OBJ-VRAM
	// ignore rule (GBATEK VRAM). Mirror each byte so straddles into the
	// upper 32K window behave.
	addr &= ~1u;
	vram_[mirrorVram(addr)] = static_cast<u8>(value & 0xFFu);
	vram_[mirrorVram(addr + 1)] = static_cast<u8>(value >> 8);
}

u8 GbaPpu::readOam(u32 off) const
{
	off &= kOamSize - 1;
	return oam_[off];
}

void GbaPpu::writeOam(u32 off, u8 value)
{
	(void) off;
	(void) value; // 8-bit OAM writes are ignored on hardware.
}

void GbaPpu::writeOam16(u32 off, u16 value)
{
	off &= kOamSize - 1;
	off &= ~1u;
	oam_[off] = static_cast<u8>(value & 0xFFu);
	oam_[off + 1] = static_cast<u8>(value >> 8);
}

// ---- LCD registers (bus-facing, 16-bit) ----

u16 GbaPpu::readIo(u32 addr) const
{
	switch (addr) {
	case 0x04000000u:
		return dispcnt_;
	case 0x04000002u:
		return greenswap_;
	case 0x04000004u:
		return dispstat_;
	case 0x04000006u:
		return static_cast<u16>(line_);
	case 0x04000008u:
	case 0x0400000Au:
	case 0x0400000Cu:
	case 0x0400000Eu:
		return bgcnt_[(addr - 0x04000008u) / 2];
	case 0x04000010u:
	case 0x04000014u:
	case 0x04000018u:
	case 0x0400001Cu:
		return 0xFFFF; // Write-only scroll offsets read back open.
	case 0x04000020u:
	case 0x04000022u:
	case 0x04000024u:
	case 0x04000026u:
	case 0x04000030u:
	case 0x04000032u:
	case 0x04000034u:
	case 0x04000036u:
		return 0xFFFF; // Write-only affine parameters.
	case 0x04000028u:
	case 0x0400002Au:
	case 0x0400002Cu:
	case 0x0400002Eu:
	case 0x04000038u:
	case 0x0400003Au:
	case 0x0400003Cu:
	case 0x0400003Eu: {
		// Reference points: readable halves of the internal registers.
		const unsigned i = (addr & 0x10u) != 0 ? 1 : 0;
		const bool is_y = (addr & 0x04u) != 0;
		const i32 v = is_y ? bgy_[i] : bgx_[i];
		if (((addr & 2u) != 0))
			return static_cast<u16>((v >> 16) & 0x0FFFu);
		return static_cast<u16>(v & 0xFFFFu);
	}
	case 0x04000040u:
	case 0x04000042u:
	case 0x04000044u:
	case 0x04000046u:
		return 0xFFFF; // Write-only window dimensions.
	case 0x04000048u:
		return winin_;
	case 0x0400004Au:
		return winout_;
	case 0x0400004Cu:
		return 0xFFFF; // Write-only mosaic.
	case 0x04000050u:
		return bldcnt_;
	case 0x04000052u:
		return bldalpha_;
	case 0x04000054u:
		return 0xFFFF; // Write-only brightness.
	default:
		return 0xFFFF;
	}
}

void GbaPpu::writeIo(u32 addr, u16 value)
{
	switch (addr) {
	case 0x04000000u:
		dispcnt_ = value;
		return;
	case 0x04000002u:
		greenswap_ = value & 1u;
		return;
	case 0x04000004u: {
		// Writable: IRQ enables + LYC. Flags recomputed; VCount edge
		// raises its IRQ like hardware (mGBA parity).
		const bool flag_before = (dispstat_ & 0x04u) != 0;
		dispstat_ = (dispstat_ & 0x07u) | (value & 0xFFF8u);
		updateVCountFlag(false);
		const bool flag_after = (dispstat_ & 0x04u) != 0;
		if (flag_after && !flag_before && (dispstat_ & 0x20u) != 0 && irq_ != nullptr) {
			irq_->raise(GbaIrq::kVCount);
		}
		return;
	}
	case 0x04000006u:
		return; // VCOUNT read-only.
	case 0x04000008u:
	case 0x0400000Au:
	case 0x0400000Cu:
	case 0x0400000Eu:
		// BG0/BG1CNT bit 13 is reserved (reads 0); BG2/BG3CNT fully readable
		// (mGBA suite io-read: 0xDFFF / 0xFFFF).
		if (addr == 0x04000008u || addr == 0x0400000Au)
			value &= 0xDFFFu;
		bgcnt_[(addr - 0x04000008u) / 2] = value;
		return;
	case 0x04000010u:
		bghofs_[0] = value & 0x1FFu;
		return;
	case 0x04000012u:
		bgvofs_[0] = value & 0x1FFu;
		return;
	case 0x04000014u:
		bghofs_[1] = value & 0x1FFu;
		return;
	case 0x04000016u:
		bgvofs_[1] = value & 0x1FFu;
		return;
	case 0x04000018u:
		bghofs_[2] = value & 0x1FFu;
		return;
	case 0x0400001Au:
		bgvofs_[2] = value & 0x1FFu;
		return;
	case 0x0400001Cu:
		bghofs_[3] = value & 0x1FFu;
		return;
	case 0x0400001Eu:
		bgvofs_[3] = value & 0x1FFu;
		return;
	case 0x04000020u:
		bgpa_[0] = value;
		return;
	case 0x04000022u:
		bgpb_[0] = value;
		return;
	case 0x04000024u:
		bgpc_[0] = value;
		return;
	case 0x04000026u:
		bgpd_[0] = value;
		return;
	case 0x04000030u:
		bgpa_[1] = value;
		return;
	case 0x04000032u:
		bgpb_[1] = value;
		return;
	case 0x04000034u:
		bgpc_[1] = value;
		return;
	case 0x04000036u:
		bgpd_[1] = value;
		return;
	case 0x04000028u:
	case 0x0400002Au:
	case 0x04000038u:
	case 0x0400003Au: {
		// Reference X halves (28-bit signed); writes go to latch.
		// Internal register is reloaded from latch at VBlank start.
		// Writes OUTSIDE VBlank copy to the internal register immediately
		// (GBATEK "Internal Reference Point Registers": the new value then
		// specifies the origin of the <current> scanline). Writes DURING
		// VBlank only stage the value for the next frame's reload.
		const unsigned i = (addr & 0x10u) != 0 ? 1 : 0;
		if ((addr & 2u) != 0) {
			bgx_latch_[i] = (bgx_latch_[i] & 0xFFFF) | ((static_cast<i32>(value & 0x0FFFu) << 16));
			if ((value & 0x0800u) != 0)
				bgx_latch_[i] |= static_cast<i32>(0xF0000000u);
			else
				bgx_latch_[i] &= 0x0FFFFFFFu;
		} else {
			bgx_latch_[i] = (bgx_latch_[i] & ~0xFFFF) | value;
		}
		if ((dispstat_ & 0x01u) == 0)
			bgx_[i] = bgx_latch_[i];
		return;
	}
	case 0x0400002Cu:
	case 0x0400002Eu:
	case 0x0400003Cu:
	case 0x0400003Eu: {
		const unsigned i = (addr & 0x10u) != 0 ? 1 : 0;
		if ((addr & 2u) != 0) {
			bgy_latch_[i] = (bgy_latch_[i] & 0xFFFF) | ((static_cast<i32>(value & 0x0FFFu) << 16));
			if ((value & 0x0800u) != 0)
				bgy_latch_[i] |= 0xF0000000u;
			else
				bgy_latch_[i] &= 0x0FFFFFFFu;
		} else {
			bgy_latch_[i] = (bgy_latch_[i] & ~0xFFFF) | value;
		}
		// Same VBlank latch rule as BGxX (see above).
		if ((dispstat_ & 0x01u) == 0)
			bgy_[i] = bgy_latch_[i];
		return;
	}
	case 0x04000040u:
		win0h_ = value;
		return;
	case 0x04000042u:
		win1h_ = value;
		return;
	case 0x04000044u:
		win0v_ = value;
		return;
	case 0x04000046u:
		win1v_ = value;
		return;
	case 0x04000048u:
		winin_ = value & 0x3F3Fu; // Reserved bits read 0 (suite io-read).
		return;
	case 0x0400004Au:
		winout_ = value & 0x3F3Fu;
		return;
	case 0x0400004Cu:
		mosaic_ = value;
		return;
	case 0x04000050u:
		bldcnt_ = value & 0x3FFFu;
		return;
	case 0x04000052u:
		bldalpha_ = value & 0x1F1Fu;
		return;
	case 0x04000054u:
		bldy_ = value & 0x1Fu;
		return;
	default:
		return;
	}
}

// ---- Timing ----

void GbaPpu::updateVCountFlag(bool raise_irq)
{
	const unsigned lyc = (dispstat_ >> 8) & 0xFFu;
	if (line_ == lyc) {
		dispstat_ |= 0x04u;
		if (raise_irq && (dispstat_ & 0x20u) != 0 && irq_ != nullptr) {
			irq_->raise(GbaIrq::kVCount);
		}
	} else {
		dispstat_ &= ~0x04u;
	}
}

void GbaPpu::enterHBlank()
{
	hblank_ = true;
	dispstat_ |= 0x02u;
	if (line_ < kHeight)
		renderScanline(*this, line_);
	// HBlank DMA fires on visible scanlines only (mGBA gates
	// GBADMARunHblank on vcount < 160; VBlank lines have no HBlank DMA
	// start condition). Firing during VBlank would overrun repeat
	// channels by 68 extra blocks/frame, desyncing raster effects.
	if (line_ < kHeight && dma_ != nullptr)
		dma_->trigger(GbaDma::Timing::HBlank);
	// HBlank IRQ on every line while enabled (mGBA parity; GBATEK's "no
	// HBlank IRQ in VBlank" note recorded as a known disagreement — a test
	// ROM will decide if this ever matters).
	if ((dispstat_ & 0x10u) != 0 && irq_ != nullptr) {
		irq_->raise(GbaIrq::kHBlank);
	}
}

void GbaPpu::endLine()
{
	hblank_ = false;
	dispstat_ &= ~0x02u;
	const unsigned finished = line_;
	line_++;
	if (line_ >= kLines)
		line_ = 0;
	updateVCountFlag(true);
	if (line_ == kVBlankLine) {
		// VBlank start: reload internal affine reference points from
		// latches (GBATEK "Internal Reference Point Registers"). The
		// reloaded values are the origin of the next frame's first
		// scanline, so no stepping happens on this transition.
		bgx_[0] = bgx_latch_[0];
		bgy_[0] = bgy_latch_[0];
		bgx_[1] = bgx_latch_[1];
		bgy_[1] = bgy_latch_[1];

		dispstat_ |= 0x01u;
		frame_ready_ = true;
		if (dma_ != nullptr)
			dma_->trigger(GbaDma::Timing::VBlank);
		if ((dispstat_ & 0x08u) != 0 && irq_ != nullptr) {
			irq_->raise(GbaIrq::kVBlank);
		}
	} else if (line_ == kLines - 1) {
		dispstat_ &= ~0x01u; // VBlank flag spans 160-226, not 227.
	}
	// Affine reference points advance after each *visible* scanline
	// (GBATEK: the reloaded values specify the first scanline's origin).
	// VBlank lines 160-227 never step; the finished==159 transition is
	// covered by the reload above.
	if (finished < kVBlankLine - 1) {
		for (int i = 0; i < 2; i++) {
			bgx_[i] += static_cast<i16>(bgpb_[i]);
			bgy_[i] += static_cast<i16>(bgpd_[i]);
		}
	}
}

void GbaPpu::step(u32 cycles)
{
	line_cycles_ += cycles;
	while (true) {
		if (!hblank_ && line_cycles_ >= kDrawEnd) {
			// Draw portion finished: render, then count the remainder
			// inside the blank period.
			line_cycles_ -= kDrawEnd;
			enterHBlank();
			continue;
		}
		if (hblank_ && line_cycles_ >= (kDotsPerLine - kDrawEnd)) {
			line_cycles_ -= (kDotsPerLine - kDrawEnd);
			endLine();
			continue;
		}
		break;
	}
}

u32 GbaPpu::rgb555(u16 c)
{
	const u32 r = c & 0x1Fu, g = (c >> 5) & 0x1Fu, b = (c >> 10) & 0x1Fu;
	const u32 r8 = (r << 3) | (r >> 2);
	const u32 g8 = (g << 3) | (g >> 2);
	const u32 b8 = (b << 3) | (b >> 2);
	return 0xFF000000u | (r8 << 16) | (g8 << 8) | b8;
}

u16 GbaPpu::palEntry(unsigned i) const
{
	i &= 0x1FFu;
	return static_cast<u16>(pal_[i * 2] | (pal_[i * 2 + 1] << 8));
}

void GbaPpu::save(StateBuffer &out) const
{
	out.write(dispcnt_);
	out.write(dispstat_);
	out.write(greenswap_);
	for (int i = 0; i < 4; i++) {
		out.write(bgcnt_[i]);
		out.write(bghofs_[i]);
		out.write(bgvofs_[i]);
	}
	for (int i = 0; i < 2; i++) {
		out.write(bgpa_[i]);
		out.write(bgpb_[i]);
		out.write(bgpc_[i]);
		out.write(bgpd_[i]);
		out.write(bgx_[i]);
		out.write(bgy_[i]);
		out.write(bgx_latch_[i]);
		out.write(bgy_latch_[i]);
	}
	out.write(win0h_);
	out.write(win1h_);
	out.write(win0v_);
	out.write(win1v_);
	out.write(winin_);
	out.write(winout_);
	out.write(mosaic_);
	out.write(bldcnt_);
	out.write(bldalpha_);
	out.write(bldy_);
	out.write(line_);
	out.write(line_cycles_);
	out.write(hblank_);
	out.write(frame_ready_);
	out.writeBytes(pal_.data(), pal_.size());
	out.writeBytes(vram_.data(), vram_.size());
	out.writeBytes(oam_.data(), oam_.size());
}

void GbaPpu::load(const StateBuffer &in)
{
	in.read(dispcnt_);
	in.read(dispstat_);
	in.read(greenswap_);
	for (int i = 0; i < 4; i++) {
		in.read(bgcnt_[i]);
		in.read(bghofs_[i]);
		in.read(bgvofs_[i]);
	}
	for (int i = 0; i < 2; i++) {
		in.read(bgpa_[i]);
		in.read(bgpb_[i]);
		in.read(bgpc_[i]);
		in.read(bgpd_[i]);
		in.read(bgx_[i]);
		in.read(bgy_[i]);
		// Latches postdate v1 states; keep them (defaults to internals
		// when absent so old states still load).
		if (!in.read(bgx_latch_[i]))
			bgx_latch_[i] = bgx_[i];
		if (!in.read(bgy_latch_[i]))
			bgy_latch_[i] = bgy_[i];
	}
	in.read(win0h_);
	in.read(win1h_);
	in.read(win0v_);
	in.read(win1v_);
	in.read(winin_);
	in.read(winout_);
	in.read(mosaic_);
	in.read(bldcnt_);
	in.read(bldalpha_);
	in.read(bldy_);
	in.read(line_);
	in.read(line_cycles_);
	in.read(hblank_);
	in.read(frame_ready_);
	in.readBytes(pal_.data(), pal_.size());
	in.readBytes(vram_.data(), vram_.size());
	in.readBytes(oam_.data(), oam_.size());
}

} // namespace gba
