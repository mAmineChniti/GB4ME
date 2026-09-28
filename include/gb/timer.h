#pragma once

#include "types.h"

namespace gb
{

class MMU; // Bus. Timer raises interrupts via MMU::request_interrupt.

// Timer: DIV divider + TIMA/TMA/TAC.
// https://gbdev.io/pandocs/Timer.html
class Timer
{
  public:
	Timer();
	~Timer() = default;

	void reset();
	void set_mmu(MMU *mmu)
	{
		mmu_ = mmu;
	}
	// Advance by M-cycles (internally stepped as T-cycles = mcycles * 4,
	// or * 8 in CGB double-speed: Timer/Divider run 2x wall-rate there).
	void step(u32 mcycles);
	// CGB double-speed tracking (Pan Docs: Timer and Divider Registers run
	// twice as fast; LCD/HDMA/sound keep their usual rate — the APU already
	// runs off its own wall-clock counter, so only the timer doubles here).
	// Wired once in MMU::do_speed_switch; cleared by reset().
	void set_double_speed(bool dbl)
	{
		double_speed_ = dbl;
	}
	u8 read_div() const;
	u8 read_tima() const;
	u8 read_tma() const;
	u8 read_tac() const;

	void write_div(u8 value);
	void write_tima(u8 value);
	void write_tma(u8 value);
	void write_tac(u8 value);

	// CGB speed-switch freeze (Pan Docs KEY1): DIV does not tick while the
	// CPU is stopped for 2050 M-cycles after STOP (TIMA frozen with it).
	void set_div_frozen(bool frozen)
	{
		div_frozen_ = frozen;
	}

  private:
	MMU *mmu_ = nullptr;

	u16 div_counter_ = 0; // Internal 16-bit divider; DIV reads the high byte.
	u8 tima_ = 0;
	u8 tma_ = 0;
	u8 tac_ = 0;
	bool div_frozen_ = false;   // Set during the STOP speed-switch stall.
	bool double_speed_ = false; // CGB KEY1: divider runs at 2x wall rate.

	void clock_tima();
};

} // namespace gb
