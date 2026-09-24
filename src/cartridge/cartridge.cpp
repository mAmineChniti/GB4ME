// Cartridge: header parsing, CGB-mode detection, and the polymorphic MBC
// implementations (NoMBC + MBC1/2/3/5).
// https://gbdev.io/pandocs/The_Cartridge_Header.html
// https://gbdev.io/pandocs/MBCs.html

#include "gb/cartridge.h"
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
	const u32 count = rom_bank_count();
	// Hardware quirk (Pan Docs "MBC1"): on cartridges small enough that the
	// upper bank bits are not wired (512 KiB and under, i.e. <= 32 banks of
	// 16 KiB) the secondary register's bits are ignored, and the 5-bit low
	// register is masked to the actual bank count. Without this a 256 KiB
	// cart programmed with bank register $10 selected bank 1 instead of
	// bank 0.
	u32 lo5 = rom_bank_low5_;
	if (count != 0)
		lo5 %= count;
	if (lo5 == 0 && count > 1)
		lo5 = 1;
	if (count > 32)
		return lo5; // No secondary bits on small carts.
	u32 bank = (static_cast<u32>(rom_bank_high2_) << 5) | lo5;
	if (count > 0)
		bank %= count;
	if (bank == 0 && count > 1)
		bank = 1;
	return bank;
}

u32 MBC1::current_ram_bank() const
{
	if (!banking_mode_)
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
			bank = (static_cast<u32>(ram_bank_) << 5);
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
		rom_bank_low5_ = value & 0x1F;
		if (rom_bank_low5_ == 0)
			rom_bank_low5_ = 1;
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
	(void)bank;
	return addr < 0x200 ? ram_[addr] : 0x0F;
}

void MBC2::raw_write(u32 bank, u16 addr, u8 value)
{
	(void)bank;
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
	u32 b = rom_bank_ % (banks == 0 ? 1 : banks);
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
		rom_bank_ = value & 0x7F;
		if (rom_bank_ == 0)
			rom_bank_ = 1;
	} else if (addr < 0x6000) {
		ram_bank_ = value;
	} else {
		// Latch clock data: 0->1 edge latches (stubbed to 0).
		// https://gbdev.io/pandocs/MBC3.html#the-clock-counter-registers
		if (latch_state_ == 0 && value == 1) {
			rtc_regs_.fill(0);
		}
		latch_state_ = value;
	}
}

u8 MBC3::read_ram(u16 addr) const
{
	if (addr >= 0x2000)
		return 0xFF;
	if (ram_bank_ <= 0x03) {
		if (!ram_enabled_ || ram_.empty())
			return 0xFF;
		const u32 banks = static_cast<u32>(ram_.size() / 0x2000);
		u32 b = ram_bank_ % (banks == 0 ? 1 : banks);
		u32 off = b * 0x2000u + addr;
		return off < ram_.size() ? ram_[off] : 0xFF;
	}
	if (rtc_ && ram_bank_ >= 0x08 && ram_bank_ <= 0x0C) {
		return rtc_regs_[ram_bank_ - 0x08];
	}
	return 0xFF;
}

void MBC3::write_ram(u16 addr, u8 value)
{
	if (addr >= 0x2000)
		return;
	if (ram_bank_ <= 0x03) {
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
		rtc_regs_[ram_bank_ - 0x08] = value;
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
		// Bits 4-7 select the rumble motor (ignored).
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

// ---------------- Cartridge ----------------

bool Cartridge::load(const std::string &path)
{
	// Cap the read before allocating: folder scanning auto-probes every
	// file, so a huge or sparse file would otherwise request a multi-GB
	// vector and throw bad_alloc (terminating the process). 64 MiB is far
	// above the 8 MiB maximum for any real GB/GBC cartridge.
	constexpr std::streamsize kMaxRomBytes = 64 * 1024 * 1024;
	std::ifstream file(path, std::ios::binary | std::ios::ate);
	if (!file)
		return false;
	std::streamsize size = file.tellg();
	if (size <= 0 || size > kMaxRomBytes)
		return false;
	file.seekg(0, std::ios::beg);
	std::vector<u8> data(static_cast<size_t>(size));
	if (!file.read(reinterpret_cast<char *>(data.data()), size))
		return false;
	return load_from_bytes(data);
}

bool Cartridge::load_from_bytes(const std::vector<u8> &data)
{
	if (data.size() < 0x150)
		return false;
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
		if (data[0x0104 + i] != kNintendoLogo[i])
			return false;
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
		mbc_ = std::make_unique<MBC1>(rom_image_, make_ram(calc_ram_size(header_.ram_size)),
									  type == 0x03);
	} else if (type == 0x05 || type == 0x06) {
		mbc_type_ = MBCType::MBC2;
		mbc_ = std::make_unique<MBC2>(rom_image_, type == 0x06);
	} else if (type >= 0x0F && type <= 0x13) {
		const bool batt = (type == 0x0F || type == 0x10 || type == 0x13);
		const bool rtc = (type == 0x0F || type == 0x10);
		mbc_type_ = MBCType::MBC3;
		mbc_ = std::make_unique<MBC3>(rom_image_, make_ram(calc_ram_size(header_.ram_size)), batt,
									  rtc);
	} else if (type >= 0x19 && type <= 0x1E) {
		const bool batt = (type == 0x1B || type == 0x1E || type == 0x1D);
		// 0x1C/1D/1E are rumble variants (motor ignored).
		mbc_type_ = MBCType::MBC5;
		mbc_ = std::make_unique<MBC5>(rom_image_, make_ram(calc_ram_size(header_.ram_size)), batt);
	} else {
		mbc_.reset();
		return false;
	}
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

} // namespace gb
