#pragma once

#include "gba/state.h"

namespace gba
{

class Arm7Tdmi;
class GbaScheduler;

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
	// A raised IF bit reaches the CPU two cycles later (GBATEK "GBA
	// Interrupts": interrupt delay; mGBA GBAIRQDelay). The flag itself is
	// visible immediately - software polls IF right after a source fires -
	// so only the delivery to the CPU is deferred through the scheduler.
	void setScheduler(GbaScheduler *scheduler)
	{
		scheduler_ = scheduler;
	}
	static constexpr u32 kIrqDelay = 2;
	// True while a deferred delivery is scheduled (save states cannot carry
	// it: the callback is type-erased, so rearmPending() re-arms it).
	bool delivery_pending() const
	{
		return delay_armed_;
	}
	// How many IRQs have been handed to the CPU (diagnostic / tests).
	u32 deliveries() const
	{
		return deliveries_;
	}
	void rearmPending();

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
	GbaScheduler *scheduler_ = nullptr;
	bool delay_armed_ = false;
	u32 deliveries_ = 0;
	u16 ie_ = 0;
	u16 if_ = 0;
	u16 ime_ = 0;
	bool trace_ = false; // GB4ME_IRQ_TRACE debug aid.
	u64 now_ = 0;

	void update();
};

} // namespace gba
