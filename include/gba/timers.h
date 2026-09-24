#pragma once

#include "gba/state.h"
#include <functional>

namespace gba
{

class GbaIrq;

// GBA timers 0-3 (Phase 5). Prescalers F/1, F/64, F/256, F/1024; timers 1-3
// support count-up (cascade) on the previous timer's overflow. Overflow
// reloads the counter, optionally raises the timer IRQ, and cascades.
// https://problemkaputt.de/gbatek.htm#gbatimers
class GbaTimers
{
  public:
	static constexpr u32 kIoBase = 0x04000100;
	static constexpr unsigned kCount = 4;

	// CNT_H bits.
	static constexpr u16 kPrescalerMask = 0x0003;
	static constexpr u16 kCascade = 0x0004;
	static constexpr u16 kIrqEnable = 0x0040;
	static constexpr u16 kEnable = 0x0080;
	static constexpr u16 kControlMask = 0x00C7;

	GbaTimers();

	void reset();
	void setIrq(GbaIrq *irq)
	{
		irq_ = irq;
	}
	// Phase 8 hook: audio FIFO consumption on timer 0/1 overflow.
	void setOverflowHook(std::function<void(unsigned)> hook)
	{
		overflow_hook_ = hook;
	}

	// Save state serialization.
	void save(StateBuffer &out) const;
	void load(const StateBuffer &in);

	// Advance all timers by `cycles` ticks.
	void step(u32 cycles);

	u16 read(u32 addr) const; // CNT_L (live counter) / CNT_H (control).
	void write(u32 addr, u16 value);

	u16 counter(unsigned i) const
	{
		return channels_[i].counter;
	}
	u16 control(unsigned i) const
	{
		return channels_[i].control;
	}

  private:
	struct Channel {
		u16 reload = 0;
		u16 counter = 0;
		u16 control = 0;
		u32 phase = 0; // Prescaler progress in ticks.
	};

	GbaIrq *irq_ = nullptr;
	std::function<void(unsigned)> overflow_hook_;
	Channel channels_[kCount]{};

	static u32 prescaler(u16 control)
	{
		static constexpr u32 kTable[4] = {1, 64, 256, 1024};
		return kTable[control & kPrescalerMask];
	}

	bool enabled(unsigned i) const
	{
		return (channels_[i].control & kEnable) != 0;
	}
	bool cascade(unsigned i) const
	{
		return i != 0 && (channels_[i].control & kCascade) != 0;
	}

	void clockChannel(unsigned i); // One counter increment (may overflow).
	void overflow(unsigned i);
};

} // namespace gba
