// Timer: DIV free-running divider + TIMA/TMA/TAC.
// Accurate model: TIMA clocks on the falling edge of (selected DIV bit AND
// Timer enable), per https://gbdev.io/pandocs/Timer.html. DIV writes reset
// the internal counter, which can itself clock TIMA via the same edge rule.

#include "gb/timer.h"
#include "gb/mmu.h"

namespace gb
{

Timer::Timer()
{
	reset();
}

void Timer::reset()
{
	div_counter_ = 0xABCC; // Post-boot value; DIV reads 0xAB.
	tima_ = 0;
	tma_ = 0;
	tac_ = 0;
}

void Timer::step(u32 mcycles)
{
	// GBC: T-cycles here scale with double_speed (KEY1); M-cycle counts from
	// the CPU stay the same, so this loop is GBC-safe as written.
	for (u32 i = 0; i < mcycles * 4; i++) {
		const u16 prev = div_counter_;
		div_counter_++;
		static constexpr int kBits[4] = {9, 3, 5, 7};
		const int bit = kBits[tac_ & 0x03];
		const bool enabled = (tac_ & 0x04) != 0;
		const bool was = enabled && ((prev & (1u << bit)) != 0);
		const bool now = enabled && ((div_counter_ & (1u << bit)) != 0);
		if (was && !now)
			clock_tima();
	}
}

void Timer::clock_tima()
{
	if (tima_ == 0xFF) {
		tima_ = tma_;
		// https://gbdev.io/pandocs/Interrupts.html (Timer vector 0x50).
		if (mmu_)
			mmu_->request_interrupt(2);
	} else {
		tima_++;
	}
}

u8 Timer::read_div() const
{
	return hi_byte(div_counter_);
}

u8 Timer::read_tima() const
{
	return tima_;
}

u8 Timer::read_tma() const
{
	return tma_;
}

u8 Timer::read_tac() const
{
	return 0xF8 | tac_;
}

void Timer::write_div(u8 /*value*/)
{
	// Any write resets the counter; the resulting falling edge can clock TIMA.
	static constexpr int kBits[4] = {9, 3, 5, 7};
	const int bit = kBits[tac_ & 0x03];
	const bool enabled = (tac_ & 0x04) != 0;
	const bool was = enabled && ((div_counter_ & (1u << bit)) != 0);
	div_counter_ = 0;
	if (was)
		clock_tima();
}

void Timer::write_tima(u8 value)
{
	tima_ = value;
}

void Timer::write_tma(u8 value)
{
	tma_ = value;
}

void Timer::write_tac(u8 value)
{
	// Changing TAC can itself produce the falling edge that clocks TIMA:
	// disabling while the selected DIV bit is high, or switching from a high
	// selected bit to a low one, must clock exactly once (Pan Docs
	// "Timer Control"). Evaluated against the OLD control value.
	static constexpr int kBits[4] = {9, 3, 5, 7};
	const u8 old = tac_;
	const int old_bit = kBits[old & 0x03];
	const bool old_enabled = (old & 0x04) != 0;
	const bool was = old_enabled && ((div_counter_ & (1u << old_bit)) != 0);
	tac_ = value & 0x07;
	const int new_bit = kBits[tac_ & 0x03];
	const bool new_enabled = (tac_ & 0x04) != 0;
	const bool now = new_enabled && ((div_counter_ & (1u << new_bit)) != 0);
	if (was && !now)
		clock_tima();
}

} // namespace gb
