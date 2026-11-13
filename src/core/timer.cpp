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
	tima_reload_ = TimaReload::Running;
	div_frozen_ = false;
	double_speed_ = false;
}

void Timer::step(u32 mcycles)
{
	// CGB STOP speed-switch stall: DIV (and TIMA with it) does not tick.
	if (div_frozen_)
		return;
	// CGB double-speed: the divider advances twice per M-cycle (2x T rate),
	// so DIV/TIMA run at double wall-rate while PPU/APU stay fixed.
	const u32 scale = double_speed_ ? 8 : 4;
	for (u32 i = 0; i < mcycles * scale; i++) {
		// SameBoy steps the reload state machine once per M-cycle, before the
		// divider edge for that M-cycle.
		if ((i % scale) == 0)
			advance_tima_reload();
		const u16 prev = div_counter_;
		div_counter_++;
		// The serial port is clocked by the same divider: the transfer runs off
		// the selected bit falling, which is why SC=0x81 does not finish in the
		// same write (SameBoy GB_set_internal_div_counter -> GB_serial_master_edge).
		if (mmu_ && (prev & mmu_->serial_mask()) != 0 && (div_counter_ & mmu_->serial_mask()) == 0)
			mmu_->serial_clock_edge();
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
	tima_++;
	if (tima_ == 0) {
		// The register takes TMA straight away, but the *reload* is not
		// finished: reads report 0 for the next M-cycle, the timer interrupt
		// arrives one M-cycle after that, and only then does a TIMA write stick
		// again. Writing TMA inside that window cancels the reload (the TMA
		// write below also lands in TIMA).
		tima_ = tma_;
		tima_reload_ = TimaReload::Reloading;
	}
}

void Timer::advance_tima_reload()
{
	// Reloaded -> Running is bookkeeping; Reloading -> Reloaded is where the
	// timer interrupt (vector 0x50) is finally raised.
	// https://gbdev.io/pandocs/Interrupts.html
	if (tima_reload_ == TimaReload::Reloaded) {
		tima_reload_ = TimaReload::Running;
	} else if (tima_reload_ == TimaReload::Reloading) {
		if (mmu_)
			mmu_->request_interrupt(2);
		tima_reload_ = TimaReload::Reloaded;
	}
}

u8 Timer::read_div() const
{
	return hi_byte(div_counter_);
}

u8 Timer::read_tima() const
{
	// During the reload window the counter reads back as 0 even though the
	// register already holds TMA (SameBoy Core/memory.c GB_IO_TIMA read).
	if (tima_reload_ == TimaReload::Reloading)
		return 0;
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
	// A write in the Reloaded window is dropped: the reload has already
	// happened, so the value would be overwritten by it anyway.
	if (tima_reload_ != TimaReload::Reloaded)
		tima_ = value;
}

void Timer::write_tma(u8 value)
{
	tma_ = value;
	// Writing TMA while the reload is pending cancels it: TIMA keeps the new
	// TMA instead of the one the overflow had already latched.
	if (tima_reload_ != TimaReload::Running)
		tima_ = value;
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

void Timer::save(StateBuffer &buf) const
{
	buf.write(div_counter_);
	buf.write(tima_);
	buf.write(tma_);
	buf.write(tac_);
	buf.write(static_cast<u8>(tima_reload_));
	buf.write(div_frozen_);
	buf.write(double_speed_);
}

void Timer::load(StateBuffer &buf)
{
	buf.read(div_counter_);
	buf.read(tima_);
	buf.read(tma_);
	buf.read(tac_);
	u8 reload = 0;
	buf.read(reload);
	tima_reload_ = static_cast<TimaReload>(reload);
	buf.read(div_frozen_);
	buf.read(double_speed_);
}
} // namespace gb
