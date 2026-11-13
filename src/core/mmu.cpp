// Bus: routes every memory access to the owning component.
// https://gbdev.io/pandocs/Memory_Map.html
//
// DMG groups below; CGB registers are active only when hardware_mode is CGB.
// https://gbdev.io/pandocs/CGB_Registers.html
// GBC: each stub will activate real behavior (WRAM/VRAM banking, HDMA, palettes).

#include "gb/mmu.h"

#include "gb/apu.h"
#include "gb/cartridge.h"
#include "gb/cheats.h"
#include "gb/cpu.h"
#include "gb/joypad.h"
#include "gb/ppu.h"
#include "gb/timer.h"
#include <cstdio>
#include <cstdlib>

namespace gb
{

MMU::MMU()
{
	reset();
}

void MMU::reset()
{
	boot_rom_.fill(0);
	for (auto &bank : wram_banks_)
		bank.fill(0);
	hram_.fill(0);
	ie_register_ = 0;
	if_register_ = 0;
	serial_sb = 0x00;
	serial_sc = 0x7E;
	serial_count_ = 0;
	serial_master_clock_ = false;
	serial_mask_ = 0x80;
	serial_log.clear();
	boot_rom_enabled = true;
	wram_bank_ = 1;
	hardware_mode_ = HardwareMode::DMG;
	key1_ = 0;
	hdma1_ = hdma2_ = hdma3_ = hdma4_ = hdma5_ = 0;
	hdma_active_ = false;
	hdma_src_ = hdma_dst_ = 0;
	hdma_len_ = 0;
	hdma_mode_hblank_ = false;
	ff72_ = ff73_ = 0;
	ff74_ = 0;
	ff75_ = 0x8F;
}

void MMU::set_hardware_mode(HardwareMode m)
{
	hardware_mode_ = m;
	if (!is_cgb_mode(m))
		wram_bank_ = 1;
	if (ppu_)
		ppu_->set_hardware_mode(m);
	if (apu_)
		apu_->set_hardware_mode(m);
}

void MMU::set_div_frozen(bool frozen)
{
	if (timer_)
		timer_->set_div_frozen(frozen);
}

void MMU::do_speed_switch(CPU *cpu)
{
	// https://gbdev.io/pandocs/CGB_Registers.html#ff4d--key1-cgb-mode-only-prepare-speed-switch
	// Toggle double_speed, clear prepare, update clock. STOP already consumed.
	bool new_double = !cpu->double_speed;
	cpu->double_speed = new_double;
	cpu->clock_speed_hz = new_double ? kDefaultDmgClockHz * 2 : kDefaultDmgClockHz;
	// Update KEY1: bit7 = new speed, bit0 = 0 (request consumed)
	key1_ = (key1_ & ~0x01) | (new_double ? 0x80 : 0x00);
	key1_ &= ~0x01;
	if (new_double)
		key1_ |= 0x80;
	else
		key1_ &= ~0x80;
	// The divider runs at 2x wall-rate in double-speed (timer/TIMA with it;
	// PPU/APU keep their usual rate — see Timer::set_double_speed).
	if (timer_)
		timer_->set_double_speed(new_double);
	// GBC: this will branch on double_speed mode (KEY1 register).
}

u8 MMU::read_key1() const
{
	if (!is_cgb())
		return 0xFF;
	// Bit7 speed, bit0 prepare, bits1-6 read as 1? Pan docs shows bits 1-6 =1
	u8 v = 0x7E;
	if (key1_ & 0x80)
		v |= 0x80;
	if (key1_ & 0x01)
		v |= 0x01;
	return v;
}

void MMU::write_key1(u8 v)
{
	if (!is_cgb())
		return;
	// Only bit0 is writable (prepare switch)
	key1_ = (key1_ & 0x80) | (v & 0x01);
}

void MMU::hdma_start()
{
	// Abort handling: writing bit7=0 while HBlank DMA active terminates it
	// per Pan Docs. Bit7=0 with active should abort, not restart.
	if (hdma_active_ && (hdma5_ & 0x80) == 0) {
		hdma_active_ = false;
		hdma5_ |= 0x80; // mark not active (bit7=1 per spec 1=Not Active)
		// Note: HDMA1-4 are not reset to 0xFF on abort per spec
		return;
	}
	hdma_src_ = (static_cast<u16>(hdma1_) << 8) | hdma2_;
	hdma_dst_ = (static_cast<u16>(hdma3_) << 8) | hdma4_;
	hdma_src_ &= 0xFFF0;
	hdma_dst_ = (hdma_dst_ & 0x1FF0) | 0x8000; // dest is 8000-9FFF only
	hdma_dst_ &= 0x9FF0;
	u8 len = (hdma5_ & 0x7F);
	hdma_len_ = len;
	hdma_mode_hblank_ = (hdma5_ & 0x80) != 0;
	if (!hdma_mode_hblank_) {
		// GDMA: transfer immediately (all blocks) — blind to PPU mode per Pan Docs,
		// but respects VBK. CPU halted 8 M-cycles per block (16 fast) is modeled
		// as immediate here; proper stall will be added with T-cycle scheduler.
		u32 blocks = len + 1;
		for (u32 b = 0; b < blocks; b++) {
			for (u32 i = 0; i < 16; i++) {
				u8 val = read(hdma_src_ + b * 16 + i);
				if (ppu_)
					ppu_->write_vram_dma(hdma_dst_ - 0x8000 + b * 16 + i, val);
				else
					write(hdma_dst_ - 0x8000 + b * 16 + i, val);
			}
		}
		hdma_src_ += blocks * 16;
		hdma_dst_ += blocks * 16;
		hdma5_ = 0xFF; // transfer done, bit7=1 not active
		hdma_active_ = false;
	} else {
		// HBlank DMA: will transfer 16 bytes per HBlank, bit7=0 active
		hdma_active_ = true;
		hdma5_ &= 0x7F; // remaining blocks -1, keep bit7=0
	}
}

void MMU::hdma_step_hblank()
{
	if (!hdma_active_)
		return;
	// Spec: must be in HBlank of visible lines 0-143, LCD on; otherwise block
	// is not transferred until next HBlank. Our PPU calls this already only
	// from the HBlank transition (see PPU::step HBlank hook), so just transfer.
	// Transfer one 16-byte block on this HBlank — respects VBK per Pan Docs
	// “should not change Destination VRAM bank (FF4F)”.
	for (u32 i = 0; i < 16; i++) {
		u8 val = read(hdma_src_ + i);
		if (ppu_)
			ppu_->write_vram_dma(hdma_dst_ - 0x8000 + i, val);
		else
			write(hdma_dst_ - 0x8000 + i, val);
	}
	// The block owns the bus for ~2us; the PPU blocks CPU VRAM/OAM access
	// for that window (CGB only, Pan Docs "CGB VRAM DMA").
	if (ppu_)
		ppu_->note_hdma_block();
	hdma_src_ += 16;
	hdma_dst_ += 16;
	if (hdma_len_ == 0) {
		hdma_active_ = false;
		hdma5_ = 0xFF; // done, bit7=1 not active
	} else {
		hdma_len_--;
		hdma5_ = hdma_len_; // keep bit7=0 active, remaining
	}
}

void MMU::serial_clock_edge()
{
	// The internal clock toggles every time the selected divider bit falls;
	// one bit is shifted per *full* period, i.e. on every other edge.
	// https://gbdev.io/pandocs/Serial_Data_Transfer_Link_Cable.html
	serial_master_clock_ = !serial_master_clock_;
	if (serial_master_clock_ || (serial_sc & 0x81) != 0x81)
		return;
	if (++serial_count_ < 8)
		return;
	serial_count_ = 0;
	serial_sc &= ~0x80; // Transfer finished: the start bit clears itself.
	// A completed transfer raises the serial interrupt (Pan Docs: requested on
	// the 1->0 edge of the transfer-complete bit). Serial = IF bit 3.
	request_interrupt(3);
	// SB is shifted left per bit, with the cable's incoming bit shifted in at
	// the bottom. Nothing is plugged into the other end here, so every incoming
	// bit reads 1 - exactly what SameBoy does without a link partner - and the
	// byte that went out is what the headless runner logs.
	const u8 sent = serial_sb;
	serial_sb = static_cast<u8>((sent << 1) | 0x01);
	// Bounded ring: a test ROM that loops printing would otherwise grow this
	// string until the process OOMs. Keeps the most recent 64KiB, which is far
	// more than any Blargg result line needs.
	if (serial_log.size() >= 65536)
		serial_log.erase(0, 32768);
	serial_log.push_back(static_cast<char>(sent));
}

u8 MMU::read(u16 addr)
{
	u8 v = 0xFF;
	if (boot_rom_enabled && addr < 0x100)
		v = boot_rom_[addr];
	else if (addr < 0x8000) {
		v = cartridge_ ? cartridge_->read_rom(addr) : 0xFF;
	} else if (addr < 0xA000) {
		// VRAM via PPU only (Rule 2: no direct array access).
		v = ppu_ ? ppu_->read_vram(addr - 0x8000) : 0xFF;
	} else if (addr < 0xC000) {
		v = cartridge_ ? cartridge_->read_ram(addr - 0xA000) : 0xFF;
	} else if (addr < 0xD000) {
		v = wram_banks_[0][addr - 0xC000];
	} else if (addr < 0xE000) {
		if (is_cgb()) {
			// GBC: D000-DFFF maps to selectable bank 1-7 via SVBK (0xFF70).
			// SVBK=0 is forced to 1.
			// GBC: this will branch on wram_bank_ (SVBK register 0xFF70).
			u8 bank = wram_bank_ & 0x07;
			if (bank == 0)
				bank = 1;
			v = wram_banks_[bank][addr - 0xD000];
		} else {
			v = wram_banks_[1][addr - 0xD000];
		}
	} else if (addr < 0xFE00) {
		v = read(addr - 0x2000); // Echo RAM mirror.
	} else if (addr < 0xFEA0) {
		v = ppu_ ? ppu_->read_oam(addr - 0xFE00) : 0xFF;
	} else if (addr < 0xFF00) {
		v = 0xFF; // Unusable memory.
	} else if (addr < 0xFF80) {
		return read_io(addr); // IO registers never take cheats.
	} else if (addr == 0xFFFF) {
		v = read_ie();
	} else {
		v = hram_[addr - 0xFF80];
	}
	// Cheats patch bus reads (ROM/RAM/WRAM/HRAM); IO is excluded above so
	// timer/serial/PPU registers can never be overridden by a typo.
	if (cheats_ != nullptr)
		v = cheats_->apply(addr, v);
	return v;
}

void MMU::write(u16 addr, u8 value)
{
	if (boot_rom_enabled && addr < 0x100)
		return; // ROM, not writable.

	if (addr < 0x8000) {
		if (cartridge_)
			cartridge_->write_rom(addr, value);
	} else if (addr < 0xA000) {
		if (ppu_)
			ppu_->write_vram(addr - 0x8000, value);
	} else if (addr < 0xC000) {
		if (cartridge_)
			cartridge_->write_ram(addr - 0xA000, value);
	} else if (addr < 0xD000) {
		wram_banks_[0][addr - 0xC000] = value;
	} else if (addr < 0xE000) {
		if (is_cgb()) {
			u8 bank = wram_bank_ & 0x07;
			if (bank == 0)
				bank = 1;
			wram_banks_[bank][addr - 0xD000] = value;
		} else {
			wram_banks_[1][addr - 0xD000] = value;
		}
	} else if (addr < 0xFE00) {
		write(addr - 0x2000, value); // Echo RAM mirror.
	} else if (addr < 0xFEA0) {
		if (ppu_)
			ppu_->write_oam(addr - 0xFE00, value);
	} else if (addr < 0xFF00) {
		// Unusable memory: writes ignored.
	} else if (addr < 0xFF80) {
		write_io(addr, value);
	} else if (addr == 0xFFFF) {
		ie_register_ = value;
	} else {
		hram_[addr - 0xFF80] = value;
	}
}

void MMU::trigger_oam_bug(u16 addr)
{
	if (ppu_)
		ppu_->trigger_oam_bug(addr);
}

u8 MMU::read_io(u16 addr)
{
	switch (addr) {
	// ---- DMG registers ----
	case 0xFF00:
		return joypad_ ? joypad_->read_p1() : 0xFF;
	case 0xFF01:
		return serial_sb;
	case 0xFF02:
		return serial_sc;
	case 0xFF04:
		return timer_ ? timer_->read_div() : 0xFF;
	case 0xFF05:
		return timer_ ? timer_->read_tima() : 0xFF;
	case 0xFF06:
		return timer_ ? timer_->read_tma() : 0xFF;
	case 0xFF07:
		return timer_ ? timer_->read_tac() : 0xFF;
	case 0xFF0F:
		return read_if();
	case 0xFF40:
		return ppu_ ? ppu_->read_lcdc() : 0xFF;
	case 0xFF41:
		return ppu_ ? ppu_->read_stat() : 0xFF;
	case 0xFF42:
		return ppu_ ? ppu_->read_scy() : 0xFF;
	case 0xFF43:
		return ppu_ ? ppu_->read_scx() : 0xFF;
	case 0xFF44:
		return ppu_ ? ppu_->read_ly() : 0xFF;
	case 0xFF45:
		return ppu_ ? ppu_->read_lyc() : 0xFF;
	case 0xFF46:
		return ppu_ ? ppu_->read_dma() : 0xFF;
	case 0xFF47:
		return ppu_ ? ppu_->read_bgp() : 0xFF;
	case 0xFF48:
		return ppu_ ? ppu_->read_obp0() : 0xFF;
	case 0xFF49:
		return ppu_ ? ppu_->read_obp1() : 0xFF;
	case 0xFF4A:
		return ppu_ ? ppu_->read_wy() : 0xFF;
	case 0xFF4B:
		return ppu_ ? ppu_->read_wx() : 0xFF;
	case 0xFF50:
		return boot_rom_enabled ? 0x00 : 0xFF;
	default:
		break;
	}
	if (addr >= 0xFF10 && addr <= 0xFF26) {
		return apu_ ? apu_->read_reg(addr) : 0xFF;
	}
	if (addr >= 0xFF30 && addr <= 0xFF3F) {
		return apu_ ? apu_->read_wave_ram(addr) : 0xFF;
	}
	// ---- CGB registers ----
	if (is_cgb()) {
		switch (addr) {
		case 0xFF4D:
			return read_key1();
		case 0xFF4F:
			return ppu_ ? ppu_->read_vbk() : 0xFE;
		case 0xFF51:
			return hdma1_;
		case 0xFF52:
			return hdma2_;
		case 0xFF53:
			return hdma3_;
		case 0xFF54:
			return hdma4_;
		case 0xFF55:
			return hdma5_;
		case 0xFF68:
			return ppu_ ? ppu_->read_bgpi() : 0xFF;
		case 0xFF69:
			return ppu_ ? ppu_->read_bgpd() : 0xFF;
		case 0xFF6A:
			return ppu_ ? ppu_->read_obpi() : 0xFF;
		case 0xFF6B:
			return ppu_ ? ppu_->read_obpd() : 0xFF;
		case 0xFF6C:
			return ppu_ ? ppu_->read_opri() : 0xFE;
		case 0xFF70:
			return 0xF8 | wram_bank_;
		case 0xFF72:
			return ff72_;
		case 0xFF73:
			return ff73_;
		case 0xFF74:
			return ff74_;
		case 0xFF75:
			return ff75_;
		default:
			break;
		}
	} else {
		// DMG: CGB registers read as 0xFF (or open bus). Keep compatible.
		switch (addr) {
		case 0xFF4D:
		case 0xFF4F:
		case 0xFF51:
		case 0xFF52:
		case 0xFF53:
		case 0xFF54:
		case 0xFF55:
		case 0xFF68:
		case 0xFF69:
		case 0xFF6A:
		case 0xFF6B:
		case 0xFF6C:
		case 0xFF70:
		case 0xFF72:
		case 0xFF73:
		case 0xFF74:
		case 0xFF75:
			return 0xFF;
		default:
			break;
		}
	}
	return 0xFF;
}

void MMU::write_io(u16 addr, u8 value)
{
	switch (addr) {
	// ---- DMG registers ----
	case 0xFF00:
		if (joypad_)
			joypad_->write_p1(value);
		return;
	case 0xFF01:
		serial_sb = value;
		return;
	case 0xFF02:
		// Blargg ROMs print via serial: SC=0x81 starts a transfer of SB, which
		// now actually takes 8192 T-cycles (fast mode 128) instead of
		// completing in the same write.
		// https://gbdev.io/pandocs/Serial_Data_Transfer_Link_Cable.html
		serial_count_ = 0;
		// Bits 2-6 are unused and read back as 1 (SameBoy writes
		// `value | ~0x83`); bit 1 selects the CGB fast clock.
		serial_sc = value | 0x7C;
		serial_mask_ = (is_cgb() && (value & 0x02)) ? 0x0004 : 0x0080;
		// An SC write during an externally-clocked transfer shifts a bit.
		if (serial_master_clock_)
			serial_clock_edge();
		return;
	case 0xFF04:
		if (timer_)
			timer_->write_div(value);
		return;
	case 0xFF05:
		if (timer_)
			timer_->write_tima(value);
		return;
	case 0xFF06:
		if (timer_)
			timer_->write_tma(value);
		return;
	case 0xFF07:
		if (timer_)
			timer_->write_tac(value);
		return;
	case 0xFF0F:
		write_if(value);
		return;
	case 0xFF40:
		if (ppu_)
			ppu_->write_lcdc(value);
		return;
	case 0xFF41:
		if (ppu_)
			ppu_->write_stat(value);
		return;
	case 0xFF42:
		if (ppu_)
			ppu_->write_scy(value);
		return;
	case 0xFF43:
		if (ppu_)
			ppu_->write_scx(value);
		return;
	case 0xFF44:
		if (ppu_)
			ppu_->write_ly(value);
		return; // Read-only; ignored.
	case 0xFF45:
		if (ppu_)
			ppu_->write_lyc(value);
		return;
	case 0xFF46:
		if (ppu_)
			ppu_->write_dma(value);
		// The copy itself is instant (it bypasses the PPU's mode blocking, DMA
		// has its own bus), but the CPU really is off the bus for 160
		// M-cycles afterwards - 160 reads at 4 T-cycles each, per Pan Docs
		// "OAM DMA Transfer". Every sprite-using game pays this once a frame,
		// so not charging it made the whole machine run fast.
		if (cpu_)
			cpu_->add_dma_stall(160);
		return;
	case 0xFF47:
		if (ppu_)
			ppu_->write_bgp(value);
		return;
	case 0xFF48:
		if (ppu_)
			ppu_->write_obp0(value);
		return;
	case 0xFF49:
		if (ppu_)
			ppu_->write_obp1(value);
		return;
	case 0xFF4A:
		if (ppu_)
			ppu_->write_wy(value);
		return;
	case 0xFF4B:
		if (ppu_)
			ppu_->write_wx(value);
		return;
	case 0xFF50:
		if (value != 0)
			boot_rom_enabled = false;
		return;
	default:
		break;
	}
	if (addr >= 0xFF10 && addr <= 0xFF26) {
		if (apu_)
			apu_->write_reg(addr, value);
		return;
	}
	if (addr >= 0xFF30 && addr <= 0xFF3F) {
		if (apu_)
			apu_->write_wave_ram(addr, value);
		return;
	}
	// ---- CGB registers ----
	if (is_cgb()) {
		switch (addr) {
		case 0xFF4D:
			write_key1(value);
			return;
		case 0xFF4F:
			if (ppu_)
				ppu_->write_vbk(value);
			return;
		case 0xFF51:
			hdma1_ = value;
			return;
		case 0xFF52:
			hdma2_ = value & 0xF0;
			return; // low nibble ignored
		case 0xFF53:
			hdma3_ = value & 0x1F;
			return; // only 0x80-0x9F valid
		case 0xFF54:
			hdma4_ = value & 0xF0;
			return;
		case 0xFF55:
			hdma5_ = value;
			hdma_start();
			return;
		case 0xFF68:
			if (ppu_)
				ppu_->write_bgpi(value);
			return;
		case 0xFF69:
			if (ppu_)
				ppu_->write_bgpd(value);
			return;
		case 0xFF6A:
			if (ppu_)
				ppu_->write_obpi(value);
			return;
		case 0xFF6B:
			if (ppu_)
				ppu_->write_obpd(value);
			return;
		case 0xFF6C:
			if (ppu_)
				ppu_->write_opri(value);
			return;
		case 0xFF70:
			wram_bank_ = value & 0x07;
			if (wram_bank_ == 0)
				wram_bank_ = 1;
			return;
		case 0xFF72:
			ff72_ = value;
			return;
		case 0xFF73:
			ff73_ = value;
			return;
		case 0xFF74:
			ff74_ = value;
			return;
		case 0xFF75:
			ff75_ = value | 0x8F;
			return; // lower 4 bits read as 1?
		default:
			return;
		}
	} else {
		// DMG: the CGB-only registers - KEY1 (FF4Dh), VBK (FF4Fh), HDMA1-5
		// (FF51h-FF55h), the palette block (FF68h-FF6Bh), OPRI (FF6Ch), SVBK
		// (FF70h) and the undocumented FF72h-FF75h - read as open bus and
		// ignore writes. Every address took that path, so the old switch (with
		// a `default` that repeated each case) is now this comment.
		(void) addr;
		return;
	}
}

void MMU::save(StateBuffer &buf) const
{
	buf.writeArray2(wram_banks_);
	buf.writeArray(hram_);
	buf.write(wram_bank_);
	buf.write(static_cast<u8>(hardware_mode_));
	buf.write(key1_);
	buf.write(hdma1_);
	buf.write(hdma2_);
	buf.write(hdma3_);
	buf.write(hdma4_);
	buf.write(hdma5_);
	buf.write(hdma_active_);
	buf.write(hdma_src_);
	buf.write(hdma_dst_);
	buf.write(hdma_len_);
	buf.write(hdma_mode_hblank_);
	buf.write(ff72_);
	buf.write(ff73_);
	buf.write(ff74_);
	buf.write(ff75_);
	buf.write(ie_register_);
	buf.write(if_register_);
	buf.write(serial_sb);
	buf.write(serial_sc);
	buf.write(serial_count_);
	buf.write(serial_master_clock_);
	buf.write(serial_mask_);
	buf.write(boot_rom_enabled);
}

void MMU::load(StateBuffer &buf)
{
	buf.readArray2(wram_banks_);
	buf.readArray(hram_);
	buf.read(wram_bank_);
	u8 mode = 0;
	buf.read(mode);
	hardware_mode_ = static_cast<HardwareMode>(mode);
	buf.read(key1_);
	buf.read(hdma1_);
	buf.read(hdma2_);
	buf.read(hdma3_);
	buf.read(hdma4_);
	buf.read(hdma5_);
	buf.read(hdma_active_);
	buf.read(hdma_src_);
	buf.read(hdma_dst_);
	buf.read(hdma_len_);
	buf.read(hdma_mode_hblank_);
	buf.read(ff72_);
	buf.read(ff73_);
	buf.read(ff74_);
	buf.read(ff75_);
	buf.read(ie_register_);
	buf.read(if_register_);
	buf.read(serial_sb);
	buf.read(serial_sc);
	buf.read(serial_count_);
	buf.read(serial_master_clock_);
	buf.read(serial_mask_);
	buf.read(boot_rom_enabled);
	// The boot ROM image itself is immutable once loaded (load_boot_rom),
	// and the PPU/APU/cartridge keep their own memory, so nothing else has
	// to be re-pointed here - the component pointers are set once in the
	// GameBoy constructor and are unaffected by a state load.
}

} // namespace gb
