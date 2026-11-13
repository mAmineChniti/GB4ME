// Cartridge: header parsing, CGB-mode detection, and the polymorphic MBC
// implementations (NoMBC + MBC1/2/3/5).
// https://gbdev.io/pandocs/The_Cartridge_Header.html
// https://gbdev.io/pandocs/MBCs.html

#include "gb/cartridge.h"
#include <cstring>
#include <ctime>
#include <fstream>

namespace gb
{

// ---------------- NoMBC ----------------

NoMBC::NoMBC(std::vector<u8> rom, std::vector<u8> ram, bool battery)
	: rom_(std::move(rom)), ram_(std::move(ram)), battery_(battery)
{
}

u8 NoMBC::read_rom(u16 addr) const
{
	return addr < rom_.size() ? rom_[addr] : 0xFF;
}

void NoMBC::write_rom(u16 /*addr*/, u8 /*value*/) {}

u8 NoMBC::read_ram(u16 addr) const
{
	return addr < ram_.size() ? ram_[addr] : 0xFF;
}

void NoMBC::write_ram(u16 addr, u8 value)
{
	if (addr < ram_.size())
		ram_[addr] = value;
}

u8 NoMBC::raw_read(u32 bank, u16 addr) const
{
	const u32 off = bank * 0x2000u + addr;
	return off < ram_.size() ? ram_[off] : 0xFF;
}

void NoMBC::raw_write(u32 bank, u16 addr, u8 value)
{
	const u32 off = bank * 0x2000u + addr;
	if (off < ram_.size())
		ram_[off] = value;
}

u32 NoMBC::raw_bank_count() const
{
	return static_cast<u32>(ram_.size() / 0x2000u);
}

// ---------------- MBC1 ----------------
// https://gbdev.io/pandocs/MBC1.html

MBC1::MBC1(std::vector<u8> rom, std::vector<u8> ram, bool battery)
	: rom_(std::move(rom)), ram_(std::move(ram)), battery_(battery)
{
}

u32 MBC1::rom_bank_count() const
{
	u32 n = static_cast<u32>(rom_.size() / 0x4000);
	return n == 0 ? 1 : n;
}

u32 MBC1::ram_bank_count() const
{
	return static_cast<u32>(ram_.size() / 0x2000);
}

u32 MBC1::current_rom_bank() const
{
	// SameBoy GB_update_mbc_mappings: the 5-bit register is stored raw and
	// the 00->01 translation compares it whole. Standard wiring feeds bits
	// 5-6 from the secondary register; MBC1M multicarts ignore bit 4 and
	// feed bits 4-5 instead (mode-1 then pages $00/$10/$20/$30).
	u32 bank = 0;
	if (multicart_)
		bank = (rom_bank_low5_ & 0x0F) | (static_cast<u32>(rom_bank_high2_) << 4);
	else
		bank = rom_bank_low5_ | (static_cast<u32>(rom_bank_high2_) << 5);
	if ((rom_bank_low5_ & 0x1F) == 0)
		bank++;
	const u32 count = rom_bank_count();
	if (count > 0)
		bank %= count;
	return bank;
}

u32 MBC1::current_ram_bank() const
{
	if (!banking_mode_)
		return 0;
	// Multicarts have no banked RAM view (SameBoy: mbc_ram_bank = 0).
	if (multicart_)
		return 0;
	const u32 count = ram_bank_count();
	if (count == 0)
		return 0;
	return ram_bank_ % count;
}

u8 MBC1::read_rom(u16 addr) const
{
	const u32 count = rom_bank_count();
	if (addr < 0x4000) {
		u32 bank = 0;
		if (banking_mode_) {
			// Mode 1 pages the low 16 KiB too: $20/$40/$60 standard,
			// $10/$20/$30 on MBC1M multicarts (SameBoy mbc_rom0_bank).
			bank = multicart_ ? (static_cast<u32>(ram_bank_) << 4)
							  : (static_cast<u32>(ram_bank_) << 5);
			if (count > 0)
				bank %= count;
		}
		u32 off = bank * 0x4000u + addr;
		return off < rom_.size() ? rom_[off] : 0xFF;
	}
	u32 off = current_rom_bank() * 0x4000u + (addr - 0x4000);
	return off < rom_.size() ? rom_[off] : 0xFF;
}

void MBC1::write_rom(u16 addr, u8 value)
{
	if (addr < 0x2000) {
		ram_enabled_ = ((value & 0x0F) == 0x0A);
	} else if (addr < 0x4000) {
		// Stored raw: the 00->01 translation compares the whole register
		// at read time (current_rom_bank), SameBoy-style.
		rom_bank_low5_ = value & 0x1F;
	} else if (addr < 0x6000) {
		u8 v = value & 0x03;
		rom_bank_high2_ = v;
		ram_bank_ = v;
	} else {
		banking_mode_ = (value & 0x01) != 0;
	}
}

u8 MBC1::read_ram(u16 addr) const
{
	if (!ram_enabled_ || ram_.empty() || addr >= 0x2000)
		return 0xFF;
	u32 off = current_ram_bank() * 0x2000u + addr;
	return off < ram_.size() ? ram_[off] : 0xFF;
}

void MBC1::write_ram(u16 addr, u8 value)
{
	if (!ram_enabled_ || ram_.empty() || addr >= 0x2000)
		return;
	u32 off = current_ram_bank() * 0x2000u + addr;
	if (off < ram_.size())
		ram_[off] = value;
}

u8 MBC1::raw_read(u32 bank, u16 addr) const
{
	const u32 off = bank * 0x2000u + addr;
	return off < ram_.size() ? ram_[off] : 0xFF;
}

void MBC1::raw_write(u32 bank, u16 addr, u8 value)
{
	const u32 off = bank * 0x2000u + addr;
	if (off < ram_.size())
		ram_[off] = value;
}

u32 MBC1::raw_bank_count() const
{
	return static_cast<u32>(ram_.size() / 0x2000u);
}

// ---------------- MBC2 ----------------
// https://gbdev.io/pandocs/MBC2.html
// 4-bit RAM mirrored every 512 bytes; only the low nibble is used.

MBC2::MBC2(std::vector<u8> rom, bool battery) : rom_(std::move(rom)), battery_(battery)
{
	ram_.fill(0);
}

u8 MBC2::read_rom(u16 addr) const
{
	const u32 banks = static_cast<u32>(rom_.size() / 0x4000);
	if (addr < 0x4000)
		return addr < rom_.size() ? rom_[addr] : 0xFF;
	u32 b = rom_bank_ % (banks == 0 ? 1 : banks);
	if (b == 0)
		b = 1;
	u32 off = b * 0x4000u + (addr - 0x4000);
	return off < rom_.size() ? rom_[off] : 0xFF;
}

void MBC2::write_rom(u16 addr, u8 value)
{
	if (addr < 0x4000) {
		// Bit 8 of the address selects RAM enable vs ROM bank.
		if ((addr & 0x100) == 0) {
			ram_enabled_ = ((value & 0x0F) == 0x0A);
		} else {
			rom_bank_ = value & 0x0F;
			if (rom_bank_ == 0)
				rom_bank_ = 1;
		}
	}
}

u8 MBC2::read_ram(u16 addr) const
{
	if (!ram_enabled_ || addr >= 0x2000)
		return 0xFF;
	return 0xF0 | (ram_[addr & 0x1FF] & 0x0F);
}

void MBC2::write_ram(u16 addr, u8 value)
{
	if (!ram_enabled_ || addr >= 0x2000)
		return;
	ram_[addr & 0x1FF] = value & 0x0F;
}

u8 MBC2::raw_read(u32 bank, u16 addr) const
{
	// MBC2 RAM is 512 x 4-bit cells stored in the low nibble. The 512-byte
	// .sav format keeps one byte per cell, upper nibble unused.
	(void) bank;
	return addr < 0x200 ? ram_[addr] : 0x0F;
}

void MBC2::raw_write(u32 bank, u16 addr, u8 value)
{
	(void) bank;
	if (addr < 0x200)
		ram_[addr] = value & 0x0F;
}

u32 MBC2::raw_bank_count() const
{
	return 1;
}

// ---------------- MBC3 ----------------
// https://gbdev.io/pandocs/MBC3.html

MBC3::MBC3(std::vector<u8> rom, std::vector<u8> ram, bool battery, bool rtc)
	: rom_(std::move(rom)), ram_(std::move(ram)), battery_(battery), rtc_(rtc)
{
}

u8 MBC3::read_rom(u16 addr) const
{
	const u32 banks = static_cast<u32>(rom_.size() / 0x4000);
	if (addr < 0x4000)
		return addr < rom_.size() ? rom_[addr] : 0xFF;
	// Standard MBC3 wires 7 ROM bank bits; MBC30 wires all 8 (SameBoy).
	u32 b = is_mbc30_ ? rom_bank_ : (rom_bank_ & 0x7F);
	b %= (banks == 0 ? 1 : banks);
	if (b == 0)
		b = 1;
	u32 off = b * 0x4000u + (addr - 0x4000);
	return off < rom_.size() ? rom_[off] : 0xFF;
}

void MBC3::write_rom(u16 addr, u8 value)
{
	if (addr < 0x2000) {
		ram_enabled_ = ((value & 0x0F) == 0x0A);
	} else if (addr < 0x4000) {
		// Stored raw (all 8 bits); the 7-bit mask applies at read time
		// unless this is an MBC30 (see read_rom).
		rom_bank_ = value;
		if (rom_bank_ == 0)
			rom_bank_ = 1;
	} else if (addr < 0x6000) {
		ram_bank_ = value;
	} else {
		// Latch clock data: a 0->1 write latches the live time.
		// https://gbdev.io/pandocs/MBC3.html#the-clock-counter-registers
		if (latch_state_ == 0 && value == 1)
			rtc_latch();
		latch_state_ = value;
	}
}

void MBC3::rtc_ensure_base()
{
	if (rtc_base_valid_)
		return;
	rtc_base_valid_ = true;
	rtc_base_host_ = static_cast<std::int64_t>(std::time(nullptr));
	rtc_base_secs_ = 0;
	rtc_halt_ = false;
}

std::int64_t MBC3::rtc_total_seconds() const
{
	if (!rtc_base_valid_)
		return 0;
	if (rtc_halt_)
		return rtc_base_secs_;
	const std::int64_t now = static_cast<std::int64_t>(std::time(nullptr));
	return rtc_base_secs_ + (now - rtc_base_host_);
}

void MBC3::rtc_latch()
{
	if (!rtc_)
		return;
	rtc_ensure_base();
	std::int64_t total = rtc_total_seconds();
	if (total < 0)
		total = 0;
	const std::int64_t days = total / 86400;
	if (days >= 512)
		rtc_carry_ = true; // Sticky until the time is (re)set.
	const u16 day = static_cast<u16>(days % 512);
	rtc_latched_[0] = static_cast<u8>((total % 60));
	rtc_latched_[1] = static_cast<u8>((total / 60) % 60);
	rtc_latched_[2] = static_cast<u8>((total / 3600) % 24);
	rtc_latched_[3] = static_cast<u8>(day & 0xFF);
	rtc_latched_[4] = static_cast<u8>(((day >> 8) & 0x01) | (rtc_halt_ ? 0x40 : 0x00) |
									  (rtc_carry_ ? 0x80 : 0x00));
}

void MBC3::rtc_apply_write(u8 index, u8 value)
{
	if (!rtc_ || index > 4)
		return;
	rtc_ensure_base();
	rtc_set_[index] = value;
	// Rebase the clock on the newly written time. Masks match hardware
	// ranges; DH carries halt (bit 6) and the sticky overflow (bit 7),
	// which is cleared by any time-set that leaves it 0.
	const u8 s = static_cast<u8>(rtc_set_[0] % 60);
	const u8 m = static_cast<u8>(rtc_set_[1] % 60);
	const u8 h = static_cast<u8>(rtc_set_[2] % 24);
	const u16 day = static_cast<u16>((((rtc_set_[4] & 0x01) << 8) | rtc_set_[3]) & 0x1FF);
	rtc_base_secs_ = ((static_cast<std::int64_t>(day) * 24 + h) * 60 + m) * 60 + s;
	rtc_base_host_ = static_cast<std::int64_t>(std::time(nullptr));
	rtc_halt_ = (rtc_set_[4] & 0x40) != 0;
	rtc_carry_ = (rtc_set_[4] & 0x80) != 0;
}

bool MBC3::save_rtc(const std::string &path) const
{
	if (!rtc_)
		return false;
	std::ofstream file(path, std::ios::binary);
	if (!file)
		return false;
	// Text sidecar: versioned, forward-compatible, debuggable.
	file << "GB4ME-RTC-1 " << rtc_base_secs_ << ' ' << rtc_base_host_ << ' ' << (rtc_halt_ ? 1 : 0)
		 << ' ' << (rtc_carry_ ? 1 : 0) << ' ' << (rtc_base_valid_ ? 1 : 0);
	for (u8 v : rtc_set_)
		file << ' ' << static_cast<unsigned>(v);
	file << '\n';
	return static_cast<bool>(file);
}

bool MBC3::load_rtc(const std::string &path)
{
	if (!rtc_)
		return false;
	std::ifstream file(path);
	if (!file)
		return false;
	std::string magic;
	int halt = 0, carry = 0, valid = 0;
	unsigned regs[5] = {};
	file >> magic >> rtc_base_secs_ >> rtc_base_host_ >> halt >> carry >> valid >> regs[0] >>
		regs[1] >> regs[2] >> regs[3] >> regs[4];
	if (!file || magic != "GB4ME-RTC-1")
		return false;
	rtc_halt_ = halt != 0;
	rtc_carry_ = carry != 0;
	rtc_base_valid_ = valid != 0;
	for (int i = 0; i < 5; i++)
		rtc_set_[i] = static_cast<u8>(regs[i] & 0xFF);
	rtc_latched_.fill(0);
	latch_state_ = 0;
	return true;
}

u8 MBC3::read_ram(u16 addr) const
{
	if (addr >= 0x2000)
		return 0xFF;
	// Standard MBC3 wires 2 RAM bank bits (0-3); MBC30 wires 3 (0-7, mGBA).
	// Anything else is RTC (8-C, when present) or open bus.
	if (ram_bank_ <= 0x03 || (is_mbc30_ && ram_bank_ <= 0x07)) {
		if (!ram_enabled_ || ram_.empty())
			return 0xFF;
		const u32 banks = static_cast<u32>(ram_.size() / 0x2000);
		u32 b = ram_bank_ % (banks == 0 ? 1 : banks);
		u32 off = b * 0x2000u + addr;
		return off < ram_.size() ? ram_[off] : 0xFF;
	}
	if (rtc_ && ram_bank_ >= 0x08 && ram_bank_ <= 0x0C) {
		if (!ram_enabled_)
			return 0xFF;
		return rtc_latched_[ram_bank_ - 0x08];
	}
	return 0xFF;
}

void MBC3::write_ram(u16 addr, u8 value)
{
	if (addr >= 0x2000)
		return;
	if (ram_bank_ <= 0x03 || (is_mbc30_ && ram_bank_ <= 0x07)) {
		if (!ram_enabled_ || ram_.empty())
			return;
		const u32 banks = static_cast<u32>(ram_.size() / 0x2000);
		u32 b = ram_bank_ % (banks == 0 ? 1 : banks);
		u32 off = b * 0x2000u + addr;
		if (off < ram_.size())
			ram_[off] = value;
		return;
	}
	if (rtc_ && ram_bank_ >= 0x08 && ram_bank_ <= 0x0C) {
		if (!ram_enabled_)
			return;
		rtc_apply_write(static_cast<u8>(ram_bank_ - 0x08), value);
	}
}

u8 MBC3::raw_read(u32 bank, u16 addr) const
{
	const u32 off = bank * 0x2000u + addr;
	return off < ram_.size() ? ram_[off] : 0xFF;
}

void MBC3::raw_write(u32 bank, u16 addr, u8 value)
{
	const u32 off = bank * 0x2000u + addr;
	if (off < ram_.size())
		ram_[off] = value;
}

u32 MBC3::raw_bank_count() const
{
	return static_cast<u32>(ram_.size() / 0x2000u);
}

// ---------------- MBC5 ----------------
// https://gbdev.io/pandocs/MBC5.html

MBC5::MBC5(std::vector<u8> rom, std::vector<u8> ram, bool battery)
	: rom_(std::move(rom)), ram_(std::move(ram)), battery_(battery)
{
}

u8 MBC5::read_rom(u16 addr) const
{
	const u32 banks = static_cast<u32>(rom_.size() / 0x4000);
	if (addr < 0x4000)
		return addr < rom_.size() ? rom_[addr] : 0xFF;
	u32 b = rom_bank_ % (banks == 0 ? 1 : banks);
	u32 off = b * 0x4000u + (addr - 0x4000);
	return off < rom_.size() ? rom_[off] : 0xFF;
}

void MBC5::write_rom(u16 addr, u8 value)
{
	if (addr < 0x2000) {
		ram_enabled_ = ((value & 0x0F) == 0x0A);
	} else if (addr < 0x3000) {
		rom_bank_ = (rom_bank_ & 0x100) | value;
	} else if (addr < 0x4000) {
		rom_bank_ = (rom_bank_ & 0xFF) | ((value & 0x01) << 8);
	} else if (addr < 0x6000) {
		// Bit 3 is the rumble motor on rumble carts, NOT a RAM bank bit
		// (Pan Docs "MBC5"; mGBA strips it at src/gb/mbc.c:246-254). If it
		// leaked into ram_bank_, a rumble cart addressing bank 0 would read
		// bank 8.
		ram_bank_ = value & 0x07;
		// Bits 4-7 select the rumble motor (kept as on/off; which of the
		// two motors a cart has is not observable from the bus).
		rumble_ = (value & 0x08) != 0;
	}
}

u8 MBC5::read_ram(u16 addr) const
{
	if (!ram_enabled_ || ram_.empty() || addr >= 0x2000)
		return 0xFF;
	const u32 banks = static_cast<u32>(ram_.size() / 0x2000);
	if (banks == 0)
		return 0xFF;
	u32 b = ram_bank_ % banks;
	u32 off = b * 0x2000u + addr;
	return off < ram_.size() ? ram_[off] : 0xFF;
}

void MBC5::write_ram(u16 addr, u8 value)
{
	if (!ram_enabled_ || ram_.empty() || addr >= 0x2000)
		return;
	const u32 banks = static_cast<u32>(ram_.size() / 0x2000);
	if (banks == 0)
		return;
	u32 b = ram_bank_ % banks;
	u32 off = b * 0x2000u + addr;
	if (off < ram_.size())
		ram_[off] = value;
}

u8 MBC5::raw_read(u32 bank, u16 addr) const
{
	const u32 off = bank * 0x2000u + addr;
	return off < ram_.size() ? ram_[off] : 0xFF;
}

void MBC5::raw_write(u32 bank, u16 addr, u8 value)
{
	const u32 off = bank * 0x2000u + addr;
	if (off < ram_.size())
		ram_[off] = value;
}

u32 MBC5::raw_bank_count() const
{
	return static_cast<u32>(ram_.size() / 0x2000u);
}

// ---------------- HuC1 ----------------
// https://gbdev.io/pandocs/HuC1.html

MBC7::MBC7(std::vector<u8> rom, std::vector<u8> ram) : rom_(std::move(rom)), ram_(std::move(ram))
{
	// The chip is a 93LC56: 2 Kbit as 128 16-bit words. SameBoy forces the
	// window to 256 bytes regardless of what the header asks for (Core/mbc.c),
	// and so do we.
	ram_.resize(0x100, 0xFF);
}

u8 MBC7::read_rom(u16 addr) const
{
	const u32 banks = static_cast<u32>(rom_.size() / 0x4000);
	if (addr < 0x4000)
		return addr < rom_.size() ? rom_[addr] : 0xFF;
	// 7-bit bank, no 00->01 translation.
	const u32 b = (rom_bank_ & 0x7F) % (banks == 0 ? 1 : banks);
	const u32 off = b * 0x4000u + (addr - 0x4000);
	return off < rom_.size() ? rom_[off] : 0xFF;
}

void MBC7::write_rom(u16 addr, u8 value)
{
	// https://gbdev.io/pandocs/MBC7.html
	if (addr < 0x2000)
		ram_enable_ = value == 0x0A;
	else if (addr < 0x4000)
		rom_bank_ = value;
	else if (addr < 0x6000)
		secondary_enable_ = value == 0x40;
	// 6000-7FFF ignored.
}

u8 MBC7::read_ram(u16 addr) const
{
	if (!ram_enable_ || !secondary_enable_)
		return 0xFF;
	if (addr >= 0x100) // the window is $A000-$A0FF
		return 0xFF;
	switch ((addr >> 4) & 0x0F) {
	case 2:
		return static_cast<u8>(x_latch_ & 0xFF);
	case 3:
		return static_cast<u8>(x_latch_ >> 8);
	case 4:
		return static_cast<u8>(y_latch_ & 0xFF);
	case 5:
		return static_cast<u8>(y_latch_ >> 8);
	case 6:
		return 0x00;
	case 8:
		return static_cast<u8>((eeprom_do_ ? 0x01 : 0) | (eeprom_di_ ? 0x02 : 0) |
							   (eeprom_clk_ ? 0x40 : 0) | (eeprom_cs_ ? 0x80 : 0));
	default:
		return 0xFF;
	}
}

void MBC7::write_ram(u16 addr, u8 value)
{
	if (!ram_enable_ || !secondary_enable_)
		return;
	if (addr >= 0x100)
		return;
	switch ((addr >> 4) & 0x0F) {
	case 0: // $A000: $55 arms the tilt latch and parks both axes at $8000
		if (value == 0x55) {
			latch_ready_ = true;
			x_latch_ = 0x8000;
			y_latch_ = 0x8000;
		}
		break;
	case 1: // $A010: $AA samples the accelerometer into the latches
		if (value == 0xAA) {
			latch_ready_ = false;
			x_latch_ = static_cast<u16>(0x81D0 + 0x70 * accel_x_);
			y_latch_ = static_cast<u16>(0x81D0 + 0x70 * accel_y_);
		}
		break;
	case 8: // $A080: bit-banged EEPROM (CS = bit 7, DI = bit 1, CLK = bit 6)
		eeprom_cs_ = (value & 0x80) != 0;
		eeprom_di_ = (value & 0x02) != 0;
		if (eeprom_cs_ && !eeprom_clk_ && (value & 0x40) != 0)
			eeprom_clock(eeprom_di_);
		eeprom_clk_ = (value & 0x40) != 0;
		break;
	default:
		break;
	}
}

void MBC7::eeprom_clock(bool di)
{
	// SameBoy Core/memory.c write_mbc7_ram, case 8. The command word is
	// 11 bits: bit 8 marks "still shifting command" vs "shifting 16 data
	// bits", and bits 6-9 hold the opcode.
	if (argument_bits_left_ == 0) {
		eeprom_do_ = (read_bits_ & 0x8000) != 0;
		read_bits_ = static_cast<u16>((read_bits_ << 1) | 1);
		command_ = static_cast<u16>((command_ << 1) | (di ? 1 : 0));
		if ((command_ & 0x0400) == 0)
			return;
		const u16 index = static_cast<u16>(command_ & 0x7F);
		switch ((command_ >> 6) & 0x0F) {
		case 0x8:
		case 0x9:
		case 0xA:
		case 0xB: // READ
			read_bits_ = static_cast<u16>(ram_[index * 2] | (ram_[index * 2 + 1] << 8));
			command_ = 0;
			break;
		case 0x3: // EWEN
			eeprom_write_enabled_ = true;
			command_ = 0;
			break;
		case 0x0: // EWDS
			eeprom_write_enabled_ = false;
			command_ = 0;
			break;
		case 0x4:
		case 0x5:
		case 0x6:
		case 0x7: // WRITE: zero the word now, then OR in the 16 shifted bits
			if (eeprom_write_enabled_) {
				ram_[index * 2] = 0;
				ram_[index * 2 + 1] = 0;
			}
			argument_bits_left_ = 16;
			break;
		case 0xC:
		case 0xD:
		case 0xE:
		case 0xF: // ERASE
			if (eeprom_write_enabled_) {
				ram_[index * 2] = 0xFF;
				ram_[index * 2 + 1] = 0xFF;
				read_bits_ = 0x3FFF;
			}
			command_ = 0;
			break;
		case 0x2: // ERAL
			if (eeprom_write_enabled_) {
				for (std::size_t i = 0; i < ram_.size(); ++i)
					ram_[i] = 0xFF;
				ram_[index * 2] = 0xFF;
				ram_[index * 2 + 1] = 0xFF;
				read_bits_ = 0x00FF;
			}
			command_ = 0;
			break;
		case 0x1: // WRAL
			if (eeprom_write_enabled_) {
				for (std::size_t i = 0; i < ram_.size(); ++i)
					ram_[i] = 0x00;
			}
			argument_bits_left_ = 16;
			break;
		default:
			command_ = 0;
			break;
		}
		return;
	}
	// Shifting one of the 16 data bits of a WRITE/WRAL.
	--argument_bits_left_;
	eeprom_do_ = true;
	if (di) {
		const u16 bit = static_cast<u16>(1u << argument_bits_left_);
		if (command_ & 0x100) { // WRITE
			ram_[static_cast<u16>(command_ & 0x7F) * 2] |= static_cast<u8>(bit & 0xFF);
			ram_[static_cast<u16>(command_ & 0x7F) * 2 + 1] |= static_cast<u8>(bit >> 8);
		} else { // WRAL
			for (std::size_t i = 0; i < 0x7F; ++i) {
				ram_[i * 2] |= static_cast<u8>(bit & 0xFF);
				ram_[i * 2 + 1] |= static_cast<u8>(bit >> 8);
			}
		}
	}
	if (argument_bits_left_ == 0) {
		command_ = 0;
		read_bits_ = (command_ & 0x100) ? 0x00FF : 0x3FFF;
	}
}

// ---------------- MBC6 ----------------
//
// Pan Docs "MBC6": the odd mapper with two *independent* 8 KiB ROM/flash bank
// registers ($4000-$5FFF and $6000-$7FFF), two independent 4 KiB SRAM bank
// registers ($A000-$AFFF and $B000-$BFFF), four memory-select registers at
// $0000-$0FFF and a Macronix MX29F008TC 1 MiB flash chip mapped into the same
// windows. The flash is a normal command-sequenced part: $AA to $5555 and $55
// to $2AAA (which the 16 KiB ROM mirrors at $4AAA/$6AAA) unlock the command
// port, and the command byte goes to $5555 or, for a sector erase, to an
// address inside the target sector.
namespace
{
// Flash geometry: 1 MiB as 128 banks of 8 KiB, eight 128 KiB sectors.
constexpr u32 kMbc6FlashBank = 0x2000;
constexpr u32 kMbc6FlashBanks = 128;
constexpr u32 kMbc6SectorSize = 0x20000;
constexpr u32 kMbc6RamBanks = 4; // 32 KiB of SRAM, in 8 KiB save banks
} // namespace

MBC6::MBC6(std::vector<u8> rom, std::vector<u8> ram) : rom_(std::move(rom)), ram_(std::move(ram))
{
	flash_.assign(kMbc6FlashBanks * kMbc6FlashBank, 0xFF);
	hidden_.assign(0x100, 0xFF);
	// Pan Docs' MBC6 has 8 SRAM banks of 4 KiB. The header can under-report
	// (or the ROM can be a bad dump), so the array is sized from the spec.
	ram_.resize(0x8000);
}

u8 MBC6::read_rom(u16 addr) const
{
	if (addr < 0x4000)
		return addr < rom_.size() ? rom_[addr] : 0xFF;
	const bool bank_b = addr >= 0x6000;
	const u8 num = bank_b ? bank_b_num_ : bank_a_num_;
	const bool use_flash = bank_b ? bank_b_flash_ : bank_a_flash_;
	const u16 off = static_cast<u16>(addr & 0x1FFF);
	if (use_flash)
		return flash_read(static_cast<u32>(num) * kMbc6FlashBank + off);
	const u32 rom_off = static_cast<u32>(num) * kMbc6FlashBank + off;
	return rom_off < rom_.size() ? rom_[rom_off] : 0xFF;
}

void MBC6::write_rom(u16 addr, u8 value)
{
	// $0000-$3FFF: four registers, mirrored every $400 (Pan Docs).
	if (addr < 0x4000) {
		write_flash(addr, value);
		return;
	}
	// Writes into $4000-$7FFF reach the flash only when that window has the
	// flash mapped *and* flash access is enabled; ROM ignores them.
	const bool bank_b = addr >= 0x6000;
	const bool use_flash = bank_b ? bank_b_flash_ : bank_a_flash_;
	if (!use_flash || !flash_enable_)
		return;
	const u8 num = bank_b ? bank_b_num_ : bank_a_num_;
	const u16 off = static_cast<u16>(addr & 0x1FFF);
	flash_write(static_cast<u32>(num) * kMbc6FlashBank + off, value);
}

void MBC6::write_flash(u16 addr, u8 value)
{
	if (addr < 0x0400) {
		ram_enable_ = value == 0x0A; // Same shape as MBC1.
	} else if (addr < 0x0800) {
		ram_bank_a_ = value & 0x07;
	} else if (addr < 0x0C00) {
		ram_bank_b_ = value & 0x07;
	} else if (addr < 0x1000) {
		flash_enable_ = (value & 0x01) != 0; // Drives the flash /CE.
	} else if (addr < 0x2000) {
		flash_wp_ = (value & 0x01) == 0; // 1 = /WP asserted = write-protected
	} else if (addr < 0x2800) {
		bank_a_num_ = value & 0x7F;
	} else if (addr < 0x3000) {
		bank_a_flash_ = (value & 0x08) != 0; // $00 ROM, $08 flash.
	} else if (addr < 0x3800) {
		bank_b_num_ = value & 0x7F;
	} else {
		bank_b_flash_ = (value & 0x08) != 0;
	}
}

u8 MBC6::read_ram(u16 addr) const
{
	if (!ram_enable_)
		return 0xFF;
	// $A000-$AFFF is bank A, $B000-$BFFF is bank B, both 4 KiB windows into
	// the same 32 KiB array.
	const u32 off = addr < 0x1000 ? static_cast<u32>(ram_bank_a_) * 0x1000 + (addr & 0x0FFF)
								  : static_cast<u32>(ram_bank_b_) * 0x1000 + (addr & 0x0FFF);
	return off < ram_.size() ? ram_[off] : 0xFF;
}

void MBC6::write_ram(u16 addr, u8 value)
{
	if (!ram_enable_)
		return;
	const u32 off = addr < 0x1000 ? static_cast<u32>(ram_bank_a_) * 0x1000 + (addr & 0x0FFF)
								  : static_cast<u32>(ram_bank_b_) * 0x1000 + (addr & 0x0FFF);
	if (off < ram_.size())
		ram_[off] = value;
}

// ---- flash ----

u8 MBC6::flash_read(u32 off) const
{
	switch (flash_state_) {
	case FlashState::Status: {
		// Erase and program complete instantly here, so "done" is always set.
		// Bit 4 is the timeout flag (never set), bit 1 mirrors the persistent
		// sector-0 protection.
		u8 status = 0x80;
		if (sector0_protected_)
			status |= 0x02;
		return status;
	}
	case FlashState::Program:
	case FlashState::ProgramHidden:
	case FlashState::Read:
		if (off < flash_.size())
			return flash_[off];
		return 0xFF;
	case FlashState::Id:
		return (off & 0xFF) == 0 ? 0xC2 : ((off & 0xFF) == 1 ? 0x81 : 0xFF);
	case FlashState::HiddenRead:
		return (off & 0xFF) < hidden_.size() ? hidden_[off & 0xFF] : 0xFF;
	}
	return 0xFF;
}

void MBC6::flash_write(u32 off, u8 value)
{
	if (flash_state_ == FlashState::Program || flash_state_ == FlashState::ProgramHidden) {
		// $F0 leaves program mode from any address, which is also why it can
		// never be the byte a program writes (Pan Docs).
		if (value == 0xF0) {
			flash_state_ = FlashState::Read;
			hidden_read_ = false;
			return;
		}
		// Real flash can only clear bits, so programming without an erase
		// first ANDs the new byte into what is there (Pan Docs' warning).
		if (flash_state_ == FlashState::Program) {
			if (off < flash_.size())
				flash_[off] &= value;
		} else if ((off & 0xFF) < hidden_.size()) {
			hidden_[off & 0xFF] &= value;
		}
		return;
	}
	if (flash_state_ == FlashState::Status) {
		// Any further command byte is ignored while a command is in flight.
		if (value == 0xF0) {
			flash_state_ = FlashState::Read;
			hidden_read_ = false;
		}
		return;
	}
	if (is_cmd_5555(off)) {
		if (value == 0xAA) {
			cmd_unlock1_ = true;
		} else if (value == 0xF0) {
			// Exit any command / back to read mode.
			flash_state_ = FlashState::Read;
			hidden_read_ = false;
		} else if (cmd_unlock1_ && cmd_unlock2_) {
			flash_command(value, off);
			cmd_unlock1_ = false;
			cmd_unlock2_ = false;
		} else {
			cmd_unlock1_ = false;
		}
		return;
	}
	if (is_cmd_2aaa(off) && cmd_unlock1_ && value == 0x55) {
		cmd_unlock2_ = true;
		return;
	}
	cmd_unlock1_ = false;
	cmd_unlock2_ = false;
}

void MBC6::flash_command(u8 cmd, u32 off)
{
	switch (cmd) {
	case 0x30: // Erase sector: the command byte's own address picks the sector.
		flash_erase_sector(off / kMbc6SectorSize);
		flash_state_ = FlashState::Status;
		break;
	case 0x10: // Erase chip (the hidden region survives).
		flash_erase_chip();
		flash_state_ = FlashState::Status;
		break;
	case 0x04: // Erase the hidden 256 bytes.
		if (flash_writable_sector0())
			std::fill(hidden_.begin(), hidden_.end(), 0xFF);
		flash_state_ = FlashState::Status;
		break;
	case 0x90:
		flash_state_ = FlashState::Id;
		break;
	case 0x77:
		flash_state_ = FlashState::HiddenRead;
		hidden_read_ = true;
		break;
	case 0xA0:
		flash_state_ = FlashState::Program;
		break;
	case 0xE0: // Program the hidden region.
		if (flash_writable_sector0())
			flash_state_ = FlashState::ProgramHidden;
		else
			flash_state_ = FlashState::Status;
		break;
	case 0x40: // Unprotect sector 0.
		if (flash_writable_sector0())
			sector0_protected_ = false;
		flash_state_ = FlashState::Status;
		break;
	case 0x20: // Protect sector 0.
		sector0_protected_ = true;
		flash_state_ = FlashState::Status;
		break;
	case 0xF0:
		flash_state_ = FlashState::Read;
		hidden_read_ = false;
		break;
	default:
		break;
	}
}

void MBC6::flash_erase_sector(u32 sector)
{
	// Sector 0 is behind both layers of protection; the write-protect bit
	// covers it and the hidden region, the command covers sector 0 only.
	if (sector == 0 && (sector0_protected_ || !flash_writable_sector0()))
		return;
	const u32 start = sector * kMbc6SectorSize;
	if (start >= flash_.size())
		return;
	std::fill(flash_.begin() + static_cast<std::ptrdiff_t>(start),
			  flash_.begin() + (static_cast<std::ptrdiff_t>(start) +
								static_cast<std::ptrdiff_t>(kMbc6SectorSize)),
			  0xFF);
}

void MBC6::flash_erase_chip()
{
	// Protected sector 0 (either mechanism) survives a chip erase.
	const bool keep0 = sector0_protected_ || !flash_writable_sector0();
	std::fill(flash_.begin(), flash_.end(), 0xFF);
	if (keep0)
		std::fill(flash_.begin(), flash_.begin() + static_cast<std::ptrdiff_t>(kMbc6SectorSize),
				  0xFF);
}

// ---------------- TAMA5 ----------------
//
// The Bandai Tamagotchi mapper has no banking registers in the usual place:
// the entire mapper is two 4-bit registers on the cartridge bus, `$A000`
// (data) and `$A001` (which register the data register names), and the game
// has to unlock the chip before anything reads sensibly. Pan Docs' Bandai
// TAMA5 page is a stub, so the protocol below follows Tauwasser's
// reverse-engineering notes (gbdev.gg8.se "TAMA5 (WIP)").
namespace
{
// The 32-byte RAM lives in the low half of the 32 KiB of address space the
// command byte addresses; bit 4 of the command (Y) is the RAM address's high
// bit and bit 5 selects read (1) over write (0).
constexpr u8 kTama5CmdRamWrite = 0x00; // $00-$1F
constexpr u8 kTama5CmdRamRead = 0x20;  // $20-$3F
constexpr u8 kTama5CmdTama6 = 0x40;    // $40-$6F (the 4-bit MCU)
constexpr u8 kTama5CmdRtc = 0x80;      // $80-$BF (the TC8521AM, via the MCU)
} // namespace

TAMA5::TAMA5(std::vector<u8> rom, const std::vector<u8> &ram) : rom_(std::move(rom))
{
	(void) ram; // The chip has 32 bytes of its own; the header's size is a lie.
}

u8 TAMA5::read_rom(u16 addr) const
{
	// The register holds address bits 14-18 of a 1 MiB ROM, i.e. it selects a
	// 32 KiB block that is remapped over the whole $0000-$7FFF window (there is
	// no fixed bank 0 on this mapper).
	const u32 bank = (static_cast<u32>(rom_bank_high_ & 0x01) << 4) | (rom_bank_low_ & 0x0F);
	const u32 off = bank * 0x8000u + addr;
	return off < rom_.size() ? rom_[off] : 0xFF;
}

void TAMA5::write_rom(u16 addr, u8 value)
{
	// Nothing in the ROM region is writable on this mapper: the registers are
	// on the RAM side, which is the whole oddity of TAMA5.
	(void) addr;
	(void) value;
}

u8 TAMA5::read_ram(u16 addr) const
{
	// $A000/$A001 are the two registers; the rest of the 8 KiB window mirrors
	// them (the notes only ever mention $A000/$A001, so the mirrors are a
	// guess - flagged in agents.md).
	const u16 off = addr & 0x1FFF;
	if (off != 0 && off != 1)
		return 0xFF;
	if (off == 1)
		return kHiZ | (unlocked_ ? 0x01 : 0x00);
	if (!unlocked_) {
		// Uninitialised, the chip's shift register clocked out by the game
		// alternates $F0/$FF.
		unlocked_read_toggle_ = !unlocked_read_toggle_;
		return unlocked_read_toggle_ ? 0xFF : kHiZ;
	}
	switch (reg_) {
	case 0xA: // Status: echoes the register and reports "ready" in bit 0.
		return kHiZ | static_cast<u8>(reg_ & 0x0F) | 0x01;
	case 0xC:
		return kHiZ | (data_out_lo_ & 0x0F);
	case 0xD:
		return kHiZ | (data_out_hi_ & 0x0F);
	default:
		return 0xFF;
	}
}

void TAMA5::write_ram(u16 addr, u8 value)
{
	const u16 off = addr & 0x1FFF;
	if (off == 1) {
		// $A001 selects the register; $0A is the documented unlock.
		if ((value & 0x0F) == 0x0A)
			unlocked_ = true;
		reg_ = static_cast<u8>(value & 0x0F);
		return;
	}
	if (off != 0 || !unlocked_)
		return;
	// $A000 is the data register: the low nibble is the value, and *which*
	// register was selected decides what it means. While the chip is locked
	// the data register is not selected, so writes to it do nothing.
	const u8 v = static_cast<u8>(value & 0x0F);
	switch (reg_) {
	case 0x0:
		rom_bank_low_ = v;
		break;
	case 0x1:
		rom_bank_high_ = static_cast<u8>(v & 0x01);
		break;
	case 0x4:
		data_in_lo_ = v;
		break;
	case 0x5:
		data_in_hi_ = v;
		break;
	case 0x6:
		addr_hi_ = v;
		break;
	case 0x7:
		addr_lo_ = v;
		// The low nibble completes the command, so it acts here.
		run_command();
		break;
	default:
		break;
	}
}

void TAMA5::run_command()
{
	const u8 cmd = command();
	const u16 index = static_cast<u16>(((addr_hi_ & 0x01) << 4) | (addr_lo_ & 0x0F));
	if ((cmd & 0xF0) == kTama5CmdRamWrite) {
		ram_[index & 0x1F] = static_cast<u8>((data_in_hi_ << 4) | data_in_lo_);
		return;
	}
	if ((cmd & 0xF0) == kTama5CmdRamRead) {
		data_out_lo_ = static_cast<u8>(ram_[index & 0x1F] & 0x0F);
		data_out_hi_ = static_cast<u8>(ram_[index & 0x1F] >> 4);
		return;
	}
	// $40-$6F address the TAMA6 MCU and $80-$BF the RTC behind it; both need
	// the chip's 4-bit firmware, which is the documented gap (agents.md).
	// $70-$7F is open bus and $C0-$FF untested on hardware.
	if ((cmd & 0xF0) == kTama5CmdTama6 || (cmd & 0xF0) == kTama5CmdRtc) {
		data_out_lo_ = 0;
		data_out_hi_ = 0;
	}
}

u8 TAMA5::raw_read(u32 bank, u16 addr) const
{
	(void) bank;
	return addr < sizeof(ram_) ? ram_[addr] : 0xFF;
}

void TAMA5::raw_write(u32 bank, u16 addr, u8 value)
{
	(void) bank;
	if (addr < sizeof(ram_))
		ram_[addr] = value;
}

u32 TAMA5::raw_bank_count() const
{
	return 1; // The 32 bytes live in one save "bank" like every other mapper.
}

void TAMA5::save(StateBuffer &buf) const
{
	buf.write(static_cast<u8>(unlocked_ ? 1 : 0));
	buf.write(reg_);
	buf.write(rom_bank_low_);
	buf.write(rom_bank_high_);
	buf.write(data_in_lo_);
	buf.write(data_in_hi_);
	buf.write(addr_hi_);
	buf.write(addr_lo_);
	buf.write(data_out_lo_);
	buf.write(data_out_hi_);
	buf.write(static_cast<u8>(unlocked_read_toggle_ ? 1 : 0));
	for (u8 byte : ram_)
		buf.write(byte);
}

void TAMA5::load(StateBuffer &buf)
{
	u8 v = 0;
	auto byte = [&] {
		buf.read(v);
		return v;
	};
	unlocked_ = byte() != 0;
	reg_ = byte();
	rom_bank_low_ = byte();
	rom_bank_high_ = byte();
	data_in_lo_ = byte();
	data_in_hi_ = byte();
	addr_hi_ = byte();
	addr_lo_ = byte();
	data_out_lo_ = byte();
	data_out_hi_ = byte();
	unlocked_read_toggle_ = byte() != 0;
	for (u8 &byte_out : ram_)
		byte_out = byte();
}

u8 MBC6::raw_read(u32 bank, u16 addr) const
{
	// The battery format is banks of 8 KiB, so the 32 KiB of SRAM is the first
	// four banks and the 1 MiB of flash the 128 after them - a save has to keep
	// the downloaded minigames, not just the cart's SRAM.
	if (bank < kMbc6RamBanks)
		return raw_read_ram(bank * 0x2000u + addr);
	const u32 off = (bank - kMbc6RamBanks) * 0x2000u + addr;
	return off < flash_.size() ? flash_[off] : 0xFF;
}

void MBC6::raw_write(u32 bank, u16 addr, u8 value)
{
	if (bank < kMbc6RamBanks) {
		raw_write_ram(bank * 0x2000u + addr, value);
		return;
	}
	const u32 off = (bank - kMbc6RamBanks) * 0x2000u + addr;
	if (off < flash_.size())
		flash_[off] = value;
}

u32 MBC6::raw_bank_count() const
{
	return kMbc6RamBanks + flash_.size() / 0x2000u; // 4 SRAM + 128 flash banks.
}

void MBC6::save(StateBuffer &buf) const
{
	buf.write(static_cast<u8>(ram_enable_ ? 1 : 0));
	buf.write(ram_bank_a_);
	buf.write(ram_bank_b_);
	buf.write(static_cast<u8>(flash_enable_ ? 1 : 0));
	buf.write(static_cast<u8>(flash_wp_ ? 1 : 0));
	buf.write(bank_a_num_);
	buf.write(static_cast<u8>(bank_a_flash_ ? 1 : 0));
	buf.write(bank_b_num_);
	buf.write(static_cast<u8>(bank_b_flash_ ? 1 : 0));
	buf.write(static_cast<u8>(flash_state_));
	buf.write(static_cast<u8>(sector0_protected_ ? 1 : 0));
	buf.write(static_cast<u8>(cmd_unlock1_ ? 1 : 0));
	buf.write(static_cast<u8>(cmd_unlock2_ ? 1 : 0));
	buf.write(static_cast<u8>(hidden_read_ ? 1 : 0));
	buf.writeBytes(ram_.data(), ram_.size());
	buf.writeBytes(flash_.data(), flash_.size());
	buf.writeBytes(hidden_.data(), hidden_.size());
}

void MBC6::load(StateBuffer &buf)
{
	u8 v = 0;
	auto byte = [&] {
		buf.read(v);
		return v;
	};
	ram_enable_ = byte() != 0;
	ram_bank_a_ = byte();
	ram_bank_b_ = byte();
	flash_enable_ = byte() != 0;
	flash_wp_ = byte() != 0;
	bank_a_num_ = byte();
	bank_a_flash_ = byte() != 0;
	bank_b_num_ = byte();
	bank_b_flash_ = byte() != 0;
	flash_state_ = static_cast<FlashState>(byte());
	sector0_protected_ = byte() != 0;
	cmd_unlock1_ = byte() != 0;
	cmd_unlock2_ = byte() != 0;
	hidden_read_ = byte() != 0;
	buf.readBytes(ram_.data(), ram_.size());
	buf.readBytes(flash_.data(), flash_.size());
	buf.readBytes(hidden_.data(), hidden_.size());
}

u8 MBC7::raw_read(u32 bank, u16 addr) const
{
	const u32 off = bank * 0x2000u + addr;
	return off < ram_.size() ? ram_[off] : 0xFF;
}

void MBC7::raw_write(u32 bank, u16 addr, u8 value)
{
	const u32 off = bank * 0x2000u + addr;
	if (off < ram_.size())
		ram_[off] = value;
}

u32 MBC7::raw_bank_count() const
{
	return 1; // The 256-byte window is one "bank" of the save file.
}

void MBC7::save(StateBuffer &buf) const
{
	buf.write(rom_bank_);
	buf.write(static_cast<u8>(ram_enable_ ? 1 : 0));
	buf.write(static_cast<u8>(secondary_enable_ ? 1 : 0));
	buf.write(static_cast<u8>(latch_ready_ ? 1 : 0));
	buf.write(x_latch_);
	buf.write(y_latch_);
	buf.write(static_cast<u8>(eeprom_cs_ ? 1 : 0));
	buf.write(static_cast<u8>(eeprom_clk_ ? 1 : 0));
	buf.write(static_cast<u8>(eeprom_do_ ? 1 : 0));
	buf.write(static_cast<u8>(eeprom_di_ ? 1 : 0));
	buf.write(static_cast<u8>(eeprom_write_enabled_ ? 1 : 0));
	buf.write(read_bits_);
	buf.write(command_);
	buf.write(argument_bits_left_);
	buf.writeVector(ram_);
}

void MBC7::load(StateBuffer &buf)
{
	buf.read(rom_bank_);
	u8 v = 0;
	buf.read(v);
	ram_enable_ = v != 0;
	buf.read(v);
	secondary_enable_ = v != 0;
	buf.read(v);
	latch_ready_ = v != 0;
	buf.read(x_latch_);
	buf.read(y_latch_);
	buf.read(v);
	eeprom_cs_ = v != 0;
	buf.read(v);
	eeprom_clk_ = v != 0;
	buf.read(v);
	eeprom_do_ = v != 0;
	buf.read(v);
	eeprom_di_ = v != 0;
	buf.read(v);
	eeprom_write_enabled_ = v != 0;
	buf.read(read_bits_);
	buf.read(command_);
	buf.read(argument_bits_left_);
	buf.readVector(ram_);
}

HuC1::HuC1(std::vector<u8> rom, std::vector<u8> ram) : rom_(std::move(rom)), ram_(std::move(ram)) {}

u8 HuC1::read_rom(u16 addr) const
{
	const u32 banks = static_cast<u32>(rom_.size() / 0x4000);
	if (addr < 0x4000)
		return addr < rom_.size() ? rom_[addr] : 0xFF;
	// Full-byte bank, no 00->01 translation (unlike every MBC).
	u32 b = rom_bank_ % (banks == 0 ? 1 : banks);
	u32 off = b * 0x4000u + (addr - 0x4000);
	return off < rom_.size() ? rom_[off] : 0xFF;
}

void HuC1::write_rom(u16 addr, u8 value)
{
	if (addr < 0x2000) {
		// $0E selects the IR view; anything else (games still write $0A and
		// $00 here out of MBC habit) selects RAM. No enable gate exists.
		ir_mode_ = ((value & 0x0F) == 0x0E);
	} else if (addr < 0x4000) {
		rom_bank_ = value;
	} else if (addr < 0x6000) {
		ram_bank_ = value;
	}
	// 6000-7FFF: ignored (games write it MBC1-style anyway).
}

u8 HuC1::read_ram(u16 addr) const
{
	if (addr >= 0x2000)
		return 0xFF;
	// No receiver on this end of the link: always "did not see light".
	if (ir_mode_)
		return 0xC0;
	if (ram_.empty())
		return 0xFF;
	const u32 banks = static_cast<u32>(ram_.size() / 0x2000);
	u32 b = ram_bank_ % (banks == 0 ? 1 : banks);
	u32 off = b * 0x2000u + addr;
	return off < ram_.size() ? ram_[off] : 0xFF;
}

void HuC1::write_ram(u16 addr, u8 value)
{
	if (addr >= 0x2000)
		return;
	if (ir_mode_) {
		tx_led_ = (value & 0x01) != 0;
		return;
	}
	if (ram_.empty())
		return;
	const u32 banks = static_cast<u32>(ram_.size() / 0x2000);
	u32 b = ram_bank_ % (banks == 0 ? 1 : banks);
	u32 off = b * 0x2000u + addr;
	if (off < ram_.size())
		ram_[off] = value;
}

u8 HuC1::raw_read(u32 bank, u16 addr) const
{
	const u32 off = bank * 0x2000u + addr;
	return off < ram_.size() ? ram_[off] : 0xFF;
}

void HuC1::raw_write(u32 bank, u16 addr, u8 value)
{
	const u32 off = bank * 0x2000u + addr;
	if (off < ram_.size())
		ram_[off] = value;
}

u32 HuC1::raw_bank_count() const
{
	return static_cast<u32>(ram_.size() / 0x2000u);
}

void HuC1::save(StateBuffer &buf) const
{
	buf.write(rom_bank_);
	buf.write(ram_bank_);
	buf.write(ir_mode_);
	buf.write(tx_led_);
	buf.writeVector(ram_);
}

void HuC1::load(StateBuffer &buf)
{
	buf.read(rom_bank_);
	buf.read(ram_bank_);
	buf.read(ir_mode_);
	buf.read(tx_led_);
	buf.readVector(ram_);
}

// ---------------- HuC3 ----------------
// https://gbdev.io/pandocs/HuC3.html (model: SameBoy huc3)

HuC3::HuC3(std::vector<u8> rom, std::vector<u8> ram) : rom_(std::move(rom)), ram_(std::move(ram)) {}

u8 HuC3::read_rom(u16 addr) const
{
	const u32 banks = static_cast<u32>(rom_.size() / 0x4000);
	if (addr < 0x4000)
		return addr < rom_.size() ? rom_[addr] : 0xFF;
	// 7-bit bank like MBC5 (Pan Docs "bank $00 can also be mapped here").
	u32 b = rom_bank_ % (banks == 0 ? 1 : banks);
	u32 off = b * 0x4000u + (addr - 0x4000);
	return off < rom_.size() ? rom_[off] : 0xFF;
}

void HuC3::write_rom(u16 addr, u8 value)
{
	if (addr < 0x2000) {
		// Only the low nybble selects the $A000 view (Pan Docs).
		mode_ = value & 0x0F;
	} else if (addr < 0x4000) {
		rom_bank_ = value & 0x7F;
	} else if (addr < 0x6000) {
		ram_bank_ = value & 0x0F;
	}
	// 6000-7FFF: ignored (games write $01 here on startup regardless).
}

u8 HuC3::read_ram(u16 addr) const
{
	if (addr >= 0x2000)
		return 0xFF;
	switch (mode_) {
	case 0xC: // RTC command/response: last command echo, or 1 after $6,2.
		if (access_flags_ == 0x2)
			return 1;
		return read_;
	case 0xD: // RTC semaphore: the MCU is synchronous here, always ready.
		return 1;
	case 0xE: // IR: no local receiver, always "did not see light".
		return 0xC0;
	case 0x0:
	case 0x0A:
		break;
	default:
		return 0xFF; // Open bus for any other view (Pan Docs).
	}
	if (ram_.empty())
		return 0xFF;
	const u32 banks = static_cast<u32>(ram_.size() / 0x2000);
	u32 b = ram_bank_ % (banks == 0 ? 1 : banks);
	u32 off = b * 0x2000u + addr;
	return off < ram_.size() ? ram_[off] : 0xFF;
}

void HuC3::write_ram(u16 addr, u8 value)
{
	if (addr >= 0x2000)
		return;
	switch (mode_) {
	case 0xB: // RTC mailbox: executes synchronously (SameBoy).
		mailbox(value);
		return;
	case 0xD: // Semaphore request: nothing to wait for.
		return;
	case 0xE:
		tx_led_ = (value & 0x01) != 0;
		return;
	case 0xC: // Response register is read-only.
		return;
	case 0x0A:
		break;
	default:
		return; // Mode 0 is read-only RAM; anything else ignores writes.
	}
	if (ram_.empty())
		return;
	const u32 banks = static_cast<u32>(ram_.size() / 0x2000);
	u32 b = ram_bank_ % (banks == 0 ? 1 : banks);
	u32 off = b * 0x2000u + addr;
	if (off < ram_.size())
		ram_[off] = value;
}

void HuC3::rtc_ensure_base() const
{
	if (rtc_base_valid_)
		return;
	rtc_base_valid_ = true;
	rtc_base_host_ = static_cast<std::int64_t>(std::time(nullptr));
}

void HuC3::rtc_advance() const
{
	rtc_ensure_base();
	const std::int64_t now = static_cast<std::int64_t>(std::time(nullptr));
	std::int64_t elapsed = (now - rtc_base_host_) / 60;
	if (elapsed <= 0)
		return;
	// Consume whole minutes so the clock neither drifts nor double-counts.
	rtc_base_host_ += elapsed * 60;
	const std::int64_t total = static_cast<std::int64_t>(days_) * 1440 + minutes_ + elapsed;
	minutes_ = static_cast<u16>(total % 1440);
	days_ = static_cast<u16>((total / 1440) & 0xFFFF);
}

void HuC3::rtc_rebase()
{
	rtc_base_valid_ = true;
	rtc_base_host_ = static_cast<std::int64_t>(std::time(nullptr));
}

void HuC3::mailbox(u8 value)
{
	const u8 cmd = (value >> 4) & 0x0F;
	const u8 arg = value & 0x0F;
	switch (cmd) {
	case 1: // Read value at the access address into the response latch.
		rtc_advance();
		if (access_index_ < 3)
			read_ = (minutes_ >> (access_index_ * 4)) & 0xF;
		else if (access_index_ < 7)
			read_ = (days_ >> ((access_index_ - 3) * 4)) & 0xF;
		access_index_++;
		break;
	case 2:
	case 3: // Write value at the access address (2 is undocumented).
		if (access_index_ < 3) {
			minutes_ &= ~(0xF << (access_index_ * 4));
			minutes_ |= ((arg << (access_index_ * 4)) & 0xFFF);
		} else if (access_index_ < 7) {
			days_ &= ~(0xF << ((access_index_ - 3) * 4));
			days_ |= (arg << ((access_index_ - 3) * 4));
		} else if (access_index_ >= 0x58 && access_index_ <= 0x5A) {
			alarm_minutes_ &= ~(0xF << ((access_index_ - 0x58) * 4));
			alarm_minutes_ |= (arg << ((access_index_ - 0x58) * 4));
		} else if (access_index_ >= 0x5B && access_index_ <= 0x5E) {
			alarm_days_ &= ~(0xF << ((access_index_ - 0x5B) * 4));
			alarm_days_ |= (arg << ((access_index_ - 0x5B) * 4));
		} else if (access_index_ == 0x5F) {
			alarm_enabled_ = (arg & 1) != 0;
		}
		if (cmd == 3)
			access_index_++;
		// A game-set clock re-anchors the wall clock (MBC3-style rebase).
		rtc_rebase();
		break;
	case 4: // Access address low nybble.
		access_index_ &= 0xF0;
		access_index_ |= arg;
		break;
	case 5: // Access address high nybble.
		access_index_ &= 0x0F;
		access_index_ |= (arg << 4);
		break;
	case 6: // Latch flags; $2 answers the $6,2 status request with 1.
		access_flags_ = arg;
		break;
	default:
		break; // Commands 0 and 7 unobserved: ignored.
	}
}

bool HuC3::save_rtc(const std::string &path) const
{
	std::ofstream file(path);
	if (!file)
		return false;
	file << "GB4ME-HUC3-1 " << minutes_ << ' ' << days_ << ' ' << alarm_minutes_ << ' '
		 << alarm_days_ << ' ' << (alarm_enabled_ ? 1 : 0) << ' ' << rtc_base_host_ << ' '
		 << (rtc_base_valid_ ? 1 : 0) << '\n';
	return true;
}

bool HuC3::load_rtc(const std::string &path)
{
	std::ifstream file(path);
	if (!file)
		return false;
	std::string magic;
	unsigned mn = 0, dy = 0, amn = 0, ady = 0;
	int en = 0, valid = 0;
	std::int64_t host = 0;
	file >> magic >> mn >> dy >> amn >> ady >> en >> host >> valid;
	if (!file || magic != "GB4ME-HUC3-1")
		return false;
	minutes_ = static_cast<u16>(mn & 0xFFF);
	days_ = static_cast<u16>(dy & 0xFFFF);
	alarm_minutes_ = static_cast<u16>(amn & 0xFFF);
	alarm_days_ = static_cast<u16>(ady & 0xFFFF);
	alarm_enabled_ = en != 0;
	rtc_base_host_ = host;
	rtc_base_valid_ = valid != 0;
	return true;
}

u8 HuC3::raw_read(u32 bank, u16 addr) const
{
	const u32 off = bank * 0x2000u + addr;
	return off < ram_.size() ? ram_[off] : 0xFF;
}

void HuC3::raw_write(u32 bank, u16 addr, u8 value)
{
	const u32 off = bank * 0x2000u + addr;
	if (off < ram_.size())
		ram_[off] = value;
}

u32 HuC3::raw_bank_count() const
{
	return static_cast<u32>(ram_.size() / 0x2000u);
}

void HuC3::save(StateBuffer &buf) const
{
	buf.write(rom_bank_);
	buf.write(ram_bank_);
	buf.write(mode_);
	buf.write(access_index_);
	buf.write(read_);
	buf.write(access_flags_);
	buf.write(alarm_minutes_);
	buf.write(alarm_days_);
	buf.write(alarm_enabled_);
	buf.write(tx_led_);
	buf.write(minutes_);
	buf.write(days_);
	buf.write(rtc_base_host_);
	buf.write(rtc_base_valid_);
	buf.writeVector(ram_);
}

void HuC3::load(StateBuffer &buf)
{
	buf.read(rom_bank_);
	buf.read(ram_bank_);
	buf.read(mode_);
	buf.read(access_index_);
	buf.read(read_);
	buf.read(access_flags_);
	buf.read(alarm_minutes_);
	buf.read(alarm_days_);
	buf.read(alarm_enabled_);
	buf.read(tx_led_);
	buf.read(minutes_);
	buf.read(days_);
	buf.read(rtc_base_host_);
	buf.read(rtc_base_valid_);
	buf.readVector(ram_);
}

// ---------------- MMM01 ----------------
// https://gbdev.io/pandocs/MMM01.html (wiring: SameBoy mmm01)

MMM01::MMM01(std::vector<u8> rom, std::vector<u8> ram, bool battery)
	: rom_(std::move(rom)), ram_(std::move(ram)), battery_(battery)
{
}

u32 MMM01::rom_bank0() const
{
	// 0000-3FFF game base. multiplex swaps mid <-> ram_low (SameBoy); the
	// mode-0 ram term keeps only locked (game-select) bits (Pan Docs).
	u32 bank = 0;
	if (multiplex_)
		bank = (static_cast<u32>(rom_high_) << 7) |
			   (static_cast<u32>(mbc1_mode_ ? ram_low_ : (ram_low_ & ram_lock2_)) << 5);
	else
		bank = (static_cast<u32>(rom_high_) << 7) | (static_cast<u32>(rom_mid_) << 5);
	bank |= (rom_low_ & rom_lock5_);
	return bank;
}

u32 MMM01::rom_bank() const
{
	// 4000-7FFF selectable bank; the $00->$01 rule sees only the unmasked
	// low bits (Pan Docs $10/$30 example), unlike SameBoy's full equality.
	u32 bank = 0;
	if (multiplex_)
		bank = rom_low_ | (static_cast<u32>(ram_low_) << 5) | (static_cast<u32>(rom_high_) << 7);
	else
		bank = rom_low_ | (static_cast<u32>(rom_mid_) << 5) | (static_cast<u32>(rom_high_) << 7);
	u32 eff = bank;
	if ((rom_low_ & ~static_cast<u32>(rom_lock5_)) == 0)
		eff = bank | 1;
	return eff;
}

u32 MMM01::ram_bank() const
{
	// Multiplex ignores the mode (RAM Bank Low is ROM bits there); plain
	// mode 0 locks game-local banking off (Pan Docs), keeping the
	// menu-selected high base.
	if (multiplex_)
		return rom_mid_ | (static_cast<u32>(ram_high_) << 2);
	if (!mbc1_mode_)
		return static_cast<u32>(ram_high_) << 2;
	return ram_low_ | (static_cast<u32>(ram_high_) << 2);
}

u8 MMM01::read_rom(u16 addr) const
{
	if (!locked_) {
		// Unmapped menu: last 32 KiB of the image at 0000-7FFF.
		if (rom_.size() < 0x8000)
			return 0xFF;
		const u32 off = static_cast<u32>(rom_.size() - 0x8000 + addr);
		return off < rom_.size() ? rom_[off] : 0xFF;
	}
	const u32 count = static_cast<u32>(rom_.size() / 0x4000);
	u32 bank = (addr < 0x4000 ? rom_bank0() : rom_bank()) % (count == 0 ? 1 : count);
	u32 off = bank * 0x4000u + (addr & 0x3FFFu);
	return off < rom_.size() ? rom_[off] : 0xFF;
}

void MMM01::write_rom(u16 addr, u8 value)
{
	const u16 region = addr & 0xF000;
	if (region == 0x0000 || region == 0x1000) {
		ram_enabled_ = ((value & 0x0F) == 0x0A);
		if (!locked_) {
			// Bits 4-5 lock the matching RAM-bank bits (Pan Docs); the
			// mapping-enable bit lives at bit 6, not in the mask.
			ram_lock2_ = (value >> 4) & 0x03;
			locked_ = (value & 0x40) != 0;
		}
	} else if (region == 0x2000 || region == 0x3000) {
		if (!locked_)
			rom_mid_ = (value >> 5) & 0x03;
		// Locked low bits keep the menu's game-select value (Pan Docs: a
		// write to a masked bit is prevented).
		rom_low_ = (rom_low_ & rom_lock5_) | (value & ~rom_lock5_ & 0x1F);
	} else if (region == 0x4000 || region == 0x5000) {
		ram_low_ = (ram_low_ & ram_lock2_) | (value & ~ram_lock2_ & 0x03);
		if (!locked_) {
			ram_high_ = (value >> 2) & 0x03;
			rom_high_ = (value >> 4) & 0x03;
			mbc1_mode_disable_ = (value & 0x40) != 0;
		}
	} else if (region == 0x6000 || region == 0x7000) {
		if (!mbc1_mode_disable_)
			mbc1_mode_ = (value & 1) != 0;
		if (!locked_) {
			// Mask bits 1-5 in position; bit 0 forced clear so ROM bit 0
			// stays writable (Pan Docs).
			rom_lock5_ = value & 0x3C;
			multiplex_ = (value & 0x40) != 0;
		}
	}
}

u8 MMM01::read_ram(u16 addr) const
{
	if (addr >= 0x2000)
		return 0xFF;
	if (!ram_enabled_ || ram_.empty())
		return 0xFF;
	const u32 banks = static_cast<u32>(ram_.size() / 0x2000);
	u32 b = ram_bank() % (banks == 0 ? 1 : banks);
	u32 off = b * 0x2000u + addr;
	return off < ram_.size() ? ram_[off] : 0xFF;
}

void MMM01::write_ram(u16 addr, u8 value)
{
	if (addr >= 0x2000 || !ram_enabled_ || ram_.empty())
		return;
	const u32 banks = static_cast<u32>(ram_.size() / 0x2000);
	u32 b = ram_bank() % (banks == 0 ? 1 : banks);
	u32 off = b * 0x2000u + addr;
	if (off < ram_.size())
		ram_[off] = value;
}

u8 MMM01::raw_read(u32 bank, u16 addr) const
{
	const u32 off = bank * 0x2000u + addr;
	return off < ram_.size() ? ram_[off] : 0xFF;
}

void MMM01::raw_write(u32 bank, u16 addr, u8 value)
{
	const u32 off = bank * 0x2000u + addr;
	if (off < ram_.size())
		ram_[off] = value;
}

u32 MMM01::raw_bank_count() const
{
	return static_cast<u32>(ram_.size() / 0x2000u);
}

void MMM01::save(StateBuffer &buf) const
{
	buf.write(ram_enabled_);
	buf.write(locked_);
	buf.write(multiplex_);
	buf.write(mbc1_mode_);
	buf.write(mbc1_mode_disable_);
	buf.write(rom_low_);
	buf.write(rom_mid_);
	buf.write(rom_high_);
	buf.write(ram_low_);
	buf.write(ram_high_);
	buf.write(rom_lock5_);
	buf.write(ram_lock2_);
	buf.writeVector(ram_);
}

void MMM01::load(StateBuffer &buf)
{
	buf.read(ram_enabled_);
	buf.read(locked_);
	buf.read(multiplex_);
	buf.read(mbc1_mode_);
	buf.read(mbc1_mode_disable_);
	buf.read(rom_low_);
	buf.read(rom_mid_);
	buf.read(rom_high_);
	buf.read(ram_low_);
	buf.read(ram_high_);
	buf.read(rom_lock5_);
	buf.read(ram_lock2_);
	buf.readVector(ram_);
}

// ---------------- Cartridge ----------------

bool Cartridge::load(const std::string &path)
{
	// Cap the read before allocating: folder scanning auto-probes every
	// file, so a huge or sparse file would otherwise request a multi-GB
	// vector and throw bad_alloc (terminating the process). 64 MiB is far
	// above the 8 MiB maximum for any real GB/GBC cartridge.
	constexpr std::streamsize kMaxRomBytes = 64 * 1024 * 1024;
	std::ifstream file(path, std::ios::binary | std::ios::ate);
	if (!file) {
		last_error_ = "cannot open file";
		return false;
	}
	std::streamsize size = file.tellg();
	if (size <= 0 || size > kMaxRomBytes) {
		last_error_ = "bad file size";
		return false;
	}
	file.seekg(0, std::ios::beg);
	std::vector<u8> data(static_cast<size_t>(size));
	if (!file.read(reinterpret_cast<char *>(data.data()), size)) {
		last_error_ = "cannot read file";
		return false;
	}
	return load_from_bytes(data);
}

bool Cartridge::load_from_bytes(const std::vector<u8> &data)
{
	if (data.size() < 0x150) {
		last_error_ = "file too small for a GB header";
		return false;
	}
	// The Nintendo logo bytes are verified by the boot ROM on hardware; a
	// ROM without them is not a Game Boy ROM (e.g. a .gg Game Gear dump).
	// https://gbdev.io/pandocs/The_Cartridge_Header.html#0104-0133--nintendo-logo
	static constexpr u8 kNintendoLogo[48] = {
		0xCE, 0xED, 0x66, 0x66, 0xCC, 0x0D, 0x00, 0x0B, 0x03, 0x73, 0x00, 0x83,
		0x00, 0x0C, 0x00, 0x0D, 0x00, 0x08, 0x11, 0x1F, 0x88, 0x89, 0x00, 0x0E,
		0xDC, 0xCC, 0x6E, 0xE6, 0xDD, 0xDD, 0xD9, 0x99, 0xBB, 0xBB, 0x67, 0x63,
		0x6E, 0x0E, 0xEC, 0xCC, 0xDD, 0xDC, 0x99, 0x9F, 0xBB, 0xB9, 0x33, 0x3E,
	};
	for (int i = 0; i < 48; i++) {
		if (data[0x0104 + i] != kNintendoLogo[i]) {
			last_error_ = "missing Nintendo logo (not a GB/GBC ROM)";
			return false;
		}
	}
	rom_image_ = data;
	if (!parse_header())
		return false;

	const u8 type = header_.cartridge_type;
	// MBC2 has 512×4-bit builtin RAM regardless of header ram_size.
	std::vector<u8> ram;
	if (type == 0x05 || type == 0x06) {
		const bool has_ram = (type == 0x05 || type == 0x06);
		(void) has_ram;
		ram.clear(); // MBC2 manages its own RAM.
	} else {
		ram.assign(calc_ram_size(header_.ram_size), 0);
	}

	auto make_ram = [&ram](u32 n) {
		ram.assign(n, 0);
		return std::move(ram);
	};

	if (type == 0x00 || type == 0x08 || type == 0x09) {
		mbc_type_ = MBCType::None;
		// 0x08 = ROM+RAM, 0x09 = ROM+RAM+BATTERY. The battery flag must be
		// carried or save_ram/load_ram are skipped and the game's SRAM is
		// never persisted.
		mbc_ = std::make_unique<NoMBC>(rom_image_, make_ram(calc_ram_size(header_.ram_size)),
									   type == 0x09);
	} else if (type >= 0x01 && type <= 0x03) {
		mbc_type_ = MBCType::MBC1;
		auto mbc1 = std::make_unique<MBC1>(rom_image_, make_ram(calc_ram_size(header_.ram_size)),
										   type == 0x03);
		// MBC1M multicart wiring (Pan Docs "MBC1M", SameBoy heuristic): a 1MB
		// MBC1 image carrying a Nintendo header in bank $10 is a compilation
		// cart with the 4-bit wiring. A wrong guess here only affects 1MB+
		// images, and a 48-byte logo coincidence in real game data is
		// effectively impossible (bad dumps with duplicated halves match too,
		// which is exactly what they are).
		bool multicart = false;
		if (rom_image_.size() >= 0x44000 && rom_image_.size() > 0x40104 + 0x30) {
			multicart = std::memcmp(&rom_image_[0x0104], &rom_image_[0x40104], 0x30) == 0;
		}
		mbc1->set_multicart(multicart);
		mbc_ = std::move(mbc1);
	} else if (type == 0x05 || type == 0x06) {
		mbc_type_ = MBCType::MBC2;
		mbc_ = std::make_unique<MBC2>(rom_image_, type == 0x06);
	} else if (type >= 0x0F && type <= 0x13) {
		const bool batt = (type == 0x0F || type == 0x10 || type == 0x13);
		const bool rtc = (type == 0x0F || type == 0x10);
		mbc_type_ = MBCType::MBC3;
		auto mbc3 = std::make_unique<MBC3>(rom_image_, make_ram(calc_ram_size(header_.ram_size)),
										   batt, rtc);
		// MBC30 (SameBoy is_mbc30): big MBC3s wire all 8 ROM bank bits (and
		// can carry >32KB RAM). No distinct header type exists.
		mbc3->set_mbc30(rom_image_.size() > 0x200000 || calc_ram_size(header_.ram_size) > 0x8000);
		mbc_ = std::move(mbc3);
	} else if (type >= 0x19 && type <= 0x1E) {
		const bool batt = (type == 0x1B || type == 0x1E || type == 0x1D);
		// 0x1C/1D/1E are rumble variants (motor ignored).
		mbc_type_ = MBCType::MBC5;
		mbc_ = std::make_unique<MBC5>(rom_image_, make_ram(calc_ram_size(header_.ram_size)), batt);
	} else if (type == 0xFF) {
		mbc_type_ = MBCType::HuC1;
		mbc_ = std::make_unique<HuC1>(rom_image_, make_ram(calc_ram_size(header_.ram_size)));
	} else if (type == 0xFE) {
		mbc_type_ = MBCType::HuC3;
		mbc_ = std::make_unique<HuC3>(rom_image_, make_ram(calc_ram_size(header_.ram_size)));
	} else if (type == 0x20) {
		mbc_type_ = MBCType::MBC6;
		mbc_ = std::make_unique<MBC6>(rom_image_, make_ram(calc_ram_size(header_.ram_size)));
	} else if (type == 0xFD) {
		mbc_type_ = MBCType::TAMA5;
		mbc_ = std::make_unique<TAMA5>(rom_image_, make_ram(calc_ram_size(header_.ram_size)));
	} else if (type == 0x22) {
		mbc_type_ = MBCType::MBC7;
		mbc_ = std::make_unique<MBC7>(rom_image_, std::vector<u8>());
	} else if (type == 0x0B || type == 0x0C || type == 0x0D) {
		mbc_type_ = MBCType::MMM01;
		mbc_ = std::make_unique<MMM01>(rom_image_, make_ram(calc_ram_size(header_.ram_size)),
									   type == 0x0D);
	} else {
		mbc_.reset();
		// Name known-but-unsupported mappers so the failure is actionable.
		// https://gbdev.io/pandocs/The_Cartridge_Header.html#0147--cartridge-type
		const char *name = nullptr;
		switch (type) {
		default:
			break;
		}
		char buf[96];
		if (name != nullptr)
			std::snprintf(buf, sizeof(buf), "unsupported mapper %s (type 0x%02X)", name, type);
		else
			std::snprintf(buf, sizeof(buf), "unsupported mapper type 0x%02X", type);
		last_error_ = buf;
		return false;
	}
	last_error_.clear();
	return true;
}

void Cartridge::reset()
{
	// RAM contents persist across reset when battery-backed; ROM banking
	// state resets by rebuilding the MBC over the same images.
	if (mbc_)
		load_from_bytes(rom_image_);
}

u8 Cartridge::read_rom(u16 addr) const
{
	return mbc_ ? mbc_->read_rom(addr) : 0xFF;
}

void Cartridge::write_rom(u16 addr, u8 value)
{
	if (mbc_)
		mbc_->write_rom(addr, value);
}

u8 Cartridge::read_ram(u16 addr) const
{
	return mbc_ ? mbc_->read_ram(addr) : 0xFF;
}

void Cartridge::write_ram(u16 addr, u8 value)
{
	if (mbc_)
		mbc_->write_ram(addr, value);
}

bool Cartridge::has_battery() const
{
	return mbc_ ? mbc_->has_battery() : false;
}

bool Cartridge::has_rtc() const
{
	return mbc_ ? mbc_->has_rtc() : false;
}

void Cartridge::set_accelerometer(int x, int y)
{
	if (mbc_)
		mbc_->set_accelerometer(x, y);
}

bool Cartridge::has_rumble() const
{
	return mbc_ != nullptr && mbc_->has_rumble();
}

bool Cartridge::rumble() const
{
	return mbc_ != nullptr && mbc_->rumble();
}

bool Cartridge::save_rtc(const std::string &path) const
{
	return mbc_ ? mbc_->save_rtc(path) : false;
}

bool Cartridge::load_rtc(const std::string &path)
{
	return mbc_ ? mbc_->load_rtc(path) : false;
}

void Cartridge::save_ram(const std::string &path) const
{
	if (!has_battery() || !mbc_ || mbc_->raw_bank_count() == 0)
		return;
	// Raw backing-store access: the RAM-enable gate and bank register are
	// ignored and the live MBC state is never touched. The old code wrote
	// 0x6000/0x4000 to walk the banks, which reset a running game's bank
	// and mode, and yielded an all-0xFF save whenever RAM was disabled.
	const u32 banks = mbc_->raw_bank_count();
	const u32 width = mbc_type_ == MBCType::MBC2 ? 0x200u : 0x2000u;
	std::ofstream file(path, std::ios::binary);
	if (!file)
		return;
	for (u32 b = 0; b < banks; b++)
		for (u32 i = 0; i < width; i++)
			file.put(static_cast<char>(mbc_->raw_read(b, static_cast<u16>(i))));
}

void Cartridge::load_ram(const std::string &path)
{
	if (!has_battery() || !mbc_ || mbc_->raw_bank_count() == 0)
		return;
	std::ifstream file(path, std::ios::binary);
	if (!file)
		return;
	const u32 banks = mbc_->raw_bank_count();
	const u32 width = mbc_type_ == MBCType::MBC2 ? 0x200u : 0x2000u;
	for (u32 b = 0; b < banks; b++) {
		for (u32 i = 0; i < width; i++) {
			const int c = file.get();
			if (c == EOF)
				return;
			mbc_->raw_write(b, static_cast<u16>(i), static_cast<u8>(c));
		}
	}
}

u32 Cartridge::ram_size() const
{
	if (mbc_type_ == MBCType::MBC2)
		return 512;
	return calc_ram_size(header_.ram_size);
}

bool Cartridge::parse_header()
{
	const u8 *h = rom_image_.data();
	for (int i = 0; i < 16; i++)
		header_.title[i] = static_cast<char>(h[0x0134 + i]);
	header_.cgb_flag = h[0x0143];
	header_.sgb_flag = h[0x0146];
	header_.cartridge_type = h[0x0147];
	header_.rom_size = h[0x0148];
	header_.ram_size = h[0x0149];
	header_.destination_code = h[0x014A];
	header_.old_licensee = h[0x014B];
	header_.mask_rom_version = h[0x014C];
	header_.header_checksum = h[0x014D];
	header_.global_checksum = make_u16(h[0x014F], h[0x014E]);

	hardware_mode_ = detect_hardware_mode(header_.cgb_flag);
	mbc_type_ = detect_mbc_type(header_.cartridge_type);

	// Validate header checksum (sum of 0x0134-0x014C, minus each byte, minus 1).
	u8 sum = 0;
	for (u16 a = 0x0134; a <= 0x014C; a++)
		sum = sum - h[a] - 1;
	(void) sum; // Logged by callers if needed; some homebrew has bad checksums.
	return true;
}

HardwareMode Cartridge::detect_hardware_mode(u8 cgb_flag)
{
	// https://gbdev.io/pandocs/The_Cartridge_Header.html#0143--cgb-flag
	if (cgb_flag == 0xC0)
		return HardwareMode::CGB_Only;
	if (cgb_flag == 0x80)
		return HardwareMode::CGB_Compatible;
	return HardwareMode::DMG;
}

MBCType Cartridge::detect_mbc_type(u8 type_byte)
{
	switch (type_byte) {
	case 0x00:
	case 0x08:
	case 0x09:
		return MBCType::None;
	case 0x01:
	case 0x02:
	case 0x03:
		return MBCType::MBC1;
	case 0x05:
	case 0x06:
		return MBCType::MBC2;
	case 0x0F:
	case 0x10:
	case 0x11:
	case 0x12:
	case 0x13:
		return MBCType::MBC3;
	case 0x19:
	case 0x1A:
	case 0x1B:
	case 0x1C:
	case 0x1D:
	case 0x1E:
		return MBCType::MBC5;
	case 0xFF:
		return MBCType::HuC1;
	case 0xFE:
		return MBCType::HuC3;
	case 0x0B:
	case 0x0C:
	case 0x0D:
		return MBCType::MMM01;
	default:
		return MBCType::None;
	}
}

u32 Cartridge::calc_rom_size(u8 size_byte)
{
	// 32KB << code (0x00..0x08).
	if (size_byte > 0x08)
		return 32 * 1024;
	return (32 * 1024u) << size_byte;
}

u32 Cartridge::calc_ram_size(u8 size_byte)
{
	switch (size_byte) {
	case 0x00:
		return 0;
	case 0x01:
		return 2 * 1024; // Unused by commercial games.
	case 0x02:
		return 8 * 1024; // 1 bank.
	case 0x03:
		return 32 * 1024; // 4 banks.
	case 0x04:
		return 128 * 1024; // 16 banks.
	case 0x05:
		return 64 * 1024; // 8 banks.
	default:
		return 0;
	}
}

// ---- Savestate ----
// Each mapper writes its bank registers and its RAM. The ROM image is
// immutable after construction and is left out on purpose (a state is only
// ever loaded into the cartridge that wrote it), as is battery_/rtc_, which
// come from the header.

void NoMBC::save(StateBuffer &buf) const
{
	buf.writeVector(ram_);
}

void NoMBC::load(StateBuffer &buf)
{
	buf.readVector(ram_);
}

void MBC1::save(StateBuffer &buf) const
{
	buf.write(ram_enabled_);
	buf.write(rom_bank_low5_);
	buf.write(rom_bank_high2_);
	buf.write(ram_bank_);
	buf.write(banking_mode_);
	buf.writeVector(ram_);
}

void MBC1::load(StateBuffer &buf)
{
	buf.read(ram_enabled_);
	buf.read(rom_bank_low5_);
	buf.read(rom_bank_high2_);
	buf.read(ram_bank_);
	buf.read(banking_mode_);
	buf.readVector(ram_);
}

void MBC2::save(StateBuffer &buf) const
{
	buf.write(ram_enabled_);
	buf.write(rom_bank_);
	buf.writeArray(ram_);
}

void MBC2::load(StateBuffer &buf)
{
	buf.read(ram_enabled_);
	buf.read(rom_bank_);
	buf.readArray(ram_);
}

void MBC3::save(StateBuffer &buf) const
{
	buf.write(ram_enabled_);
	buf.write(rom_bank_);
	buf.write(ram_bank_);
	buf.writeArray(rtc_latched_);
	buf.writeArray(rtc_set_);
	buf.write(rtc_base_secs_);
	buf.write(rtc_base_host_);
	buf.write(rtc_base_valid_);
	buf.write(rtc_halt_);
	buf.write(rtc_carry_);
	buf.write(latch_state_);
	buf.writeVector(ram_);
}

void MBC3::load(StateBuffer &buf)
{
	buf.read(ram_enabled_);
	buf.read(rom_bank_);
	buf.read(ram_bank_);
	buf.readArray(rtc_latched_);
	buf.readArray(rtc_set_);
	buf.read(rtc_base_secs_);
	buf.read(rtc_base_host_);
	buf.read(rtc_base_valid_);
	buf.read(rtc_halt_);
	buf.read(rtc_carry_);
	buf.read(latch_state_);
	buf.readVector(ram_);
}

void MBC5::save(StateBuffer &buf) const
{
	buf.write(ram_enabled_);
	buf.write(rumble_);
	buf.write(rom_bank_);
	buf.write(ram_bank_);
	buf.writeVector(ram_);
}

void MBC5::load(StateBuffer &buf)
{
	buf.read(ram_enabled_);
	buf.read(rumble_);
	buf.read(rom_bank_);
	buf.read(ram_bank_);
	buf.readVector(ram_);
}

void Cartridge::save(StateBuffer &buf) const
{
	if (mbc_)
		mbc_->save(buf);
}

void Cartridge::load(StateBuffer &buf)
{
	if (mbc_)
		mbc_->load(buf);
}

} // namespace gb
