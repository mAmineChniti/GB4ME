#pragma once

#include "state.h"
#include "types.h"
#include <array>
#include <cstddef>
#include <cstdint>
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
	// Rumble motor state (MBC5 bit 3, Pak13/other rumble mappers). The core
	// only records it; the frontend drives the controller from it.
	virtual bool has_rumble() const
	{
		return false;
	}
	virtual bool rumble() const
	{
		return false;
	}
	// MBC3 real-time clock persistence. Default: no RTC hardware.
	virtual bool has_rtc() const
	{
		return false;
	}
	// MBC7 tilt sensor. Default: no accelerometer.
	virtual void set_accelerometer(int x, int y)
	{
		(void) x;
		(void) y;
	}
	virtual bool save_rtc(const std::string &path) const
	{
		(void) path;
		return false;
	}
	virtual bool load_rtc(const std::string &path)
	{
		(void) path;
		return false;
	}
	// Savestate. The ROM image is immutable once the cartridge is loaded and
	// a state is only ever loaded back into the machine that wrote it, so
	// each MBC saves its MUTABLE state: the bank registers, the RAM/HRAM
	// contents and the RTC. NoMBC and ROM-only mappers need none of it.
	virtual void save(StateBuffer &buf) const
	{
		(void) buf;
	}
	virtual void load(StateBuffer &buf)
	{
		(void) buf;
	}
};

// ROM-only cartridge (no MBC). 32KB ROM, optional 8KB RAM.
class NoMBC : public MBC
{
  public:
	NoMBC(std::vector<u8> rom, std::vector<u8> ram, bool battery = false);
	void save(StateBuffer &buf) const override;
	void load(StateBuffer &buf) override;
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
	void save(StateBuffer &buf) const override;
	void load(StateBuffer &buf) override;
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
	// MBC1M multicart wiring, set once by the loader's header heuristic.
	void set_multicart(bool v)
	{
		multicart_ = v;
	}
	bool multicart() const
	{
		return multicart_;
	}

  private:
	std::vector<u8> rom_;
	std::vector<u8> ram_;
	bool battery_ = false;
	bool ram_enabled_ = false;
	u8 rom_bank_low5_ = 1;      // raw 5-bit register (bit 4 ignored iff multicart)
	u8 rom_bank_high2_ = 0;     // upper 2 bits (bits 5-6, or 4-5 iff multicart)
	u8 ram_bank_ = 0;           // 2-bit RAM bank / ROM bank upper bits
	bool banking_mode_ = false; // false = ROM banking, true = RAM banking
	// MBC1M multicart wiring (Pan Docs "MBC1M", SameBoy GB_MBC1M_WIRING):
	// detected at load from the bank-$10 Nintendo header, never serialized
	// (like mbc_type_, it comes from the ROM, not the state).
	bool multicart_ = false;

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
	void save(StateBuffer &buf) const override;
	void load(StateBuffer &buf) override;
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
	void save(StateBuffer &buf) const override;
	void load(StateBuffer &buf) override;
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
	bool has_rtc() const override
	{
		return rtc_;
	}
	// MBC30 (big MBC3: >2MB ROM or >32KB RAM, SameBoy is_mbc30): full 8-bit
	// ROM bank instead of the 7-bit mask. Set once by the loader's size
	// heuristic, never serialized (load-time property like mbc_type_).
	void set_mbc30(bool v)
	{
		is_mbc30_ = v;
	}
	bool is_mbc30() const
	{
		return is_mbc30_;
	}
	bool save_rtc(const std::string &path) const override;
	bool load_rtc(const std::string &path) override;

  private:
	std::vector<u8> rom_;
	std::vector<u8> ram_;
	bool battery_ = false;
	bool rtc_ = false;
	bool is_mbc30_ = false;
	bool ram_enabled_ = false;
	u8 rom_bank_ = 1; // 7 bits.
	u8 ram_bank_ = 0; // 0-3 = RAM, 8-C = RTC.
	// Real-time clock (types 0x0F/0x10): host wall-clock backed.
	// https://gbdev.io/pandocs/MBC3.html#the-clock-counter-registers
	// `rtc_latched_` is what reads return (updated by the 0->1 latch edge);
	// `rtc_set_` is the game-written base. The live time is
	// `rtc_base_secs_ + (halted ? 0 : host_now - rtc_base_host_)`.
	std::array<u8, 5> rtc_latched_{}; // S M H DL DH (latched reads).
	std::array<u8, 5> rtc_set_{};     // S M H DL DH shadow (game writes).
	std::int64_t rtc_base_secs_ = 0;  // Emulated RTC seconds at rebase.
	std::int64_t rtc_base_host_ = 0;  // Host time() at rebase.
	bool rtc_base_valid_ = false;
	bool rtc_halt_ = false;
	bool rtc_carry_ = false; // Sticky day-counter overflow (>= 512 days).
	u8 latch_state_ = 0;
	void rtc_ensure_base();
	std::int64_t rtc_total_seconds() const;
	void rtc_latch();
	void rtc_apply_write(u8 index, u8 value);
};

// MBC5: up to 8MB ROM / 128KB RAM, with the rumble motor (bit 3 of a
// 0x4000-0x5FFF write selects it; Pan Docs "MBC5").
// https://gbdev.io/pandocs/MBC5.html
class MBC5 : public MBC
{
  public:
	MBC5(std::vector<u8> rom, std::vector<u8> ram, bool battery);
	void save(StateBuffer &buf) const override;
	void load(StateBuffer &buf) override;
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
	bool has_rumble() const override
	{
		return true;
	}
	bool rumble() const override
	{
		return rumble_;
	}

  private:
	std::vector<u8> rom_;
	std::vector<u8> ram_;
	bool battery_ = false;
	bool ram_enabled_ = false;
	bool rumble_ = false; // MBC5 rumble motor bit.
	u16 rom_bank_ = 1;    // 9 bits (0 maps to 1).
	u8 ram_bank_ = 0;     // 4 bits.
};

// MBC7: accelerometer + EEPROM mapper (Pan Docs "MBC7", SameBoy's mbc7
// model). 7-bit ROM bank and a 256-byte $A000 window that is NOT plain RAM:
// $A020/$A030 report the latched tilt, $A080 bit-bangs a 93LC56 EEPROM (128 x
// 16-bit words), and every other address in the window reads $FF. Both enables
// must be set for any of it ($0000 = $0A and $4000 = $40).
class MBC7 : public MBC
{
  public:
	MBC7(std::vector<u8> rom, std::vector<u8> ram);
	void save(StateBuffer &buf) const override;
	void load(StateBuffer &buf) override;
	u8 read_rom(u16 addr) const override;
	void write_rom(u16 addr, u8 value) override;
	u8 read_ram(u16 addr) const override;
	void write_ram(u16 addr, u8 value) override;
	u8 raw_read(u32 bank, u16 addr) const override;
	void raw_write(u32 bank, u16 addr, u8 value) override;
	u32 raw_bank_count() const override;
	bool has_battery() const override
	{
		return true; // Type 0x22 is MBC7+ACCEL+EEPROM+BATTERY.
	}
	// Tilt for the $A020/$A030 latch: level (0,0) reads 0x81D0 on both axes,
	// which is what a game sees with the cart lying flat.
	void set_accelerometer(int x, int y) override
	{
		accel_x_ = x;
		accel_y_ = y;
	}

  private:
	// One EEPROM clock edge: the $A080 bit-bang shifts one bit of the command
	// or of the 16-bit data word in or out.
	void eeprom_clock(bool di);
	std::vector<u8> rom_;
	std::vector<u8> ram_; // 256 bytes: 128 16-bit EEPROM words.
	u8 rom_bank_ = 1;
	bool ram_enable_ = false;
	bool secondary_enable_ = false;
	// Tilt latch: $55 to $A000 arms it, $AA to $A010 samples the accelerometer.
	bool latch_ready_ = true;
	u16 x_latch_ = 0x8000;
	u16 y_latch_ = 0x8000;
	int accel_x_ = 0;
	int accel_y_ = 0;
	// EEPROM bit-bang state (93LC56).
	bool eeprom_cs_ = false;
	bool eeprom_clk_ = false;
	bool eeprom_do_ = true;
	bool eeprom_di_ = false;
	bool eeprom_write_enabled_ = false;
	u16 read_bits_ = 0xFFFF;
	u16 command_ = 0;
	u8 argument_bits_left_ = 0;
};

// HuC1: Hudson Soft MBC (Pan Docs "HuC1"). Despite the name it is NOT MBC1:
// no RAM gate (0000-1FFF picks the RAM vs IR view), full-byte ROM/RAM banks,
// no 00->01 translation, 6000-7FFF ignored. The IR receiver has no local
// source in single-player, so reads report "no light" ($C0); the TX LED
// still latches and is observable for tests.
class HuC1 : public MBC
{
  public:
	HuC1(std::vector<u8> rom, std::vector<u8> ram);
	void save(StateBuffer &buf) const override;
	void load(StateBuffer &buf) override;
	u8 read_rom(u16 addr) const override;
	void write_rom(u16 addr, u8 value) override;
	u8 read_ram(u16 addr) const override;
	void write_ram(u16 addr, u8 value) override;
	u8 raw_read(u32 bank, u16 addr) const override;
	void raw_write(u32 bank, u16 addr, u8 value) override;
	u32 raw_bank_count() const override;
	bool has_battery() const override
	{
		return true; // Type 0xFF is HuC1+RAM+BATTERY, the only HuC1 type.
	}
	bool ir_led() const
	{
		return tx_led_;
	}

  private:
	std::vector<u8> rom_;
	std::vector<u8> ram_;
	u8 rom_bank_ = 1;
	u8 ram_bank_ = 0;
	bool ir_mode_ = false;
	bool tx_led_ = false;
};

// HuC3: Hudson Soft MBC + RTC (Pan Docs "HuC-3", SameBoy huc3 model). Banking
// like HuC1 (full-byte banks, no translation) plus a mode-selected $A000
// window: RAM (0 = read-only, $A = read/write), RTC mailbox ($B write / $C
// read / $D always-ready semaphore), IR ($E). The mailbox executes
// synchronously on write (SameBoy), so no MCU timing is modeled. The clock
// itself is host wall-clock backed like MBC3 (minute/day counters advanced
// lazily on read; sidecar `<rom>.rtc`). The tone generator accepts writes
// silently (no beeper). IR receive reports no-light (no local source).
class HuC3 : public MBC
{
  public:
	HuC3(std::vector<u8> rom, std::vector<u8> ram);
	void save(StateBuffer &buf) const override;
	void load(StateBuffer &buf) override;
	u8 read_rom(u16 addr) const override;
	void write_rom(u16 addr, u8 value) override;
	u8 read_ram(u16 addr) const override;
	void write_ram(u16 addr, u8 value) override;
	u8 raw_read(u32 bank, u16 addr) const override;
	void raw_write(u32 bank, u16 addr, u8 value) override;
	u32 raw_bank_count() const override;
	bool has_battery() const override
	{
		return true; // Type 0xFE is HuC3+RAM+BATTERY, the only HuC3 type.
	}
	bool has_rtc() const override
	{
		return true;
	}
	bool ir_led() const
	{
		return tx_led_;
	}
	bool save_rtc(const std::string &path) const override;
	bool load_rtc(const std::string &path) override;

  private:
	std::vector<u8> rom_;
	std::vector<u8> ram_;
	u8 rom_bank_ = 1; // 7 bits.
	u8 ram_bank_ = 0; // 4 bits.
	u8 mode_ = 0x0A;  // $A000 view: RAM read/write at power-on.
	u8 access_index_ = 0;
	u8 read_ = 0; // Last mailbox response nybble.
	u8 access_flags_ = 0;
	u16 alarm_minutes_ = 0;
	u16 alarm_days_ = 0;
	bool alarm_enabled_ = false;
	bool tx_led_ = false;
	// Wall-clock backing (mutable: advanced lazily inside const reads, like
	// the mutable cursor in StateBuffer). Counters tick with the host, so
	// no per-frame hook is needed.
	mutable u16 minutes_ = 0;
	mutable u16 days_ = 0;
	mutable std::int64_t rtc_base_host_ = 0;
	mutable bool rtc_base_valid_ = false;
	void rtc_ensure_base() const;
	void rtc_advance() const; // Fold elapsed host minutes into the counters.
	void rtc_rebase();        // Re-anchor the clock after a game time-set.
	void mailbox(u8 value);   // Execute one $B write immediately.
};

// MMM01: multicart controller (Pan Docs "MMM01"; register writes follow
// SameBoy's shape, mask semantics follow Pan Docs). Boots "unmapped" (last
// 32 KiB of ROM at 0000-7FFF, so the menu header lives at size-32KiB+$100);
// the menu picks a game via the extra bank/mask registers and locks into
// "mapped" MBC1 emulation. Write locks keep the menu's game-select bits
// (locked writes are prevented, not forced); the mapped $00->$01 rule sees
// only unmasked low bits (Pan Docs $10/$30->$11 example). Multiplex mode was
// never used commercially. New type: no old states exist.
class MMM01 : public MBC
{
  public:
	MMM01(std::vector<u8> rom, std::vector<u8> ram, bool battery);
	void save(StateBuffer &buf) const override;
	void load(StateBuffer &buf) override;
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
	bool locked_ = false; // Mapped (MBC1 emulation) once the menu locks.
	bool multiplex_ = false;
	bool mbc1_mode_ = false; // RAM banking (plain) / advanced ROM (multiplex).
	bool mbc1_mode_disable_ = false;
	u8 rom_low_ = 0;   // 7-bit register; $00 behaves as $01 (masked form).
	u8 rom_mid_ = 0;   // Extra ROM bits, menu-locked.
	u8 rom_high_ = 0;  // Extra ROM bits, menu-locked.
	u8 ram_low_ = 0;   // MBC1 RAM bank (or extra ROM bits if multiplexed).
	u8 ram_high_ = 0;  // Extra RAM bits, menu-locked.
	u8 rom_lock5_ = 0; // Write locks for rom_low_, in position (bit 0 clear).
	u8 ram_lock2_ = 0; // Write locks for ram_low_, in position.
	u32 rom_bank0() const;
	u32 rom_bank() const;
	u32 ram_bank() const;
};

// MBC6: two independently switchable 8 KiB ROM/flash banks + two 4 KiB RAM
// banks, wrapping a Macronix MX29F008TC 1 MiB flash chip. Used by Net de Get:
// Minigame @ 100, which downloads minigames into the flash over the Mobile
// Adapter. https://gbdev.io/pandocs/MBC6.html
class MBC6 : public MBC
{
  public:
	MBC6(std::vector<u8> rom, std::vector<u8> ram);
	void save(StateBuffer &buf) const override;
	void load(StateBuffer &buf) override;
	u8 read_rom(u16 addr) const override;
	void write_rom(u16 addr, u8 value) override;
	u8 read_ram(u16 addr) const override;
	void write_ram(u16 addr, u8 value) override;
	u8 raw_read(u32 bank, u16 addr) const override;
	void raw_write(u32 bank, u16 addr, u8 value) override;
	u32 raw_bank_count() const override;
	bool has_battery() const override
	{
		return true; // Type 0x20 is MBC6+RAM+BATTERY.
	}
	// Tilt for the accelerometer some MBC6 carts add on top of the flash.
	void set_accelerometer(int x, int y) override
	{
		accel_x_ = x;
		accel_y_ = y;
	}

  private:
	// ---- the register file ($0000-$3FFF, 8-bit mirrors of 4 registers) ----
	void write_flash(u16 addr, u8 value);
	u8 flash_read(u32 off) const;
	void flash_write(u32 off, u8 value);
	void flash_command(u8 cmd, u32 off);
	void flash_erase_sector(u32 sector);
	void flash_erase_chip();
	// Is `off` the $5555 / $2AAA command pair (within an 8 KiB bank)?
	bool is_cmd_5555(u32 off) const
	{
		return (off & 0x1FFF) == 0x1555;
	}
	bool is_cmd_2aaa(u32 off) const
	{
		return (off & 0x1FFF) == 0x0AAA;
	}
	// Pan Docs: the commands marked * need the Flash Write Enable bit.
	bool flash_writable_sector0() const
	{
		return flash_wp_ == 0;
	}

	std::vector<u8> rom_;
	std::vector<u8> ram_; // 8 x 4 KiB SRAM (banks A and B are views into it)
	// Battery-save access for the whole 32 KiB array.
	u8 raw_read_ram(u32 off) const
	{
		return off < ram_.size() ? ram_[off] : 0xFF;
	}
	void raw_write_ram(u32 off, u8 value)
	{
		if (off < ram_.size())
			ram_[off] = value;
	}
	std::vector<u8> flash_;  // 1 MiB: 128 banks of 8 KiB
	std::vector<u8> hidden_; // The flash's hidden 256 bytes ($77 command)
	bool ram_enable_ = false;
	u8 ram_bank_a_ = 0;
	u8 ram_bank_b_ = 0;
	bool flash_enable_ = false;
	bool flash_wp_ = true; // /WP is high after power up, i.e. write-protected
	u8 bank_a_num_ = 0;
	bool bank_a_flash_ = false;
	u8 bank_b_num_ = 0;
	bool bank_b_flash_ = false;
	// Flash command state machine.
	enum class FlashState : u8 {
		Read,          // normal reads
		Status,        // erase/program in flight: reads return the status byte
		Program,       // $A0: writes are programmed into the array
		ProgramHidden, // $E0
		Id,            // $90: C2 81 at offsets 0/1
		HiddenRead,    // $77: the hidden 256 bytes are readable
	};
	FlashState flash_state_ = FlashState::Read;
	// The two-step command unlock ($AA to $5555, then $55 to $2AAA).
	bool cmd_unlock1_ = false;
	bool cmd_unlock2_ = false;
	// Kept for clarity: the $77 command selects the hidden region, which is
	// what FlashState::HiddenRead already means.
	bool hidden_read_ = false;
	bool sector0_protected_ = false; // Non-volatile: the Protect Sector 0 command.
	int accel_x_ = 0;
	int accel_y_ = 0;
};

// TAMA5 (Bandai, type $FD): the Tamagotchi mapper. It has no $0000-$1FFF
// register file at all - the whole mapper is two 4-bit registers on the cart
// bus, `$A000` (data) and `$A001` (register select), and every access is a
// nibble-at-a-time conversation with the chip. Used by one game: Game de
// Hakken!! Tamagotchi - Osutchi to Mesutchi (Tamagotchi 3).
// Sources: Pan Docs "Bandai TAMA5" (a stub - it only names the chip set) and
// Tauwasser's reverse-engineering notes (gbdev.gg8.se, "TAMA5 (WIP)"), which
// are the only detailed description of the protocol.
class TAMA5 : public MBC
{
  public:
	TAMA5(std::vector<u8> rom, const std::vector<u8> &ram);
	void save(StateBuffer &buf) const override;
	void load(StateBuffer &buf) override;
	u8 read_rom(u16 addr) const override;
	void write_rom(u16 addr, u8 value) override;
	u8 read_ram(u16 addr) const override;
	void write_ram(u16 addr, u8 value) override;
	u8 raw_read(u32 bank, u16 addr) const override;
	void raw_write(u32 bank, u16 addr, u8 value) override;
	u32 raw_bank_count() const override;
	bool has_battery() const override
	{
		return true; // Type $FD is battery-backed.
	}

  private:
	// `$A000` is the data register, `$A001` the register index. The high
	// nibble of a data read is hi-Z on hardware, so every read ORs $F0 in.
	static constexpr u8 kHiZ = 0xF0;
	// Applies the command assembled by the $4/$5 (data in) and $6/$7
	// (address/command) register writes. Called when $7 is written, i.e. when
	// the low nibble completes the command.
	void run_command();
	u8 command() const
	{
		return static_cast<u8>((addr_hi_ << 4) | addr_lo_);
	}

	std::vector<u8> rom_;
	u8 ram_[32] = {}; // 32 bytes, not a banked SRAM
	bool unlocked_ = false;
	u8 reg_ = 0;           // register selected through $A001
	u8 rom_bank_low_ = 0;  // register 0: address bits 14-17
	u8 rom_bank_high_ = 0; // register 1: address bit 18
	u8 data_in_lo_ = 0;    // register 4
	u8 data_in_hi_ = 0;    // register 5
	u8 addr_hi_ = 0;       // register 6 (also the command's high nibble)
	u8 addr_lo_ = 0;       // register 7 (also the command's low nibble)
	u8 data_out_lo_ = 0;   // register $C
	u8 data_out_hi_ = 0;   // register $D
	// Reads before the unlock alternate $F0/$FF (the chip's shift register is
	// running with nothing selected), so the pattern needs a toggle - mutable
	// because `read_ram` is const (every other mapper's read path is).
	mutable bool unlocked_read_toggle_ = false;
};

enum class MBCType : u8 {
	None = 0,
	MBC1 = 1,
	MBC2 = 2,
	MBC3 = 3,
	MBC5 = 5,
	HuC1 = 6,
	HuC3 = 7,
	MMM01 = 8,
	MBC7 = 9,
	MBC6 = 10,
	TAMA5 = 11,
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
	// MBC3 RTC wall-clock persistence (`<rom>.rtc` sidecar). No-op without
	// RTC hardware; safe to call unconditionally from the frontend.
	bool has_rtc() const;
	// MBC7 accelerometer, in the tilt sensor's own units (SameBoy's
	// accelerometer, roughly -7..+7 with 0 = level). Safe to call on any cart.
	void set_accelerometer(int x, int y);
	// Rumble motor (MBC5 bit 3). The frontend polls this each frame and
	// drives the controller; no-op on carts without a motor.
	bool has_rumble() const;
	bool rumble() const;
	bool save_rtc(const std::string &path) const;
	bool load_rtc(const std::string &path);
	// Savestate: delegates to the mapper (see MBC::save). mbc_type_ and
	// hardware_mode_ come from the ROM header at load() time and are not
	// state - a state is only loaded into the cartridge that wrote it.
	void save(StateBuffer &buf) const;
	void load(StateBuffer &buf);

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
	// Override the header-derived model. Running a CGB-flagged ROM on DMG
	// hardware is a normal emulator option and several DMG-only test ROMs
	// (Blargg oam_bug) only behave correctly that way.
	void set_hardware_mode(HardwareMode mode)
	{
		hardware_mode_ = mode;
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
	// Human-readable reason the last load() failed (unsupported mapper,
	// bad header, short file). Empty when the last load succeeded.
	const std::string &last_error() const
	{
		return last_error_;
	}

  private:
	std::vector<u8> rom_image_;
	std::string last_error_;
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
