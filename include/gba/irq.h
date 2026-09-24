#pragma once

#include "gba/state.h"

namespace gba
{

class Arm7Tdmi;

// GBA interrupt controller (Phase 5): IE/IF/IME + level-triggered IRQ
// delivery to the CPU. Follows the gb::MMU pattern (bus routes the three
// registers here; devices raise via raise()).
// https://problemkaputt.de/gbatek.htm#gbainterruptcontrol
class GbaIrq
{
  public:
	// IF bit assignments (GBATEK); shared with IntrWait masks.
	static constexpr u16 kVBlank = 1u << 0;
	static constexpr u16 kHBlank = 1u << 1;
	static constexpr u16 kVCount = 1u << 2;
	static constexpr u16 kTimer0 = 1u << 3;
	static constexpr u16 kTimer1 = 1u << 4;
	static constexpr u16 kTimer2 = 1u << 5;
	static constexpr u16 kTimer3 = 1u << 6;
	static constexpr u16 kSerial = 1u << 7;
	static constexpr u16 kDma0 = 1u << 8;
	static constexpr u16 kDma1 = 1u << 9;
	static constexpr u16 kDma2 = 1u << 10;
	static constexpr u16 kDma3 = 1u << 11;
	static constexpr u16 kKeypad = 1u << 12;
	static constexpr u16 kGamePak = 1u << 13;

	GbaIrq();

	void reset();
	void setCpu(Arm7Tdmi *cpu)
	{
		cpu_ = cpu;
	}

	u16 readIe() const
	{
		return ie_;
	}
	u16 readIf() const
	{
		return if_;
	}
	u16 readIme() const
	{
		return ime_;
	}
	void writeIe(u16 v);
	void writeIf(u16 v); // Write-1-clears (IRQ acknowledge).
	void writeIme(u16 v);

	// Save state serialization.
	void save(StateBuffer &out) const;
	void load(const StateBuffer &in);

	// Device interrupt requests (timers, DMA, PPU, keypad, serial).
	void raise(u16 mask);
	// Global clock advance (trace timestamps only).
	void tick(u64 cycles);

  private:
	Arm7Tdmi *cpu_ = nullptr;
	u16 ie_ = 0;
	u16 if_ = 0;
	u16 ime_ = 0;
	bool trace_ = false; // GB4ME_IRQ_TRACE debug aid.
	u64 now_ = 0;

	void update();
};

} // namespace gba
