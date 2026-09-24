#pragma once

#include "types.h"
#include <array>
#include <cstddef>
#include <memory>
#include <string>
#include <vector>

namespace gb
{

// ---- GBC-Ready Rule 5: cartridge MBC is a polymorphic interface ----
// https://gbdev.io/pandocs/MBCs.html
// The Bus talks only to this interface. Adding MBC2/3/5 or GBC-specific
// MBCs later means adding new subclasses, never changing the Bus.
class MBC
{
  public:
	virtual ~MBC() = default;
	virtual u8 read_rom(u16 addr) const = 0;
	virtual void write_rom(u16 addr, u8 value) = 0;
	virtual u8 read_ram(u16 addr) const = 0;
	virtual void write_ram(u16 addr, u8 value) = 0;
	// Direct backing-store access used by battery-save load/save. These
	// bypass the RAM-enable gate and the bank register, so persisting a
	// save never depends on (or disturbs) the live MBC state.
	virtual u8 raw_read(u32 bank, u16 addr) const = 0;
	virtual void raw_write(u32 bank, u16 addr, u8 value) = 0;
	virtual u32 raw_bank_count() const = 0;
	virtual bool has_battery() const
	{
		return false;
	}
};

// ROM-only cartridge (no MBC). 32KB ROM, optional 8KB RAM.
class NoMBC : public MBC
{
  public:
	NoMBC(std::vector<u8> rom, std::vector<u8> ram, bool battery = false);
	u8 read_rom(u16 addr) const override;
	void write_rom(u16 addr, u8 value) override;
	u8 read_ram(u16 addr) const override;
	void write_ram(u16 addr, u8 value) override;
	u8 raw_read(u32 bank, u16 addr) const override;
	void raw_write(u32 bank, u16 addr, u8 value) override;
	u32 raw_bank_count() const override;
	bool has_battery() const override
	{
		return battery_;
	}

  private:
	std::vector<u8> rom_;
	std::vector<u8> ram_;
	bool battery_ = false;
};

// MBC1: up to 2MB ROM / 32KB RAM.
// https://gbdev.io/pandocs/MBC1.html
class MBC1 : public MBC
{
  public:
	MBC1(std::vector<u8> rom, std::vector<u8> ram, bool battery);
	u8 read_rom(u16 addr) const override;
	void write_rom(u16 addr, u8 value) override;
	u8 read_ram(u16 addr) const override;
	void write_ram(u16 addr, u8 value) override;
	u8 raw_read(u32 bank, u16 addr) const override;
	void raw_write(u32 bank, u16 addr, u8 value) override;
	u32 raw_bank_count() const override;
	bool has_battery() const override
	{
		return battery_;
	}

  private:
	std::vector<u8> rom_;
	std::vector<u8> ram_;
	bool battery_ = false;
	bool ram_enabled_ = false;
	u8 rom_bank_low5_ = 1;      // lower 5 bits (0 maps to 1)
	u8 rom_bank_high2_ = 0;     // upper 2 bits
	u8 ram_bank_ = 0;           // 2-bit RAM bank / ROM bank upper bits
	bool banking_mode_ = false; // false = ROM banking, true = RAM banking

	u32 rom_bank_count() const;
	u32 ram_bank_count() const;
	u32 current_rom_bank() const;
	u32 current_ram_bank() const;
};

// MBC2: 256KB ROM, 512×4-bit builtin RAM.
// https://gbdev.io/pandocs/MBC2.html
class MBC2 : public MBC
{
  public:
	MBC2(std::vector<u8> rom, bool battery);
	u8 read_rom(u16 addr) const override;
	void write_rom(u16 addr, u8 value) override;
	u8 read_ram(u16 addr) const override;
	void write_ram(u16 addr, u8 value) override;
	u8 raw_read(u32 bank, u16 addr) const override;
	void raw_write(u32 bank, u16 addr, u8 value) override;
	u32 raw_bank_count() const override;
	bool has_battery() const override
	{
		return battery_;
	}

  private:
	std::vector<u8> rom_;
	std::array<u8, 512> ram_{};
	bool battery_ = false;
	bool ram_enabled_ = false;
	u8 rom_bank_ = 1;
};

// MBC3: up to 8MB ROM / 32KB RAM + optional RTC.
// https://gbdev.io/pandocs/MBC3.html
// RTC is stubbed (latched regs read 0, writes latched) — enough for
// non-RTC games; RTC games need a ticking clock later.
class MBC3 : public MBC
{
  public:
	MBC3(std::vector<u8> rom, std::vector<u8> ram, bool battery, bool rtc);
	u8 read_rom(u16 addr) const override;
	void write_rom(u16 addr, u8 value) override;
	u8 read_ram(u16 addr) const override;
	void write_ram(u16 addr, u8 value) override;
	u8 raw_read(u32 bank, u16 addr) const override;
	void raw_write(u32 bank, u16 addr, u8 value) override;
	u32 raw_bank_count() const override;
	bool has_battery() const override
	{
		return battery_;
	}

  private:
	std::vector<u8> rom_;
	std::vector<u8> ram_;
	bool battery_ = false;
	bool rtc_ = false;
	bool ram_enabled_ = false;
	u8 rom_bank_ = 1;              // 7 bits.
	u8 ram_bank_ = 0;              // 0-3 = RAM, 8-C = RTC (stubbed).
	std::array<u8, 5> rtc_regs_{}; // S M H DL DH (latched, stubbed).
	u8 latch_state_ = 0;
};

// MBC5: up to 8MB ROM / 128KB RAM (+ rumble, ignored).
// https://gbdev.io/pandocs/MBC5.html
class MBC5 : public MBC
{
  public:
	MBC5(std::vector<u8> rom, std::vector<u8> ram, bool battery);
	u8 read_rom(u16 addr) const override;
	void write_rom(u16 addr, u8 value) override;
	u8 read_ram(u16 addr) const override;
	void write_ram(u16 addr, u8 value) override;
	u8 raw_read(u32 bank, u16 addr) const override;
	void raw_write(u32 bank, u16 addr, u8 value) override;
	u32 raw_bank_count() const override;
	bool has_battery() const override
	{
		return battery_;
	}

  private:
	std::vector<u8> rom_;
	std::vector<u8> ram_;
	bool battery_ = false;
	bool ram_enabled_ = false;
	u16 rom_bank_ = 1; // 9 bits (0 maps to 1).
	u8 ram_bank_ = 0;  // 4 bits.
};

enum class MBCType : u8 {
	None = 0,
	MBC1 = 1,
	MBC2 = 2,
	MBC3 = 3,
	MBC5 = 5,
};

struct CartridgeHeader {
	std::array<char, 16> title{};
	u8 cgb_flag = 0; // 0x0143. 0x80 = compatible, 0xC0 = CGB-only.
	u8 sgb_flag = 0;
	u8 cartridge_type = 0;
	u8 rom_size = 0;
	u8 ram_size = 0;
	u8 destination_code = 0;
	u8 old_licensee = 0;
	u8 mask_rom_version = 0;
	u8 header_checksum = 0;
	u16 global_checksum = 0;
};

class Cartridge
{
  public:
	Cartridge() = default;
	~Cartridge() = default;

	Cartridge(const Cartridge &) = delete;
	Cartridge &operator=(const Cartridge &) = delete;

	bool load(const std::string &path);
	bool load_from_bytes(const std::vector<u8> &data);
	void reset();

	u8 read_rom(u16 addr) const;
	void write_rom(u16 addr, u8 value);
	u8 read_ram(u16 addr) const;
	void write_ram(u16 addr, u8 value);

	bool has_battery() const;
	void save_ram(const std::string &path) const;
	void load_ram(const std::string &path);

	const CartridgeHeader &header() const
	{
		return header_;
	}
	MBCType mbc_type() const
	{
		return mbc_type_;
	}
	// ---- GBC-Ready Rule 8: CGB mode detected at load, stored, ignored in DMG ----
	// https://gbdev.io/pandocs/The_Cartridge_Header.html#0143--cgb-flag
	// GBC: CGB_Compatible/CGB_Only will select CGB boot, palettes, WRAM/VRAM banks.
	HardwareMode hardware_mode() const
	{
		return hardware_mode_;
	}
	u32 rom_size() const
	{
		return static_cast<u32>(rom_image_.size());
	}
	u32 ram_size() const;
	bool loaded() const
	{
		return mbc_ != nullptr;
	}

  private:
	std::vector<u8> rom_image_;
	CartridgeHeader header_{};
	MBCType mbc_type_ = MBCType::None;
	HardwareMode hardware_mode_ = HardwareMode::DMG;
	std::unique_ptr<MBC> mbc_;

	bool parse_header();
	static HardwareMode detect_hardware_mode(u8 cgb_flag);
	static MBCType detect_mbc_type(u8 type_byte);
	static u32 calc_rom_size(u8 size_byte);
	static u32 calc_ram_size(u8 size_byte);
};

} // namespace gb
