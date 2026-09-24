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
	// Advance by M-cycles (internally stepped as T-cycles = mcycles * 4).
	// GBC: T-cycle rate scales with double_speed; M-cycle accounting is unchanged.
	void step(u32 mcycles);

	u8 read_div() const;
	u8 read_tima() const;
	u8 read_tma() const;
	u8 read_tac() const;

	void write_div(u8 value);
	void write_tima(u8 value);
	void write_tma(u8 value);
	void write_tac(u8 value);

  private:
	MMU *mmu_ = nullptr;

	u16 div_counter_ = 0; // Internal 16-bit divider; DIV reads the high byte.
	u8 tima_ = 0;
	u8 tma_ = 0;
	u8 tac_ = 0;

	void clock_tima();
};

} // namespace gb
