#pragma once

#include "types.h"
#include <array>
#include <string>

namespace gb
{

class Cartridge;
class PPU;
class APU;
class Timer;
class Joypad;
class CPU;

// Bus: the ONLY path between components and memory.
// https://gbdev.io/pandocs/Memory_Map.html
// GBC-Ready Rule 2: CPU/PPU/APU/Timer/Joypad MUST NOT touch each other's
// arrays directly; everything goes through read()/write() so GBC register
// handling can be swapped in later without touching components.
class MMU
{
  public:
	MMU();
	~MMU() = default;

	MMU(const MMU &) = delete;
	MMU &operator=(const MMU &) = delete;

	void reset();
	void set_cartridge(Cartridge *cart)
	{
		cartridge_ = cart;
	}
	void set_ppu(PPU *ppu)
	{
		ppu_ = ppu;
	}
	void set_apu(APU *apu)
	{
		apu_ = apu;
	}
	void set_timer(Timer *timer)
	{
		timer_ = timer;
	}
	void set_joypad(Joypad *joypad)
	{
		joypad_ = joypad;
	}

	u8 read(u16 addr);
	void write(u16 addr, u8 value);

	// Interrupt flag (0xFF0F) and interrupt enable (0xFFFF).
	// https://gbdev.io/pandocs/Interrupts.html
	u8 read_if() const
	{
		return 0xE0 | if_register_;
	}
	void write_if(u8 value)
	{
		if_register_ = value & 0x1F;
	}
	u8 read_ie() const
	{
		return ie_register_;
	}
	void write_ie(u8 value)
	{
		ie_register_ = value;
	}
	void request_interrupt(u8 bit)
	{
		if_register_ |= (1u << bit);
	}

	// Serial (for Blargg test output over SB/SC).
	// https://gbdev.io/pandocs/Serial_Data_Transfer_Link_Cable.html
	u8 serial_sb = 0;
	u8 serial_sc = 0;
	// Bytes shifted out while SC has the transfer-clock bit set. Blargg
	// test ROMs print "Passed"/"Failed" here; the headless runner dumps it.
	std::string serial_log;

	bool boot_rom_enabled = true;

	// Boot ROM load (DMG 256B at 0x0000-0x00FF).
	void load_boot_rom(const std::array<u8, 0x100> &data)
	{
		boot_rom_ = data;
	}

	void set_hardware_mode(HardwareMode m);
	HardwareMode hardware_mode() const
	{
		return hardware_mode_;
	}
	bool is_cgb() const
	{
		return is_cgb_mode(hardware_mode_);
	}

	// CGB speed switch: called by CPU STOP handler.
	bool prepare_speed_switch() const
	{
		return (key1_ & 0x01) != 0;
	}
	void do_speed_switch(class CPU *cpu);
	u8 read_key1() const;
	void write_key1(u8 v);

	// HDMA
	void step_hdma(u32 cycles); // called from update loop
	bool hdma_active() const
	{
		return hdma_active_;
	}
	void hdma_step_hblank();

  private:
	Cartridge *cartridge_ = nullptr;
	PPU *ppu_ = nullptr;
	APU *apu_ = nullptr;
	Timer *timer_ = nullptr;
	Joypad *joypad_ = nullptr;

	std::array<u8, 0x100> boot_rom_{};

	// ---- GBC-Ready Rule 4: WRAM modelled as banked from day one ----
	// C000-CFFF = bank 0 (fixed). D000-DFFF = bank 1 in DMG mode.
	// (Real hardware maps the upper half to bank 1 when SVBK reads 0;
	// mapping it to bank 0 aliases C000-CFFF and corrupts memory.)
	// Do NOT flatten to a single 8KB array.
	// GBC: D000-DFFF reads/writes will branch on wram_bank_ (SVBK).
	std::array<std::array<u8, 0x1000>, 8> wram_banks_{};
	u8 wram_bank_ = 1; // DMG: fixed 1. GBC: SVBK selection (1-7, 0 means 1).

	HardwareMode hardware_mode_ = HardwareMode::DMG;

	// CGB registers
	// KEY1 0xFF4D — bit7 speed, bit0 prepare
	u8 key1_ = 0; // lower bit prepare; high bit tracked via CPU double_speed but mirrored here
	// SVBK already via wram_bank_
	// HDMA 0xFF51-55
	u8 hdma1_ = 0, hdma2_ = 0, hdma3_ = 0, hdma4_ = 0, hdma5_ = 0;
	bool hdma_active_ = false;
	u16 hdma_src_ = 0, hdma_dst_ = 0;
	u8 hdma_len_ = 0; // remaining blocks
	bool hdma_mode_hblank_ = false;
	// Unused CGB regs FF72-75, OPRI is in PPU
	u8 ff72_ = 0, ff73_ = 0, ff74_ = 0, ff75_ = 0x8F;

	std::array<u8, 0x80> hram_{};
	u8 ie_register_ = 0;
	u8 if_register_ = 0;

	// ---- Memory-mapped registers grouped by hardware revision (Rule 6) ----
	// DMG registers: P1/SB/SC/DIV/TIMA/TMA/TAC/IF/LCDC/STAT/SCY/SCX/LY/LYC/
	//   DMA/BGP/OBP0/OBP1/WY/WX/IE.
	// Future CGB registers (stubs returning 0xFF, Rule 9): KEY1 (0xFF4D),
	//   VBK (0xFF4F), HDMA1-5 (0xFF51-55), BGPI/BGPD (0xFF68-69),
	//   OBPI/OBPD (0xFF6A-6B), SVBK (0xFF70).
	// GBC: each stub below will activate real behavior.
	u8 read_io(u16 addr);
	void write_io(u16 addr, u8 value);
	void hdma_start();
};

} // namespace gb
